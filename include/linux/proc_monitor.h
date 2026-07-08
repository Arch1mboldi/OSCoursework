/* SPDX-License-Identifier: GPL-2.0 */
/*
 * proc_monitor.h — 进程监控系统调用：内核-用户态共享数据结构
 *
 * 此头文件定义内核和用户态程序共同使用的结构体。
 * 放在 include/linux/ 下，内核代码和用户态代码均可 #include。
 *
 * 对应系统调用:
 *   sys_proc_collect  (462) — 收集所有进程信息
 *   sys_proc_snapshot (463) — 进程树拓扑快照
 *   sys_proc_stat     (464) — 系统进程统计
 */

#ifndef _LINUX_PROC_MONITOR_H
#define _LINUX_PROC_MONITOR_H

#include <linux/types.h>

#define TASK_COMM_LEN 16

/*
 * struct proc_info — 单个进程的完整信息
 *
 * 由 sys_proc_collect 填充，用户态分配数组接收。
 * 字段精心选择：既覆盖监控核心需求，又不暴露 task_struct 内部细节。
 */
struct proc_info {
	pid_t		pid;		/* 进程ID (TGID)              */
	pid_t		ppid;		/* 父进程ID                    */
	char		comm[TASK_COMM_LEN];	/* 进程名称 (最大15字符+'\0') */
	int		state;		/* 进程状态 (TASK_RUNNING=0, …) */
	unsigned long	utime;		/* 用户态CPU时间 (时钟滴答)    */
	unsigned long	stime;		/* 内核态CPU时间 (时钟滴答)    */
	unsigned long	vsize;		/* 虚拟内存大小 (字节)         */
	unsigned long	rss;		/* 常驻内存集 (页数)           */
	int		nice;		/* nice 优先级 (-20 ~ 19)      */
	int		num_threads;	/* 线程组内线程数              */
	uid_t		uid;		/* 真实用户ID                  */
};

/*
 * struct proc_tree_node — 进程树节点
 *
 * 由 sys_proc_snapshot 填充。
 * 用户态可基于 pid/ppid 关系重建完整进程树。
 */
struct proc_tree_node {
	pid_t		pid;		/* 进程ID                      */
	pid_t		ppid;		/* 父进程ID                    */
	char		comm[TASK_COMM_LEN];	/* 进程名称            */
	int		level;		/* 树深度 (init=0)             */
};

/*
 * struct proc_stat — 系统进程聚合统计
 *
 * 由 sys_proc_stat 填充。
 * 一次性返回所有状态类别的进程计数。
 */
struct proc_stat {
	int	total_processes;	/* 进程总数                    */
	int	running_processes;	/* TASK_RUNNING                */
	int	sleeping_processes;	/* TASK_INTERRUPTIBLE          */
	int	uninterruptible;	/* TASK_UNINTERRUPTIBLE        */
	int	stopped_processes;	/* TASK_STOPPED / TASK_TRACED  */
	int	zombie_processes;	/* EXIT_ZOMBIE                 */
	int	idle_processes;		/* TASK_IDLE / TASK_NEW        */
	int	kernel_threads;		/* mm==NULL 的内核线程         */
	int	user_threads;		/* mm!=NULL 的用户线程         */
};

#endif /* _LINUX_PROC_MONITOR_H */
