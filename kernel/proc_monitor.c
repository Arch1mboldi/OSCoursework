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
 *   - 所有用户态数据通过 copy_to_user() 传递，杜绝内核指针泄露
 *   - 遍历进程链表时持有锁（read_lock(&tasklist_lock)）
 *   - 持锁期间保存 next 指针，释放锁拷贝，杜绝 use-after-free
 *   - 逐个条目拷贝，避免内核栈上分配过大数组
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
#include <linux/proc_monitor.h>

/* =========================================================================
 * sys_proc_collect — 一次性收集所有进程的完整信息
 *
 * @user_buf:  用户态 struct proc_info 数组指针
 * @max_count: 数组容量 (最大条目数)
 * @ret_count: 输出参数 — 实际填充的进程数
 *
 * 返回: 0 成功; -EINVAL 参数无效; -EFAULT 内存拷贝失败
 *
 * 实现要点:
 *   1. 手动遍历进程链表（替代 for_each_process），持锁时保存 next 指针
 *   2. 安全读取 task_struct 各字段 (get_task_comm / task_tgid_nr / …)
 *   3. 内核线程 mm==NULL，vsize/rss 填 0
 *   4. 释放锁后 copy_to_user，重新加锁后用保存的 next 继续遍历
 *      （避免持锁睡眠 + 杜绝 task_struct 释放导致的 UAF）
 * ========================================================================= */
SYSCALL_DEFINE3(proc_collect,
		struct proc_info __user *, user_buf,
		int, max_count,
		int __user *, ret_count)
{
	struct proc_info kinfo;
	struct task_struct *task, *next;
	int count = 0;
	unsigned long nr_threads;

	if (!user_buf || max_count <= 0 || !ret_count)
		return -EINVAL;

	read_lock(&tasklist_lock);
	task = next_task(&init_task);  /* 跳过头结点 init_task */

	while (task != &init_task) {
		if (count >= max_count)
			break;

		memset(&kinfo, 0, sizeof(kinfo));

		/* 安全获取进程名 (最多 TASK_COMM_LEN-1 字符) */
		get_task_comm(kinfo.comm, task);

		/* PID / PPID */
		kinfo.pid  = task_tgid_nr(task);
		kinfo.ppid = task_tgid_nr(task->real_parent);

		/* 进程状态: Linux 5.14+ 使用 __state 存储调度状态。
		 * exit_state (EXIT_ZOMBIE/EXIT_DEAD) 独立存储。
		 * 合并两个字段使上层能正确识别僵尸进程。 */
		kinfo.state = task->__state | task->exit_state;

		/* CPU time: task_cputime() -> nanoseconds, then
		 * nsec_to_clock_t() -> USER_HZ ticks (sysconf(_SC_CLK_TCK)).
		 * This matches /proc/[pid]/stat behaviour. */
		{
			u64 ut_ns, st_ns;
			task_cputime(task, &ut_ns, &st_ns);
			kinfo.utime = nsec_to_clock_t(ut_ns);
			kinfo.stime = nsec_to_clock_t(st_ns);
		}

		/* 内存信息: 内核线程 mm==NULL，无用户态内存 */
		if (task->mm) {
			kinfo.vsize = task->mm->total_vm << PAGE_SHIFT;
			kinfo.rss   = get_mm_rss(task->mm);
		} else {
			kinfo.vsize = 0;
			kinfo.rss   = 0;
		}

		/* nice 值 (-20..19, 越小优先级越高) */
		kinfo.nice = task_nice(task);

		/* 线程数 (线程组内线程计数) */
		if (task->signal)
			nr_threads = task->signal->nr_threads;
		else
			nr_threads = 1;
		kinfo.num_threads = (int)nr_threads;

		/* 用户 ID (从内核 uid 命名空间映射到当前) */
		kinfo.uid = from_kuid_munged(current_user_ns(),
					     task_uid(task));

		/*
		 * 保存 next 指针后再释放锁:
		 *   - copy_to_user() 可能睡眠，不能在持锁时调用
		 *   - task 在释放锁期间可能因进程退出而被 RCU 回收
		 *   - 必须用保存的 next 指针继续遍历，而非依赖 task->tasks.next
		 * 参见: Documentation/RCU/listRCU.rst (RCU grace period)
		 */
		next = next_task(task);
		read_unlock(&tasklist_lock);

		if (copy_to_user(&user_buf[count], &kinfo, sizeof(kinfo))) {
			/* 拷贝失败，尽力返回已成功拷贝的数量 */
			if (copy_to_user(ret_count, &count, sizeof(int)))
				return -EFAULT;
			return -EFAULT;
		}

		count++;
		read_lock(&tasklist_lock);

		/* 使用保存的 next 继续遍历 (task 可能已被释放) */
		task = next;
	}

	read_unlock(&tasklist_lock);

	/* 写回实际填充数量 */
	if (copy_to_user(ret_count, &count, sizeof(int)))
		return -EFAULT;

	return 0;
}

/* =========================================================================
 * compute_level — 计算进程在进程树中的深度
 *
 * 从当前进程向上追溯到 init_task (PID=0)，累计层级数。
 * 调用者需持有 tasklist_lock (至少读锁)。
 * 参见: include/linux/sched.h (task_struct::real_parent)
 * ========================================================================= */
static int compute_level(struct task_struct *task)
{
	int level = 0;
	struct task_struct *parent = task->real_parent;

	while (parent && parent != &init_task) {
		level++;
		parent = parent->real_parent;
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
 * 返回: 0 成功; -EINVAL 参数无效; -EFAULT 拷贝失败
 *
 * 用户态可基于 pid/ppid 关系重建完整父子树，也可直接使用 level
 * 字段按缩进打印树结构。导出为 Graphviz DOT 格式可图形化展示。
 *
 * 锁策略: 同 sys_proc_collect — 持锁保存 next → 释放锁拷贝 → 重新加锁继续。
 * ========================================================================= */
SYSCALL_DEFINE3(proc_snapshot,
		struct proc_tree_node __user *, user_buf,
		int, max_count,
		int __user *, ret_count)
{
	struct proc_tree_node knode;
	struct task_struct *task, *next;
	int count = 0;

	if (!user_buf || max_count <= 0 || !ret_count)
		return -EINVAL;

	read_lock(&tasklist_lock);
	task = next_task(&init_task);

	while (task != &init_task) {
		if (count >= max_count)
			break;

		memset(&knode, 0, sizeof(knode));

		knode.pid   = task_tgid_nr(task);
		knode.ppid  = task_tgid_nr(task->real_parent);
		get_task_comm(knode.comm, task);
		knode.level = compute_level(task);

		/* 保存 next 指针后释放锁，避免 copy_to_user 持锁睡眠
		 * 以及 task 被释放导致的 UAF。 */
		next = next_task(task);
		read_unlock(&tasklist_lock);

		if (copy_to_user(&user_buf[count], &knode, sizeof(knode))) {
			if (copy_to_user(ret_count, &count, sizeof(int)))
				return -EFAULT;
			return -EFAULT;
		}

		count++;
		read_lock(&tasklist_lock);
		task = next;
	}

	read_unlock(&tasklist_lock);

	if (copy_to_user(ret_count, &count, sizeof(int)))
		return -EFAULT;

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
 * 状态检测: Linux 5.14+ 将退出状态 (EXIT_ZOMBIE/EXIT_DEAD) 分离到
 * task->exit_state，调度状态保留在 task->__state。
 * 必须分别检查两个字段 — 参见 include/linux/sched.h。
 * ========================================================================= */
SYSCALL_DEFINE1(proc_stat,
		struct proc_stat __user *, stat)
{
	struct proc_stat kstat;
	struct task_struct *task;

	if (!stat)
		return -EINVAL;

	memset(&kstat, 0, sizeof(kstat));

	read_lock(&tasklist_lock);

	for_each_process(task) {
		kstat.total_processes++;

		/*
		 * 先检查退出状态 (exit_state):
		 *   EXIT_ZOMBIE — 僵尸进程，等待父进程 wait()
		 *   EXIT_DEAD   — 进程正被回收，不计数
		 * 这两个值不会出现在 __state 中。
		 */
		if (task->exit_state & EXIT_ZOMBIE) {
			kstat.zombie_processes++;
		} else if (task->exit_state & EXIT_DEAD) {
			/* EXIT_DEAD: 跳过，不计入任何活跃分类 */
		} else {
			/* 调度状态 (__state): 对活跃进程分类 */
			switch (task->__state) {
			case TASK_RUNNING:
				kstat.running_processes++;
				break;
			case TASK_INTERRUPTIBLE:
				kstat.sleeping_processes++;
				break;
			case TASK_UNINTERRUPTIBLE:
				kstat.uninterruptible++;
				break;
			case TASK_STOPPED:
			case TASK_TRACED:
				kstat.stopped_processes++;
				break;
			default:
				/* TASK_IDLE, TASK_NEW 等 */
				kstat.idle_processes++;
				break;
			}
		}

		/* 内核线程 (mm==NULL) vs 用户线程 */
		if (task->mm == NULL)
			kstat.kernel_threads++;
		else
			kstat.user_threads++;
	}

	read_unlock(&tasklist_lock);

	if (copy_to_user(stat, &kstat, sizeof(kstat)))
		return -EFAULT;

	return 0;
}
