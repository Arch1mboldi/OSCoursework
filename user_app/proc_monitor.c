/*
 * proc_monitor.c — Linux 进程全量监控 TUI (ncurses 版)
 *
 * 通过自定义系统调用 (470/471/472) 获取数据。
 * 模仿 osfs-system/gui.c 的多面板 ncurses 布局风格。
 *
 * 面板:
 *   win_main   — 进程列表 / 进程树
 *   win_info   — 选中进程详情 + 按键提示
 *   win_status — 全局统计 / 排序过滤状态
 *   win_input  — 过滤/命令输入 (仅激活时可见)
 *   win_hint   — 按键提示栏
 *
 * 操作:
 *   方向键/jk  选择       Tab / s  排序        t  列表/树
 *   /          过滤       r        清除过滤    e  导出进程树
 *   h          帮助       q/ESC    退出
 *
 * 编译: gcc -Wall -O2 -o proc_monitor proc_monitor.c -lncurses
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
#include <ncurses.h>
#include "proc_monitor.h"

/* ===================================================================
 * 常量
 * =================================================================== */

#define MAX_PROCS     8192
#define MAX_CMD_LEN   256

/* 运行时从 sysconf 获取 (替代硬编码 HZ=100) */
static long clk_tck = 100;

/* ===================================================================
 * ncurses 窗口
 * =================================================================== */

static WINDOW *win_main, *win_info, *win_status, *win_input;

/* ===================================================================
 * 树绘制字符 (UTF-8 环境用 Unicode, 否则 ASCII fallback)
 * =================================================================== */

static const char *T_VER, *T_TEE, *T_ELB, *T_BLNK;

/* ===================================================================
 * 全局状态
 * =================================================================== */

static struct proc_info       g_procs[MAX_PROCS];
static struct proc_tree_node  g_tree_nodes[MAX_PROCS];
static struct proc_stat       g_stat;
static int g_proc_count  = 0;
static int g_tree_count  = 0;
static int g_selected    = 0;
static int g_scroll      = 0;
static int g_running     = 1;

/* 视图模式 */
static int g_view_list   = 1;    /* 1=list, 0=tree */

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

/* CPU% 计算: 记录上一帧每进程的 CPU 滴答 + 时间戳 */
struct prev_cpu {
	pid_t pid;
	unsigned long total;  /* utime + stime, clock ticks */
};
static struct prev_cpu g_prev[MAX_PROCS];
static int             g_prev_count = 0;
static struct timeval  g_last_tv;

/* 上次错误/提示 */
static char g_last_msg[256] = "";

/* ===================================================================
 * 辅助函数
 * =================================================================== */

static const char *cstr(const char comm[16])
{
	static char buf[17];
	int n;
	for (n = 0; n < 16 && comm[n]; n++)
		buf[n] = comm[n];
	buf[n] = '\0';
	return buf;
}

static char state_char(int state)
{
	switch (state) {
	case 0:  return 'R';
	case 1:  return 'S';
	case 2:  return 'D';
	case 4:  return 'T';
	case 8:  return 't';
	case 16: return 'Z';
	case 32: return 'X';
	default: return '?';
	}
}

static const char *state_name(int state)
{
	switch (state) {
	case 0:  return "RUNNING";
	case 1:  return "SLEEPING";
	case 2:  return "DISK_WAIT";
	case 4:  return "STOPPED";
	case 8:  return "TRACED";
	case 16: return "ZOMBIE";
	default: return "IDLE/OTHER";
	}
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
		return 0.0;  /* PID reused, 重置 */

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

/* 保存当前帧各进程 CPU 值, 供下一帧计算 CPU% */
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

/* ===================================================================
 * 系统调用层
 * =================================================================== */

static int fetch_procs(void)
{
	int count = 0;
	long ret;
	ret = syscall(SYS_proc_collect, g_procs, MAX_PROCS, &count);
	if (ret < 0) {
		snprintf(g_last_msg, sizeof(g_last_msg),
			 "proc_collect: %s", strerror(errno));
		return -1;
	}
	g_proc_count = count;
	return 0;
}

static int fetch_tree(void)
{
	int count = 0;
	long ret;
	ret = syscall(SYS_proc_snapshot, g_tree_nodes, MAX_PROCS, &count);
	if (ret < 0) {
		snprintf(g_last_msg, sizeof(g_last_msg),
			 "proc_snapshot: %s", strerror(errno));
		return -1;
	}
	g_tree_count = count;
	return 0;
}

static int fetch_stat(void)
{
	long ret;
	ret = syscall(SYS_proc_stat, &g_stat);
	if (ret < 0) {
		snprintf(g_last_msg, sizeof(g_last_msg),
			 "proc_stat: %s", strerror(errno));
		return -1;
	}
	return 0;
}

static int update_data(void)
{
	if (fetch_procs() < 0) return -1;
	if (fetch_tree() < 0) return -1;
	if (fetch_stat() < 0) return -1;
	g_last_msg[0] = '\0';
	return 0;
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
	char text[64];
	int i;

	if (!g_filter_active || g_filter[0] == '\0')
		return 1;

	for (i = 0; g_filter[i]; i++)
		text[i] = (char)(g_filter[i] >= 'A' && g_filter[i] <= 'Z'
				 ? g_filter[i] + 32 : g_filter[i]);
	text[i] = '\0';

	if (text[0] == '=' && text[1]) {
		char want = (char)(text[1] >= 'a' ? text[1] - 32 : text[1]);
		return state_char(p->state) == want;
	}

	if (text[0] == ':') {
		int pid = atoi(text + 1);
		return p->pid == pid;
	}

	{
		char name[17];
		int j;
		for (j = 0; j < 16 && p->comm[j]; j++)
			name[j] = (char)(p->comm[j] >= 'A' && p->comm[j] <= 'Z'
					 ? p->comm[j] + 32 : p->comm[j]);
		name[j] = '\0';
		return strstr(name, text) != NULL;
	}
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
 * 进程树导出
 * =================================================================== */

static void export_tree(const char *filename)
{
	FILE *fp;
	int i;

	fp = fopen(filename, "w");
	if (!fp) {
		snprintf(g_last_msg, sizeof(g_last_msg),
			 "export: %s", strerror(errno));
		return;
	}

	fprintf(fp, "// Process tree exported by proc_monitor\n");
	fprintf(fp, "// Render: dot -Tpng %s -o tree.png\n", filename);
	fprintf(fp, "digraph proc_tree {\n");
	fprintf(fp, "  rankdir=TB;\n");
	fprintf(fp, "  node [shape=box,style=filled,fillcolor=lightyellow];\n\n");

	for (i = 0; i < g_tree_count; i++) {
		fprintf(fp, "  p%d [label=\"%s\\nPID=%d\"];\n",
			g_tree_nodes[i].pid,
			cstr(g_tree_nodes[i].comm),
			g_tree_nodes[i].pid);
		if (g_tree_nodes[i].ppid != 0 && g_tree_nodes[i].pid != 1)
			fprintf(fp, "  p%d -> p%d;\n",
				g_tree_nodes[i].ppid,
				g_tree_nodes[i].pid);
	}
	fprintf(fp, "}\n");
	fclose(fp);
	snprintf(g_last_msg, sizeof(g_last_msg),
		 "Tree exported: %s (%d nodes)", filename, g_tree_count);
}

/* ===================================================================
 * ncurses GUI
 * =================================================================== */

static void gui_init(void)
{
	int rows, cols;
	const char *codeset;

	setlocale(LC_ALL, "");
	codeset = nl_langinfo(CODESET);

	/* UTF-8 检测: 终端支持则用 Unicode 树线, 否则 ASCII fallback */
	if (codeset && strstr(codeset, "UTF")) {
		T_VER  = "\xe2\x94\x82   ";  /* │    */
		T_TEE  = "\xe2\x94\x9c\xe2\x94\x80\xe2\x94\x80 ";  /* ├──  */
		T_ELB  = "\xe2\x94\x94\xe2\x94\x80\xe2\x94\x80 ";  /* └──  */
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
		init_pair(1, COLOR_GREEN,  COLOR_BLACK);  /* R state */
		init_pair(2, COLOR_RED,    COLOR_BLACK);  /* Z state */
		init_pair(3, COLOR_YELLOW, COLOR_BLACK);  /* D state */
		init_pair(4, COLOR_CYAN,   COLOR_BLACK);  /* tree/title */
		init_pair(5, COLOR_WHITE,  COLOR_BLUE);   /* header */
	}

	getmaxyx(stdscr, rows, cols);

	win_main   = newwin(rows - 5, cols, 0, 0);
	win_info   = newwin(2, cols, rows - 5, 0);
	win_status = newwin(1, cols, rows - 3, 0);
	win_input  = newwin(1, cols, rows - 1, 0);

	keypad(win_main, TRUE);
	keypad(win_input, TRUE);
	scrollok(win_main, FALSE);

	/* 1 秒超时, 自动刷新数据 */
	wtimeout(win_input, 1000);
}

static void gui_cleanup(void)
{
	delwin(win_main);
	delwin(win_info);
	delwin(win_status);
	delwin(win_input);
	endwin();
}

static void gui_redraw(void)
{
	int i, row, max_rows, cols;
	const char *fields[] = { "PID", "CPU", "MEM", "NAME" };

	getmaxyx(win_main, max_rows, cols);
	(void)cols;

	/* ========== win_main ========== */
	wclear(win_main);
	box(win_main, 0, 0);

	if (g_view_list) {
		wattron(win_main, COLOR_PAIR(5));
		mvwprintw(win_main, 0, 2, " ProcMon ");
		wattroff(win_main, COLOR_PAIR(5));
		mvwprintw(win_main, 0, 12, "| Sort:%s %s | %s | %s",
			  (const char *[]){"PID","CPU","MEM","NAME"}[g_sort_field],
			  g_sort_desc ? "desc" : "asc",
			  g_filter_active ? "FILT" : "",
			  g_last_msg);

		mvwprintw(win_main, 2, 2,
			  "%-7s %-7s %-14s %c %-7s %-7s %-6s %-8s %-8s %s",
			  "PID", "PPID", "NAME", 'S',
			  "UTIME", "STIME", "CPU%",
			  "VSIZE", "RSS", "UID");
		mvwhline(win_main, 3, 1, ACS_HLINE, cols - 2);

		{   /* 统计过滤后数量 + clamp 滚动 */
			int total = 0, shown;
			for (i = 0; i < g_proc_count; i++)
				if (match_filter(&g_procs[i])) total++;

			if (g_selected >= total) g_selected = total - 1;
			if (g_selected < 0) g_selected = 0;
			if (g_selected < g_scroll)
				g_scroll = g_selected;
			if (g_selected >= g_scroll + (max_rows - 5))
				g_scroll = g_selected - (max_rows - 5) + 1;
			if (g_scroll < 0) g_scroll = 0;

			shown = 0;
			for (i = 0, row = 4;
			     i < g_proc_count && row < max_rows - 1; i++) {
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
				default:  cp = 0; break;
				}
				if (cp && shown != g_selected)
					wattron(win_main, COLOR_PAIR(cp));

				mvwprintw(win_main, row, 2,
					  "%-7d %-7d %-14.14s %c "
					  "%-7.1f %-7.1f %-5.1f "
					  "%-8s %-8s %d",
					  p->pid, p->ppid, cstr(p->comm), st,
					  cpu_utime_sec(p),
					  cpu_stime_sec(p),
					  cpu_pct(p->pid,
						  p->utime + p->stime),
					  fmt_size(p->vsize),
					  fmt_size(p->rss * 4096),
					  p->uid);

				if (cp && shown != g_selected)
					wattroff(win_main, COLOR_PAIR(cp));
				if (shown == g_selected)
					wattroff(win_main, A_REVERSE);
				shown++;
				row++;
			}
			g_last_msg[0] = '\0';  /* 已显示, 清除 */
		}
	} else {
		/* ---- 进程树视图 ---- */
		wattron(win_main, COLOR_PAIR(5));
		mvwprintw(win_main, 0, 2, " Process Tree ");
		wattroff(win_main, COLOR_PAIR(5));
		mvwprintw(win_main, 0, 18, "| %d nodes | %s",
			  g_tree_count, g_last_msg);
		mvwhline(win_main, 1, 1, ACS_HLINE, cols - 2);

		if (g_selected >= g_tree_count) g_selected = g_tree_count - 1;
		if (g_selected < 0) g_selected = 0;
		if (g_selected < g_scroll)
			g_scroll = g_selected;
		if (g_selected >= g_scroll + (max_rows - 3))
			g_scroll = g_selected - (max_rows - 3) + 1;
		if (g_scroll < 0) g_scroll = 0;

		{
			int shown = 0;
			for (i = 0, row = 2;
			     i < g_tree_count && row < max_rows - 1; i++) {
				int level = g_tree_nodes[i].level;
				int last  = tree_is_last(i, g_tree_count);
				char prefix[128] = "";
				int lev;

				if (shown < g_scroll) { shown++; continue; }

				for (lev = 0; lev < level; lev++) {
					if (tree_has_continuation(i,
							g_tree_count, lev))
						strcat(prefix, T_VER);
					else
						strcat(prefix, T_BLNK);
				}
				strcat(prefix, last ? T_ELB : T_TEE);

				if (shown == g_selected)
					wattron(win_main, A_REVERSE);
				wattron(win_main, COLOR_PAIR(4));
				mvwprintw(win_main, row, 2, "%s%s(%d)",
					  prefix,
					  cstr(g_tree_nodes[i].comm),
					  g_tree_nodes[i].pid);
				wattroff(win_main, COLOR_PAIR(4));
				if (shown == g_selected)
					wattroff(win_main, A_REVERSE);
				shown++;
				row++;
			}
		}
		g_last_msg[0] = '\0';
	}

	wrefresh(win_main);

	/* ========== win_info: 按键提示 + 选中项详情 ========== */
	wclear(win_info);
	box(win_info, 0, 0);

	{
		const char *hints =
			"Keys: q=quit / =filter r=clear t=tree/list "
			"s=sort Tab=field e=export h=help";

		if (g_view_list) {
			int cnt = 0, sel_idx = -1;
			for (i = 0; i < g_proc_count; i++) {
				if (!match_filter(&g_procs[i])) continue;
				if (cnt == g_selected) { sel_idx = i; break; }
				cnt++;
			}
			if (sel_idx >= 0) {
				const struct proc_info *p = &g_procs[sel_idx];
				mvwprintw(win_info, 0, 2,
					  "PID=%-6d PPID=%-6d %s(%c) "
					  "UTIME=%.1fs STIME=%.1fs "
					  "CPU%%=%.1f NICE=%d THR=%d "
					  "VSIZE=%s RSS=%s",
					  p->pid, p->ppid,
					  state_name(p->state),
					  state_char(p->state),
					  cpu_utime_sec(p),
					  cpu_stime_sec(p),
					  cpu_pct(p->pid,
						  p->utime + p->stime),
					  p->nice, p->num_threads,
					  fmt_size(p->vsize),
					  fmt_size(p->rss * 4096));
			} else {
				mvwprintw(win_info, 0, 2, "%s", hints);
			}
		} else {
			if (g_selected < g_tree_count) {
				mvwprintw(win_info, 0, 2,
					  "PID=%-6d PPID=%-6d LVL=%d %s",
					  g_tree_nodes[g_selected].pid,
					  g_tree_nodes[g_selected].ppid,
					  g_tree_nodes[g_selected].level,
					  cstr(g_tree_nodes[g_selected].comm));
			}
		}
		mvwprintw(win_info, 1, 2, "%s", hints);
	}

	wrefresh(win_info);

	/* ========== win_status ========== */
	wclear(win_status);
	wattron(win_status, COLOR_PAIR(5));
	mvwprintw(win_status, 0, 0,
		  " T:%d R:%d S:%d D:%d Z:%d T:%d | "
		  "Kthr:%d Uthr:%d | %s %s | %d/%d",
		  g_stat.total_processes,
		  g_stat.running_processes,
		  g_stat.sleeping_processes,
		  g_stat.uninterruptible,
		  g_stat.zombie_processes,
		  g_stat.stopped_processes,
		  g_stat.kernel_threads,
		  g_stat.user_threads,
		  g_view_list ? "LIST" : "TREE",
		  fields[g_sort_field],
		  g_selected,
		  g_view_list ? g_proc_count : g_tree_count);
	wattroff(win_status, COLOR_PAIR(5));
	wrefresh(win_status);

	/* ========== win_input ========== */
	wclear(win_input);
	if (g_filter_mode) {
		mvwprintw(win_input, 0, 0, "filter: %s", g_cmd);
	} else if (g_cmd_pos > 0) {
		mvwprintw(win_input, 0, 0, "> %s", g_cmd);
	}
	/* 正常模式不显示任何提示符 —— 按键提示在 win_info 中 */
	wrefresh(win_input);
}

/* ===================================================================
 * 主循环
 * =================================================================== */

int main(void)
{
	int ch;

	clk_tck = sysconf(_SC_CLK_TCK);
	if (clk_tck <= 0) clk_tck = 100;

	gui_init();

	if (update_data() < 0) {
		gui_cleanup();
		fprintf(stderr, "Failed to fetch data. "
			"Are the custom syscalls (470-472) installed?\n"
			"Try: sudo ./proc_monitor\n");
		return 1;
	}
	sort_procs();
	save_prev_cpu();   /* 首帧: CPU% = 0 */

	while (g_running) {
		gui_redraw();

		ch = wgetch(win_input);

		if (ch == ERR) {
			save_prev_cpu();  /* 保存当前值供下一帧算增量 */
			update_data();
			sort_procs();
			continue;
		}

		/* ---- 过滤模式: 逐字符处理 ---- */
		if (g_filter_mode) {
			if (ch == '\n' || ch == KEY_ENTER) {
				/* 应用过滤 */
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
			} else if (ch == 27) {
				/* ESC: 取消过滤 */
				g_filter_mode = 0;
				g_cmd_pos = 0;
				g_cmd[0] = '\0';
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

		case '\t':   /* Tab: 循环排序字段 */
			g_sort_field = (g_sort_field + 1) % 4;
			sort_procs();
			g_selected = 0;
			g_scroll = 0;
			break;

		case 's':    /* s: 切换升降序 */
			g_sort_desc = !g_sort_desc;
			sort_procs();
			break;

		case 't':    /* t: 切换列表/树视图 */
			g_view_list = !g_view_list;
			g_selected = 0;
			g_scroll = 0;
			break;

		case 'r':    /* r: 清除过滤 */
			g_filter[0] = '\0';
			g_filter_active = 0;
			g_selected = 0;
			g_scroll = 0;
			break;

		case '/':    /* /: 进入过滤模式 */
			g_filter_mode = 1;
			g_cmd_pos = 0;
			g_cmd[0] = '\0';
			curs_set(1);
			break;

		case 'e':    /* e: 导出进程树 */
			{
				char fname[64] = "proc_tree.dot";
				def_prog_mode();
				endwin();
				printf("Export process tree to [%s]: ",
				       fname);
				fflush(stdout);
				{
					char buf[256];
					if (fgets(buf, sizeof(buf), stdin)
					    && buf[0] != '\n') {
						size_t l = strlen(buf);
						if (l > 0 && buf[l-1] == '\n')
							buf[l-1] = '\0';
						snprintf(fname, sizeof(fname),
							 "%s", buf);
					}
				}
				reset_prog_mode();
				refresh();
				fetch_tree();
				export_tree(fname);
			}
			break;

		case 'h': case '?':
			def_prog_mode();
			endwin();
			printf("\n==== Process Monitor Help ====\n");
			printf("  q/ESC    Quit\n");
			printf("  up/down  Select process\n");
			printf("  Tab      Cycle sort field\n");
			printf("  s        Toggle asc/desc\n");
			printf("  t        Toggle list/tree view\n");
			printf("  /        Enter filter mode\n");
			printf("  r        Clear filter\n");
			printf("  e        Export tree to DOT file\n");
			printf("  h        Show this help\n");
			printf("\nFilter syntax:\n");
			printf("  text     Name substring (case-insens)\n");
			printf("  =R       State (R/S/D/T/Z)\n");
			printf("  :1234    Exact PID\n");
			printf("\nPress Enter to continue...");
			getchar();
			reset_prog_mode();
			refresh();
			break;

		default:
			break;
		}
	}

	gui_cleanup();
	return 0;
}
