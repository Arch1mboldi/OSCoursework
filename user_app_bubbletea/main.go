// procmon — Linux 进程全量监控 TUI
//
// 基于 Bubble Tea 框架，通过自定义系统调用 (470/471/472) 获取数据。
// 功能: 多条件过滤 · 实时刷新(1s) · 排序 · 进程树可视化
//
// 编译 (在 VM 内):
//
//	go mod tidy
//	go build -o procmon .
//	sudo ./procmon
//
// 依赖: github.com/charmbracelet/bubbletea, github.com/charmbracelet/lipgloss

package main

import (
	"fmt"
	"os"
	"sort"
	"strconv"
	"strings"
	"syscall"
	"time"
	"unsafe"

	tea "github.com/charmbracelet/bubbletea"
	"github.com/charmbracelet/lipgloss"
)

// ============================================================================
// Syscall 数据结构 — 必须与内核 include/linux/proc_monitor.h 字节级一致
// ============================================================================

// ProcInfo 对应内核 struct proc_info (sizeof=80, x86_64)
type ProcInfo struct {
	Pid        int32    // offset 0
	Ppid       int32    // offset 4
	Comm       [16]byte // offset 8
	State      int32    // offset 24
	_pad1      [4]byte  // offset 28 → align uint64
	Utime      uint64   // offset 32
	Stime      uint64   // offset 40
	Vsize      uint64   // offset 48
	Rss        uint64   // offset 56
	Nice       int32    // offset 64
	NumThreads int32    // offset 68
	Uid        uint32   // offset 72
	_pad2      [4]byte  // offset 76 → align struct
}

// ProcTreeNode 对应内核 struct proc_tree_node (sizeof=32)
type ProcTreeNode struct {
	Pid   int32    // offset 0
	Ppid  int32    // offset 4
	Comm  [16]byte // offset 8
	Level int32    // offset 24
	_pad  [4]byte  // offset 28
}

// ProcStat 对应内核 struct proc_stat (sizeof=40)
type ProcStat struct {
	Total           int32
	Running         int32
	Sleeping        int32
	Uninterruptible int32
	Stopped         int32
	Zombie          int32
	Idle            int32
	KernelThreads   int32
	UserThreads     int32
	_pad            [4]byte
}

// ============================================================================
// 系统调用层
// ============================================================================

const (
	SYS_PROC_COLLECT  = 470
	SYS_PROC_SNAPSHOT = 471
	SYS_PROC_STAT     = 472
	MAX_PROCS         = 8192
	HZ                = 100 // Linux x86_64 标准时钟滴答频率
)

// cstr 将内核的定长 comm[16] 转为 Go string
func cstr(b [16]byte) string {
	n := 0
	for n < 16 && b[n] != 0 {
		n++
	}
	return string(b[:n])
}

// fetchProcs 调用 sys_proc_collect(470)，返回所有进程信息
func fetchProcs() ([]ProcInfo, error) {
	buf := make([]ProcInfo, MAX_PROCS)
	var count int32
	_, _, errno := syscall.Syscall(
		SYS_PROC_COLLECT,
		uintptr(unsafe.Pointer(&buf[0])),
		uintptr(MAX_PROCS),
		uintptr(unsafe.Pointer(&count)),
	)
	if errno != 0 {
		return nil, fmt.Errorf("proc_collect: %s", errno)
	}
	return buf[:count], nil
}

// fetchTree 调用 sys_proc_snapshot(471)，返回进程树拓扑
func fetchTree() ([]ProcTreeNode, error) {
	buf := make([]ProcTreeNode, MAX_PROCS)
	var count int32
	_, _, errno := syscall.Syscall(
		SYS_PROC_SNAPSHOT,
		uintptr(unsafe.Pointer(&buf[0])),
		uintptr(MAX_PROCS),
		uintptr(unsafe.Pointer(&count)),
	)
	if errno != 0 {
		return nil, fmt.Errorf("proc_snapshot: %s", errno)
	}
	return buf[:count], nil
}

// fetchStat 调用 sys_proc_stat(472)，返回进程统计摘要
func fetchStat() (ProcStat, error) {
	var stat ProcStat
	_, _, errno := syscall.Syscall(
		SYS_PROC_STAT,
		uintptr(unsafe.Pointer(&stat)),
		0, 0,
	)
	if errno != 0 {
		return stat, fmt.Errorf("proc_stat: %s", errno)
	}
	return stat, nil
}

// ============================================================================
// 进程树构建
// ============================================================================

// TreeNode 用于内存中构建和渲染进程树
type TreeNode struct {
	Info     ProcTreeNode
	Children []*TreeNode
}

// buildTree 从扁平 pid/ppid 列表构建树，以 PID=1 为根
func buildTree(nodes []ProcTreeNode) *TreeNode {
	nodeMap := make(map[int32]*TreeNode, len(nodes))
	for _, n := range nodes {
		nodeMap[n.Pid] = &TreeNode{Info: n}
	}

	var root *TreeNode
	for _, n := range nodes {
		node := nodeMap[n.Pid]
		if parent, ok := nodeMap[n.Ppid]; ok && n.Pid != n.Ppid {
			parent.Children = append(parent.Children, node)
		} else if n.Pid == 1 || n.Ppid == 0 {
			root = node
		}
	}

	// 回收孤儿节点，挂到 init 下
	for _, n := range nodes {
		if _, hasParent := nodeMap[n.Ppid]; !hasParent && n.Pid != 1 {
			if initNode, ok := nodeMap[1]; ok {
				initNode.Children = append(initNode.Children, nodeMap[n.Pid])
			}
		}
	}

	// 按 PID 排序子节点，保证输出稳定
	if root != nil {
		sortTree(root)
	}
	return root
}

func sortTree(n *TreeNode) {
	sort.Slice(n.Children, func(i, j int) bool {
		return n.Children[i].Info.Pid < n.Children[j].Info.Pid
	})
	for _, c := range n.Children {
		sortTree(c)
	}
}

// renderLines 递归渲染进程树为字符串行列表
func (n *TreeNode) renderLines() []string {
	var lines []string
	n.render(&lines, "", true)
	return lines
}

func (n *TreeNode) render(lines *[]string, prefix string, isLast bool) {
	connector := "├── "
	if isLast {
		connector = "└── "
	}
	*lines = append(*lines, fmt.Sprintf("%s%s%s(%d)",
		prefix, connector, cstr(n.Info.Comm), n.Info.Pid))

	childPrefix := prefix + "│   "
	if isLast {
		childPrefix = prefix + "    "
	}
	for i, child := range n.Children {
		child.render(lines, childPrefix, i == len(n.Children)-1)
	}
}

// ============================================================================
// Bubble Tea 消息类型
// ============================================================================

// TickMsg 每秒触发一次数据刷新
type TickMsg time.Time

// ============================================================================
// 视图模式 & 排序字段
// ============================================================================

type ViewMode int

const (
	ViewList ViewMode = iota
	ViewTree
	ViewHelp
)

type SortField int

const (
	SortByPID SortField = iota
	SortByCPU
	SortByMem
	SortByName
)

func (s SortField) Label() string {
	return [...]string{"PID", "CPU", "MEM", "NAME"}[s]
}

// ============================================================================
// Model
// ============================================================================

type model struct {
	// 数据
	procs     []ProcInfo
	treeNodes []ProcTreeNode
	stat      ProcStat
	treeRoot  *TreeNode

	// 视图状态
	mode      ViewMode
	sortField SortField
	sortDesc  bool

	// 过滤
	filterText string
	filtering  bool // 正在编辑过滤条件

	// 滚动
	scrollY int

	// 终端尺寸
	width  int
	height int

	// 状态
	ready   bool
	lastErr string
	quit    bool
}

func initialModel() model {
	return model{
		mode:      ViewList,
		sortField: SortByPID,
		sortDesc:  false,
	}
}

// ============================================================================
// 数据获取
// ============================================================================

func (m *model) fetchAll() {
	procs, err := fetchProcs()
	if err != nil {
		m.lastErr = fmt.Sprintf("fetch: %v", err)
		return
	}
	m.procs = procs
	m.sortProcs()

	treeNodes, err := fetchTree()
	if err != nil {
		m.lastErr = fmt.Sprintf("tree: %v", err)
		return
	}
	m.treeNodes = treeNodes
	m.treeRoot = buildTree(treeNodes)

	stat, err := fetchStat()
	if err != nil {
		m.lastErr = fmt.Sprintf("stat: %v", err)
		return
	}
	m.stat = stat

	m.lastErr = ""
	m.ready = true
}

// ============================================================================
// 排序
// ============================================================================

func (m *model) sortProcs() {
	if m.sortField == SortByPID {
		sort.Slice(m.procs, func(i, j int) bool {
			if m.sortDesc {
				return m.procs[i].Pid > m.procs[j].Pid
			}
			return m.procs[i].Pid < m.procs[j].Pid
		})
		return
	}
	if m.sortField == SortByName {
		sort.Slice(m.procs, func(i, j int) bool {
			ni, nj := cstr(m.procs[i].Comm), cstr(m.procs[j].Comm)
			if m.sortDesc {
				return ni > nj
			}
			return ni < nj
		})
		return
	}

	// CPU 或 MEM 排序（数值降序更直观 → 默认降序）
	desc := m.sortDesc
	if m.sortField == SortByCPU {
		sort.Slice(m.procs, func(i, j int) bool {
			ci := m.procs[i].Utime + m.procs[i].Stime
			cj := m.procs[j].Utime + m.procs[j].Stime
			if desc {
				return ci > cj
			}
			return ci < cj
		})
	} else { // SortByMem
		sort.Slice(m.procs, func(i, j int) bool {
			if desc {
				return m.procs[i].Rss > m.procs[j].Rss
			}
			return m.procs[i].Rss < m.procs[j].Rss
		})
	}
}

// ============================================================================
// 过滤
// ============================================================================

// filteredProcs 返回符合当前过滤条件的进程列表
func (m *model) filteredProcs() []ProcInfo {
	if m.filterText == "" {
		return m.procs
	}
	text := strings.ToLower(m.filterText)

	var result []ProcInfo
	for _, p := range m.procs {
		if m.matchFilter(p, text) {
			result = append(result, p)
		}
	}
	return result
}

// matchFilter 检查单个进程是否匹配过滤条件
// 支持语法:
//
//	"systemd"     — 进程名子串匹配 (忽略大小写)
//	"=R"          — 按状态过滤 (R/S/D/T/Z/I)
//	":1234"       — 按 PID 精确匹配
func (m *model) matchFilter(p ProcInfo, text string) bool {
	switch {
	case strings.HasPrefix(text, "="):
		// 状态过滤
		stateChar := strings.ToUpper(strings.TrimPrefix(text, "="))
		for _, r := range stateChar {
			if len(r) == 0 {
				continue
			}
			got := stateByte(p.State)
			if got == byte(r) {
				return true
			}
		}
		return false

	case strings.HasPrefix(text, ":"):
		// PID 过滤
		pidStr := strings.TrimPrefix(text, ":")
		pid, err := strconv.Atoi(pidStr)
		if err != nil {
			return false
		}
		return int(p.Pid) == pid

	default:
		// 进程名子串匹配
		return strings.Contains(strings.ToLower(cstr(p.Comm)), text)
	}
}

// stateByte 将内核状态码转为可显示字符
func stateByte(state int32) byte {
	switch state {
	case 0:
		return 'R'
	case 1:
		return 'S'
	case 2:
		return 'D'
	case 4:
		return 'T'
	case 8:
		return 't' // traced
	case 16:
		return 'Z'
	case 32:
		return 'X'
	case 1026:
		return 'I'
	default:
		return '?'
	}
}

// cpuSec 计算进程 CPU 时间（秒）
func cpuSec(p ProcInfo) float64 {
	return float64(p.Utime+p.Stime) / HZ
}

// ============================================================================
// Bubble Tea Init
// ============================================================================

// InitDataMsg 在程序启动时触发首次数据加载
type InitDataMsg struct{}

// Init 返回启动命令：进入 AltScreen + 首次数据加载
func (m model) Init() tea.Cmd {
	return tea.Batch(
		tea.EnterAltScreen,
		tea.SetWindowTitle("Process Monitor"),
		func() tea.Msg { return InitDataMsg{} },
	)
}

func tickCmd() tea.Cmd {
	return tea.Tick(time.Second, func(t time.Time) tea.Msg {
		return TickMsg(t)
	})
}

// ============================================================================
// Bubble Tea Update
// ============================================================================

func (m model) Update(msg tea.Msg) (tea.Model, tea.Cmd) {
	switch msg := msg.(type) {

	case InitDataMsg:
		m.fetchAll()
		return m, tickCmd()

	case TickMsg:
		m.fetchAll()
		return m, tickCmd()

	case tea.WindowSizeMsg:
		m.width = msg.Width
		m.height = msg.Height
		return m, nil

	case tea.KeyMsg:
		return m.handleKey(msg)

	case tea.QuitMsg:
		m.quit = true
		return m, tea.Quit
	}

	return m, nil
}

func (m model) handleKey(msg tea.KeyMsg) (tea.Model, tea.Cmd) {
	key := msg.String()

	// ── 过滤模式 ──
	if m.filtering {
		switch key {
		case "enter":
			m.filtering = false
		case "esc":
			m.filterText = ""
			m.filtering = false
		case "backspace":
			if len(m.filterText) > 0 {
				m.filterText = m.filterText[:len(m.filterText)-1]
			}
		default:
			if len(msg.Runes) == 1 && len(m.filterText) < 40 {
				m.filterText += string(msg.Runes[0])
			}
		}
		m.scrollY = 0
		return m, nil
	}

	// ── 全局快捷键 ──
	switch key {
	case "q", "ctrl+c":
		return m, tea.Quit

	case "t":
		// 切换列表 / 树
		m.scrollY = 0
		if m.mode == ViewTree {
			m.mode = ViewList
		} else {
			m.mode = ViewTree
		}

	case "?":
		// 切换帮助
		if m.mode == ViewHelp {
			m.mode = ViewList
		} else {
			m.mode = ViewHelp
		}

	case "/":
		// 进入过滤编辑模式
		m.filtering = true

	case "esc":
		// 清除过滤
		m.filterText = ""
		m.scrollY = 0

	case "s":
		// 循环排序字段
		m.sortField = (m.sortField + 1) % 4
		m.sortProcs()
		m.scrollY = 0

	case "S":
		// 切换升降序
		m.sortDesc = !m.sortDesc
		m.sortProcs()
		m.scrollY = 0

	case "j", "down":
		m.scrollY++

	case "k", "up":
		if m.scrollY > 0 {
			m.scrollY--
		}

	case "pgdown", "ctrl+d":
		m.scrollY += (m.height - 6)
		if m.scrollY < 0 {
			m.scrollY = 0
		}

	case "pgup", "ctrl+u":
		m.scrollY -= (m.height - 6)
		if m.scrollY < 0 {
			m.scrollY = 0
		}

	case "home", "g":
		m.scrollY = 0

	case "end", "G":
		m.scrollY = 1 << 30 // 一个很大的数，view 层会 clamp
	}

	return m, nil
}

// ============================================================================
// Bubble Tea View
// ============================================================================

func (m model) View() string {
	if m.quit {
		return ""
	}
	if m.width == 0 {
		return "Initializing... (resize terminal if stuck)\n"
	}

	var b strings.Builder

	// 顶部统计栏
	b.WriteString(m.renderStatsBar())
	b.WriteString("\n")

	// 分隔线
	b.WriteString(m.hLine())
	b.WriteString("\n")

	// 主内容区
	switch m.mode {
	case ViewList:
		b.WriteString(m.renderList())
	case ViewTree:
		b.WriteString(m.renderTreeView())
	case ViewHelp:
		b.WriteString(m.renderHelp())
	}

	// 底部状态栏
	b.WriteString("\n")
	b.WriteString(m.hLine())
	b.WriteString("\n")
	b.WriteString(m.renderStatusBar())

	if m.lastErr != "" {
		b.WriteString("\n")
		b.WriteString(errStyle.Render(" ⚠ " + m.lastErr))
	}

	return b.String()
}

// ============================================================================
// 渲染组件
// ============================================================================

func (m *model) hLine() string {
	if m.width < 1 {
		return ""
	}
	return strings.Repeat("─", m.width)
}

// renderStatsBar 顶部统计行
func (m *model) renderStatsBar() string {
	if !m.ready {
		return dimStyle.Render(" Loading...")
	}
	s := m.stat
	parts := []string{
		titleStyle.Render(" Process Monitor "),
		fmt.Sprintf("Total:%d", s.Total),
		stateStyle("R", stateRStyle).Render(fmt.Sprintf("R:%d", s.Running)),
		stateStyle("S", dimStyle).Render(fmt.Sprintf("S:%d", s.Sleeping)),
		stateStyle("D", warnStyle).Render(fmt.Sprintf("D:%d", s.Uninterruptible)),
		stateStyle("Z", errStyle).Render(fmt.Sprintf("Z:%d", s.Zombie)),
		stateStyle("T", dimStyle).Render(fmt.Sprintf("T:%d", s.Stopped)),
		fmt.Sprintf("KThr:%d", s.KernelThreads),
		fmt.Sprintf("UThr:%d", s.UserThreads),
	}
	return strings.Join(parts, "  ")
}

// renderList 进程列表视图
func (m *model) renderList() string {
	procs := m.filteredProcs()

	// 可用高度
	avail := m.height - 4 // 统计栏 + 两条分隔线 + 状态栏
	if avail < 2 {
		return ""
	}

	// 列头
	header := fmt.Sprintf("%-7s %-7s %-18s %c %-4s %-8s %-9s %-9s %-10s %s",
		"PID", "PPID", "NAME", 'S', "NICE", "THR",
		"VSIZE", "RSS", "CPU(s)", "UID")

	var b strings.Builder

	// 过滤提示
	if m.filterText != "" {
		b.WriteString(filterStyle.Render(fmt.Sprintf(
			" Filter: \"%s\" (%d matched)", m.filterText, len(procs))))
	} else {
		b.WriteString(dimStyle.Render(fmt.Sprintf(
			" Sort: %s %s", m.sortField.Label(), arrow(m.sortDesc))))
	}
	b.WriteString("\n")
	b.WriteString(tableHeaderStyle.Render(header))
	b.WriteString("\n")

	// Clamp 滚动
	maxScroll := len(procs) - avail + 1
	if maxScroll < 0 {
		maxScroll = 0
	}
	if m.scrollY > maxScroll {
		m.scrollY = maxScroll
	}
	if m.scrollY < 0 {
		m.scrollY = 0
	}

	start := m.scrollY
	end := start + avail
	if end > len(procs) {
		end = len(procs)
	}

	for i := start; i < end; i++ {
		p := procs[i]
		state := stateByte(p.State)

		line := fmt.Sprintf("%-7d %-7d %-18.18s %c %-4d %-8d %-9s %-9s %-10.1f %d",
			p.Pid, p.Ppid, cstr(p.Comm), state,
			p.Nice, p.NumThreads,
			formatSize(p.Vsize), formatSize(p.Rss*4096),
			cpuSec(p), p.Uid)

		// 按状态着色
		switch state {
		case 'R':
			line = stateRStyle.Render(line)
		case 'Z':
			line = errStyle.Render(line)
		case 'D':
			line = warnStyle.Render(line)
		default:
			line = dimStyle.Render(line)
		}
		b.WriteString(line)
		b.WriteString("\n")
	}

	return b.String()
}

// renderTreeView 进程树视图
func (m *model) renderTreeView() string {
	if m.treeRoot == nil {
		return " No process tree data (is the kernel module loaded?)"
	}

	lines := m.treeRoot.renderLines()
	avail := m.height - 4
	if avail < 2 {
		return ""
	}

	maxScroll := len(lines) - avail
	if maxScroll < 0 {
		maxScroll = 0
	}
	if m.scrollY > maxScroll {
		m.scrollY = maxScroll
	}
	if m.scrollY < 0 {
		m.scrollY = 0
	}

	start := m.scrollY
	end := start + avail
	if end > len(lines) {
		end = len(lines)
	}

	var b strings.Builder
	b.WriteString(treeTitleStyle.Render(fmt.Sprintf(" Process Tree (%d nodes)", len(m.treeNodes))))
	b.WriteString("\n")

	for i := start; i < end; i++ {
		line := lines[i]
		// 树连接线用青色
		b.WriteString(treeStyle.Render(line))
		b.WriteString("\n")
	}

	return b.String()
}

// renderHelp 帮助视图
func (m *model) renderHelp() string {
	lines := []string{
		"",
		helpTitleStyle.Render("  Key Bindings"),
		"",
		"  " + keyStyle.Render("q / Ctrl+C") + "    Quit",
		"  " + keyStyle.Render("t") + "             Toggle process list / tree view",
		"  " + keyStyle.Render("s") + "             Cycle sort: PID → CPU → MEM → NAME",
		"  " + keyStyle.Render("S") + "             Toggle sort direction (asc / desc)",
		"  " + keyStyle.Render("/") + "             Enter filter mode",
		"  " + keyStyle.Render("Esc") + "           Clear filter",
		"  " + keyStyle.Render("↑↓ / j k") + "     Scroll up / down",
		"  " + keyStyle.Render("PgUp / PgDn") + "   Page up / down",
		"  " + keyStyle.Render("g / Home") + "     Jump to top",
		"  " + keyStyle.Render("G / End") + "      Jump to bottom",
		"  " + keyStyle.Render("?") + "             Toggle this help",
		"",
		helpTitleStyle.Render("  Filter Syntax"),
		"",
		"  " + keyStyle.Render("systemd") + "      Match process name (case-insensitive substring)",
		"  " + keyStyle.Render("=R") + "           Match by state (R/S/D/T/Z/I)",
		"  " + keyStyle.Render(":1234") + "        Match by exact PID",
		"",
		helpTitleStyle.Render("  Data Source"),
		"",
		"  All data comes from 3 custom Linux syscalls (470/471/472):",
		"    sys_proc_collect  — process info",
		"    sys_proc_snapshot — process tree topology",
		"    sys_proc_stat     — aggregate statistics",
		"  No /proc, ps, eBPF, or ptrace is used.",
	}

	return strings.Join(lines, "\n")
}

// renderStatusBar 底部操作提示
func (m *model) renderStatusBar() string {
	if m.filtering {
		prompt := fmt.Sprintf(" Filter: %s█", m.filterText)
		ret := filterStyle.Render(prompt)
		hint := dimStyle.Render("  [Enter: apply  Esc: cancel]")
		return ret + hint
	}

	scrollInfo := fmt.Sprintf("%d/%d", m.scrollY,
		func() int {
			if m.mode == ViewTree && m.treeRoot != nil {
				return len(m.treeRoot.renderLines())
			}
			return len(m.filteredProcs())
		}())

	var parts []string
	parts = append(parts, dimStyle.Render(scrollInfo))
	parts = append(parts, keyHint("q", "quit"))
	parts = append(parts, keyHint("/", "filter"))
	parts = append(parts, keyHint("s", "sort"))
	if m.mode == ViewTree {
		parts = append(parts, keyHint("t", "list"))
	} else {
		parts = append(parts, keyHint("t", "tree"))
	}
	parts = append(parts, keyHint("?", "help"))

	// 右对齐填充
	result := strings.Join(parts, "  ")
	if m.width > len(result) {
		result += strings.Repeat(" ", m.width-len(result))
	}
	return result
}

// ============================================================================
// 样式定义
// ============================================================================

var (
	titleStyle       = lipgloss.NewStyle().Bold(true).Foreground(lipgloss.Color("15")).Background(lipgloss.Color("4")).Padding(0, 1)
	tableHeaderStyle = lipgloss.NewStyle().Bold(true).Foreground(lipgloss.Color("15")).Background(lipgloss.Color("8"))
	helpTitleStyle   = lipgloss.NewStyle().Bold(true).Foreground(lipgloss.Color("6"))
	treeTitleStyle   = lipgloss.NewStyle().Bold(true).Foreground(lipgloss.Color("6"))
	treeStyle        = lipgloss.NewStyle().Foreground(lipgloss.Color("6"))  // cyan connectors
	keyStyle         = lipgloss.NewStyle().Foreground(lipgloss.Color("3")).Bold(true) // yellow keys
	filterStyle      = lipgloss.NewStyle().Foreground(lipgloss.Color("3")) // yellow filter
	dimStyle         = lipgloss.NewStyle().Foreground(lipgloss.Color("8")) // gray
	stateRStyle      = lipgloss.NewStyle().Foreground(lipgloss.Color("2")) // green
	warnStyle        = lipgloss.NewStyle().Foreground(lipgloss.Color("3")) // yellow
	errStyle         = lipgloss.NewStyle().Foreground(lipgloss.Color("1")) // red
)

func stateStyle(name string, style lipgloss.Style) lipgloss.Style {
	return style.Copy().Bold(true)
}

func keyHint(key, desc string) string {
	return keyStyle.Render(key) + " " + dimStyle.Render(desc)
}

func arrow(desc bool) string {
	if desc {
		return "▼"
	}
	return "▲"
}

// ============================================================================
// 格式化工具
// ============================================================================

// formatSize 格式化字节数为可读字符串
func formatSize(bytes uint64) string {
	switch {
	case bytes >= 1<<30:
		return fmt.Sprintf("%.1fG", float64(bytes)/(1<<30))
	case bytes >= 1<<20:
		return fmt.Sprintf("%.1fM", float64(bytes)/(1<<20))
	case bytes >= 1<<10:
		return fmt.Sprintf("%.1fK", float64(bytes)/(1<<10))
	default:
		return fmt.Sprintf("%dB", bytes)
	}
}

// ============================================================================
// 入口
// ============================================================================

func main() {
	// 验证结构体大小与内核一致（编译时安全网）
	if unsafe.Sizeof(ProcInfo{}) != 80 {
		fmt.Fprintf(os.Stderr, "FATAL: ProcInfo size=%d, expected 80\n", unsafe.Sizeof(ProcInfo{}))
		os.Exit(1)
	}
	if unsafe.Sizeof(ProcTreeNode{}) != 32 {
		fmt.Fprintf(os.Stderr, "FATAL: ProcTreeNode size=%d, expected 32\n", unsafe.Sizeof(ProcTreeNode{}))
		os.Exit(1)
	}
	if unsafe.Sizeof(ProcStat{}) != 40 {
		fmt.Fprintf(os.Stderr, "FATAL: ProcStat size=%d, expected 40\n", unsafe.Sizeof(ProcStat{}))
		os.Exit(1)
	}

	m := initialModel()
	m.fetchAll()

	p := tea.NewProgram(m, tea.WithAltScreen())
	if _, err := p.Run(); err != nil {
		fmt.Fprintf(os.Stderr, "Error: %v\n", err)
		os.Exit(1)
	}
}
