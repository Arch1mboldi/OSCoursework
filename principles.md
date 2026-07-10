# 进程全量监控系统 —— 原理与实现

> 操作系统课程设计 · 实验三  
> Linux 6.18 内核 · 自定义系统调用 (470/471/472)  
> 2026年7月

---

## 目录

1. [系统架构](#1-系统架构)
2. [数据结构：内核-用户态 ABI 契约](#2-数据结构内核-用户态-abi-契约)
3. [内核侧：3 个系统调用](#3-内核侧3-个系统调用)
   - [3.1 sys_proc_collect (470) — 进程全量采集](#31-sys_proc_collect-470--进程全量采集)
   - [3.2 sys_proc_snapshot (471) — 进程树快照](#32-sys_proc_snapshot-471--进程树快照)
   - [3.3 sys_proc_stat (472) — 聚合统计](#33-sys_proc_stat-472--聚合统计)
4. [内核关键机制](#4-内核关键机制)
   - [4.1 锁策略：lock-drop-copy-relock](#41-锁策略lock-drop-copy-relock)
   - [4.2 进程状态编码](#42-进程状态编码)
   - [4.3 CPU 时间采集](#43-cpu-时间采集)
   - [4.4 内存信息采集](#44-内存信息采集)
   - [4.5 安全性保证](#45-安全性保证)
5. [用户态：ncurses 调试版 TUI](#5-用户态ncurses-调试版-tui)
   - [5.1 面板布局](#51-面板布局)
   - [5.2 系统调用层与耗时测量](#52-系统调用层与耗时测量)
   - [5.3 状态解码算法](#53-状态解码算法)
   - [5.4 CPU% 计算](#54-cpu-计算)
   - [5.5 五视图模式](#55-五视图模式)
   - [5.6 过滤引擎](#56-过滤引擎)
   - [5.7 进程树渲染](#57-进程树渲染)
   - [5.8 数据导出](#58-数据导出)
   - [5.9 交叉验证](#59-交叉验证)
6. [编译与部署](#6-编译与部署)
7. [调试指南](#7-调试指南)

---

## 1. 系统架构

```
┌─────────────────────────────────────────────┐
│                  Userspace                   │
│                                             │
│  ┌──────────────┐  ┌──────────────────────┐ │
│  │ proc_monitor │  │ test_syscall         │ │
│  │ (ncurses TUI)│  │ (验证程序)            │ │
│  └──────┬───────┘  └──────────┬───────────┘ │
│         │    syscall(SYS_xxx) │             │
│         └──────────┬─────────┘             │
│                    │                        │
├────────────────────│─── syscall boundary ───┤
│                    │                        │
│  ┌─────────────────▼──────────────────────┐ │
│  │         Linux 6.18 Kernel              │ │
│  │                                         │ │
│  │  ┌──────────────────────────────────┐  │ │
│  │  │ kernel/proc_monitor.c            │  │ │
│  │  │                                  │  │ │
│  │  │  sys_proc_collect  (470)         │  │ │
│  │  │  sys_proc_snapshot (471)         │  │ │
│  │  │  sys_proc_stat     (472)         │  │ │
│  │  └──────────┬───────────────────────┘  │ │
│  │             │ read_lock(&tasklist_lock) │ │
│  │             ▼                           │ │
│  │  ┌──────────────────────────────────┐  │ │
│  │  │  task_struct 链表 (RCU 保护)      │  │ │
│  │  │  init_task → task1 → task2 → ...  │  │ │
│  │  └──────────────────────────────────┘  │ │
│  └─────────────────────────────────────────┘ │
└─────────────────────────────────────────────┘
```

**数据流向**: 内核遍历 `task_struct` 链表 → 提取字段填入 `proc_info` / `proc_tree_node` → `copy_to_user()` → 用户态 buffer → TUI 渲染。

---

## 2. 数据结构：内核-用户态 ABI 契约

三个结构体定义在 `include/linux/proc_monitor.h`（内核侧）和 `user_app/proc_monitor.h`（用户态侧）。
两边定义**必须逐字节一致**——struct 大小、字段偏移、对齐方式完全相同，构成二进制 ABI 契约。

### 2.1 `struct proc_info` — 进程完整信息

```c
struct proc_info {
    pid_t         pid;          // 进程ID (TGID, 线程组ID)
    pid_t         ppid;         // 父进程ID
    char          comm[16];     // 进程名 (最多15字符+NUL)
    int           state;        // __state | exit_state (位掩码)
    unsigned long utime;        // 用户态CPU时间 (USER_HZ时钟滴答)
    unsigned long stime;        // 内核态CPU时间 (USER_HZ时钟滴答)
    unsigned long vsize;        // 虚拟内存大小 (字节)
    unsigned long rss;          // 常驻内存集 (页数, 4KB/页)
    int           nice;         // nice优先级 (-20~19, 越低优先级越高)
    int           num_threads;  // 线程组内线程数
    uid_t         uid;          // 真实用户ID
};
```

**x86_64 内存布局** (LP64 ABI, sizeof=80):

```
Offset  Size  Field        Notes
────────────────────────────────────
  0      4    pid          int (4B)
  4      4    ppid         int (4B)
  8     16    comm[16]     char[16]
 24      4    state        int (4B)
 28      4    (padding)    对齐 unsigned long (8B)
 32      8    utime        unsigned long (8B)
 40      8    stime        unsigned long (8B)
 48      8    vsize        unsigned long (8B)
 56      8    rss          unsigned long (8B)
 64      4    nice         int (4B)
 68      4    num_threads  int (4B)
 72      4    uid          uid_t (4B)
 76      4    (padding)    对齐到 8B 边界
────────────────────────────────────
Total: 80 bytes
```

### 2.2 `struct proc_tree_node` — 进程树节点

```c
struct proc_tree_node {
    pid_t pid;       // 进程ID
    pid_t ppid;      // 父进程ID
    char  comm[16];  // 进程名
    int   level;     // 树深度 (init_task=0)
};
// sizeof = 28 on x86_64 (4+4+16+4)
```

`level` 由内核 `compute_level()` 计算：从当前进程沿 `real_parent` 向上追溯到 `init_task`，累计层数。

### 2.3 `struct proc_stat` — 聚合统计

```c
struct proc_stat {
    int total_processes;     // 进程总数
    int running_processes;   // TASK_RUNNING
    int sleeping_processes;  // TASK_INTERRUPTIBLE
    int uninterruptible;     // TASK_UNINTERRUPTIBLE
    int stopped_processes;   // TASK_STOPPED / TASK_TRACED
    int zombie_processes;    // EXIT_ZOMBIE
    int idle_processes;      // TASK_IDLE / TASK_NEW 等
    int kernel_threads;      // mm==NULL 的内核线程
    int user_threads;        // mm!=NULL 的用户线程
};
// sizeof = 36 on x86_64 (9 × int)
```

---

## 3. 内核侧：3 个系统调用

系统调用号在 `arch/x86/entry/syscalls/syscall_64.tbl` 中注册：

```
470  common  proc_collect   sys_proc_collect
471  common  proc_snapshot  sys_proc_snapshot
472  common  proc_stat      sys_proc_stat
```

### 3.1 sys_proc_collect (470) — 进程全量采集

**签名**: `long sys_proc_collect(struct proc_info *user_buf, int max_count, int *ret_count)`

**功能**: 遍历所有进程，为每个进程填充一个 `proc_info` 结构体，通过 `copy_to_user()` 逐个拷贝到用户态数组。

**字段采集逻辑**:

| 字段 | 内核 API | 说明 |
|:---|:---|:---|
| `comm` | `get_task_comm()` | 安全复制进程名（最多 TASK_COMM_LEN-1 字符） |
| `pid` | `task_tgid_nr(task)` | 线程组 ID，即用户态视角的 PID |
| `ppid` | `task_tgid_nr(task->real_parent)` | 真实父进程的 TGID |
| `state` | `task->__state \| task->exit_state` | 合并 __state 和 exit_state（见 §4.2） |
| `utime` / `stime` | `task_cputime()` + `nsec_to_clock_t()` | 纳秒→USER_HZ 滴答（见 §4.3） |
| `vsize` | `task->mm->total_vm << PAGE_SHIFT` | 虚拟内存（页数→字节） |
| `rss` | `get_mm_rss(task->mm)` | 常驻内存集（页数） |
| `nice` | `task_nice(task)` | -20~19 |
| `num_threads` | `task->signal->nr_threads` | 线程组内线程数 |
| `uid` | `from_kuid_munged()` | 映射到调用者命名空间 |

**内核线程处理**: 当 `task->mm == NULL` 时，`vsize = 0`, `rss = 0`。

**返回值**: 0 成功；`-EINVAL` 参数无效；`-EFAULT` copy_to_user 失败（尽力返回已拷贝数量）。

### 3.2 sys_proc_snapshot (471) — 进程树快照

**签名**: `long sys_proc_snapshot(struct proc_tree_node *user_buf, int max_count, int *ret_count)`

**功能**: 返回进程树拓扑数据，每个节点包含 `pid/ppid/comm/level`。

**level 计算** (`compute_level()`):
```
从 task->real_parent 向上走，直到 init_task：
  task → parent → grandparent → ... → init_task
  每步 level++，初始 level=0
```

用户态可基于 `pid/ppid` 自行重建树结构，也可直接使用 `level` 按缩进渲染。

### 3.3 sys_proc_stat (472) — 聚合统计

**签名**: `long sys_proc_stat(struct proc_stat *stat)`

**功能**: 一次性返回按状态分类的进程计数。

**分类逻辑**:

```
for_each_process(task):
    total_processes++

    if (task->exit_state & EXIT_ZOMBIE)     → zombie_processes++
    else if (task->exit_state & EXIT_DEAD)  → (skip, 不计入活跃分类)
    else:
        switch (task->__state):
            TASK_RUNNING         → running_processes++
            TASK_INTERRUPTIBLE   → sleeping_processes++
            TASK_UNINTERRUPTIBLE → uninterruptible++
            TASK_STOPPED/TRACED  → stopped_processes++
            default              → idle_processes++

    if (task->mm == NULL) → kernel_threads++
    else                  → user_threads++
```

> **注意**: `for_each_process()` 使用 `read_lock(&tasklist_lock)` 持锁遍历，全程持锁不释放——与 collect/snapshot 的锁策略不同，因为此函数不在持锁期间调用可能睡眠的 `copy_to_user()`。

---

## 4. 内核关键机制

### 4.1 锁策略：lock-drop-copy-relock

这是 **sys_proc_collect** 和 **sys_proc_snapshot** 最核心的设计。

```
read_lock(&tasklist_lock);
task = next_task(&init_task);

while (task != &init_task) {
    填充 kinfo/knode (持锁读取 task_struct 字段)

    next = next_task(task);   // ← 关键: 保存后继指针
    read_unlock(&tasklist_lock);

    copy_to_user(&user_buf[count], &kinfo, sizeof(kinfo));
    // ↑ 可能睡眠 (page fault), 必须释放锁

    count++;
    read_lock(&tasklist_lock);
    task = next;              // ← 用保存的指针继续, 而非 task->tasks.next
}

read_unlock(&tasklist_lock);
```

**为什么必须这样做**:

1. **RCU 读者不可睡眠** — `read_lock(&tasklist_lock)` 持有期间调用 `copy_to_user()` 可能触发缺页异常导致睡眠，违反 RCU 语义
2. **Use-After-Free 防护** — 释放锁后当前 `task` 可能因进程退出被 RCU 回收，直接访问 `task->tasks.next` 是 UAF。提前保存 `next_task(task)` 避免此问题
3. **RCU 宽限期保证** — 即使 `task` 被回收，`next` 指向的 task_struct 在 RCU 宽限期内仍然有效

**sys_proc_stat 为什么不同**: 该函数在持锁循环内不做任何可能睡眠的操作（不调用 `copy_to_user()`），只在循环结束后统一拷贝一次，因此可以全程持锁。

### 4.2 进程状态编码

Linux 5.14+ 将原来的 `task->state` 拆分为两个字段：

| 字段 | 存储内容 |
|:---|:---|
| `task->__state` | 调度状态: `TASK_RUNNING(0x00)`, `TASK_INTERRUPTIBLE(0x01)`, `TASK_UNINTERRUPTIBLE(0x02)`, `__TASK_STOPPED(0x04)`, `__TASK_TRACED(0x08)`, `TASK_DEAD(0x40)`, `TASK_WAKEKILL(0x80)` |
| `task->exit_state` | 退出状态: `EXIT_ZOMBIE(0x20)`, `EXIT_DEAD(0x10)` |

**内核传递给用户态**: `state = __state | exit_state`

这意味着一个僵尸进程的 state 可能是 `0x21` = `TASK_INTERRUPTIBLE(0x01) | EXIT_ZOMBIE(0x20)`（僵尸在死前处于可中断睡眠）。

**用户态解码必须用位检测，不能用精确匹配**:

```c
// 正确做法 — 按优先级检测位
if (state & 0x20)       return 'Z';  // EXIT_ZOMBIE 优先级最高
if (state & 0x10)       return 'X';  // EXIT_DEAD
if (state & 0x08)       return 't';  // __TASK_TRACED
if (state & 0x04)       return 'T';  // __TASK_STOPPED
switch (state & 0x03) { ... }       // 低2位 = 基本调度状态
```

### 4.3 CPU 时间采集

发行版内核 (`CONFIG_VIRT_CPU_ACCOUNTING_GEN=y`) 以**纳秒**精度存储 CPU 时间。直接读 `task->utime` 会得到纳秒值，而非传统的 jiffies。

**采集流程**:

```
task_cputime(task, &ut_ns, &st_ns)  → 纳秒精度 CPU 时间
nsec_to_clock_t(ut_ns)              → USER_HZ 时钟滴答 (与 /proc/pid/stat 一致)
```

- `task_cputime()`: 读取 utime + stime，单位纳秒
- `nsec_to_clock_t()`: 转换为 `USER_HZ` 滴答，值 = `sysconf(_SC_CLK_TCK)`（通常 100）

### 4.4 内存信息采集

| 字段 | 采集方式 | 单位 | 与 /proc/pid/stat 关系 |
|:---|:---|:---|:---|
| `vsize` | `task->mm->total_vm << PAGE_SHIFT` | 字节 | 对应 /proc/pid/stat 的 vsize (23) |
| `rss` | `get_mm_rss(task->mm)` | 页数 (4KB/页) | 对应 /proc/pid/stat 的 rss (24)，但 /proc 输出需 × PAGE_SIZE |

**内核线程**: `task->mm == NULL` → 无用户态地址空间 → `vsize = 0, rss = 0`

### 4.5 安全性保证

| 安全措施 | 机制 |
|:---|:---|
| 杜绝内核指针泄露 | 所有数据通过 `copy_to_user()` 传递，不暴露 `task_struct*` 等内核地址 |
| 杜绝 UAF | 持锁保存 next 指针 → 释放锁 → 用保存的指针继续（见 §4.1） |
| 杜绝栈溢出 | 逐个条目 copy_to_user，不在内核栈分配大数组 |
| 用户身份映射 | `from_kuid_munged()` 将内核 UID 映射到调用者命名空间 |
| 参数校验 | 所有 `__user` 指针在解引用前检查 NULL |

---

## 5. 用户态：ncurses 调试版 TUI

### 5.1 面板布局

```
┌── win_main (rows - 6) ─────────────────────────────┐
│  主视图: LIST | DEBUG-TABLE | TREE | HEX | SYSCALL │
│  按 v 键循环切换，按 1-5 直达                       │
└────────────────────────────────────────────────────┘
┌── win_detail (2 rows) ─────────────────────────────┐
│  Line 0: 选中进程的 ALL 11 个 proc_info 字段       │
│  Line 1: 时间/内存原始值 + __state/exit_state 解码  │
└────────────────────────────────────────────────────┘
┌── win_syscall (2 rows) ────────────────────────────┐
│  Line 0: 3 个 syscall 的 ret/errno/latency/count   │
│  Line 1: proc_stat 聚合 (T/R/S/D/Z) + CLK_TCK     │
└────────────────────────────────────────────────────┘
┌── win_hint (1 row) ────────────────────────────────┐
│  按键提示: v:view ↑↓:sel Tab:sort /:filter q:quit  │
└────────────────────────────────────────────────────┘
```

### 5.2 系统调用层与耗时测量

每个 syscall 通过 `syscall()` 函数调用，并用 `clock_gettime(CLOCK_MONOTONIC)` 测量耗时：

```c
struct sc_result {
    long ret;
    int  err;
    long latency_us;  // 微秒
    int  count;
};

static int fetch_procs(void) {
    long t0 = time_now_us();  // clock_gettime(CLOCK_MONOTONIC)
    g_sc_collect.ret = syscall(SYS_proc_collect, g_procs, MAX_PROCS, &count);
    long t1 = time_now_us();
    g_sc_collect.latency_us = t1 - t0;
    g_sc_collect.err = (ret < 0) ? errno : 0;
    g_sc_collect.count = count;
    ...
}
```

`update_data()` 按顺序调用 `fetch_procs()` → `fetch_tree()` → `fetch_stat()`，每次记录独立耗时。

### 5.3 状态解码算法

state 字段 = `__state | exit_state`，解码分三步：

**Step 1**: 分离两部分
```
exit_state_bits = state & (0x10 | 0x20)     // EXIT_DEAD | EXIT_ZOMBIE
__state_bits    = state & ~(0x10 | 0x20)     // 其余所有位
```

**Step 2**: 按优先级检测退出状态
```
if (state & 0x20) → 'Z' (ZOMBIE)
if (state & 0x10) → 'X' (DEAD)
```

**Step 3**: 检测调度状态位
```
if (state & 0x08) → 't' (TRACED)
if (state & 0x04) → 'T' (STOPPED)
switch (state & 0x03):
    case 0x00 → 'R' (RUNNING)
    case 0x01 → 'S' (SLEEPING / INTERRUPTIBLE)
    case 0x02 → 'D' (DISK_WAIT / UNINTERRUPTIBLE)
    default  → '?'
```

> **为什么用位检测不用 `switch(state)`**: 因为 Linux 6.x 可能在 __state 中设置额外标志位（如 `TASK_WAKEKILL=0x80`、`TASK_NOLOAD=0x400`），导致 state 值不等于单一的枚举值。位检测只关心我们感兴趣的位。

### 5.4 CPU% 计算

CPU% = 进程在两次采集之间消耗的 CPU 时间 / 实际经过的 wall-clock 时间。

**算法**:

```
第一帧:
  save_prev_cpu()  → 保存每个进程的 (pid, utime+stime)
  g_has_prev = 0   → 第一帧不计算 CPU% (无历史数据)

后续帧:
  save_prev_cpu()  → 保存上一帧的 CPU 滴答值
  update_data()    → 采集新数据 + 记录 g_tv_prev, g_tv_curr
  g_has_prev = 1

  对每个进程:
    delta_ticks = cur_total - prev_total    (两次采集间的 CPU 滴答差)
    elapsed     = g_tv_curr - g_tv_prev     (两次采集的时间间隔, 秒)
    cpu%        = delta_ticks / (clk_tck × elapsed) × 100
```

**时间戳管理** (修复后的版本):

| 变量 | 含义 | 设置者 |
|:---|:---|:---|
| `g_tv_prev` | 上一帧数据采集时刻 | `update_data()` 入口: `g_tv_prev = g_tv_curr` |
| `g_tv_curr` | 当前帧数据采集时刻 | `update_data()` 入口: `gettimeofday(&g_tv_curr)` |
| `g_has_prev` | 是否有上一帧数据 | 首次 `update_data()` 成功后置 1 |

> **关键修复**: 早期版本在 `save_prev_cpu()` 中记录时间戳，导致 `elapsed ≈ 0`（save 和 render 几乎同时发生），CPU% 被放大数百倍。修复后改为在 `update_data()` 中记录，`elapsed` 正确反映两次 syscall 间的真实间隔。

**边界情况**:
- 新进程 (PID 不在 g_prev 中): CPU% = 0.0 (无历史)
- 已退出进程 (PID 消失): 不计算 (g_procs 中已不存在)
- PID 复用 (cur_total < prev_total): CPU% = 0.0 (视为新进程)
- elapsed ≤ 0: CPU% = 0.0 (防御性处理)

### 5.5 五视图模式

| 模式 | 编号 | 用途 | 数据来源 |
|:---|:---|:---|:---|
| **LIST** | 1 | 紧凑表格, 含 state hex 原始值, CPU%, 11 字段 | proc_info |
| **DEBUG-TABLE** | 2 | 所有字段原始数值, 不做任何格式化 | proc_info |
| **TREE** | 3 | 进程树层次结构, Unicode 树线, 按深度着色 | proc_tree_node |
| **HEX-DUMP** | 4 | 选中进程 proc_info 的原始字节 + 字段偏移标注 | proc_info (raw bytes) |
| **SYSCALL** | 5 | 系统调用诊断: ret/errno/latency/struct 大小/cross-validation | sc_result + proc_stat |

**LIST 视图列**:
```
PID  PPID  NAME            S:HEX  CPU%   UTIME   STIME   VSIZE    RSS      NICE THR UID
1    0     systemd         R:0000  0.0  12.3    5.6     180.0M   15.0M     0   23  0
```

**DEBUG-TABLE 视图列** (全部原始数值):
```
PID  PPID  COMM            STATE       UTIME(tick) STIME(tick) VSIZE(B)    RSS(pg) NICE THR UID
1    0     systemd         0x00000000  1234        567         188743680  3840    0    23  0
```

**HEX-DUMP 视图**: 16 字节/行 hex dump + ASCII sidebar + 字段布局标注表：
```
Offset    Hex                             ASCII
--------  ------------------------------  ----------------
00000000  01 00 00 00 00 00 00 00        ................
00000008  73 79 73 74 65 6D 64 00        systemd.........
...

Field Map:
  +000 (0x00)  pid           4 bytes
  +004 (0x04)  ppid          4 bytes
  +008 (0x08)  comm[16]     16 bytes
  ...
```

### 5.6 过滤引擎

过滤语法（在 `/` 键进入的过滤模式下输入）：

| 语法 | 含义 | 实现 |
|:---|:---|:---|
| `bash` | 进程名包含 "bash" (大小写不敏感) | `strstr(lower(name), lower(filter))` |
| `=R` | state 字符匹配 (R/S/D/T/Z/X/t) | `state_char(p->state) == filter[1]` |
| `=0x22` | state 位掩码精确匹配 (调试用) | `p->state == strtol("=0x22", NULL, 16)` |
| `:1234` | PID 精确匹配 | `p->pid == atoi(":1234")` |

过滤模式下 `filtered_count()` 和 `filtered_index(n)` 用于统计过滤后数量和定位第 n 个可见条目。

### 5.7 进程树渲染

基于 `proc_tree_node` 数组渲染 ASCII/Unicode 树：

**树连接符**: UTF-8 终端用 `│ ├── └──`，ASCII fallback 用 `| |-- \`--`。

**绘制算法**:
1. 对每个节点构建前缀字符串 `prefix`
2. 对每个 `level` 层级，检查当前节点之后是否还有同一祖先的兄弟节点：
   - 有 → 添加 `│   ` (竖线)
   - 无 → 添加 `    ` (空白)
3. 当前节点是父节点的最后一个子节点 → `└── `，否则 → `├── `

**`tree_is_last(i, count)`**: 检查节点 i 之后是否还有相同 ppid 的节点。
**`tree_has_continuation(i, count, target_level)`**: 检查当前节点之后是否还有同一祖先的后代（用于决定竖线的延续）。

### 5.8 数据导出

按 `x` 键，根据当前视图模式选择导出格式：

| 视图 | 导出格式 | 文件名 (默认) | 内容 |
|:---|:---|:---|:---|
| LIST / DEBUG | CSV | `proc_list.csv` | 所有进程的 proc_info 字段, 含 clk_tck 头 |
| TREE | DOT (Graphviz) | `proc_tree.dot` | 进程树有向图, 可 `dot -Tpng` 渲染 |
| HEX / SYSCALL | 二进制 dump | `proc_dump.bin` | magic("PROC") + count + struct_size + g_procs 原始字节 |

### 5.9 交叉验证

SYSCALL 视图底部包含交叉验证：用 `proc_info[]` 数组自己数出来的状态分布 vs `proc_stat` 报告的值。

```
Cross-check:
R: proc_info=12    proc_stat=12    OK
S: proc_info=280   proc_stat=280   OK
D: proc_info=5     proc_stat=5     OK
Z: proc_info=3     proc_stat=2     MISMATCH!
```

**出现 MISMATCH 的原因**: `fetch_procs()` 和 `fetch_stat()` 是两个独立的系统调用，存在时间窗口。在这两次调用之间，可能有进程退出（僵尸减少）或新进程创建。轻微不一致是正常的；大量不一致则提示内核模块有 bug。

---

## 6. 编译与部署

### 6.1 内核侧

```bash
# 放入源码树
cp include/linux/proc_monitor.h  ~/linux-6.18/include/linux/
cp kernel/proc_monitor.c         ~/linux-6.18/kernel/

# 注册编译
echo "obj-y += proc_monitor.o" >> ~/linux-6.18/kernel/Makefile

# 注册系统调用号 (syscall_64.tbl)
# 470  common  proc_collect   sys_proc_collect
# 471  common  proc_snapshot  sys_proc_snapshot
# 472  common  proc_stat      sys_proc_stat

# 编译 & 安装
cd ~/linux-6.18
make localmodconfig && make -j$(nproc) && make modules -j$(nproc)
sudo make modules_install && sudo make install
sudo update-grub && sudo reboot
```

### 6.2 用户态

```bash
sudo apt install libncurses-dev    # ncurses 开发库

cd OScoursework/user_app
make                                # 编译 proc_monitor
make test                           # 运行 syscall 验证
sudo ./proc_monitor                 # 启动 TUI (需 root)
```

### 6.3 Makefile

```makefile
CC      = gcc
CFLAGS  = -Wall -Wextra -O2 -std=c11
LDFLAGS = -lncursesw

TARGET  = proc_monitor
TEST    = test_syscall

all: $(TARGET)
$(TARGET): proc_monitor.o proc_monitor.h
	$(CC) $(CFLAGS) -o $@ proc_monitor.o $(LDFLAGS)
```

---

## 7. 调试指南

### 7.1 验证 struct 大小一致性

启动 `proc_monitor` 后观察 stderr 输出：
```
[DEBUG] struct sizes: proc_info=80  proc_tree_node=28  proc_stat=36  clk_tck=100
```

或切换到 SYSCALL 视图 (按 `5`)，查看 Struct Sizes 区域。如果用户态 sizeof 与内核不一致，说明头文件不同步或 ABI 不匹配。

### 7.2 检查 state 位编码

1. 切换到 LIST 视图，观察 `S:HEX` 列 — 显示原始 state 的十六进制值
2. 选中一个进程，查看 `win_detail` 面板 Line 1 — 完整解码 `__state` 和 `exit_state`
3. 使用 `=0xNN` 过滤器精确匹配特定状态组合

### 7.3 检查 syscall 性能

切换到 SYSCALL 视图 (按 `5`)，查看每个 syscall 的延迟：
- `proc_collect` 通常 < 2000μs（取决于进程数）
- `proc_snapshot` 类似耗时
- `proc_stat` 通常 < 100μs（只做计数）

如果延迟异常高，检查是否有大量进程或内核锁竞争。

### 7.4 检查 struct 内存布局

切换到 HEX-DUMP 视图 (按 `4`)，对比字段偏移标注与实际字节内容：
- 检查 padding 是否正确（偏移 28 和 76 应为 4 字节填充）
- 检查 `unsigned long` 字段是否在 8 字节对齐的地址上（偏移 32/40/48/56）
- 如果布局不对，说明编译选项 (LP64 vs ILP32) 或 struct 定义不一致

### 7.5 导出原始数据离线分析

```bash
# CSV 导出 (LIST/DEBUG 视图中按 x)
cat proc_list.csv | head -5

# DOT 导出 (TREE 视图中按 x)
dot -Tpng proc_tree.dot -o tree.png

# 二进制 dump (HEX/SYSCALL 视图中按 x)
xxd proc_dump.bin | head -20
# Header: magic="PROC"(4B) + count(4B) + struct_size(4B) + g_procs[N]
```

### 7.6 交叉验证

在 SYSCALL 视图中检查 Cross-check 区域。如果 `proc_info[]` 自己统计的值与 `proc_stat` 报告的值持续不一致（而非偶发），可能原因：

1. `sys_proc_stat` 的分类逻辑有 bug（如 `__state` vs `exit_state` 混淆）
2. 两次 syscall 之间的时间窗口过大（检查 latency 值）
3. `MAX_PROCS` 不够大，部分进程被截断

---

## 附录 A: 内核状态位参考

```
__state bits (include/linux/sched.h):
  0x0000  TASK_RUNNING         可运行/正在运行
  0x0001  TASK_INTERRUPTIBLE   可中断睡眠
  0x0002  TASK_UNINTERRUPTIBLE 不可中断睡眠 (D 状态)
  0x0004  __TASK_STOPPED       已停止 (SIGSTOP)
  0x0008  __TASK_TRACED        被跟踪 (ptrace)
  0x0040  TASK_DEAD            已死亡 (调度中)
  0x0080  TASK_WAKEKILL        可被致命信号唤醒
  0x0100  TASK_WAKING          正在被唤醒
  0x0200  TASK_PARKED          已停驻
  0x0400  TASK_NOLOAD          不计入负载
  0x0800  TASK_NEW             新创建

exit_state bits:
  0x0010  EXIT_DEAD            进程已完全销毁
  0x0020  EXIT_ZOMBIE          僵尸进程
  0x0040  EXIT_TRACE           退出跟踪中
```

## 附录 B: proc_info 字段与 /proc/pid/stat 对照

| proc_info 字段 | /proc/pid/stat 字段 | 索引 | 单位 |
|:---|:---|:---|:---|
| `pid` | pid | 1 | — |
| `comm` | comm | 2 | 字符串 |
| `state` | state | 3 | 字符 (合并了 __state 和 exit_state) |
| `ppid` | ppid | 4 | — |
| `utime` | utime | 14 | USER_HZ ticks |
| `stime` | stime | 15 | USER_HZ ticks |
| `nice` | nice | 19 | -20~19 |
| `num_threads` | num_threads | 20 | — |
| `vsize` | vsize | 23 | 字节 |
| `rss` | rss | 24 | 页数 (× PAGE_SIZE = 字节) |
| `uid` | (来自 /proc/pid/status) | — | — |

---

## 作者

Arch1mboldi · 2026年春季操作系统课程设计
