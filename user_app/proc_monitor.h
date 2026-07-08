/*
 * user_app/proc_monitor.h — 用户态进程监控程序头文件
 *
 * 此头文件与内核 include/linux/proc_monitor.h 保持结构体定义一致。
 * 独立维护一份是为了避免用户态程序依赖完整内核源码树。
 */

#ifndef _USER_PROC_MONITOR_H
#define _USER_PROC_MONITOR_H

#include <sys/types.h>

#define TASK_COMM_LEN 16

/* ---- 系统调用号 (必须与内核注册的一致) ---- */
#define SYS_proc_collect  470
#define SYS_proc_snapshot 471
#define SYS_proc_stat     472

/* ---- 数据结构 (必须与内核定义字节级兼容) ---- */

struct proc_info {
	pid_t		pid;
	pid_t		ppid;
	char		comm[TASK_COMM_LEN];
	int		state;
	unsigned long	utime;
	unsigned long	stime;
	unsigned long	vsize;
	unsigned long	rss;
	int		nice;
	int		num_threads;
	uid_t		uid;
};

struct proc_tree_node {
	pid_t		pid;
	pid_t		ppid;
	char		comm[TASK_COMM_LEN];
	int		level;
};

struct proc_stat {
	int	total_processes;
	int	running_processes;
	int	sleeping_processes;
	int	uninterruptible;
	int	stopped_processes;
	int	zombie_processes;
	int	idle_processes;
	int	kernel_threads;
	int	user_threads;
};

#endif /* _USER_PROC_MONITOR_H */
