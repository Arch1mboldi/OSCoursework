/*
 * proc_monitor.c — Linux 进程全量监控 TUI (Debug 版)
 *
 * 通过自定义系统调用 (470/471/472) 获取内核进程数据。
 * 以调试友好为设计目标：展示所有内核传回的原始字段、
 * 状态位解码、系统调用耗时、结构体内存布局。
 *
 * 面板布局 (4 区域):
 *   win_main    — 主视图 (5 种模式可切换: v 键)
 *   win_detail  — 选中进程的全部 proc_info 字段 + 状态位解码
 *   win_syscall — 系统调用返回值/errno/耗时 + proc_stat 聚合
 *   win_hint    — 按键提示栏
 *
 * 视图模式 (v 键循环):
 *   1. LIST         — 紧凑进程列表 (含原始 state hex)
 *   2. DEBUG-TABLE  — 所有 proc_info 字段作为原始数值展示
 *   3. TREE         — 进程树层次结构
 *   4. HEX-DUMP     — 选中进程 proc_info 的原始字节 + 字段布局标注
 *   5. SYSCALL      — 系统调用诊断面板 (返回值/耗时/struct大小/proc_stat)
 *
 * 编译: gcc -Wall -Wextra -O2 -std=c11 -o proc_monitor proc_monitor.c -lncurses
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <sys/ioctl.h>
#include <locale.h>
#include <langinfo.h>
#include <sys/time.h>
#include <time.h>
#include <ncurses.h>
#include "proc_monitor.h"

/* ===================================================================
 * 常量
 * =================================================================== */

#define MAX_PROCS     8192
#define MAX_CMD_LEN   256
#define N_VIEWS       5

/* 运行时从 sysconf 获取 (替代硬编码 HZ=100) */
static long clk_tck = 100;

/* ===================================================================
 * 系统调用耗时追踪
 * =================================================================== */

struct sc_result {
	long    ret;        /* 返回值 */
	int     err;        /* errno (失败时) */
	long    latency_us; /* 耗时 (微秒) */
	int     count;      /* 返回的条目数 */
};

/* ===================================================================
 * 进程信息字段描述 (用于 Hex Dump 标注)
 *
 * struct proc_info 在 x86_64 上的内存布局 (LP64 ABI):
 *   offset  0: pid          (4B, int)
 *   offset  4: ppid         (4B, int)
 *   offset  8: comm[16]     (16B, char[])
 *   offset 24: state        (4B, int)
 *   offset 28: (padding)    (4B)  — 对齐 unsigned long
 *   offset 32: utime        (8B, unsigned long)
 *   offset 40: stime        (8B, unsigned long)
 *   offset 48: vsize        (8B, unsigned long)
 *   offset 56: rss          (8B, unsigned long)
 *   offset 64: nice         (4B, int)
 *   offset 68: num_threads  (4B, int)
 *   offset 72: uid          (4B, uid_t)
 *   offset 76: (padding)    (4B)  — 对齐到 8 字节边界
 *   total: 80 字节
 * =================================================================== */

struct field_desc {
	const char *name;
	int         offset;
	int         size;
};

static const struct field_desc g_fields[] = {
	{"pid",          0,  4},
	{"ppid",         4,  4},
	{"comm[16]",     8, 16},
	{"state",       24,  4},
	{"(pad)",       28,  4},
	{"utime",       32,  8},
	{"stime",       40,  8},
	{"vsize",       48,  8},
	{"rss",         56,  8},
	{"nice",        64,  4},
	{"num_threads", 68,  4},
	{"uid",         72,  4},
	{"(pad)",       76,  4},
};
#define N_FIELDS (int)(sizeof(g_fields) / sizeof(g_fields[0]))

/* ===================================================================
 * ncurses 窗口
 * =================================================================== */

static WINDOW *win_main, *win_detail, *win_syscall, *win_hint;

/* ===================================================================
 * 树绘制字符 (UTF-8 环境用 Unicode, 否则 ASCII fallback)
 * =================================================================== */

static const char *T_VER, *T_TEE, *T_ELB, *T_BLNK;

/* ===================================================================
 * 全局数据
 * =================================================================== */

static struct proc_info       g_procs[MAX_PROCS];
static struct proc_tree_node  g_tree_nodes[MAX_PROCS];
static struct proc_stat       g_stat;
static int g_proc_count  = 0;
static int g_tree_count  = 0;
static int g_selected    = 0;    /* 过滤后列表中的选中索引 */
static int g_scroll      = 0;
static int g_running     = 1;

/* 视图 */
static int g_view_mode   = 0;    /* 0=list,1=debug,2=tree,3=hex,4=syscall */
static const char *g_view_names[N_VIEWS] = {
	"LIST", "DEBUG-TABLE", "TREE", "HEX-DUMP", "SYSCALL"
};

/* 排序 */
static int  g_sort_field = 0;    /* 0=pid,1=cpu,2=mem,3=name */
static int  g_sort_desc  = 0;

/* 过滤 */
static char g_filter[64] = "";
static int  g_filter_active = 0;
static int  g_filter_mode   = 0; /* 是否正在输入过滤条件 */

/* 命令缓冲区 */
static char g_cmd[MAX_CMD_LEN];
static int  g_cmd_pos = 0;

/* 上次错误/提示消息 */
static char g_last_msg[256] = "";

/* CPU% 计算 */
struct prev_cpu {
	pid_t         pid;
	unsigned long total;
};
static struct prev_cpu g_prev[MAX_PROCS];
static int             g_prev_count = 0;
static struct timeval  g_last_tv;

/* 系统调用耗时记录 */
static struct sc_result g_sc_collect;
static struct sc_result g_sc_snapshot;
static struct sc_result g_sc_stat;

/* ===================================================================
 * 辅助函数
 * =================================================================== */

/* 安全复制 comm[16] -> C 字符串 */
static const char *cstr(const char comm[16])
{
	static char buf[17];
	int n;
	for (n = 0; n < 16 && comm[n]; n++)
		buf[n] = comm[n];
	buf[n] = '\0';
	return buf;
}

/*
 * Linux 6.x 进程状态使用位掩码编码:
 *   __state bits:  TASK_RUNNING=0x00, INTERRUPTIBLE=0x01, UNINTERRUPTIBLE=0x02,
 *                  __TASK_STOPPED=0x04, __TASK_TRACED=0x08,
 *                  TASK_DEAD=0x40, TASK_WAKEKILL=0x80
 *   exit_state:    EXIT_DEAD=0x10, EXIT_ZOMBIE=0x20
 * 内核报告 state = __state | exit_state, 故需用位检测而非精确匹配。
 * 参见: include/linux/sched.h (Linux 6.18)
 */
static char state_char(int state)
{
	if (state & 0x20) return 'Z';  /* EXIT_ZOMBIE   */
	if (state & 0x10) return 'X';  /* EXIT_DEAD     */
	if (state & 0x08) return 't';  /* __TASK_TRACED  */
	if (state & 0x04) return 'T';  /* __TASK_STOPPED */
	switch (state & 0x03) {
	case 0:  return 'R';
	case 1:  return 'S';
	case 2:  return 'D';
	default: return '?';
	}
}

static const char *state_name(int state)
{
	if (state & 0x20) return "ZOMBIE";
	if (state & 0x10) return "DEAD";
	if (state & 0x08) return "TRACED";
	if (state & 0x04) return "STOPPED";
	switch (state & 0x03) {
	case 0:  return "RUNNING";
	case 1:  return "SLEEPING";
	case 2:  return "DISK_WAIT";
	default: return "IDLE/OTHER";
	}
}

/*
 * 解码 state 字段为 __state 和 exit_state 两部分
 * 将详细描述写入 buf (调用者保证 >= 256 字节)
 */
static void state_decode(int state, char *buf, size_t bufsz)
{
	int __s  = state & ~(0x10 | 0x20);  /* 去掉 exit_state 位 */
	int exit = state &  (0x10 | 0x20);

	snprintf(buf, bufsz, "__state=0x%02X(", __s);

	if (__s == 0x00)
		strncat(buf, "RUNNING", bufsz - strlen(buf) - 1);
	else {
		int first = 1;
		if (__s & 0x01) { strncat(buf, "INTR", bufsz - strlen(buf) - 1); first = 0; }
		if (__s & 0x02) { if (!first) strncat(buf, "|", bufsz - strlen(buf) - 1);
		                  strncat(buf, "UNINTR", bufsz - strlen(buf) - 1); first = 0; }
		if (__s & 0x04) { if (!first) strncat(buf, "|", bufsz - strlen(buf) - 1);
		                  strncat(buf, "STOP", bufsz - strlen(buf) - 1); first = 0; }
		if (__s & 0x08) { if (!first) strncat(buf, "|", bufsz - strlen(buf) - 1);
		                  strncat(buf, "TRACE", bufsz - strlen(buf) - 1); first = 0; }
		if (__s & 0x40) { if (!first) strncat(buf, "|", bufsz - strlen(buf) - 1);
		                  strncat(buf, "DEAD", bufsz - strlen(buf) - 1); first = 0; }
		if (__s & 0x80) { if (!first) strncat(buf, "|", bufsz - strlen(buf) - 1);
		                  strncat(buf, "WAKEKILL", bufsz - strlen(buf) - 1); }
	}

	/* exit_state 部分 */
	{
		size_t used = strlen(buf);
		snprintf(buf + used, bufsz - used, ") exit_state=0x%02X(", exit);
	}
	if (exit == 0) {
		strncat(buf, "NONE", bufsz - strlen(buf) - 1);
	} else {
		int first = 1;
		if (exit & 0x10) { strncat(buf, "DEAD", bufsz - strlen(buf) - 1); first = 0; }
		if (exit & 0x20) {
			if (!first) strncat(buf, "|", bufsz - strlen(buf) - 1);
			strncat(buf, "ZOMBIE", bufsz - strlen(buf) - 1);
		}
	}
	strncat(buf, ")", bufsz - strlen(buf) - 1);
}

static double cpu_utime_sec(const struct proc_info *p)
{
	return (double)p->utime / (double)clk_tck;
}
static double cpu_stime_sec(const struct proc_info *p)
{
	return (double)p->stime / (double)clk_tck;
}

/* CPU%: 与上一帧的 CPU 滴答差 / (clk_tck * elapsed) */
static int cmp_prev_pid(const void *a, const void *b)
{
	const struct prev_cpu *pa = a, *pb = b;
	return (pa->pid > pb->pid) - (pa->pid < pb->pid);
}

static double cpu_pct(pid_t pid, unsigned long cur_total)
{
	struct prev_cpu key = { .pid = pid };
	struct prev_cpu *found;
	unsigned long prev_total;
	double elapsed;

	if (g_prev_count <= 0)
		return 0.0;

	found = bsearch(&key, g_prev, g_prev_count,
			sizeof(struct prev_cpu), cmp_prev_pid);
	if (!found)
		return 0.0;

	prev_total = found->total;
	if (cur_total < prev_total)
		return 0.0;

	{
		struct timeval now;
		gettimeofday(&now, NULL);
		elapsed = (now.tv_sec  - g_last_tv.tv_sec) +
			  (now.tv_usec - g_last_tv.tv_usec) / 1000000.0;
	}
	if (elapsed <= 0.0)
		return 0.0;

	return (double)(cur_total - prev_total)
	       / (double)clk_tck / elapsed * 100.0;
}

static void save_prev_cpu(void)
{
	int i;
	g_prev_count = 0;
	for (i = 0; i < g_proc_count && i < MAX_PROCS; i++) {
		g_prev[i].pid   = g_procs[i].pid;
		g_prev[i].total = g_procs[i].utime + g_procs[i].stime;
		g_prev_count++;
	}
	qsort(g_prev, g_prev_count, sizeof(struct prev_cpu), cmp_prev_pid);
	gettimeofday(&g_last_tv, NULL);
}

static const char *fmt_size(unsigned long bytes)
{
	static char buf[16];
	if (bytes >= 1UL << 30)
		snprintf(buf, sizeof(buf), "%.1fG", (double)bytes / (1UL << 30));
	else if (bytes >= 1UL << 20)
		snprintf(buf, sizeof(buf), "%.1fM", (double)bytes / (1UL << 20));
	else if (bytes >= 1UL << 10)
		snprintf(buf, sizeof(buf), "%.1fK", (double)bytes / (1UL << 10));
	else
		snprintf(buf, sizeof(buf), "%luB", bytes);
	return buf;
}

/* 获取单调时钟微秒时间戳 */
static long time_now_us(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000000L + ts.tv_nsec / 1000L;
}

/* ===================================================================
 * 系统调用层 (带耗时测量)
 * =================================================================== */

static int fetch_procs(void)
{
	long t0, t1;
	int count = 0;

	t0 = time_now_us();
	g_sc_collect.ret = syscall(SYS_proc_collect, g_procs, MAX_PROCS, &count);
	t1 = time_now_us();

	g_sc_collect.latency_us = t1 - t0;
	g_sc_collect.err = (g_sc_collect.ret < 0) ? errno : 0;
	g_sc_collect.count = count;

	if (g_sc_collect.ret < 0) {
		snprintf(g_last_msg, sizeof(g_last_msg),
			 "proc_collect: ret=%ld errno=%d (%s)",
			 g_sc_collect.ret, errno, strerror(errno));
		return -1;
	}
	g_proc_count = count;
	return 0;
}

static int fetch_tree(void)
{
	long t0, t1;
	int count = 0;

	t0 = time_now_us();
	g_sc_snapshot.ret = syscall(SYS_proc_snapshot, g_tree_nodes, MAX_PROCS, &count);
	t1 = time_now_us();

	g_sc_snapshot.latency_us = t1 - t0;
	g_sc_snapshot.err = (g_sc_snapshot.ret < 0) ? errno : 0;
	g_sc_snapshot.count = count;

	if (g_sc_snapshot.ret < 0) {
		snprintf(g_last_msg, sizeof(g_last_msg),
			 "proc_snapshot: ret=%ld errno=%d (%s)",
			 g_sc_snapshot.ret, errno, strerror(errno));
		return -1;
	}
	g_tree_count = count;
	return 0;
}

static int fetch_stat(void)
{
	long t0, t1;

	t0 = time_now_us();
	g_sc_stat.ret = syscall(SYS_proc_stat, &g_stat);
	t1 = time_now_us();

	g_sc_stat.latency_us = t1 - t0;
	g_sc_stat.err = (g_sc_stat.ret < 0) ? errno : 0;
	g_sc_stat.count = 0;

	if (g_sc_stat.ret < 0) {
		snprintf(g_last_msg, sizeof(g_last_msg),
			 "proc_stat: ret=%ld errno=%d (%s)",
			 g_sc_stat.ret, errno, strerror(errno));
		return -1;
	}
	return 0;
}

static int update_data(void)
{
	int ok = 1;
	if (fetch_procs() < 0) ok = 0;
	if (fetch_tree() < 0)  ok = 0;
	if (fetch_stat() < 0)  ok = 0;
	if (ok) g_last_msg[0] = '\0';
	return ok ? 0 : -1;
}

/* ===================================================================
 * 排序
 * =================================================================== */

static int cmp_pid(const void *a, const void *b)
{
	const struct proc_info *pa = a, *pb = b;
	return (pa->pid > pb->pid) - (pa->pid < pb->pid);
}
static int cmp_cpu(const void *a, const void *b)
{
	const struct proc_info *pa = a, *pb = b;
	unsigned long ca = pa->utime + pa->stime;
	unsigned long cb = pb->utime + pb->stime;
	return (ca > cb) - (ca < cb);
}
static int cmp_mem(const void *a, const void *b)
{
	const struct proc_info *pa = a, *pb = b;
	return (pa->rss > pb->rss) - (pa->rss < pb->rss);
}
static int cmp_name(const void *a, const void *b)
{
	const struct proc_info *pa = a, *pb = b;
	return strcmp(cstr(pa->comm), cstr(pb->comm));
}

static void sort_procs(void)
{
	int (*cmps[])(const void *, const void *) =
		{ cmp_pid, cmp_cpu, cmp_mem, cmp_name };

	qsort(g_procs, g_proc_count, sizeof(struct proc_info),
	      cmps[g_sort_field]);

	if (g_sort_desc) {
		int i, j;
		struct proc_info tmp;
		for (i = 0, j = g_proc_count - 1; i < j; i++, j--) {
			tmp = g_procs[i];
			g_procs[i] = g_procs[j];
			g_procs[j] = tmp;
		}
	}
}

/* ===================================================================
 * 过滤
 * =================================================================== */

static int match_filter(const struct proc_info *p)
{
	int i;

	if (!g_filter_active || g_filter[0] == '\0')
		return 1;

	/* =0xNN: 精确匹配 state 的十六进制值 (调试用) */
	if (g_filter[0] == '=' && g_filter[1] == '0' &&
	    (g_filter[2] == 'x' || g_filter[2] == 'X')) {
		int want = (int)strtol(g_filter + 1, NULL, 16);
		return p->state == want;
	}

	/* =R: 状态字符 */
	if (g_filter[0] == '=' && g_filter[1]) {
		char want = (char)(g_filter[1] >= 'a' ? g_filter[1] - 32 : g_filter[1]);
		return state_char(p->state) == want;
	}

	/* :1234: 精确 PID */
	if (g_filter[0] == ':') {
		int pid = atoi(g_filter + 1);
		return p->pid == pid;
	}

	/* 名称子串 (大小写不敏感) */
	{
		char name[17], lower[64];
		int j;
		for (j = 0; j < 16 && p->comm[j]; j++)
			name[j] = (char)(p->comm[j] >= 'A' && p->comm[j] <= 'Z'
					 ? p->comm[j] + 32 : p->comm[j]);
		name[j] = '\0';

		for (i = 0; g_filter[i] && i < 63; i++)
			lower[i] = (char)(g_filter[i] >= 'A' && g_filter[i] <= 'Z'
					  ? g_filter[i] + 32 : g_filter[i]);
		lower[i] = '\0';

		return strstr(name, lower) != NULL;
	}
}

/*
 * 从 g_procs 中找到过滤后第 target 个条目的索引
 * 返回 -1 表示未找到
 */
static int filtered_index(int target)
{
	int i, cnt = 0;
	for (i = 0; i < g_proc_count; i++) {
		if (match_filter(&g_procs[i])) {
			if (cnt == target) return i;
			cnt++;
		}
	}
	return -1;
}

/* 统计过滤后条目数 */
static int filtered_count(void)
{
	int i, cnt = 0;
	for (i = 0; i < g_proc_count; i++)
		if (match_filter(&g_procs[i])) cnt++;
	return cnt;
}

/* ===================================================================
 * 进程树渲染辅助
 * =================================================================== */

static int tree_is_last(int i, int count)
{
	pid_t ppid = g_tree_nodes[i].ppid;
	int j;
	for (j = i + 1; j < count; j++) {
		if (g_tree_nodes[j].ppid == ppid)
			return 0;
	}
	return 1;
}

static int tree_has_continuation(int i, int count, int target_level)
{
	pid_t ancestor_pid = g_tree_nodes[i].pid;
	int cur_level = g_tree_nodes[i].level;
	int j;

	/* 向上追溯到 target_level 的祖先 */
	while (cur_level > target_level) {
		int found = 0;
		for (j = 0; j < count; j++) {
			if (g_tree_nodes[j].pid == ancestor_pid) {
				ancestor_pid = g_tree_nodes[j].ppid;
				cur_level = g_tree_nodes[j].level;
				found = 1;
				break;
			}
		}
		if (!found) return 0;
	}

	/* 检查后面是否还有同一祖先的兄弟节点 */
	for (j = i + 1; j < count; j++) {
		pid_t aid = g_tree_nodes[j].pid;
		int cl = g_tree_nodes[j].level;
		while (cl > target_level) {
			int found = 0;
			int k;
			for (k = 0; k < count; k++) {
				if (g_tree_nodes[k].pid == aid) {
					aid = g_tree_nodes[k].ppid;
					cl = g_tree_nodes[k].level;
					found = 1;
					break;
				}
			}
			if (!found) break;
		}
		if (cl == target_level && aid == ancestor_pid)
			return 1;
	}
	return 0;
}

/* ===================================================================
 * ncurses GUI 初始化/清理
 * =================================================================== */

static void gui_init(void)
{
	int rows, cols;
	const char *codeset;

	setlocale(LC_ALL, "");
	codeset = nl_langinfo(CODESET);

	if (codeset && strstr(codeset, "UTF")) {
		T_VER  = "\xe2\x94\x82   ";   /* |    */
		T_TEE  = "\xe2\x94\x9c\xe2\x94\x80\xe2\x94\x80 ";  /* +--  */
		T_ELB  = "\xe2\x94\x94\xe2\x94\x80\xe2\x94\x80 ";  /* \--  */
		T_BLNK = "    ";
	} else {
		T_VER  = "|   ";
		T_TEE  = "|-- ";
		T_ELB  = "`-- ";
		T_BLNK = "    ";
	}

	initscr();
	cbreak();
	noecho();
	curs_set(0);
	keypad(stdscr, TRUE);

	if (has_colors()) {
		start_color();
		init_pair(1, COLOR_GREEN,   COLOR_BLACK);  /* R state        */
		init_pair(2, COLOR_RED,     COLOR_BLACK);  /* Z state        */
		init_pair(3, COLOR_YELLOW,  COLOR_BLACK);  /* D state        */
		init_pair(4, COLOR_CYAN,    COLOR_BLACK);  /* tree / misc    */
		init_pair(5, COLOR_WHITE,   COLOR_BLUE);   /* header / title */
		init_pair(6, COLOR_MAGENTA, COLOR_BLACK);  /* T/t state      */
		init_pair(7, COLOR_WHITE,   COLOR_RED);    /* error          */
		init_pair(8, COLOR_BLACK,   COLOR_CYAN);   /* hex highlight  */
	}

	getmaxyx(stdscr, rows, cols);

	/* 4 面板布局: main (rows-6) + detail(2) + syscall(2) + hint(1) + 底部留空 */
	win_main    = newwin(rows - 6, cols, 0, 0);
	win_detail  = newwin(2,        cols, rows - 6, 0);
	win_syscall = newwin(2,        cols, rows - 4, 0);
	win_hint    = newwin(1,        cols, rows - 1, 0);

	keypad(win_main, TRUE);
	scrollok(win_main, FALSE);

	/* 1 秒超时，自动刷新 */
	wtimeout(win_main, 1000);
}

static void gui_cleanup(void)
{
	delwin(win_main);
	delwin(win_detail);
	delwin(win_syscall);
	delwin(win_hint);
	endwin();
}

/* ===================================================================
 * 视图渲染器
 * =================================================================== */

/* ---- View 0: List View ---- */
static void render_list_view(int max_rows, int cols)
{
	const char *sort_names[] = {"PID","CPU","MEM","NAME"};
	int i, row, total;

	(void)cols;

	/* 标题栏 */
	wattron(win_main, COLOR_PAIR(5));
	mvwprintw(win_main, 0, 2, " ProcMon [LIST] ");
	wattroff(win_main, COLOR_PAIR(5));
	mvwprintw(win_main, 0, 18, "| Sort:%s %s | %s | %d procs | %s",
		  sort_names[g_sort_field],
		  g_sort_desc ? "v" : "^",
		  g_filter_active ? "FILT" : "",
		  g_proc_count,
		  g_last_msg);

	/* 表头 */
	mvwprintw(win_main, 2, 1,
		  "%-7s %-7s %-14s %-5s %-6s %-7s %-7s %-8s %-8s "
		  "%-4s %-3s %-5s",
		  "PID", "PPID", "NAME", "S", "CPU%",
		  "UTIME", "STIME", "VSIZE", "RSS",
		  "NICE", "THR", "UID");
	mvwhline(win_main, 3, 0, ACS_HLINE, cols);

	/* 过滤后数量 + 修正滚动 */
	total = filtered_count();
	if (g_selected >= total) g_selected = total - 1;
	if (g_selected < 0)     g_selected = 0;
	if (g_selected < g_scroll)
		g_scroll = g_selected;
	if (g_selected >= g_scroll + (max_rows - 5))
		g_scroll = g_selected - (max_rows - 5) + 1;
	if (g_scroll < 0) g_scroll = 0;

	{
		int shown = 0;
		for (i = 0, row = 4; i < g_proc_count && row < max_rows - 1; i++) {
			const struct proc_info *p = &g_procs[i];
			char st;
			int cp;
			char hex_state[6];

			if (!match_filter(p)) continue;
			if (shown < g_scroll) { shown++; continue; }

			st = state_char(p->state);
			snprintf(hex_state, sizeof(hex_state), "%04X", (unsigned int)p->state);

			/* 高亮选中行 */
			if (shown == g_selected)
				wattron(win_main, A_REVERSE);

			/* 状态颜色 */
			switch (st) {
			case 'R': cp = 1; break;
			case 'Z': cp = 2; break;
			case 'D': cp = 3; break;
			case 'T': case 't': cp = 6; break;
			default:  cp = 0; break;
			}
			if (cp && shown != g_selected)
				wattron(win_main, COLOR_PAIR(cp));

			mvwprintw(win_main, row, 1,
				  "%-7d %-7d %-14.14s %c:%-4s "
				  "%-5.1f %-7.1f %-7.1f "
				  "%-8s %-8s %-4d %-3d %-5d",
				  p->pid, p->ppid, cstr(p->comm), st, hex_state,
				  cpu_pct(p->pid, p->utime + p->stime),
				  cpu_utime_sec(p),
				  cpu_stime_sec(p),
				  fmt_size(p->vsize),
				  fmt_size(p->rss * 4096),
				  p->nice, p->num_threads, p->uid);

			if (cp && shown != g_selected)
				wattroff(win_main, COLOR_PAIR(cp));
			if (shown == g_selected)
				wattroff(win_main, A_REVERSE);

			shown++;
			row++;
		}
	}
}

/* ---- View 1: Debug Table (原始数值) ---- */
static void render_debug_view(int max_rows, int cols)
{
	const char *sort_names[] = {"PID","CPU","MEM","NAME"};
	int i, row, total;

	(void)cols;

	/* 标题栏 */
	wattron(win_main, COLOR_PAIR(5));
	mvwprintw(win_main, 0, 2, " ProcMon [DEBUG-TABLE] ");
	wattroff(win_main, COLOR_PAIR(5));
	mvwprintw(win_main, 0, 24, "| ALL RAW VALUES | Sort:%s %s | %s",
		  sort_names[g_sort_field],
		  g_sort_desc ? "v" : "^",
		  g_last_msg);

	/* 表头: 所有 proc_info 字段作为原始数值 */
	mvwprintw(win_main, 2, 1,
		  "%-7s %-7s %-14s %-10s %-12s %-12s %-12s %-12s "
		  "%-4s %-3s %-5s",
		  "PID", "PPID", "COMM", "STATE", "UTIME(tick)",
		  "STIME(tick)", "VSIZE(B)", "RSS(pg)",
		  "NICE", "THR", "UID");
	mvwhline(win_main, 3, 0, ACS_HLINE, cols);

	total = filtered_count();
	if (g_selected >= total) g_selected = total - 1;
	if (g_selected < 0)     g_selected = 0;
	if (g_selected < g_scroll)
		g_scroll = g_selected;
	if (g_selected >= g_scroll + (max_rows - 5))
		g_scroll = g_selected - (max_rows - 5) + 1;
	if (g_scroll < 0) g_scroll = 0;

	{
		int shown = 0;
		for (i = 0, row = 4; i < g_proc_count && row < max_rows - 1; i++) {
			const struct proc_info *p = &g_procs[i];
			char st;
			int cp;

			if (!match_filter(p)) continue;
			if (shown < g_scroll) { shown++; continue; }

			st = state_char(p->state);

			if (shown == g_selected)
				wattron(win_main, A_REVERSE);

			switch (st) {
			case 'R': cp = 1; break;
			case 'Z': cp = 2; break;
			case 'D': cp = 3; break;
			case 'T': case 't': cp = 6; break;
			default:  cp = 0; break;
			}
			if (cp && shown != g_selected)
				wattron(win_main, COLOR_PAIR(cp));

			mvwprintw(win_main, row, 1,
				  "%-7d %-7d %-14.14s 0x%08X "
				  "%-12lu %-12lu %-12lu %-12lu "
				  "%-4d %-3d %-5d",
				  p->pid, p->ppid, cstr(p->comm),
				  (unsigned int)p->state,
				  p->utime, p->stime,
				  p->vsize, p->rss,
				  p->nice, p->num_threads, p->uid);

			if (cp && shown != g_selected)
				wattroff(win_main, COLOR_PAIR(cp));
			if (shown == g_selected)
				wattroff(win_main, A_REVERSE);

			shown++;
			row++;
		}
	}
}

/* ---- View 2: Tree View ---- */
static void render_tree_view(int max_rows, int cols)
{
	int i, row;
	(void)cols;

	wattron(win_main, COLOR_PAIR(5));
	mvwprintw(win_main, 0, 2, " ProcMon [TREE] ");
	wattroff(win_main, COLOR_PAIR(5));
	mvwprintw(win_main, 0, 18, "| %d nodes | %s",
		  g_tree_count, g_last_msg);
	mvwhline(win_main, 1, 0, ACS_HLINE, cols);

	if (g_selected >= g_tree_count) g_selected = g_tree_count - 1;
	if (g_selected < 0) g_selected = 0;
	if (g_selected < g_scroll)
		g_scroll = g_selected;
	if (g_selected >= g_scroll + (max_rows - 3))
		g_scroll = g_selected - (max_rows - 3) + 1;
	if (g_scroll < 0) g_scroll = 0;

	{
		int shown = 0;
		for (i = 0, row = 2; i < g_tree_count && row < max_rows - 1; i++) {
			int level = g_tree_nodes[i].level;
			int last  = tree_is_last(i, g_tree_count);
			char prefix[256] = "";
			int lev;

			if (shown < g_scroll) { shown++; continue; }

			/* 构建树前缀 */
			for (lev = 0; lev < level; lev++) {
				if (tree_has_continuation(i, g_tree_count, lev))
					strncat(prefix, T_VER,
						sizeof(prefix) - strlen(prefix) - 1);
				else
					strncat(prefix, T_BLNK,
						sizeof(prefix) - strlen(prefix) - 1);
			}
			strncat(prefix, last ? T_ELB : T_TEE,
				sizeof(prefix) - strlen(prefix) - 1);

			if (shown == g_selected)
				wattron(win_main, A_REVERSE);

			/* 按深度着色 */
			{
				int cp = 4 + (level % 3);
				if (cp > 6) cp = 4;
				if (shown != g_selected)
					wattron(win_main, COLOR_PAIR(cp));
			}

			mvwprintw(win_main, row, 2,
				  "%s%-14s (PID=%-6d PPID=%-6d LVL=%d)",
				  prefix,
				  cstr(g_tree_nodes[i].comm),
				  g_tree_nodes[i].pid,
				  g_tree_nodes[i].ppid,
				  g_tree_nodes[i].level);

			if (shown != g_selected)
				wattroff(win_main, COLOR_PAIR(4));

			if (shown == g_selected)
				wattroff(win_main, A_REVERSE);

			shown++;
			row++;
		}
	}
}

/* ---- View 3: Hex Dump ---- */
static void render_hex_view(int max_rows, int cols)
{
	int idx, row, i;
	(void)cols;

	wattron(win_main, COLOR_PAIR(5));
	mvwprintw(win_main, 0, 2, " ProcMon [HEX-DUMP] ");
	wattroff(win_main, COLOR_PAIR(5));
	mvwprintw(win_main, 0, 22, "| sizeof(proc_info)=%zu bytes | %s",
		  sizeof(struct proc_info), g_last_msg);
	mvwhline(win_main, 1, 0, ACS_HLINE, cols);

	/* 找到过滤后选中的进程 */
	idx = filtered_index(g_selected);
	if (idx < 0) {
		mvwprintw(win_main, 3, 2, "(no process selected)");
		return;
	}

	{
		const unsigned char *raw = (const unsigned char *)&g_procs[idx];
		int total_size = (int)sizeof(struct proc_info);
		int n_rows = (total_size + 15) / 16;

		/* 修正滚动: 在 hex 视图中 g_selected = hex 行号 */
		if (g_selected >= n_rows) g_selected = n_rows - 1;
		if (g_selected < 0) g_selected = 0;
		if (g_selected < g_scroll)
			g_scroll = g_selected;
		if (g_selected >= g_scroll + (max_rows - 8))
			g_scroll = g_selected - (max_rows - 8) + 1;
		if (g_scroll < 0) g_scroll = 0;

		row = 2;

		/* 列标题 */
		mvwprintw(win_main, row, 1,
			  "%-10s  %-48s  %-16s",
			  "Offset", "Hex", "ASCII");
		row++;
		mvwprintw(win_main, row, 1,
			  "----------  ------------------------------------------------  "
			  "----------------");
		row++;

		/* 十六进制行 */
		for (i = 0; i < n_rows && row < max_rows - 1; i++) {
			int off = i * 16;
			char hex[64] = "";
			char ascii[20] = "";
			int j, pos = 0;

			if (i < g_scroll) continue;

			for (j = 0; j < 16 && (off + j) < total_size; j++) {
				pos += snprintf(hex + pos, sizeof(hex) - pos,
						"%02X ", raw[off + j]);
				ascii[j] = (raw[off + j] >= 32 && raw[off + j] < 127)
					   ? (char)raw[off + j] : '.';
			}
			ascii[j] = '\0';

			/* 填充不满的行 */
			while (j < 16) {
				pos += snprintf(hex + pos, sizeof(hex) - pos, "   ");
				j++;
			}

			if (i == g_selected)
				wattron(win_main, A_REVERSE);

			mvwprintw(win_main, row, 1, "%08X    %-48s  |%-16s|",
				  off, hex, ascii);

			if (i == g_selected)
				wattroff(win_main, A_REVERSE);

			row++;
		}

		/* 字段标注区 */
		row++;
		if (row < max_rows - 1) {
			wattron(win_main, COLOR_PAIR(4));
			mvwprintw(win_main, row, 1, "--- Field Layout (x86_64 LP64 ABI) ---");
			wattroff(win_main, COLOR_PAIR(4));
			row++;

			for (i = 0; i < N_FIELDS && row < max_rows - 1; i++) {
				mvwprintw(win_main, row, 1,
					  "  +%03d (0x%02X)  %-16s  %d byte%s",
					  g_fields[i].offset, g_fields[i].offset,
					  g_fields[i].name,
					  g_fields[i].size,
					  g_fields[i].size > 1 ? "s" : " ");
				row++;
			}
		}
	}
}

/* ---- View 4: Syscall Dashboard ---- */
static void render_syscall_view(int max_rows, int cols)
{
	int row = 2;
	(void)cols;

	wattron(win_main, COLOR_PAIR(5));
	mvwprintw(win_main, 0, 2, " ProcMon [SYSCALL-DASHBOARD] ");
	wattroff(win_main, COLOR_PAIR(5));
	mvwhline(win_main, 1, 0, ACS_HLINE, cols);

	/* 系统调用结果 */
	wattron(win_main, COLOR_PAIR(4));
	mvwprintw(win_main, row, 2, "--- Syscall Results (latest call) ---");
	wattroff(win_main, COLOR_PAIR(4));
	row += 2;

	mvwprintw(win_main, row, 4,
		  "%-25s  ret=%-4ld  errno=%-3d  latency=%-8ld us  count=%-6d",
		  "proc_collect (470)",
		  g_sc_collect.ret, g_sc_collect.err,
		  g_sc_collect.latency_us, g_sc_collect.count);
	row++;

	mvwprintw(win_main, row, 4,
		  "%-25s  ret=%-4ld  errno=%-3d  latency=%-8ld us  count=%-6d",
		  "proc_snapshot (471)",
		  g_sc_snapshot.ret, g_sc_snapshot.err,
		  g_sc_snapshot.latency_us, g_sc_snapshot.count);
	row++;

	mvwprintw(win_main, row, 4,
		  "%-25s  ret=%-4ld  errno=%-3d  latency=%-8ld us",
		  "proc_stat (472)",
		  g_sc_stat.ret, g_sc_stat.err,
		  g_sc_stat.latency_us);
	row += 2;

	/* struct 大小 */
	wattron(win_main, COLOR_PAIR(4));
	mvwprintw(win_main, row, 2, "--- Struct Sizes (userspace, verify vs kernel) ---");
	wattroff(win_main, COLOR_PAIR(4));
	row += 2;

	mvwprintw(win_main, row, 4,
		  "sizeof(proc_info)      = %zu bytes  (expected ~80 on x86_64 LP64)",
		  sizeof(struct proc_info));
	row++;
	mvwprintw(win_main, row, 4,
		  "sizeof(proc_tree_node) = %zu bytes  (expected ~28 on x86_64)",
		  sizeof(struct proc_tree_node));
	row++;
	mvwprintw(win_main, row, 4,
		  "sizeof(proc_stat)      = %zu bytes  (expected ~36 on x86_64)",
		  sizeof(struct proc_stat));
	row++;
	mvwprintw(win_main, row, 4,
		  "SC_CLK_TCK             = %ld ticks/sec", clk_tck);
	row++;
	mvwprintw(win_main, row, 4,
		  "MAX_PROCS              = %d", MAX_PROCS);
	row += 2;

	/* proc_stat 字段 */
	wattron(win_main, COLOR_PAIR(4));
	mvwprintw(win_main, row, 2, "--- proc_stat Breakdown (from syscall 472) ---");
	wattroff(win_main, COLOR_PAIR(4));
	row += 2;

#define STAT_ROW(label, field) \
	mvwprintw(win_main, row, 4, "%-24s = %d", label, g_stat.field); row++

	STAT_ROW("total_processes",     total_processes);
	STAT_ROW("running_processes",   running_processes);
	STAT_ROW("sleeping_processes",  sleeping_processes);
	STAT_ROW("uninterruptible",     uninterruptible);
	STAT_ROW("stopped_processes",   stopped_processes);
	STAT_ROW("zombie_processes",    zombie_processes);
	STAT_ROW("idle_processes",      idle_processes);
	STAT_ROW("kernel_threads",      kernel_threads);
	STAT_ROW("user_threads",        user_threads);

#undef STAT_ROW

	/* 交叉验证: 用 proc_info 数出的状态与 proc_stat 对比 */
	row++;
	if (row < max_rows - 1) {
		int local_r = 0, local_s = 0, local_d = 0, local_t = 0, local_z = 0;
		for (int i2 = 0; i2 < g_proc_count; i2++) {
			int st = g_procs[i2].state;
			if (st & 0x20) local_z++;
			else if (!(st & 0x10)) {
				switch (st & 0x03) {
				case 0: local_r++; break;
				case 1: local_s++; break;
				case 2: local_d++; break;
				default: break;
				}
			}
			if (st & 0x04) local_t++;
		}
		wattron(win_main, COLOR_PAIR(4));
		mvwprintw(win_main, row, 2, "--- Cross-check (proc_info[] vs proc_stat) ---");
		wattroff(win_main, COLOR_PAIR(4));
		row++;
		mvwprintw(win_main, row, 4,
			  "R: proc_info=%-5d  proc_stat=%-5d  %s",
			  local_r, g_stat.running_processes,
			  local_r == g_stat.running_processes ? "OK" : "MISMATCH!");
		row++;
		mvwprintw(win_main, row, 4,
			  "S: proc_info=%-5d  proc_stat=%-5d  %s",
			  local_s, g_stat.sleeping_processes,
			  local_s == g_stat.sleeping_processes ? "OK" : "MISMATCH!");
		row++;
		mvwprintw(win_main, row, 4,
			  "D: proc_info=%-5d  proc_stat=%-5d  %s",
			  local_d, g_stat.uninterruptible,
			  local_d == g_stat.uninterruptible ? "OK" : "MISMATCH!");
		row++;
		mvwprintw(win_main, row, 4,
			  "Z: proc_info=%-5d  proc_stat=%-5d  %s",
			  local_z, g_stat.zombie_processes,
			  local_z == g_stat.zombie_processes ? "OK" : "MISMATCH!");
		row++;
		mvwprintw(win_main, row, 4,
			  "Note: mismatches expected due to race between collect & stat syscalls");
	}
}

/* ===================================================================
 * 底部面板渲染
 * =================================================================== */

/* 详情面板: 显示选中进程的所有 proc_info 字段 + 状态位解码 */
static void render_detail_panel(void)
{
	int idx;
	char state_buf[256];

	wclear(win_detail);
	box(win_detail, 0, 0);

	if (g_view_mode == 2) {
		/* 树视图: 显示树节点信息 */
		if (g_selected < g_tree_count) {
			const struct proc_tree_node *t = &g_tree_nodes[g_selected];
			mvwprintw(win_detail, 0, 2,
				  "TREE: PID=%-6d PPID=%-6d LVL=%-3d comm=\"%s\"",
				  t->pid, t->ppid, t->level, cstr(t->comm));
		}
		wrefresh(win_detail);
		return;
	}

	if (g_view_mode == 4) {
		mvwprintw(win_detail, 0, 2,
			  "Syscall Dashboard — use v to switch views, x to export raw dump");
		wrefresh(win_detail);
		return;
	}

	idx = filtered_index(g_selected);
	if (idx < 0) {
		mvwprintw(win_detail, 0, 2,
			  "(no process selected — %d total, %d after filter)",
			  g_proc_count, filtered_count());
		wrefresh(win_detail);
		return;
	}

	{
		const struct proc_info *p = &g_procs[idx];
		state_decode(p->state, state_buf, sizeof(state_buf));

		/* Line 0: 全部 11 字段 (紧凑) */
		mvwprintw(win_detail, 0, 2,
			  "PID=%-6d PPID=%-6d comm=\"%s\" "
			  "state=0x%04X(%c:%s) nice=%-4d threads=%-3d uid=%-5d",
			  p->pid, p->ppid, cstr(p->comm),
			  (unsigned int)p->state,
			  state_char(p->state), state_name(p->state),
			  p->nice, p->num_threads, p->uid);

		/* Line 1: 时间/内存 + 状态位解码 */
		mvwprintw(win_detail, 1, 2,
			  "utime=%-10lu stime=%-10lu vsize=%s(%luB) "
			  "rss=%s(%lupg) | %s",
			  p->utime, p->stime,
			  fmt_size(p->vsize), p->vsize,
			  fmt_size(p->rss * 4096), p->rss,
			  state_buf);
	}

	wrefresh(win_detail);
}

/* 系统调用面板: 返回值/耗时 + proc_stat 聚合 */
static void render_syscall_panel(void)
{
	wclear(win_syscall);
	box(win_syscall, 0, 0);

	/* Line 0: 三个 syscall 的结果 (ret/errno/latency/count) */
	mvwprintw(win_syscall, 0, 2,
		  "collect(470): ret=%ld err=%d %ldus/%d | "
		  "snapshot(471): ret=%ld err=%d %ldus/%d | "
		  "stat(472): ret=%ld err=%d %ldus",
		  g_sc_collect.ret, g_sc_collect.err,
		  g_sc_collect.latency_us, g_sc_collect.count,
		  g_sc_snapshot.ret, g_sc_snapshot.err,
		  g_sc_snapshot.latency_us, g_sc_snapshot.count,
		  g_sc_stat.ret, g_sc_stat.err,
		  g_sc_stat.latency_us);

	/* Line 1: proc_stat 聚合 + 关键参数 */
	mvwprintw(win_syscall, 1, 2,
		  "T:%d R:%d S:%d D:%d T:%d Z:%d I:%d | "
		  "Kthr:%d Uthr:%d | "
		  "CLK_TCK=%ld | View:%s | %s",
		  g_stat.total_processes,
		  g_stat.running_processes,
		  g_stat.sleeping_processes,
		  g_stat.uninterruptible,
		  g_stat.stopped_processes,
		  g_stat.zombie_processes,
		  g_stat.idle_processes,
		  g_stat.kernel_threads,
		  g_stat.user_threads,
		  clk_tck,
		  g_view_names[g_view_mode],
		  g_filter_active ? "FILTERED" : "");

	wrefresh(win_syscall);
}

/* 提示栏 */
static void render_hint_panel(void)
{
	wclear(win_hint);

	if (g_filter_mode) {
		wattron(win_hint, A_REVERSE);
		mvwprintw(win_hint, 0, 0, " FILTER: %s_", g_cmd);
		wattroff(win_hint, A_REVERSE);
	} else {
		wattron(win_hint, COLOR_PAIR(5));
		mvwprintw(win_hint, 0, 0,
			  " v:view[%s] %s%s:sel Tab:sort s:rev "
			  "/:filter r:clear x:export h:help q:quit",
			  g_view_names[g_view_mode],
			  "\xe2\x86\x91\xe2\x86\x93",  /* ↑↓ (UTF-8 arrows) */
			  "");
		wattroff(win_hint, COLOR_PAIR(5));
	}

	wrefresh(win_hint);
}

/* ===================================================================
 * 主重绘
 * =================================================================== */

static void gui_redraw(void)
{
	int max_rows, cols;

	getmaxyx(win_main, max_rows, cols);

	wclear(win_main);
	box(win_main, 0, 0);

	switch (g_view_mode) {
	case 0: render_list_view(max_rows, cols);    break;
	case 1: render_debug_view(max_rows, cols);   break;
	case 2: render_tree_view(max_rows, cols);    break;
	case 3: render_hex_view(max_rows, cols);     break;
	case 4: render_syscall_view(max_rows, cols); break;
	}

	wrefresh(win_main);
	render_detail_panel();
	render_syscall_panel();
	render_hint_panel();
}

/* ===================================================================
 * 导出函数
 * =================================================================== */

static void export_csv(const char *filename)
{
	FILE *fp;
	int i;

	fp = fopen(filename, "w");
	if (!fp) {
		snprintf(g_last_msg, sizeof(g_last_msg),
			 "export CSV: %s", strerror(errno));
		return;
	}

	fprintf(fp, "# proc_monitor CSV export\n");
	fprintf(fp, "# clk_tck=%ld\n", clk_tck);
	fprintf(fp, "pid,ppid,comm,state,state_hex,utime,stime,"
		"vsize,rss,nice,num_threads,uid\n");

	for (i = 0; i < g_proc_count; i++) {
		const struct proc_info *p = &g_procs[i];
		fprintf(fp, "%d,%d,%s,%c,0x%04X,%lu,%lu,%lu,%lu,%d,%d,%d\n",
			p->pid, p->ppid, cstr(p->comm),
			state_char(p->state), (unsigned int)p->state,
			p->utime, p->stime,
			p->vsize, p->rss,
			p->nice, p->num_threads, p->uid);
	}

	fclose(fp);
	snprintf(g_last_msg, sizeof(g_last_msg),
		 "CSV exported: %s (%d procs)", filename, g_proc_count);
}

static void export_tree_dot(const char *filename)
{
	FILE *fp;
	int i;

	fp = fopen(filename, "w");
	if (!fp) {
		snprintf(g_last_msg, sizeof(g_last_msg),
			 "export DOT: %s", strerror(errno));
		return;
	}

	fprintf(fp, "// Process tree exported by proc_monitor (debug edition)\n");
	fprintf(fp, "// Render: dot -Tpng %s -o tree.png\n", filename);
	fprintf(fp, "digraph proc_tree {\n");
	fprintf(fp, "  rankdir=TB;\n");
	fprintf(fp, "  node [shape=box,style=filled,fillcolor=lightyellow];\n\n");

	for (i = 0; i < g_tree_count; i++) {
		fprintf(fp, "  p%d [label=\"%s\\nPID=%d\\nLVL=%d\"];\n",
			g_tree_nodes[i].pid,
			cstr(g_tree_nodes[i].comm),
			g_tree_nodes[i].pid,
			g_tree_nodes[i].level);
		if (g_tree_nodes[i].ppid != 0 && g_tree_nodes[i].pid != 1)
			fprintf(fp, "  p%d -> p%d;\n",
				g_tree_nodes[i].ppid,
				g_tree_nodes[i].pid);
	}
	fprintf(fp, "}\n");
	fclose(fp);
	snprintf(g_last_msg, sizeof(g_last_msg),
		 "DOT exported: %s (%d nodes)", filename, g_tree_count);
}

static void export_raw(const char *filename)
{
	FILE *fp;

	fp = fopen(filename, "wb");
	if (!fp) {
		snprintf(g_last_msg, sizeof(g_last_msg),
			 "export RAW: %s", strerror(errno));
		return;
	}

	/* 写入文件头: magic + count + struct_size */
	{
		unsigned int magic = 0x50524F43;  /* "PROC" */
		int count = g_proc_count;
		int sz    = (int)sizeof(struct proc_info);
		fwrite(&magic, 4, 1, fp);
		fwrite(&count, 4, 1, fp);
		fwrite(&sz,    4, 1, fp);
	}

	fwrite(g_procs, sizeof(struct proc_info), (size_t)g_proc_count, fp);
	fclose(fp);
	snprintf(g_last_msg, sizeof(g_last_msg),
		 "RAW dumped: %s (%d procs x %zu bytes = %zu total)",
		 filename, g_proc_count,
		 sizeof(struct proc_info),
		 (size_t)g_proc_count * sizeof(struct proc_info));
}

/* ===================================================================
 * 帮助覆盖层
 * =================================================================== */

static void show_help(void)
{
	def_prog_mode();
	endwin();
	printf("\n");
	printf("==== Process Monitor — Debug Edition ====\n");
	printf("\n");
	printf("VIEW MODES (press v to cycle, or number keys 1-5):\n");
	printf("  [1] LIST         Compact process table (state hex, CPU%%, all fields)\n");
	printf("  [2] DEBUG-TABLE  All proc_info fields as RAW numbers\n");
	printf("  [3] TREE         Process hierarchy with indentation\n");
	printf("  [4] HEX-DUMP     Raw bytes of selected proc_info + field layout\n");
	printf("  [5] SYSCALL      Syscall results, struct sizes, cross-validation\n");
	printf("\n");
	printf("NAVIGATION:\n");
	printf("  Up/Down j/k     Move selection\n");
	printf("  PgUp/PgDn       Page scroll\n");
	printf("  Home/End        Jump to first/last\n");
	printf("\n");
	printf("SORT & FILTER:\n");
	printf("  Tab             Cycle sort field (PID/CPU/MEM/NAME)\n");
	printf("  s               Toggle asc/desc\n");
	printf("  /               Enter filter mode\n");
	printf("  r               Clear filter\n");
	printf("\n");
	printf("FILTER SYNTAX:\n");
	printf("  bash            Name substring (case-insensitive)\n");
	printf("  =R              State char (R/S/D/T/Z/X)\n");
	printf("  =0x22           Exact state hex value (for debugging!)\n");
	printf("  :1234           Exact PID\n");
	printf("\n");
	printf("EXPORT (x key):\n");
	printf("  LIST/DEBUG      -> CSV file\n");
	printf("  TREE            -> DOT (Graphviz) file\n");
	printf("  HEX/SYSCALL     -> Raw binary dump (.bin)\n");
	printf("\n");
	printf("OTHER:\n");
	printf("  h / ?           Show this help\n");
	printf("  q / ESC         Quit\n");
	printf("\n");
	printf("SIZEOF (verify vs kernel):\n");
	printf("  proc_info      = %zu bytes\n", sizeof(struct proc_info));
	printf("  proc_tree_node = %zu bytes\n", sizeof(struct proc_tree_node));
	printf("  proc_stat      = %zu bytes\n", sizeof(struct proc_stat));
	printf("  clk_tck        = %ld\n", clk_tck);
	printf("\nPress Enter to continue...");
	getchar();
	reset_prog_mode();
	refresh();
}

/* ===================================================================
 * 交互式导出 (带文件名输入)
 * =================================================================== */

static void do_export(void)
{
	char fname[256];

	def_prog_mode();
	endwin();

	switch (g_view_mode) {
	case 0: /* LIST -> CSV */
	case 1: /* DEBUG -> CSV */
		snprintf(fname, sizeof(fname), "proc_list.csv");
		printf("Export CSV to [%s]: ", fname);
		break;
	case 2: /* TREE -> DOT */
		snprintf(fname, sizeof(fname), "proc_tree.dot");
		printf("Export DOT to [%s]: ", fname);
		break;
	default: /* HEX/SYSCALL -> RAW */
		snprintf(fname, sizeof(fname), "proc_dump.bin");
		printf("Export RAW to [%s]: ", fname);
		break;
	}
	fflush(stdout);

	{
		char buf[256];
		if (fgets(buf, sizeof(buf), stdin) && buf[0] != '\n') {
			size_t l = strlen(buf);
			if (l > 0 && buf[l-1] == '\n') buf[l-1] = '\0';
			snprintf(fname, sizeof(fname), "%s", buf);
		}
	}

	reset_prog_mode();
	refresh();

	/* 确保拿到最新数据 */
	update_data();

	switch (g_view_mode) {
	case 0:
	case 1:
		export_csv(fname);
		break;
	case 2:
		export_tree_dot(fname);
		break;
	default:
		export_raw(fname);
		break;
	}
}

/* ===================================================================
 * 主循环
 * =================================================================== */

int main(void)
{
	int ch;

	clk_tck = sysconf(_SC_CLK_TCK);
	if (clk_tck <= 0) clk_tck = 100;

	/* 初始化 syscall 结果 */
	memset(&g_sc_collect,  0, sizeof(g_sc_collect));
	memset(&g_sc_snapshot, 0, sizeof(g_sc_snapshot));
	memset(&g_sc_stat,     0, sizeof(g_sc_stat));

	gui_init();

	/* 首次获取数据 */
	if (update_data() < 0) {
		gui_cleanup();
		fprintf(stderr, "ERROR: Failed to fetch data.\n"
			"Are the custom syscalls (470-472) installed?\n"
			"Try: sudo ./proc_monitor\n\n");
		fprintf(stderr, "[DEBUG] struct sizes (userspace):\n"
			"  sizeof(proc_info)      = %zu\n"
			"  sizeof(proc_tree_node) = %zu\n"
			"  sizeof(proc_stat)      = %zu\n"
			"  SC_CLK_TCK             = %ld\n",
			sizeof(struct proc_info),
			sizeof(struct proc_tree_node),
			sizeof(struct proc_stat),
			clk_tck);
		return 1;
	}
	sort_procs();
	save_prev_cpu();

	/* 启动时输出 struct 大小到 stderr (便于与内核对比) */
	fprintf(stderr, "[DEBUG] struct sizes: "
		"proc_info=%zu  proc_tree_node=%zu  proc_stat=%zu  clk_tck=%ld\n",
		sizeof(struct proc_info), sizeof(struct proc_tree_node),
		sizeof(struct proc_stat), clk_tck);

	while (g_running) {
		gui_redraw();

		ch = wgetch(win_main);

		if (ch == ERR) {
			/* 超时: 自动刷新数据 */
			save_prev_cpu();
			update_data();
			sort_procs();
			continue;
		}

		/* ---- 过滤模式: 逐字符处理 ---- */
		if (g_filter_mode) {
			if (ch == '\n' || ch == KEY_ENTER) {
				if (g_cmd_pos > 0) {
					g_cmd[g_cmd_pos] = '\0';
					strncpy(g_filter, g_cmd,
						sizeof(g_filter) - 1);
					g_filter[sizeof(g_filter) - 1] = '\0';
					g_filter_active = 1;
				}
				g_filter_mode = 0;
				g_cmd_pos = 0;
				g_cmd[0] = '\0';
				g_selected = 0;
				g_scroll = 0;
				curs_set(0);
			} else if (ch == 27) {
				/* ESC: 取消过滤 */
				g_filter_mode = 0;
				g_cmd_pos = 0;
				g_cmd[0] = '\0';
				curs_set(0);
			} else if (ch == KEY_BACKSPACE || ch == 127) {
				if (g_cmd_pos > 0)
					g_cmd[--g_cmd_pos] = '\0';
			} else if (ch >= 32 && ch < 127 &&
				   g_cmd_pos < MAX_CMD_LEN - 1) {
				g_cmd[g_cmd_pos++] = (char)ch;
			}
			continue;
		}

		/* ---- 普通模式 ---- */
		switch (ch) {
		case 'q':
		case 27:  /* ESC */
			g_running = 0;
			break;

		/* 视图切换 */
		case 'v':
			g_view_mode = (g_view_mode + 1) % N_VIEWS;
			g_selected = 0;
			g_scroll = 0;
			break;

		case '1': g_view_mode = 0; g_selected = 0; g_scroll = 0; break;
		case '2': g_view_mode = 1; g_selected = 0; g_scroll = 0; break;
		case '3': g_view_mode = 2; g_selected = 0; g_scroll = 0; break;
		case '4': g_view_mode = 3; g_selected = 0; g_scroll = 0; break;
		case '5': g_view_mode = 4; g_selected = 0; g_scroll = 0; break;

		/* 导航 */
		case KEY_UP: case 'k':
			if (g_selected > 0) g_selected--;
			break;

		case KEY_DOWN: case 'j':
			g_selected++;
			break;

		case KEY_PPAGE:
			g_selected -= 20;
			if (g_selected < 0) g_selected = 0;
			break;

		case KEY_NPAGE:
			g_selected += 20;
			break;

		case KEY_HOME:
			g_selected = 0;
			g_scroll = 0;
			break;

		case KEY_END:
			g_selected = 999999;  /* clamp 在 render 时做 */
			break;

		/* 排序 */
		case '\t':
			g_sort_field = (g_sort_field + 1) % 4;
			sort_procs();
			g_selected = 0;
			g_scroll = 0;
			break;

		case 's':
			g_sort_desc = !g_sort_desc;
			sort_procs();
			break;

		/* 过滤 */
		case 'r':
			g_filter[0] = '\0';
			g_filter_active = 0;
			g_selected = 0;
			g_scroll = 0;
			break;

		case '/':
			g_filter_mode = 1;
			g_cmd_pos = 0;
			memset(g_cmd, 0, sizeof(g_cmd));
			curs_set(1);
			break;

		/* 导出 */
		case 'x':
			do_export();
			break;

		/* 帮助 */
		case 'h': case '?':
			show_help();
			/* 从 help 回来后需要强制重绘 */
			touchwin(win_main);
			touchwin(win_detail);
			touchwin(win_syscall);
			touchwin(win_hint);
			break;

		/* 终端 resize */
		case KEY_RESIZE:
			{
				int nr, nc;
				getmaxyx(stdscr, nr, nc);
				wresize(win_main,    nr - 6, nc);
				wresize(win_detail,  2,       nc);
				wresize(win_syscall, 2,       nc);
				wresize(win_hint,    1,       nc);
				mvwin(win_detail,  nr - 6, 0);
				mvwin(win_syscall, nr - 4, 0);
				mvwin(win_hint,    nr - 1, 0);
			}
			break;

		default:
			break;
		}
	}

	gui_cleanup();
	return 0;
}
