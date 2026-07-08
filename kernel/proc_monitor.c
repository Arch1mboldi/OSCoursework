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
 *   462  proc_collect
 *   463  proc_snapshot
 *   464  proc_stat
 *
 * 安全设计:
 *   - 所有用户态数据通过 copy_to_user() 传递，杜绝内核指针泄露
 *   - 遍历进程链表时持有 read_lock(&tasklist_lock)
 *   - 逐个条目拷贝，避免内核栈上分配过大数组
 */

#include <linux/kernel.h>
#include <linux/syscalls.h>
#include <linux/sched.h>
#include <linux/sched/signal.h>
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
 *   1. for_each_process() 遍历全局进程链表
 *   2. 安全读取 task_struct 各字段 (get_task_comm / task_tgid_nr / …)
 *   3. 内核线程 mm==NULL，vsize/rss 填 0
 *   4. 每条记录单独 copy_to_user，避免大内核栈分配
 * ========================================================================= */
SYSCALL_DEFINE3(proc_collect,
		struct proc_info __user *, user_buf,
		int, max_count,
		int __user *, ret_count)
{
	struct proc_info kinfo;
	struct task_struct *task;
	int count = 0;
	unsigned long nr_threads;

	if (!user_buf || max_count <= 0 || !ret_count)
		return -EINVAL;

	read_lock(&tasklist_lock);

	for_each_process(task) {
		if (count >= max_count)
			break;

		memset(&kinfo, 0, sizeof(kinfo));

		/* 安全获取进程名 (最多 TASK_COMM_LEN-1 字符) */
		get_task_comm(kinfo.comm, task);

		/* PID / PPID */
		kinfo.pid  = task_tgid_nr(task);
		kinfo.ppid = task_tgid_nr(task->real_parent);

		/* 进程状态 (Linux 6.x 使用 __state) */
		kinfo.state = task->__state;

		/* CPU 时间 (以时钟滴答为单位) */
		kinfo.utime = task->utime;
		kinfo.stime = task->stime;

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
		 * 释放锁后再做 copy_to_user，避免在持锁期间发生缺页
		 * (copy_to_user 可能睡眠，而 tasklist_lock 不允许睡眠)
		 */
		read_unlock(&tasklist_lock);

		if (copy_to_user(&user_buf[count], &kinfo, sizeof(kinfo))) {
			/* 拷贝失败，尽力返回已成功拷贝的数量 */
			if (copy_to_user(ret_count, &count, sizeof(int)))
				return -EFAULT;
			return -EFAULT;
		}

		count++;
		read_lock(&tasklist_lock);
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
 * 从当前进程向上追溯到 init_task (PID=1)，累计层级数。
 * 调用者需持有 tasklist_lock (至少读锁)。
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
 * ========================================================================= */
SYSCALL_DEFINE3(proc_snapshot,
		struct proc_tree_node __user *, user_buf,
		int, max_count,
		int __user *, ret_count)
{
	struct proc_tree_node knode;
	struct task_struct *task;
	int count = 0;

	if (!user_buf || max_count <= 0 || !ret_count)
		return -EINVAL;

	read_lock(&tasklist_lock);

	for_each_process(task) {
		if (count >= max_count)
			break;

		memset(&knode, 0, sizeof(knode));

		knode.pid  = task_tgid_nr(task);
		knode.ppid = task_tgid_nr(task->real_parent);
		get_task_comm(knode.comm, task);
		knode.level = compute_level(task);

		read_unlock(&tasklist_lock);

		if (copy_to_user(&user_buf[count], &knode, sizeof(knode))) {
			if (copy_to_user(ret_count, &count, sizeof(int)))
				return -EFAULT;
			return -EFAULT;
		}

		count++;
		read_lock(&tasklist_lock);
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

		/* 按状态分类统计
		 * 注意: Linux 6.x 使用 __state, 其编码值与旧版 state 不同
		 */
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
		case EXIT_ZOMBIE:
			kstat.zombie_processes++;
			break;
		case EXIT_DEAD:
			/* EXIT_DEAD 不计入任何活跃分类 */
			break;
		default:
			/* TASK_IDLE, TASK_NEW 等 */
			kstat.idle_processes++;
			break;
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
