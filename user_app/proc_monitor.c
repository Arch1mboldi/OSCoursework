/*
 * user_app/proc_monitor.c — Linux 进程全量监控与分析系统 (用户态)
 *
 * ===== 核心约束 =====
 * 本程序通过 syscall() 直接调用 3 个自定义系统调用来获取数据:
 *   - sys_proc_collect  (470): 收集所有进程完整信息
 *   - sys_proc_snapshot (471): 进程树拓扑快照
 *   - sys_proc_stat     (472): 系统进程聚合统计
 *
 * 严格禁止使用: ps 命令、/proc 文件系统、eBPF、ptrace 等旁路手段。
 *
 * ===== 功能 =====
 *   stat                查看系统进程统计摘要
 *   list                显示进程列表 (支持排序/过滤)
 *   tree <file>         导出进程树 (Graphviz DOT 格式)
 *   watch               实时刷新模式 (2秒间隔)
 *   sort pid|cpu|mem|name  设置排序字段
 *   order               切换升序/降序
 *   filter pid=N        按 PID 过滤
 *   filter name=XXX     按进程名过滤
 *   filter off          关闭过滤
 *   help                显示帮助
 *   quit                退出
 *
 * ===== 编译 =====
 *   gcc -Wall -Wextra -O2 -std=c11 -o proc_monitor proc_monitor.c
 *
 * ===== 运行 =====
 *   sudo ./proc_monitor
 *   (需要 root 权限才能调用自定义系统调用)
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <signal.h>
#include <sys/ioctl.h>
#include "proc_monitor.h"

/* ---- 常量 ---- */
#define MAX_PROCS    4096        /* 最大进程数 */
#define REFRESH_SEC  2           /* 实时刷新间隔 (秒) */

/* ---- 全局状态 ---- */
static struct proc_info      g_procs[MAX_PROCS];
static struct proc_tree_node g_tree[MAX_PROCS];
static struct proc_stat      g_stat;
static int g_proc_count = 0;
static int g_tree_count = 0;

/* 排序与过滤 */
static int  g_sort_field = 0;     /* 0=pid, 1=cpu, 2=mem, 3=name */
static int  g_sort_desc  = 0;     /* 0=升序, 1=降序 */
static char g_filter_name[256] = "";
static pid_t g_filter_pid = 0;
static int  g_filter_active = 0;

/* 运行控制 */
static volatile sig_atomic_t g_running = 1;

/* ===================================================================
 * 系统调用包装层
 *
 * 这是本程序获取进程数据的唯一入口。每个函数通过 syscall()
 * 直接陷入内核，调用对应的自定义系统调用。
 * =================================================================== */

static int call_proc_collect(void)
{
	int ret, count = 0;
	ret = syscall(SYS_proc_collect, g_procs, MAX_PROCS, &count);
	if (ret < 0) {
		perror("syscall(SYS_proc_collect)");
		return -1;
	}
	g_proc_count = count;
	return 0;
}

static int call_proc_snapshot(void)
{
	int ret, count = 0;
	ret = syscall(SYS_proc_snapshot, g_tree, MAX_PROCS, &count);
	if (ret < 0) {
		perror("syscall(SYS_proc_snapshot)");
		return -1;
	}
	g_tree_count = count;
	return 0;
}

static int call_proc_stat(void)
{
	int ret;
	ret = syscall(SYS_proc_stat, &g_stat);
	if (ret < 0) {
		perror("syscall(SYS_proc_stat)");
		return -1;
	}
	return 0;
}

/* ===================================================================
 * 辅助函数
 * =================================================================== */

/* 将内核状态码转为可读字符 */
static char state_char(int state)
{
	switch (state) {
	case 0:   return 'R';   /* TASK_RUNNING           */
	case 1:   return 'S';   /* TASK_INTERRUPTIBLE     */
	case 2:   return 'D';   /* TASK_UNINTERRUPTIBLE   */
	case 4:   return 'T';   /* TASK_STOPPED           */
	case 8:   return 't';   /* TASK_TRACED            */
	case 16:  return 'Z';   /* EXIT_ZOMBIE            */
	case 32:  return 'X';   /* EXIT_DEAD              */
	case 1026:return 'I';   /* TASK_IDLE              */
	default:  return '?';
	}
}

/* 获取终端宽度 (用于自适应表格) */
static int term_width(void)
{
	struct winsize w;
	if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) == 0 && w.ws_col > 0)
		return w.ws_col;
	return 80;
}

/* ===================================================================
 * 排序函数 (qsort 回调)
 * =================================================================== */

static int cmp_pid_asc(const void *a, const void *b)
{
	const struct proc_info *pa = a, *pb = b;
	return pa->pid - pb->pid;
}
static int cmp_pid_desc(const void *a, const void *b)
{
	return cmp_pid_asc(b, a);
}

static int cmp_cpu_asc(const void *a, const void *b)
{
	const struct proc_info *pa = a, *pb = b;
	unsigned long ta = pa->utime + pa->stime;
	unsigned long tb = pb->utime + pb->stime;
	if (ta < tb) return -1;
	if (ta > tb) return 1;
	return 0;
}
static int cmp_cpu_desc(const void *a, const void *b)
{
	return cmp_cpu_asc(b, a);
}

static int cmp_mem_asc(const void *a, const void *b)
{
	const struct proc_info *pa = a, *pb = b;
	return pa->rss - pb->rss;
}
static int cmp_mem_desc(const void *a, const void *b)
{
	return cmp_mem_asc(b, a);
}

static int cmp_name_asc(const void *a, const void *b)
{
	const struct proc_info *pa = a, *pb = b;
	return strcmp(pa->comm, pb->comm);
}
static int cmp_name_desc(const void *a, const void *b)
{
	return cmp_name_asc(b, a);
}

static void sort_procs(void)
{
	typedef int (*cmp_t)(const void *, const void *);
	static const cmp_t table[4][2] = {
		{ cmp_pid_asc,  cmp_pid_desc  },
		{ cmp_cpu_asc,  cmp_cpu_desc  },
		{ cmp_mem_asc,  cmp_mem_desc  },
		{ cmp_name_asc, cmp_name_desc },
	};
	qsort(g_procs, g_proc_count, sizeof(struct proc_info),
	      table[g_sort_field][g_sort_desc]);
}

/* ===================================================================
 * 过滤匹配
 * =================================================================== */

static int match_filter(const struct proc_info *p)
{
	if (g_filter_pid > 0 && p->pid != g_filter_pid)
		return 0;
	if (g_filter_name[0] && !strstr(p->comm, g_filter_name))
		return 0;
	return 1;
}

/* ===================================================================
 * 显示功能
 * =================================================================== */

static void display_stat(void)
{
	call_proc_stat();

	printf("\n");
	printf("╔══════════════════════════════════════════╗\n");
	printf("║       系 统 进 程 统 计 摘 要           ║\n");
	printf("╠══════════════════════════════════════════╣\n");
	printf("║  进程总数:     %6d                    ║\n", g_stat.total_processes);
	printf("║  运行中 (R):   %6d                    ║\n", g_stat.running_processes);
	printf("║  可中断睡眠(S):%6d                    ║\n", g_stat.sleeping_processes);
	printf("║  不可中断(D):  %6d                    ║\n", g_stat.uninterruptible);
	printf("║  停止 (T):     %6d                    ║\n", g_stat.stopped_processes);
	printf("║  僵尸 (Z):     %6d                    ║\n", g_stat.zombie_processes);
	printf("║  空闲 (I):     %6d                    ║\n", g_stat.idle_processes);
	printf("╠══════════════════════════════════════════╣\n");
	printf("║  内核线程:     %6d                    ║\n", g_stat.kernel_threads);
	printf("║  用户进程:     %6d                    ║\n", g_stat.user_threads);
	printf("╚══════════════════════════════════════════╝\n");
}

static void display_procs(void)
{
	int i, shown = 0;

	call_proc_collect();
	sort_procs();

	/* 表头 */
	printf("\n");
	printf("%-8s %-8s %-16s %c %-4s %-6s %-10s %-8s %s\n",
	       "PID", "PPID", "NAME", 'S', "NICE", "NTHR",
	       "VSIZE(KB)", "RSS(KB)", "UID");
	printf("%.*s\n", term_width(),
	       "------------------------------------------------------------"
	       "------------------------------------------------------------");

	/* 表体 */
	for (i = 0; i < g_proc_count; i++) {
		if (g_filter_active && !match_filter(&g_procs[i]))
			continue;

		printf("%-8d %-8d %-16.16s %c %-4d %-6d %-10lu %-8lu %d\n",
		       g_procs[i].pid,
		       g_procs[i].ppid,
		       g_procs[i].comm,
		       state_char(g_procs[i].state),
		       g_procs[i].nice,
		       g_procs[i].num_threads,
		       g_procs[i].vsize / 1024,
		       g_procs[i].rss * 4,         /* 页大小按 4KB 算 */
		       g_procs[i].uid);
		shown++;
	}

	/* 过滤状态提示 */
	if (g_filter_active) {
		printf("\n--- 过滤条件: ");
		if (g_filter_pid > 0)
			printf("PID=%d ", g_filter_pid);
		if (g_filter_name[0])
			printf("NAME~=%s", g_filter_name);
		printf(" | 匹配: %d / 总数: %d ---\n", shown, g_proc_count);
	} else {
		printf("\n--- 总数: %d ---\n", g_proc_count);
	}
}

static void export_process_tree(const char *filename)
{
	FILE *fp;
	int i;

	call_proc_snapshot();

	fp = fopen(filename, "w");
	if (!fp) {
		perror("fopen");
		return;
	}

	/* 输出 Graphviz DOT 格式 */
	fprintf(fp, "// 进程树 — 由 proc_monitor 生成\n");
	fprintf(fp, "// 渲染: dot -Tpng %s -o tree.png\n", filename);
	fprintf(fp, "digraph proc_tree {\n");
	fprintf(fp, "  rankdir = TB;\n");
	fprintf(fp, "  node [shape=box, style=filled, fillcolor=\"#fffde7\", "
		     "fontname=\"monospace\", fontsize=10];\n");
	fprintf(fp, "  edge [color=\"#546e7a\", arrowhead=normal];\n\n");

	for (i = 0; i < g_tree_count; i++) {
		/* 按状态着色 */
		fprintf(fp,
			"  p%d [label=\"%s\\nPID=%d\"];\n",
			g_tree[i].pid, g_tree[i].comm, g_tree[i].pid);

		/* 连线 (跳过 init 自身的父进程) */
		if (g_tree[i].ppid != 0 && g_tree[i].pid != 1) {
			fprintf(fp, "  p%d -> p%d;\n",
				g_tree[i].ppid, g_tree[i].pid);
		}
	}

	fprintf(fp, "}\n");
	fclose(fp);

	printf("进程树已导出到: %s (%d 个节点)\n", filename, g_tree_count);
	printf("渲染命令: dot -Tpng %s -o tree.png\n", filename);
}

/* ===================================================================
 * 实时刷新模式
 * =================================================================== */

static void sigint_handler(int sig)
{
	(void)sig;
	g_running = 0;
}

static void live_mode(void)
{
	struct sigaction sa_old, sa_new;

	printf("进入实时刷新模式 (按 Ctrl+C 返回交互模式)...\n");
	sleep(1);

	/* 设置临时 SIGINT 处理器 */
	sa_new.sa_handler = sigint_handler;
	sigemptyset(&sa_new.sa_mask);
	sa_new.sa_flags = 0;
	sigaction(SIGINT, &sa_new, &sa_old);

	g_running = 1;
	while (g_running) {
		printf("\033[2J\033[H");       /* 清屏 + 光标归位 */
		display_stat();
		display_procs();
		printf("\n刷新间隔: %ds | Ctrl+C 返回\n", REFRESH_SEC);
		fflush(stdout);
		sleep(REFRESH_SEC);
	}

	/* 恢复默认 SIGINT */
	sigaction(SIGINT, &sa_old, NULL);
	g_running = 1;
}

/* ===================================================================
 * 交互命令处理
 * =================================================================== */

static void show_help(void)
{
	printf("\n");
	printf("╔══════════════════════════════════════════════════════╗\n");
	printf("║    Linux 进程全量监控与分析系统 — 命令列表        ║\n");
	printf("╠══════════════════════════════════════════════════════╣\n");
	printf("║  stat               查看系统进程统计摘要           ║\n");
	printf("║  list               显示进程列表 (含排序/过滤)     ║\n");
	printf("║  tree [文件名]      导出进程树 (DOT 格式)          ║\n");
	printf("║  watch              实时刷新模式                   ║\n");
	printf("║  sort pid|cpu|mem|name  设置排序字段               ║\n");
	printf("║  order              切换升序/降序                  ║\n");
	printf("║  filter pid=N       按 PID 过滤                    ║\n");
	printf("║  filter name=XXX    按进程名过滤 (子串匹配)        ║\n");
	printf("║  filter off         关闭过滤                       ║\n");
	printf("║  help               显示此帮助                     ║\n");
	printf("║  quit               退出程序                       ║\n");
	printf("╚══════════════════════════════════════════════════════╝\n");
}

static void handle_command(char *cmd)
{
	char *arg = strchr(cmd, ' ');
	if (arg) {
		*arg = '\0';
		arg++;
		/* 跳过前导空白 */
		while (*arg == ' ' || *arg == '\t') arg++;
	}

	if (strcmp(cmd, "help") == 0 || strcmp(cmd, "h") == 0) {
		show_help();

	} else if (strcmp(cmd, "list") == 0 || strcmp(cmd, "l") == 0) {
		display_procs();

	} else if (strcmp(cmd, "stat") == 0 || strcmp(cmd, "s") == 0) {
		display_stat();

	} else if (strcmp(cmd, "tree") == 0 || strcmp(cmd, "t") == 0) {
		export_process_tree(arg ? arg : "proc_tree.dot");

	} else if (strcmp(cmd, "watch") == 0 || strcmp(cmd, "w") == 0) {
		live_mode();

	} else if (strcmp(cmd, "sort") == 0) {
		if (!arg) {
			printf("用法: sort pid|cpu|mem|name\n");
		} else if (strcmp(arg, "pid") == 0)  g_sort_field = 0;
		else if (strcmp(arg, "cpu") == 0)   g_sort_field = 1;
		else if (strcmp(arg, "mem") == 0)   g_sort_field = 2;
		else if (strcmp(arg, "name") == 0)  g_sort_field = 3;
		else printf("未知排序字段: %s\n", arg);
		printf("排序: %s (%s)\n",
		       (const char *[]){"PID","CPU","MEM","NAME"}[g_sort_field],
		       g_sort_desc ? "降序" : "升序");

	} else if (strcmp(cmd, "order") == 0) {
		g_sort_desc = !g_sort_desc;
		printf("排序方向: %s\n", g_sort_desc ? "降序" : "升序");

	} else if (strcmp(cmd, "filter") == 0) {
		if (!arg || strcmp(arg, "off") == 0) {
			g_filter_active = 0;
			g_filter_pid = 0;
			g_filter_name[0] = '\0';
			printf("过滤已关闭\n");
		} else if (strncmp(arg, "pid=", 4) == 0) {
			g_filter_pid = (pid_t)atoi(arg + 4);
			g_filter_active = 1;
			printf("过滤: PID = %d\n", g_filter_pid);
		} else if (strncmp(arg, "name=", 5) == 0) {
			strncpy(g_filter_name, arg + 5,
				sizeof(g_filter_name) - 1);
			g_filter_name[sizeof(g_filter_name) - 1] = '\0';
			g_filter_active = 1;
			printf("过滤: NAME ~= \"%s\"\n", g_filter_name);
		} else {
			printf("用法: filter pid=N | filter name=XXX | filter off\n");
		}

	} else if (strcmp(cmd, "quit") == 0 || strcmp(cmd, "q") == 0) {
		g_running = 0;

	} else if (cmd[0] != '\0') {
		printf("未知命令: '%s' (输入 help 查看帮助)\n", cmd);
	}
}

/* ===================================================================
 * 主函数
 * =================================================================== */

int main(void)
{
	char line[512];

	printf("\n");
	printf("╔══════════════════════════════════════════╗\n");
	printf("║   Linux 进程全量监控与分析系统         ║\n");
	printf("║   基于自定义系统调用 (syscall 462-464) ║\n");
	printf("╚══════════════════════════════════════════╝\n");
	printf("\n输入 'help' 查看命令列表, 'quit' 退出\n");

	/* 启动时展示统计概览 */
	display_stat();

	while (g_running) {
		printf("\nprocmon> ");
		fflush(stdout);

		if (!fgets(line, sizeof(line), stdin))
			break;

		/* 去除末尾换行 */
		line[strcspn(line, "\n")] = '\0';

		if (line[0] == '\0')
			continue;

		handle_command(line);
	}

	printf("\n再见!\n");
	return 0;
}
