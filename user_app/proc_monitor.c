/*
 * proc_monitor.c — Linux 进程全量监控 TUI (ncurses 版)
 *
 * 通过自定义系统调用 (470/471/472) 获取数据。
 * 模仿 osfs-system/gui.c 的多面板 ncurses 布局风格。
 *
 * 面板布局:
 *   win_main   — 进程列表 / 进程树 (主体)
 *   win_info   — 选中进程详情 / 统计摘要
 *   win_status — 状态栏 (总数/排序/过滤)
 *   win_input  — 命令输入栏
 *
 * 操作:
 *   方向键/↑↓  选择进程
 *   Enter     查看详情 / 执行命令
 *   Tab       切换排序字段
 *   t         切换列表/树视图
 *   /         输入过滤条件
 *   r         清除过滤
 *   s         切换升降序
 *   h         帮助
 *   q/ESC     退出
 *
 * 命令:
 *   sort pid|cpu|mem|name
 *   filter <text>  |  filter =R  |  filter :1234
 *   filter off
 *   tree            — 显示进程树
 *   list            — 显示进程列表
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
#include <signal.h>
#include <ncurses.h>
#include "proc_monitor.h"

/* ===================================================================
 * 常量
 * =================================================================== */

#define MAX_PROCS     8192
#define MAX_CMD_LEN   256
#define HZ            100     /* Linux x86_64 标准 ticks/sec */

/* ===================================================================
 * ncurses 窗口
 * =================================================================== */

static WINDOW *win_main, *win_info, *win_status, *win_input;

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

/* 命令缓冲区 */
static char g_cmd[MAX_CMD_LEN];
static int  g_cmd_pos = 0;

/* 上次错误 */
static char g_last_err[256] = "";

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

static double cpu_sec(const struct proc_info *p)
{
	return (double)(p->utime + p->stime) / HZ;
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
		snprintf(g_last_err, sizeof(g_last_err),
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
		snprintf(g_last_err, sizeof(g_last_err),
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
		snprintf(g_last_err, sizeof(g_last_err),
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
	g_last_err[0] = '\0';
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
		/* reverse in-place */
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

	/* =R, =Z, =S, ... : 按状态过滤 */
	if (text[0] == '=' && text[1]) {
		char want = (char)(text[1] >= 'a' ? text[1] - 32 : text[1]);
		return state_char(p->state) == want;
	}

	/* :1234 : 按 PID 过滤 */
	if (text[0] == ':') {
		int pid = atoi(text + 1);
		return p->pid == pid;
	}

	/* 默认: 进程名子串匹配 */
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

/* 判断 nodes[i] 是否为其父节点的最后一个孩子 */
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

/* 判断从 nodes[i] 回溯到 level 层的祖先是否还有后续兄弟 */
static int tree_has_continuation(int i, int count, int target_level)
{
	/* 找到 i 的位置处处于 target_level 的祖先 */
	pid_t ancestor_pid = g_tree_nodes[i].pid;
	int cur_level = g_tree_nodes[i].level;
	int j;

	/* 回溯找到 target_level 的祖先 */
	while (cur_level > target_level) {
		pid_t target_ppid = -1;
		/* 找 ancestor_pid 在 nodes 中的位置 */
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

	/* 现在 ancestor_pid 处于 target_level, 检查其后是否有兄弟 */
	for (j = i + 1; j < count; j++) {
		/* 找 nodes[j] 的 target_level 祖先 */
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
 * ncurses GUI
 * =================================================================== */

static void gui_init(void)
{
	int rows, cols;

	initscr();
	cbreak();
	noecho();
	curs_set(1);
	keypad(stdscr, TRUE);

	if (has_colors()) {
		start_color();
		init_pair(1, COLOR_GREEN,  COLOR_BLACK);  /* R state */
		init_pair(2, COLOR_RED,    COLOR_BLACK);  /* Z state */
		init_pair(3, COLOR_YELLOW, COLOR_BLACK);  /* D state */
		init_pair(4, COLOR_CYAN,   COLOR_BLACK);  /* title */
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

	/* 1 秒超时 → 自动刷新数据 */
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
	int shown, total;
	const char *fields[] = { "PID", "CPU", "MEM", "NAME" };

	getmaxyx(win_main, max_rows, cols);
	(void)cols;

	/* ---- win_main: 进程列表 / 树 ---- */
	wclear(win_main);
	box(win_main, 0, 0);

	if (g_view_list) {
		/* 表头 */
		wattron(win_main, COLOR_PAIR(5));
		mvwprintw(win_main, 0, 2, " ProcMon ");
		wattroff(win_main, COLOR_PAIR(5));
		mvwprintw(win_main, 0, 12, "| Sort:%s %s | %s",
			  (const char *[]){"PID","CPU","MEM","NAME"}[g_sort_field],
			  g_sort_desc ? "desc" : "asc",
			  g_filter_active ? "FILTER=ON" : "");

		mvwprintw(win_main, 2, 2,
			  "%-7s %-7s %-18s %c %-4s %-6s %-8s %-8s %-8s %s",
			  "PID", "PPID", "NAME", 'S', "NICE", "THR",
			  "VSIZE", "RSS", "CPU(s)", "UID");

		/* 分隔线 */
		mvwhline(win_main, 3, 1, ACS_HLINE, cols - 2);

		/* 数据行 */
		shown = 0; total = 0;
		for (i = 0; i < g_proc_count; i++) {
			if (!match_filter(&g_procs[i]))
				continue;
			total++;
		}

		if (g_selected >= total) g_selected = total - 1;
		if (g_selected < 0) g_selected = 0;

		/* 滚动 clamp */
		if (g_selected < g_scroll)
			g_scroll = g_selected;
		if (g_selected >= g_scroll + (max_rows - 5))
			g_scroll = g_selected - (max_rows - 5) + 1;
		if (g_scroll < 0) g_scroll = 0;

		shown = 0;
		for (i = 0, row = 4; i < g_proc_count && row < max_rows - 1; i++) {
			const struct proc_info *p = &g_procs[i];
			char st;
			int color_pair;

			if (!match_filter(p))
				continue;
			if (shown < g_scroll) {
				shown++;
				continue;
			}

			st = state_char(p->state);

			/* 高亮选中行 */
			if (shown == g_selected)
				wattron(win_main, A_REVERSE);

			/* 按状态着色 */
			switch (st) {
			case 'R': color_pair = 1; break;
			case 'Z': color_pair = 2; break;
			case 'D': color_pair = 3; break;
			default:  color_pair = 0; break;
			}
			if (color_pair && shown != g_selected)
				wattron(win_main, COLOR_PAIR(color_pair));

			mvwprintw(win_main, row, 2,
				  "%-7d %-7d %-18.18s %c %-4d %-6d "
				  "%-8s %-8s %-8.1f %d",
				  p->pid, p->ppid, cstr(p->comm), st,
				  p->nice, p->num_threads,
				  fmt_size(p->vsize),
				  fmt_size(p->rss * 4096),
				  cpu_sec(p), p->uid);

			if (color_pair && shown != g_selected)
				wattroff(win_main, COLOR_PAIR(color_pair));
			if (shown == g_selected)
				wattroff(win_main, A_REVERSE);

			shown++;
			row++;
		}

	} else {
		/* 进程树视图 */
		wattron(win_main, COLOR_PAIR(5));
		mvwprintw(win_main, 0, 2, " Process Tree ");
		wattroff(win_main, COLOR_PAIR(5));
		mvwprintw(win_main, 0, 18, "| %d nodes", g_tree_count);

		mvwhline(win_main, 1, 1, ACS_HLINE, cols - 2);

		if (g_selected >= g_tree_count) g_selected = g_tree_count - 1;
		if (g_selected < 0) g_selected = 0;

		if (g_selected < g_scroll)
			g_scroll = g_selected;
		if (g_selected >= g_scroll + (max_rows - 3))
			g_scroll = g_selected - (max_rows - 3) + 1;
		if (g_scroll < 0) g_scroll = 0;

		shown = 0;
		for (i = 0, row = 2; i < g_tree_count && row < max_rows - 1; i++) {
			int level = g_tree_nodes[i].level;
			int last  = tree_is_last(i, g_tree_count);
			char prefix[128] = "";
			int lev;

			if (shown < g_scroll) { shown++; continue; }

			/* 构建缩进前缀 */
			for (lev = 0; lev < level; lev++) {
				if (tree_has_continuation(i, g_tree_count, lev))
					strcat(prefix, "│   ");
				else
					strcat(prefix, "    ");
			}
			strcat(prefix, last ? "└── " : "├── ");

			if (shown == g_selected)
				wattron(win_main, A_REVERSE);
			wattron(win_main, COLOR_PAIR(4));
			mvwprintw(win_main, row, 2, "%s%s(%d)",
				  prefix, cstr(g_tree_nodes[i].comm),
				  g_tree_nodes[i].pid);
			wattroff(win_main, COLOR_PAIR(4));
			if (shown == g_selected)
				wattroff(win_main, A_REVERSE);

			shown++;
			row++;
		}
	}

	wrefresh(win_main);

	/* ---- win_info: 选中项详情 / 统计 ---- */
	wclear(win_info);
	box(win_info, 0, 0);
	wattron(win_info, COLOR_PAIR(4));
	mvwprintw(win_info, 0, 2, "Info");
	wattroff(win_info, COLOR_PAIR(4));

	if (g_view_list) {
		int sel_idx = -1, cnt = 0;
		for (i = 0; i < g_proc_count; i++) {
			if (!match_filter(&g_procs[i])) continue;
			if (cnt == g_selected) { sel_idx = i; break; }
			cnt++;
		}
		if (sel_idx >= 0) {
			const struct proc_info *p = &g_procs[sel_idx];
			mvwprintw(win_info, 0, 8,
				  "PID=%-6d PPID=%-6d STATE=%s(%c)  "
				  "NICE=%-4d THREADS=%-4d UID=%d",
				  p->pid, p->ppid,
				  state_name(p->state),
				  state_char(p->state),
				  p->nice, p->num_threads, p->uid);
			mvwprintw(win_info, 1, 2,
				  "VSIZE=%s  RSS=%s  CPU=%.1fs  "
				  "utime=%lu  stime=%lu",
				  fmt_size(p->vsize),
				  fmt_size(p->rss * 4096),
				  cpu_sec(p), p->utime, p->stime);
		}
	} else {
		if (g_selected < g_tree_count) {
			mvwprintw(win_info, 0, 8,
				  "PID=%-6d PPID=%-6d LEVEL=%d  NAME=%s",
				  g_tree_nodes[g_selected].pid,
				  g_tree_nodes[g_selected].ppid,
				  g_tree_nodes[g_selected].level,
				  cstr(g_tree_nodes[g_selected].comm));
		}
	}

	wrefresh(win_info);

	/* ---- win_status: 状态栏 ---- */
	wclear(win_status);
	wattron(win_status, COLOR_PAIR(5));
	mvwprintw(win_status, 0, 0,
		  " T:%d R:%d S:%d D:%d Z:%d T:%d | Kthr:%d Uthr:%d | "
		  "%d/%d | %s: %s %s ",
		  g_stat.total_processes,
		  g_stat.running_processes,
		  g_stat.sleeping_processes,
		  g_stat.uninterruptible,
		  g_stat.zombie_processes,
		  g_stat.stopped_processes,
		  g_stat.kernel_threads,
		  g_stat.user_threads,
		  g_selected,
		  g_view_list ? g_proc_count : g_tree_count,
		  g_view_list ? "LIST" : "TREE",
		  fields[g_sort_field],
		  g_sort_desc ? "▼" : "▲");
	wattroff(win_status, COLOR_PAIR(5));

	if (g_last_err[0] && g_last_err[0] != 'O') {
		wattron(win_status, COLOR_PAIR(2));
		mvwprintw(win_status, 0, cols - 40, " %s ", g_last_err);
		wattroff(win_status, COLOR_PAIR(2));
	}
	wrefresh(win_status);

	/* ---- win_input ---- */
	wclear(win_input);
	mvwprintw(win_input, 0, 0, "> %s", g_cmd);
	wrefresh(win_input);
}

/* ===================================================================
 * 命令处理
 * =================================================================== */

static void exec_command(const char *cmd)
{
	char copy[MAX_CMD_LEN];
	char *args[8];
	int argc = 0;
	char *token, *save;

	strncpy(copy, cmd, MAX_CMD_LEN - 1);
	copy[MAX_CMD_LEN - 1] = '\0';

	token = strtok_r(copy, " \t", &save);
	while (token && argc < 8) {
		args[argc++] = token;
		token = strtok_r(NULL, " \t", &save);
	}

	if (argc == 0) return;

	if (strcmp(args[0], "sort") == 0 && argc >= 2) {
		if (strcmp(args[1], "pid") == 0)  g_sort_field = 0;
		else if (strcmp(args[1], "cpu") == 0) g_sort_field = 1;
		else if (strcmp(args[1], "mem") == 0) g_sort_field = 2;
		else if (strcmp(args[1], "name") == 0) g_sort_field = 3;
		sort_procs();
		g_selected = 0;
		g_scroll = 0;
	} else if (strcmp(args[0], "filter") == 0) {
		if (argc < 2 || strcmp(args[1], "off") == 0) {
			g_filter[0] = '\0';
			g_filter_active = 0;
		} else {
			/* 拼接空格分隔的过滤参数 */
			int pos = 0, k;
			for (k = 1; k < argc && pos < (int)sizeof(g_filter) - 1; k++) {
				if (k > 1) g_filter[pos++] = ' ';
				strncpy(g_filter + pos, args[k],
					sizeof(g_filter) - pos - 1);
				pos += (int)strlen(args[k]);
			}
			g_filter[pos] = '\0';
			g_filter_active = 1;
		}
		g_selected = 0;
		g_scroll = 0;
	} else if (strcmp(args[0], "tree") == 0) {
		g_view_list = 0;
		g_selected = 0;
		g_scroll = 0;
	} else if (strcmp(args[0], "list") == 0) {
		g_view_list = 1;
		g_selected = 0;
		g_scroll = 0;
	} else if (strcmp(args[0], "help") == 0 || strcmp(args[0], "h") == 0) {
		def_prog_mode();
		endwin();
		printf("\n======== Process Monitor Help ========\n");
		printf("  sort pid|cpu|mem|name   Set sort field\n");
		printf("  filter <text>           Filter by name substring\n");
		printf("  filter =R               Filter by state (R/S/D/T/Z)\n");
		printf("  filter :1234            Filter by PID\n");
		printf("  filter off              Clear filter\n");
		printf("  tree / list             Switch view mode\n");
		printf("  help                    Show this help\n");
		printf("  q / quit / ESC          Exit\n");
		printf("\nKeys: ↑↓ select | Tab change sort | "
		       "s toggle asc/desc\n");
		printf("      t toggle tree/list | r clear filter\n");
		printf("=======================================\n");
		printf("Press Enter to continue...");
		getchar();
		reset_prog_mode();
		refresh();
	} else if (strcmp(args[0], "q") == 0 ||
		   strcmp(args[0], "quit") == 0) {
		g_running = 0;
	} else {
		snprintf(g_last_err, sizeof(g_last_err),
			 "Unknown: %s (try 'help')", args[0]);
	}
}

/* ===================================================================
 * 主循环
 * =================================================================== */

int main(void)
{
	int ch;

	gui_init();

	/* 首次加载数据 */
	if (update_data() < 0) {
		gui_cleanup();
		fprintf(stderr, "Failed to fetch data. "
			"Are the custom syscalls (470-472) installed?\n"
			"Try: sudo ./proc_monitor\n");
		return 1;
	}
	sort_procs();

	while (g_running) {
		gui_redraw();

		ch = wgetch(win_input);

		if (ch == ERR) {
			/* timeout → 刷新数据 */
			update_data();
			sort_procs();
			continue;
		}

		if (ch == KEY_UP || ch == 'k') {
			if (g_selected > 0) g_selected--;
		} else if (ch == KEY_DOWN || ch == 'j') {
			g_selected++;
		} else if (ch == KEY_PPAGE) {
			g_selected -= 20;
			if (g_selected < 0) g_selected = 0;
		} else if (ch == KEY_NPAGE) {
			g_selected += 20;
		} else if (ch == '\t') {
			/* Tab: 切换排序字段 */
			g_sort_field = (g_sort_field + 1) % 4;
			sort_procs();
			g_selected = 0;
			g_scroll = 0;
		} else if (ch == 's') {
			/* s: 切换升降序 */
			g_sort_desc = !g_sort_desc;
			sort_procs();
		} else if (ch == 't') {
			/* t: 切换列表/树 */
			g_view_list = !g_view_list;
			g_selected = 0;
			g_scroll = 0;
		} else if (ch == 'r') {
			/* r: 清除过滤 */
			g_filter[0] = '\0';
			g_filter_active = 0;
			g_selected = 0;
			g_scroll = 0;
		} else if (ch == '/') {
			/* /: 输入过滤文本 */
			g_cmd_pos = 0;
			g_cmd[0] = '\0';
			mvwprintw(win_input, 0, 0, "filter ");
			wrefresh(win_input);
			echo();
			curs_set(1);
			wgetnstr(win_input, g_cmd, MAX_CMD_LEN - 1);
			noecho();
			curs_set(0);
			if (g_cmd[0]) {
				char full_cmd[MAX_CMD_LEN + 10];
				snprintf(full_cmd, sizeof(full_cmd),
					 "filter %s", g_cmd);
				exec_command(full_cmd);
			}
			g_cmd[0] = '\0';
			g_cmd_pos = 0;
		} else if (ch == '\n' || ch == KEY_ENTER) {
			if (g_cmd_pos > 0) {
				g_cmd[g_cmd_pos] = '\0';
				exec_command(g_cmd);
				g_cmd_pos = 0;
				g_cmd[0] = '\0';
			}
		} else if (ch == 27 || ch == 'q') {
			/* ESC / q → 退出 */
			g_running = 0;
		} else if (ch == KEY_BACKSPACE || ch == 127) {
			if (g_cmd_pos > 0)
				g_cmd[--g_cmd_pos] = '\0';
		} else if (ch >= 32 && ch <= 126 &&
			   g_cmd_pos < MAX_CMD_LEN - 1) {
			g_cmd[g_cmd_pos++] = (char)ch;
		}
	}

	gui_cleanup();
	return 0;
}
