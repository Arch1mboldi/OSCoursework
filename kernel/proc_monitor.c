// SPDX-License-Identifier: GPL-2.0
/*
 * kernel/proc_monitor.c — 进程全量监控系统调用实现
 *
 * 提供 3 个自定义系统调用:
 *   sys_proc_collect  — 一次性收集所有进程完整信息
 *   sys_proc_snapshot — 进程树父子关系拓扑快照
 *   sys_proc_stat     — 系统整体进程统计
 *
 * 系统调用号 (x86_64):
 *   470  proc_collect
 *   471  proc_snapshot
 *   472  proc_stat
 *
 * 安全设计:
 *   - rcu_read_lock() 保护遍历，杜绝 UAF: 所有 task_struct 在 RCU
 *     读临界区内不会被释放（__put_task_struct 通过 call_rcu 延迟回收）
 *   - vmalloc 内核缓冲区: 持锁一次性填充，释放锁后统一
 *     copy_to_user，消除持锁睡眠和锁间竞争窗口
 *   - 所有用户态数据通过 copy_to_user() 传递，杜绝内核指针泄露
 *
 * 依据:
 *   Documentation/process/adding-syscalls.rst   — SYSCALL_DEFINE 规范
 *   Documentation/RCU/listRCU.rst               — for_each_process RCU 遍历
 *   Documentation/RCU/rcu.rst                   — RCU 读者不可睡眠
 *   Documentation/filesystems/proc.rst           — /proc/pid/stat 字段定义
 *   Documentation/mm/active_mm.rst              — mm==NULL 内核线程语义
 *   include/linux/sched.h                       — __state / exit_state 分离
 */

#include <linux/kernel.h>
#include <linux/syscalls.h>
#include <linux/sched.h>
#include <linux/sched/signal.h>
#include <linux/sched/cputime.h>
#include <linux/jiffies.h>
#include <linux/uaccess.h>
#include <linux/cred.h>
#include <linux/pid.h>
#include <linux/mm.h>
#include <linux/vmalloc.h>
#include <linux/proc_monitor.h>

/* =========================================================================
 * sys_proc_collect — 一次性收集所有进程的完整信息
 *
 * @user_buf:  用户态 struct proc_info 数组指针
 * @max_count: 数组容量 (最大条目数)
 * @ret_count: 输出参数 — 实际填充的进程数
 *
 * 返回: 0 成功; -EINVAL 参数无效; -ENOMEM 内核内存不足; -EFAULT 拷贝失败
 *
 * 实现要点:
 *   1. vmalloc 分配内核中转缓冲区
 *   2. rcu_read_lock() + for_each_process() 一次性遍历所有进程填入 kbuf
 *   3. 释放 RCU 锁后一次性 copy_to_user，杜绝 UAF 和持锁睡眠
 *   4. RCU 保护期间 task_struct 不会被释放（见 __put_task_struct → call_rcu）
 * ========================================================================= */
SYSCALL_DEFINE3(proc_collect,
		struct proc_info __user *, user_buf,
		int, max_count,
		int __user *, ret_count)
{
	struct proc_info *kbuf;
	struct task_struct *task;
	int count = 0;

	if (!user_buf || max_count <= 0 || !ret_count)
		return -EINVAL;

	/* 内核分配中转缓冲区。
	 * vmalloc 分配虚拟连续内存，适合可能很大的数组。 */
	kbuf = vmalloc(max_count * sizeof(struct proc_info));
	if (!kbuf)
		return -ENOMEM;

	/*
	 * RCU 读临界区保护遍历: 期间所有 task_struct 不会被释放。
	 * for_each_process 是 RCU-safe 的进程链表遍历宏，
	 * 等价于 for (task = &init_task; (task = next_task(task)) != &init_task; )
	 *
	 * 注意: rcu_read_lock() 期间绝对不能睡眠、不能调用 copy_to_user！
	 */
	rcu_read_lock();
	for_each_process(task) {
		if (count >= max_count)
			break;

		memset(&kbuf[count], 0, sizeof(struct proc_info));

		/* 安全获取进程名 (最多 TASK_COMM_LEN-1 字符) */
		get_task_comm(kbuf[count].comm, task);

		/* PID / PPID。
		 * rcu_dereference() 确保编译器不会将 real_parent 的读取
		 * 优化到临界区之外，且正确处理 reparent 的并发更新。 */
		kbuf[count].pid  = task_tgid_nr(task);
		kbuf[count].ppid = task_tgid_nr(rcu_dereference(task->real_parent));

		/* 进程状态: Linux 5.14+ 将退出状态 (EXIT_ZOMBIE/EXIT_DEAD)
		 * 分离到 task->exit_state，调度状态保留在 task->__state。
		 * 合并两个字段使上层能正确识别僵尸进程。 */
		kbuf[count].state = task->__state | task->exit_state;

		/* CPU time: task_cputime() -> nanoseconds, then
		 * nsec_to_clock_t() -> USER_HZ ticks (sysconf(_SC_CLK_TCK)).
		 * This matches /proc/[pid]/stat behaviour. */
		{
			u64 ut_ns, st_ns;
			task_cputime(task, &ut_ns, &st_ns);
			kbuf[count].utime = nsec_to_clock_t(ut_ns);
			kbuf[count].stime = nsec_to_clock_t(st_ns);
		}

		/* 内存信息: 内核线程 mm==NULL，无用户态内存 */
		if (task->mm) {
			kbuf[count].vsize = task->mm->total_vm << PAGE_SHIFT;
			kbuf[count].rss   = get_mm_rss(task->mm);
		} else {
			kbuf[count].vsize = 0;
			kbuf[count].rss   = 0;
		}

		/* nice 值 (-20..19, 越小优先级越高) */
		kbuf[count].nice = task_nice(task);

		/* 线程数 (线程组内线程计数)。
		 * task->signal 在进程退出期间可能变为 NULL (见 __exit_signal)，
		 * 此处安全: RCU 保证 task_struct 地址有效，且 signal 指针
		 * 的读写是原子的。NULL 时保守取 1。 */
		if (task->signal)
			kbuf[count].num_threads = (int)task->signal->nr_threads;
		else
			kbuf[count].num_threads = 1;

		/* 用户 ID (从内核 uid 命名空间映射到当前) */
		kbuf[count].uid = from_kuid_munged(current_user_ns(),
						   task_uid(task));

		count++;
	}
	rcu_read_unlock();

	/* RCU 锁已释放，可以安全调用可能睡眠的 copy_to_user */
	if (copy_to_user(user_buf, kbuf, count * sizeof(struct proc_info))) {
		vfree(kbuf);
		return -EFAULT;
	}
	if (copy_to_user(ret_count, &count, sizeof(int))) {
		vfree(kbuf);
		return -EFAULT;
	}

	vfree(kbuf);
	return 0;
}

/* =========================================================================
 * compute_level — 计算进程在进程树中的深度
 *
 * 从当前进程向上追溯到 init_task (PID=0)，累计层级数。
 * 调用者需持有 rcu_read_lock()。
 *
 * rcu_dereference() 用于每次访问 real_parent，因为 reparent
 * 操作（父进程退出时子进程被收养到 init/subreaper）会在
 * 我们遍历期间并发修改父指针。RCU 保证被引用的 task_struct
 * 地址有效，但父指针内容可能变化，因此需要 RCU 保护读取。
 *
 * 参见: include/linux/sched.h (task_struct::real_parent)
 *       kernel/exit.c (exit_notify → reparent_leader)
 * ========================================================================= */
static int compute_level(struct task_struct *task)
{
	struct task_struct *parent;
	int level = 0;

	parent = rcu_dereference(task->real_parent);
	while (parent && parent != &init_task) {
		level++;
		parent = rcu_dereference(parent->real_parent);
	}
	return level;
}

/* =========================================================================
 * sys_proc_snapshot — 基于进程树返回父子关系拓扑图
 *
 * @user_buf:  用户态 struct proc_tree_node 数组
 * @max_count: 数组容量
 * @ret_count: 输出 — 实际条目数
 *
 * 返回: 0 成功; -EINVAL 参数无效; -ENOMEM 内存不足; -EFAULT 拷贝失败
 *
 * 用户态可基于 pid/ppid 关系重建完整父子树，也可直接使用 level
 * 字段按缩进打印树结构。导出为 Graphviz DOT 格式可图形化展示。
 *
 * 实现: 同 sys_proc_collect — vmalloc 中转缓冲区 + RCU 遍历。
 * ========================================================================= */
SYSCALL_DEFINE3(proc_snapshot,
		struct proc_tree_node __user *, user_buf,
		int, max_count,
		int __user *, ret_count)
{
	struct proc_tree_node *kbuf;
	struct task_struct *task;
	int count = 0;

	if (!user_buf || max_count <= 0 || !ret_count)
		return -EINVAL;

	kbuf = vmalloc(max_count * sizeof(struct proc_tree_node));
	if (!kbuf)
		return -ENOMEM;

	rcu_read_lock();
	for_each_process(task) {
		if (count >= max_count)
			break;

		memset(&kbuf[count], 0, sizeof(struct proc_tree_node));

		kbuf[count].pid   = task_tgid_nr(task);
		kbuf[count].ppid  = task_tgid_nr(rcu_dereference(task->real_parent));
		get_task_comm(kbuf[count].comm, task);
		kbuf[count].level = compute_level(task);

		count++;
	}
	rcu_read_unlock();

	if (copy_to_user(user_buf, kbuf, count * sizeof(struct proc_tree_node))) {
		vfree(kbuf);
		return -EFAULT;
	}
	if (copy_to_user(ret_count, &count, sizeof(int))) {
		vfree(kbuf);
		return -EFAULT;
	}

	vfree(kbuf);
	return 0;
}

/* =========================================================================
 * sys_proc_stat — 返回系统整体进程统计
 *
 * @stat: 用户态 struct proc_stat 指针 (输出参数)
 *
 * 返回: 0 成功; -EINVAL 参数无效; -EFAULT 拷贝失败
 *
 * 一次系统调用即可获取所有状态类别的进程计数，避免用户态多次
 * 系统调用的开销。
 *
 * 实现要点:
 *   - rcu_read_lock() + for_each_process() 遍历所有线程组 leader
 *   - 对每个 leader 进行状态分类 (__state + exit_state 位掩码分析)
 *   - for_each_thread() 遍历每个进程下的所有子线程，
 *     精确统计 kernel_threads 和 user_threads
 *
 * 状态检测: Linux 5.14+ 将退出状态 (EXIT_ZOMBIE/EXIT_DEAD) 分离到
 * task->exit_state，调度状态保留在 task->__state。
 * 必须分别检查两个字段 — 参见 include/linux/sched.h。
 * ========================================================================= */
SYSCALL_DEFINE1(proc_stat,
		struct proc_stat __user *, stat)
{
	struct proc_stat kstat;
	struct task_struct *p, *t;

	if (!stat)
		return -EINVAL;

	memset(&kstat, 0, sizeof(kstat));

	rcu_read_lock();

	for_each_process(p) {
		kstat.total_processes++;

		/*
		 * 状态分类: 先检查退出状态 (exit_state)，再检查调度状态。
		 *
		 * exit_state 取值:
		 *   EXIT_ZOMBIE — 僵尸进程，等待父进程 wait()
		 *   EXIT_DEAD   — 进程正被回收，不计数
		 *
		 * __state 调度状态 (掩码清除 TASK_WAKEKILL | TASK_WAKING 后):
		 *   TASK_RUNNING         (0x0000) — 正在运行或就绪
		 *   TASK_INTERRUPTIBLE   (0x0001) — 可中断睡眠
		 *   TASK_UNINTERRUPTIBLE (0x0002) — 不可中断睡眠
		 *   __TASK_STOPPED       (0x0004) — SIGSTOP 暂停
		 *   __TASK_TRACED        (0x0008) — ptrace 跟踪暂停
		 *
		 * __state 可能带有标志位:
		 *   TASK_WAKEKILL (0x0080) — 可被信号杀死 (附加在 INTERRUPTIBLE 上)
		 *   TASK_WAKING   (0x0100) — 正在被唤醒
		 *   TASK_NOLOAD   (0x0400) — 空闲任务,不计负载 (附加在 UNINTERRUPTIBLE
		 *                            上即为 TASK_IDLE)
		 *
		 * 必须用掩码清除标志位后做 switch，否则含标志位的值无法匹配
		 * 任何 case（如 0x0081 = INTERRUPTIBLE|WAKEKILL 匹配不到 0x0001）。
		 */
		if (p->exit_state & EXIT_ZOMBIE) {
			kstat.zombie_processes++;
		} else if (p->exit_state & EXIT_DEAD) {
			/* EXIT_DEAD: 跳过，不计入任何活跃分类 */
		} else {
			unsigned int s = p->__state;

			if (s & TASK_NOLOAD) {
				/* TASK_IDLE 等空闲内核线程 */
				kstat.idle_processes++;
			} else {
				s &= ~(TASK_WAKEKILL | TASK_WAKING);
				switch (s) {
				case TASK_RUNNING:
					kstat.running_processes++;
					break;
				case TASK_INTERRUPTIBLE:
					kstat.sleeping_processes++;
					break;
				case TASK_UNINTERRUPTIBLE:
					kstat.uninterruptible++;
					break;
				case __TASK_STOPPED:
				case __TASK_TRACED:
					kstat.stopped_processes++;
					break;
				default:
					/* TASK_NEW, TASK_DEAD 等过渡状态 */
					kstat.idle_processes++;
					break;
				}
			}
		}

		/*
		 * 遍历该线程组的所有子线程，精确统计内核线程和用户线程。
		 *
		 * for_each_thread(p, t) 遍历以 p 为 leader 的线程组中所有
		 * task_struct，包括 p 自身（主线程）。
		 *
		 * 这纠正了之前仅用 for_each_process 的 bug:
		 * for_each_process 只遍历线程组 leader，导致 user_threads
		 * 和 kernel_threads 实际统计的是"进程数"而非线程数。
		 */
		for_each_thread(p, t) {
			if (t->mm == NULL)
				kstat.kernel_threads++;
			else
				kstat.user_threads++;
		}
	}

	rcu_read_unlock();

	if (copy_to_user(stat, &kstat, sizeof(kstat)))
		return -EFAULT;

	return 0;
}
