// procmon — Linux 进程全量监控 TUI (纯标准库, 零外部依赖)
//
// 通过自定义系统调用 (470/471/472) 获取数据。
// 使用原始终端模式 + ANSI 转义序列实现 TUI。
//
// 编译:
//   go build -o procmon .
//   sudo ./procmon
//
// 功能: 多条件过滤 · 实时刷新(1s) · 排序 · 进程树可视化 · 纯键盘操作

package main

import (
	"fmt"
	"os"
	"os/signal"
	"sort"
	"strconv"
	"strings"
	"syscall"
	"time"
	"unsafe"
)

// ============================================================================
// Syscall 数据结构 — 必须与内核 include/linux/proc_monitor.h 字节级一致
// ============================================================================

type ProcInfo struct {
	Pid        int32
	Ppid       int32
	Comm       [16]byte
	State      int32
	_pad1      [4]byte
	Utime      uint64
	Stime      uint64
	Vsize      uint64
	Rss        uint64
	Nice       int32
	NumThreads int32
	Uid        uint32
	_pad2      [4]byte
}

type ProcTreeNode struct {
	Pid   int32
	Ppid  int32
	Comm  [16]byte
	Level int32
	_pad  [4]byte
}

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
	HZ                = 100
)

func cstr(b [16]byte) string {
	n := 0
	for n < 16 && b[n] != 0 {
		n++
	}
	return string(b[:n])
}

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
// 进程树
// ============================================================================

type TreeNode struct {
	Info     ProcTreeNode
	Children []*TreeNode
}

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
	for _, n := range nodes {
		if _, hasParent := nodeMap[n.Ppid]; !hasParent && n.Pid != 1 {
			if initNode, ok := nodeMap[1]; ok {
				initNode.Children = append(initNode.Children, nodeMap[n.Pid])
			}
		}
	}
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
// ANSI 样式函数 (替代 lipgloss, 零依赖)
// ============================================================================

const (
	ansiReset  = "\033[0m"
	ansiBold   = "\033[1m"
	ansiDim    = "\033[2m"
	ansiRed    = "\033[31m"
	ansiGreen  = "\033[32m"
	ansiYellow = "\033[33m"
	ansiBlue   = "\033[34m"
	ansiCyan   = "\033[36m"
	ansiWhite  = "\033[37m"
	ansiGray   = "\033[90m"

	bgBlue   = "\033[44m"
	bgGray   = "\033[100m"
)

type colorFn func(string) string

func makeColor(open string) colorFn {
	return func(s string) string { return open + s + ansiReset }
}

var (
	titleFg   = makeColor(ansiBold + ansiWhite + bgBlue)
	headerFg  = makeColor(ansiBold + ansiWhite + bgGray)
	helpTitle = makeColor(ansiBold + ansiCyan)
	treeLine  = makeColor(ansiCyan)
	keyFg     = makeColor(ansiBold + ansiYellow)
	filterFg  = makeColor(ansiYellow)
	dimFg     = makeColor(ansiDim + ansiGray)
	greenFg   = makeColor(ansiGreen)
	warnFg    = makeColor(ansiYellow)
	redFg     = makeColor(ansiRed)
)

func stateColor(state byte) colorFn {
	switch state {
	case 'R':
		return greenFg
	case 'Z':
		return redFg
	case 'D':
		return warnFg
	default:
		return dimFg
	}
}

// ============================================================================
// 终端原始模式
// ============================================================================

const (
	ioctlTCGETS = 0x5401 // linux/amd64
	ioctlTCSETS = 0x5402
	ioctlGWINSZ = 0x5413

	// termios flags
	flagIGNBRK = 0000001
	flagBRKINT = 0000002
	flagPARMRK = 0000010
	flagISTRIP = 0000040
	flagINLCR  = 0000100
	flagIGNCR  = 0000200
	flagICRNL  = 0000400
	flagIXON   = 0002000
	flagOPOST  = 0000001
	flagECHO   = 0000010
	flagECHONL = 0000100
	flagICANON = 0000002
	flagISIG   = 0000001
	flagIEXTEN = 0100000
	flagCSIZE  = 0000060
	flagPARENB = 0000400
	flagCS8    = 0000060
)

type termios struct {
	Iflag  uint32
	Oflag  uint32
	Cflag  uint32
	Lflag  uint32
	Line   uint8
	Cc     [32]uint8
	Ispeed uint32
	Ospeed uint32
}

type winsize struct {
	Row    uint16
	Col    uint16
	Xpixel uint16
	Ypixel uint16
}

func setRaw(fd uintptr) (*termios, error) {
	var old termios
	if _, _, errno := syscall.Syscall(syscall.SYS_IOCTL,
		fd, ioctlTCGETS, uintptr(unsafe.Pointer(&old))); errno != 0 {
		return nil, fmt.Errorf("TCGETS: %s", errno)
	}
	raw := old
	raw.Iflag &^= flagIGNBRK | flagBRKINT | flagPARMRK | flagISTRIP |
		flagINLCR | flagIGNCR | flagICRNL | flagIXON
	raw.Oflag &^= flagOPOST
	raw.Lflag &^= flagECHO | flagECHONL | flagICANON | flagISIG | flagIEXTEN
	raw.Cflag &^= flagCSIZE | flagPARENB
	raw.Cflag |= flagCS8
	raw.Cc[syscall.VMIN] = 1
	raw.Cc[syscall.VTIME] = 0

	if _, _, errno := syscall.Syscall(syscall.SYS_IOCTL,
		fd, ioctlTCSETS, uintptr(unsafe.Pointer(&raw))); errno != 0 {
		return nil, fmt.Errorf("TCSETS: %s", errno)
	}
	return &old, nil
}

func restoreTerm(fd uintptr, old *termios) {
	syscall.Syscall(syscall.SYS_IOCTL, fd, ioctlTCSETS, uintptr(unsafe.Pointer(old)))
	fmt.Print("\033[?1049l") // exit alt screen
	fmt.Print("\033[?25h")   // show cursor
}

func getTermSize() (int, int) {
	var ws winsize
	_, _, errno := syscall.Syscall(syscall.SYS_IOCTL,
		os.Stdout.Fd(), ioctlGWINSZ, uintptr(unsafe.Pointer(&ws)))
	if errno != 0 {
		return 80, 24
	}
	w := int(ws.Col)
	h := int(ws.Row)
	if w < 40 {
		w = 40
	}
	if h < 10 {
		h = 10
	}
	return w, h
}

// ============================================================================
// 键盘输入解析
// ============================================================================

type keyEvent struct {
	code rune
	alt  bool
}

const (
	keyUp     rune = 0x100 + iota
	keyDown
	keyRight
	keyLeft
	keyPgUp
	keyPgDn
	keyHome
	keyEnd
)

func readStdin(ch chan<- keyEvent) {
	buf := make([]byte, 64)
	state := 0       // 0=normal, 1=saw ESC, 2=saw ESC[
	var seq []byte

	for {
		n, err := os.Stdin.Read(buf)
		if err != nil {
			close(ch)
			return
		}
		for i := 0; i < n; i++ {
			b := buf[i]

			switch state {
			case 0: // normal
				switch {
				case b == 27: // ESC
					state = 1
					seq = []byte{b}
				case b == 127 || b == 8: // backspace
					ch <- keyEvent{code: 127}
				case b == 13 || b == 10: // enter
					ch <- keyEvent{code: 13}
				case b == 9: // tab
					ch <- keyEvent{code: 9}
				case b == 3: // Ctrl+C
					ch <- keyEvent{code: 3}
				case b == 4: // Ctrl+D
					ch <- keyEvent{code: keyPgDn}
				case b == 21: // Ctrl+U
					ch <- keyEvent{code: keyPgUp}
				case b < 32:
					// other control chars, ignore
				default:
					ch <- keyEvent{code: rune(b)}
				}
			case 1: // saw ESC
				if b == '[' {
					state = 2
					seq = append(seq, b)
				} else if b == 27 {
					ch <- keyEvent{code: 27} // ESC key
					seq = seq[:0]
				} else {
					// Alt+key
					ch <- keyEvent{code: rune(b), alt: true}
					state = 0
				}
			case 2: // saw ESC[ — gather CSI params
				seq = append(seq, b)
				if (b >= 'A' && b <= 'Z') || (b >= 'a' && b <= 'z') || b == '~' {
					state = 0
					s := string(seq)
					switch s {
					case "[A":
						ch <- keyEvent{code: keyUp}
					case "[B":
						ch <- keyEvent{code: keyDown}
					case "[C":
						ch <- keyEvent{code: keyRight}
					case "[D":
						ch <- keyEvent{code: keyLeft}
					case "[H", "[1~":
						ch <- keyEvent{code: keyHome}
					case "[F", "[4~":
						ch <- keyEvent{code: keyEnd}
					case "[5~":
						ch <- keyEvent{code: keyPgUp}
					case "[6~":
						ch <- keyEvent{code: keyPgDn}
					}
					seq = seq[:0]
				}
			}
		}
	}
}

// ============================================================================
// Model & 事件循环
// ============================================================================

type viewMode int

const (
	viewList viewMode = iota
	viewTree
	viewHelp
)

type sortField int

const (
	sortByPID sortField = iota
	sortByCPU
	sortByMem
	sortByName
)

func (s sortField) label() string {
	return [...]string{"PID", "CPU", "MEM", "NAME"}[s]
}

type model struct {
	procs     []ProcInfo
	treeNodes []ProcTreeNode
	stat      ProcStat
	treeRoot  *TreeNode

	mode      viewMode
	sortF     sortField
	sortDesc  bool

	filterText string
	filtering  bool

	scrollY int
	width   int
	height  int

	lastErr string
	ready   bool
	quit    bool
}

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

func (m *model) sortProcs() {
	if m.sortF == sortByPID {
		sort.Slice(m.procs, func(i, j int) bool {
			if m.sortDesc {
				return m.procs[i].Pid > m.procs[j].Pid
			}
			return m.procs[i].Pid < m.procs[j].Pid
		})
		return
	}
	if m.sortF == sortByName {
		sort.Slice(m.procs, func(i, j int) bool {
			ni, nj := cstr(m.procs[i].Comm), cstr(m.procs[j].Comm)
			if m.sortDesc {
				return ni > nj
			}
			return ni < nj
		})
		return
	}
	desc := m.sortDesc
	if m.sortF == sortByCPU {
		sort.Slice(m.procs, func(i, j int) bool {
			ci := m.procs[i].Utime + m.procs[i].Stime
			cj := m.procs[j].Utime + m.procs[j].Stime
			if desc {
				return ci > cj
			}
			return ci < cj
		})
	} else {
		sort.Slice(m.procs, func(i, j int) bool {
			if desc {
				return m.procs[i].Rss > m.procs[j].Rss
			}
			return m.procs[i].Rss < m.procs[j].Rss
		})
	}
}

func (m *model) filteredProcs() []ProcInfo {
	if m.filterText == "" {
		return m.procs
	}
	text := strings.ToLower(m.filterText)
	var result []ProcInfo
	for _, p := range m.procs {
		if matchFilter(p, text) {
			result = append(result, p)
		}
	}
	return result
}

func matchFilter(p ProcInfo, text string) bool {
	switch {
	case strings.HasPrefix(text, "="):
		for _, r := range strings.TrimPrefix(text, "=") {
			if stateByte(p.State) == byte(r) {
				return true
			}
		}
		return false
	case strings.HasPrefix(text, ":"):
		pid, err := strconv.Atoi(strings.TrimPrefix(text, ":"))
		if err != nil {
			return false
		}
		return int(p.Pid) == pid
	default:
		return strings.Contains(strings.ToLower(cstr(p.Comm)), text)
	}
}

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
	case 16:
		return 'Z'
	case 1026:
		return 'I'
	default:
		return '?'
	}
}

func cpuSec(p ProcInfo) float64 {
	return float64(p.Utime+p.Stime) / HZ
}

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
// 渲染
// ============================================================================

func (m *model) render() {
	m.width, m.height = getTermSize()

	var buf strings.Builder
	buf.WriteString("\033[H") // cursor to (0,0)

	// 1. Stats bar
	m.writeStatsBar(&buf)
	buf.WriteByte('\n')

	// 2. Separator
	m.writeHLine(&buf)
	buf.WriteByte('\n')

	// 3. Main content
	switch m.mode {
	case viewList:
		m.writeListView(&buf)
	case viewTree:
		m.writeTreeView(&buf)
	case viewHelp:
		m.writeHelpView(&buf)
	}

	// 4. Footer
	buf.WriteByte('\n')
	m.writeHLine(&buf)
	buf.WriteByte('\n')
	m.writeStatusBar(&buf)

	if m.lastErr != "" {
		buf.WriteString("\n " + redFg("⚠ "+m.lastErr))
	}

	buf.WriteString("\033[J") // clear below
	os.Stdout.WriteString(buf.String())
}

func (m *model) writeStatsBar(buf *strings.Builder) {
	if !m.ready {
		buf.WriteString(dimFg(" Loading..."))
		return
	}
	s := m.stat
	parts := []string{
		titleFg(" Process Monitor "),
		fmt.Sprintf("Total:%d", s.Total),
		greenFg(fmt.Sprintf("R:%d", s.Running)),
		dimFg(fmt.Sprintf("S:%d", s.Sleeping)),
		warnFg(fmt.Sprintf("D:%d", s.Uninterruptible)),
		redFg(fmt.Sprintf("Z:%d", s.Zombie)),
		dimFg(fmt.Sprintf("T:%d", s.Stopped)),
		fmt.Sprintf("KThr:%d", s.KernelThreads),
		fmt.Sprintf("UThr:%d", s.UserThreads),
	}
	buf.WriteString(strings.Join(parts, "  "))
}

func (m *model) writeHLine(buf *strings.Builder) {
	if m.width > 0 {
		buf.WriteString(strings.Repeat("─", m.width))
	}
}

func (m *model) writeListView(buf *strings.Builder) {
	procs := m.filteredProcs()
	avail := m.height - 4
	if avail < 2 {
		return
	}

	// Sort/filter hint
	if m.filterText != "" {
		fmt.Fprintf(buf, " %s (%d matched)", filterFg("Filter: \""+m.filterText+"\""), len(procs))
	} else {
		arrow := "▲"
		if m.sortDesc {
			arrow = "▼"
		}
		fmt.Fprintf(buf, " %s %s", dimFg("Sort:"+m.sortF.label()), dimFg(arrow))
	}
	buf.WriteByte('\n')

	// Table header
	header := fmt.Sprintf("%-7s %-7s %-18s %c %-4s %-8s %-9s %-9s %-9s %s",
		"PID", "PPID", "NAME", 'S', "NICE", "THR", "VSIZE", "RSS", "CPU(s)", "UID")
	buf.WriteString(headerFg(header))
	buf.WriteByte('\n')

	// Scroll clamp
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
		st := stateByte(p.State)
		line := fmt.Sprintf("%-7d %-7d %-18.18s %c %-4d %-8d %-9s %-9s %-9.1f %d",
			p.Pid, p.Ppid, cstr(p.Comm), st,
			p.Nice, p.NumThreads,
			formatSize(p.Vsize), formatSize(p.Rss*4096),
			cpuSec(p), p.Uid)
		buf.WriteString(stateColor(st)(line))
		buf.WriteByte('\n')
	}
}

func (m *model) writeTreeView(buf *strings.Builder) {
	if m.treeRoot == nil {
		buf.WriteString(dimFg(" No process tree data"))
		return
	}
	lines := m.treeRoot.renderLines()
	avail := m.height - 4
	if avail < 2 {
		return
	}

	buf.WriteString(helpTitle(fmt.Sprintf(" Process Tree (%d nodes)", len(m.treeNodes))))
	buf.WriteByte('\n')

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

	for i := start; i < end; i++ {
		buf.WriteString(treeLine(lines[i]))
		buf.WriteByte('\n')
	}
}

func (m *model) writeHelpView(buf *strings.Builder) {
	lines := []string{
		"",
		helpTitle("  Key Bindings"),
		"",
		"  " + keyFg("q / Ctrl+C") + "    Quit",
		"  " + keyFg("t") + "             Toggle process list / tree view",
		"  " + keyFg("s") + "             Cycle sort: PID → CPU → MEM → NAME",
		"  " + keyFg("S") + "             Toggle sort direction",
		"  " + keyFg("/") + "             Enter filter mode",
		"  " + keyFg("Esc") + "           Clear filter",
		"  " + keyFg("↑↓ / j k") + "     Scroll",
		"  " + keyFg("PgUp / PgDn") + "   Page up / down",
		"  " + keyFg("g / Home") + "     Jump to top",
		"  " + keyFg("G / End") + "      Jump to bottom",
		"  " + keyFg("?") + "             Toggle this help",
		"",
		helpTitle("  Filter Syntax"),
		"",
		"  " + keyFg("systemd") + "      Match process name (case-insensitive substring)",
		"  " + keyFg("=R") + "           Match by state (R/S/D/T/Z/I)",
		"  " + keyFg(":1234") + "        Match by exact PID",
		"",
		helpTitle("  Data Source"),
		"",
		"  All data from custom Linux syscalls (470/471/472).",
		"  No /proc, ps, eBPF, or ptrace is used.",
	}
	buf.WriteString(strings.Join(lines, "\n"))
}

func (m *model) writeStatusBar(buf *strings.Builder) {
	if m.filtering {
		prompt := fmt.Sprintf(" Filter: %s█", m.filterText)
		buf.WriteString(filterFg(prompt))
		buf.WriteString(dimFg("  [Enter: apply  Esc: cancel]"))
		return
	}

	totalLines := len(m.filteredProcs())
	if m.mode == viewTree && m.treeRoot != nil {
		totalLines = len(m.treeRoot.renderLines())
	}

	scrollInfo := fmt.Sprintf("%d/%d", m.scrollY, totalLines)
	parts := []string{
		dimFg(scrollInfo),
		keyFg("q") + " quit",
		keyFg("/") + " filter",
		keyFg("s") + " sort",
	}
	if m.mode == viewTree {
		parts = append(parts, keyFg("t")+" list")
	} else {
		parts = append(parts, keyFg("t")+" tree")
	}
	parts = append(parts, keyFg("?")+" help")
	buf.WriteString(strings.Join(parts, "  "))
}

// ============================================================================
// 事件处理
// ============================================================================

func (m *model) handleKey(ev keyEvent) {
	// In filter mode, handle text input
	if m.filtering {
		switch ev.code {
		case 13: // Enter
			m.filtering = false
		case 27: // Esc
			m.filterText = ""
			m.filtering = false
		case 127: // Backspace
			if len(m.filterText) > 0 {
				m.filterText = m.filterText[:len(m.filterText)-1]
			}
		default:
			if ev.code >= 32 && ev.code < 127 && len(m.filterText) < 40 {
				m.filterText += string(ev.code)
			}
		}
		m.scrollY = 0
		return
	}

	// Normal mode
	switch ev.code {
	case 'q', 3: // q or Ctrl+C
		m.quit = true

	case 't':
		m.scrollY = 0
		if m.mode == viewTree {
			m.mode = viewList
		} else {
			m.mode = viewTree
		}

	case '?':
		if m.mode == viewHelp {
			m.mode = viewList
		} else {
			m.mode = viewHelp
		}

	case '/':
		m.filtering = true

	case 27: // Esc
		m.filterText = ""
		m.scrollY = 0

	case 's':
		m.sortF = (m.sortF + 1) % 4
		m.sortProcs()
		m.scrollY = 0

	case 'S':
		m.sortDesc = !m.sortDesc
		m.sortProcs()

	case 'j', keyDown:
		m.scrollY++

	case 'k', keyUp:
		if m.scrollY > 0 {
			m.scrollY--
		}

	case keyPgDn:
		m.scrollY += m.height - 5

	case keyPgUp:
		m.scrollY -= m.height - 5
		if m.scrollY < 0 {
			m.scrollY = 0
		}

	case 'g', keyHome:
		m.scrollY = 0

	case 'G', keyEnd:
		m.scrollY = 1 << 30 // clamped in render
	}
}

// ============================================================================
// 入口
// ============================================================================

func main() {
	// 验证结构体大小
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

	// 设置原始终端模式
	fd := os.Stdin.Fd()
	oldTerm, err := setRaw(fd)
	if err != nil {
		fmt.Fprintf(os.Stderr, "Failed to set raw terminal: %v\n", err)
		fmt.Fprintf(os.Stderr, "Try: sudo ./procmon\n")
		os.Exit(1)
	}
	defer restoreTerm(fd, oldTerm)

	// 进入 Alt Screen, 隐藏光标
	fmt.Print("\033[?1049h")
	fmt.Print("\033[?25l")

	// 处理 SIGWINCH (虽然 raw mode 下 ISIG 关闭, 但可以手动捕获)
	sigCh := make(chan os.Signal, 1)
	signal.Notify(sigCh, syscall.SIGWINCH)
	defer signal.Stop(sigCh)

	// 键盘输入
	keyCh := make(chan keyEvent, 64)
	go readStdin(keyCh)

	// Ticker
	ticker := time.NewTicker(time.Second)
	defer ticker.Stop()

	m := &model{
		mode:  viewList,
		sortF: sortByPID,
	}
	m.fetchAll()

	// 事件循环
	for !m.quit {
		m.render()

		select {
		case <-ticker.C:
			m.fetchAll()

		case <-sigCh:
			m.width, m.height = getTermSize()

		case ev, ok := <-keyCh:
			if !ok {
				m.quit = true
				break
			}
			m.handleKey(ev)
		}
	}
}
