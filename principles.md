# 进程全量监控系统 —— 原理与实现

> 操作系统课程设计 · 实验三  
> Linux 6.18 内核 · 自定义系统调用 (470/471/472)  
> 2026年7月

---

## 目录

1. [系统架构](#1-系统架构)
2. [开发者速览 (Developer Quick Start)](#2-开发者速览-developer-quick-start)
   - [2.1 项目概览](#21-项目概览)
   - [2.2 两端约定 (Frontend/Backend Contract)](#22-两端约定-frontendbackend-contract)
   - [2.3 必知的操作系统概念](#23-必知的操作系统概念)
   - [2.4 常见陷阱 (Gotchas)](#24-常见陷阱-gotchas)
3. [数据结构：内核-用户态 ABI 契约](#3-数据结构内核-用户态-abi-契约)
4. [内核侧：3 个系统调用](#4-内核侧3-个系统调用)
   - [4.1 sys_proc_collect (470) — 进程全量采集](#41-sys_proc_collect-470--进程全量采集)
   - [4.2 sys_proc_snapshot (471) — 进程树快照](#42-sys_proc_snapshot-471--进程树快照)
   - [4.3 sys_proc_stat (472) — 聚合统计](#43-sys_proc_stat-472--聚合统计)
5. [内核关键机制](#5-内核关键机制)
   - [5.1 锁策略：lock-drop-copy-relock](#51-锁策略lock-drop-copy-relock)
   - [5.2 进程状态编码](#52-进程状态编码)
   - [5.3 CPU 时间采集](#53-cpu-时间采集)
   - [5.4 内存信息采集](#54-内存信息采集)
   - [5.5 安全性保证](#55-安全性保证)
6. [用户态：ncurses 调试版 TUI](#6-用户态ncurses-调试版-tui)
   - [6.1 面板布局](#61-面板布局)
   - [6.2 系统调用层与耗时测量](#62-系统调用层与耗时测量)
   - [6.3 状态解码算法](#63-状态解码算法)
   - [6.4 CPU% 计算](#64-cpu-计算)
   - [6.5 五视图模式](#65-五视图模式)
   - [6.6 过滤引擎](#66-过滤引擎)
   - [6.7 进程树渲染](#67-进程树渲染)
   - [6.8 数据导出](#68-数据导出)
   - [6.9 交叉验证](#69-交叉验证)
7. [编译与部署](#7-编译与部署)
8. [调试指南](#8-调试指南)
附录 A. [内核状态位参考](#附录-a-内核状态位参考)
附录 B. [proc_info 字段与 /proc/pid/stat 对照](#附录-b-proc_info-字段与-procpidstat-对照)
附录 C. [Bugs Fixed & Lessons Learned](#附录-c-bugs-fixed--lessons-learned)

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

## 2. 开发者速览 (Developer Quick Start)

> 如果你只有 5 分钟，读这一节就够了。其余章节是详细参考手册。

### 2.1 项目概览

本项目的目标是**监控 Linux 系统中所有进程的状态**，像一个自制的 `htop`。

| 组件 | 位置 | 角色 |
|:---|:---|:---|
| **后端 (内核模块)** | `kernel/proc_monitor.c` | 3 个自定义系统调用，遍历 `task_struct` 链表采集数据 |
| **前端 (用户态 TUI)** | `user_app/proc_monitor.c` | ncurses 终端界面，5 种视图模式展示数据 |
| **ABI 头文件** | `include/linux/proc_monitor.h` ↔ `user_app/proc_monitor.h` | 结构体定义，两边必须逐字节一致 |
| **数据通道** | `syscall(470/471/472)` → `copy_to_user()` | 内核栈 → 用户态 buffer，单向流动 |

关键数字: **3** 个系统调用 · **3** 个核心结构体 (80B / 28B / 36B on x86_64) · **5** 种视图 · **8192** 进程上限。

### 2.2 两端约定 (Frontend/Backend Contract)

前后端通过**二进制 ABI** 通信。理解这 3 条规则即可安全修改代码：

**规则 1: 结构体定义必须逐字节一致**

```
内核: include/linux/proc_monitor.h  ┐
                                    ├─ 字段名、类型、顺序、对齐完全一致
用户: user_app/proc_monitor.h       ┘
```

修改任何 `struct proc_info` / `proc_tree_node` / `proc_stat` 时，**两边必须同时改**。用 HEX-DUMP 视图 (按 `4`) 验证内存布局。sizeof 不一致会导致数据错位、字段读取乱码。

**规则 2: 数据单向流动，通过 `copy_to_user()`**

```
内核 task_struct ──提取字段──► proc_info (内核栈)
        └── copy_to_user() ──► 用户态 g_procs[]
```

- 内核**绝不**向用户态暴露指针 (`task_struct*`、`mm*` 等内核地址)
- `from_kuid_munged()` 将内核 UID 映射到调用者命名空间
- `nsec_to_clock_t()` 将纳秒 CPU 时间转为 USER_HZ 滴答（与 `/proc/pid/stat` 一致）

**规则 3: 状态编码必须两端一致解释** (详见 [§5.2](#52-进程状态编码) + [§C.4](#c4-状态分类失配-state-classification-mismatch))

内核传递 `state = task->__state | task->exit_state`。这是一个**位掩码**，不是枚举值。附加标志位 (`TASK_WAKEKILL=0x80`, `TASK_NOLOAD=0x400`) 会 OR 到基础状态上 (e.g. `0x01 | 0x80 = 0x81`)。**内核分类和用户态解码都必须先用掩码清除标志位**，再用位检测 (`& 0x03`) 判断基础状态。精确值 `switch(state)` 会漏掉所有带标志位的进程——曾经修过这个 bug。

### 2.3 必知的操作系统概念

在动手修改代码前，至少了解以下 6 个概念。

**`task_struct`** — 内核描述一个进程/线程的结构体，包含 PID、状态、CPU 时间、内存、父进程指针等所有信息。本项目本质上是 `task_struct` 的"远程查看器"。参见内核源码 `include/linux/sched.h`。

**RCU (Read-Copy-Update) + `tasklist_lock`** — 进程链表通过 RCU 保护。读端用 `read_lock(&tasklist_lock)` 加锁。**关键约束: RCU 临界区内不能睡眠**——如果持锁期间 `copy_to_user()` 触发缺页异常，会睡眠等待磁盘 I/O，破坏 RCU 语义甚至死锁。这就是 "lock-drop-copy-relock" 模式存在的根本原因。参见 `Documentation/RCU/listRCU.rst`。

**`copy_to_user()` 可能睡眠** — 用户态 buffer 所在页面可能被换出 (swap)。访问时触发缺页异常 → 磁盘 I/O → 睡眠。因此 **绝不能在持有自旋锁或 RCU 读锁时调用 `copy_to_user()`**。正确做法: 持锁读取数据 → 保存 next 指针 → 释放锁 → `copy_to_user()` → 重新加锁 → 用保存的 next 继续遍历。

**进程状态位掩码** — Linux 6.x 将状态拆分为 `__state` (调度状态) 和 `exit_state` (退出状态):
```
__state:     0x00=RUNNING  0x01=INTERRUPTIBLE  0x02=UNINTERRUPTIBLE
             0x04=STOPPED  0x08=TRACED  0x40=DEAD
             0x80=WAKEKILL (标志)  0x100=WAKING (标志)  0x400=NOLOAD (标志)
exit_state:  0x10=EXIT_DEAD  0x20=EXIT_ZOMBIE
```
标志位会 OR 到基础状态上: `TASK_INTERRUPTIBLE | TASK_WAKEKILL = 0x0081`。**必须掩码清除标志位后才能 switch 基础状态**（内核用 `s &= ~(TASK_WAKEKILL|TASK_WAKING)`，用户态用 `state & 0x03`）。

**USER_HZ / 时钟滴答** — CPU 时间以"时钟滴答"(jiffies) 为单位。`sysconf(_SC_CLK_TCK)` 获取，通常为 100 (每滴答 10ms)。内核用 `nsec_to_clock_t()` 将纳秒转为滴答，用户态用 `cpu_pct()` 计算相邻两帧之间的 CPU%。参见 `man 5 proc` 中 `/proc/pid/stat` 的 (14)(15) 字段。

**内核线程** — `task->mm == NULL` 表示没有用户态地址空间。内核线程的 `vsize=0, rss=0`，单独计入 `kernel_threads`。用户态通过检查 `mm != NULL` 区分。参见 `Documentation/mm/active_mm.rst`。

### 2.4 常见陷阱 (Gotchas)

| # | 陷阱 | 表现 | 教训 | 详见 |
|---|------|------|------|------|
| 1 | 排序度量与显示不一致 | 按 CPU% 列排序但实际按累计滴答排 | `cmp_cpu()` 必须用 delta-based CPU%，不是 `utime+stime` | [§C.1](#c1-cpu-排序度量错误-cpu-sort-metric) |
| 2 | 按键只处理小写 | 按 `V` 无法切换视图 | ncurses 区分大小写，`case 'V':` 也要加 | [§C.2](#c2-v-键视图切换失效-v-key-not-switching) |
| 3 | 状态变量跨视图复用 | HEX-DUMP 中上下键切换进程而非滚动 | 不同视图的"光标"含义不同，需独立状态变量 | [§C.3](#c3-hex-dump-导航混乱-hex-dump-navigation) |
| 4 | 精确匹配状态值 | proc_stat 与 proc_info 交叉验证大量 MISMATCH | 必须掩码清除标志位，不能 `switch(state)` 精确匹配 | [§C.4](#c4-状态分类失配-state-classification-mismatch) |

---

## 3. 数据结构：内核-用户态 ABI 契约

三个结构体定义在 `include/linux/proc_monitor.h`（内核侧）和 `user_app/proc_monitor.h`（用户态侧）。
两边定义**必须逐字节一致**——struct 大小、字段偏移、对齐方式完全相同，构成二进制 ABI 契约。

### 3.1 `struct proc_info` — 进程完整信息

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

### 3.2 `struct proc_tree_node` — 进程树节点

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

### 3.3 `struct proc_stat` — 聚合统计

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

## 4. 内核侧：3 个系统调用

系统调用号在 `arch/x86/entry/syscalls/syscall_64.tbl` 中注册：

```
470  common  proc_collect   sys_proc_collect
471  common  proc_snapshot  sys_proc_snapshot
472  common  proc_stat      sys_proc_stat
```

### 4.1 sys_proc_collect (470) — 进程全量采集

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

### 4.2 sys_proc_snapshot (471) — 进程树快照

**签名**: `long sys_proc_snapshot(struct proc_tree_node *user_buf, int max_count, int *ret_count)`

**功能**: 返回进程树拓扑数据，每个节点包含 `pid/ppid/comm/level`。

**level 计算** (`compute_level()`):
```
从 task->real_parent 向上走，直到 init_task：
  task → parent → grandparent → ... → init_task
  每步 level++，初始 level=0
```

用户态可基于 `pid/ppid` 自行重建树结构，也可直接使用 `level` 按缩进渲染。

### 4.3 sys_proc_stat (472) — 聚合统计

**签名**: `long sys_proc_stat(struct proc_stat *stat)`

**功能**: 一次性返回按状态分类的进程计数。

**分类逻辑**:

```
for_each_process(task):
    total_processes++

    if (task->exit_state & EXIT_ZOMBIE)     → zombie_processes++
    else if (task->exit_state & EXIT_DEAD)  → (skip, 不计入活跃分类)
    else:
        unsigned int s = task->__state;

        if (s & TASK_NOLOAD):
            // TASK_IDLE = UNINTERRUPTIBLE | NOLOAD, 语义上属空闲
            idle_processes++
        else:
            s &= ~(TASK_WAKEKILL | TASK_WAKING);  // 清除标志位
            switch (s):
                TASK_RUNNING             → running_processes++
                TASK_INTERRUPTIBLE       → sleeping_processes++
                TASK_UNINTERRUPTIBLE     → uninterruptible++
                __TASK_STOPPED / __TASK_TRACED → stopped_processes++
                default                  → idle_processes++

    if (task->mm == NULL) → kernel_threads++
    else                  → user_threads++
```

> **为什么必须用位掩码而不是精确匹配**: Linux 6.x 的 `__state` 可能带有额外标志位，如 `TASK_WAKEKILL (0x0080)`、`TASK_WAKING (0x0100)` 和 `TASK_NOLOAD (0x0400)`。一个可被杀死的睡眠进程 `__state = 0x01 | 0x80 = 0x0081`。若用精确 `switch(task->__state)` 匹配，`0x0081` 不等于 `TASK_INTERRUPTIBLE (0x0001)`，会落入 `default` 分支被误判为 idle。掩码策略: 先检测 `TASK_NOLOAD`（因为 `TASK_IDLE` 语义上就是空闲），再 `s &= ~(TASK_WAKEKILL|TASK_WAKING)` 清除干扰位，最后 switch 匹配基础状态。早期版本因未做此处理导致 proc_stat 与 proc_info 交叉验证出现大量 MISMATCH。详见 [附录 C.4](#c4-状态分类失配-state-classification-mismatch)。

> **注意**: `for_each_process()` 使用 `read_lock(&tasklist_lock)` 持锁遍历，全程持锁不释放——与 collect/snapshot 的锁策略不同，因为此函数不在持锁期间调用可能睡眠的 `copy_to_user()`。

---

## 5. 内核关键机制

### 5.1 锁策略：lock-drop-copy-relock

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

### 5.2 进程状态编码

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


> **两侧问题 (Kernel + Userspace)**: 相同的标志位陷阱同时影响内核和用户态。内核
> `sys_proc_stat` 也必须掩码清除 `TASK_WAKEKILL` 和 `TASK_WAKING`，否则
> `state=0x0081` 会落入 `default` → idle（详见 [§4.3](#43-sys_proc_stat-472--聚合统计)）。
> 用户态交叉验证代码同步做了同样的掩码处理（先检查 `TASK_NOLOAD`，再
> `st & ~0x0080` 清除 WAKEKILL，最后 `& 0x03` 分类）。这不是可选优化，
> 而是 **Linux 6.x 上正确分类的必要条件**。早期版本因两侧都未掩码，
> 导致交叉验证出现大量 MISMATCH（详见 [§C.4](#c4-状态分类失配-state-classification-mismatch)）。

### 5.3 CPU 时间采集

发行版内核 (`CONFIG_VIRT_CPU_ACCOUNTING_GEN=y`) 以**纳秒**精度存储 CPU 时间。直接读 `task->utime` 会得到纳秒值，而非传统的 jiffies。

**采集流程**:

```
task_cputime(task, &ut_ns, &st_ns)  → 纳秒精度 CPU 时间
nsec_to_clock_t(ut_ns)              → USER_HZ 时钟滴答 (与 /proc/pid/stat 一致)
```

- `task_cputime()`: 读取 utime + stime，单位纳秒
- `nsec_to_clock_t()`: 转换为 `USER_HZ` 滴答，值 = `sysconf(_SC_CLK_TCK)`（通常 100）

### 5.4 内存信息采集

| 字段 | 采集方式 | 单位 | 与 /proc/pid/stat 关系 |
|:---|:---|:---|:---|
| `vsize` | `task->mm->total_vm << PAGE_SHIFT` | 字节 | 对应 /proc/pid/stat 的 vsize (23) |
| `rss` | `get_mm_rss(task->mm)` | 页数 (4KB/页) | 对应 /proc/pid/stat 的 rss (24)，但 /proc 输出需 × PAGE_SIZE |

**内核线程**: `task->mm == NULL` → 无用户态地址空间 → `vsize = 0, rss = 0`

### 5.5 安全性保证

| 安全措施 | 机制 |
|:---|:---|
| 杜绝内核指针泄露 | 所有数据通过 `copy_to_user()` 传递，不暴露 `task_struct*` 等内核地址 |
| 杜绝 UAF | 持锁保存 next 指针 → 释放锁 → 用保存的指针继续（见 §4.1） |
| 杜绝栈溢出 | 逐个条目 copy_to_user，不在内核栈分配大数组 |
| 用户身份映射 | `from_kuid_munged()` 将内核 UID 映射到调用者命名空间 |
| 参数校验 | 所有 `__user` 指针在解引用前检查 NULL |

---

## 6. 用户态：ncurses 调试版 TUI

### 6.1 面板布局

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

### 6.2 系统调用层与耗时测量

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

### 6.3 状态解码算法

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

### 6.4 CPU% 计算

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

### 6.5 五视图模式

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

### 6.6 过滤引擎

过滤语法（在 `/` 键进入的过滤模式下输入）：

| 语法 | 含义 | 实现 |
|:---|:---|:---|
| `bash` | 进程名包含 "bash" (大小写不敏感) | `strstr(lower(name), lower(filter))` |
| `=R` | state 字符匹配 (R/S/D/T/Z/X/t) | `state_char(p->state) == filter[1]` |
| `=0x22` | state 位掩码精确匹配 (调试用) | `p->state == strtol("=0x22", NULL, 16)` |
| `:1234` | PID 精确匹配 | `p->pid == atoi(":1234")` |

过滤模式下 `filtered_count()` 和 `filtered_index(n)` 用于统计过滤后数量和定位第 n 个可见条目。

### 6.7 进程树渲染

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

### 6.8 数据导出

按 `x` 键，根据当前视图模式选择导出格式：

| 视图 | 导出格式 | 文件名 (默认) | 内容 |
|:---|:---|:---|:---|
| LIST / DEBUG | CSV | `proc_list.csv` | 所有进程的 proc_info 字段, 含 clk_tck 头 |
| TREE | DOT (Graphviz) | `proc_tree.dot` | 进程树有向图, 可 `dot -Tpng` 渲染 |
| HEX / SYSCALL | 二进制 dump | `proc_dump.bin` | magic("PROC") + count + struct_size + g_procs 原始字节 |

### 6.9 交叉验证

SYSCALL 视图底部包含交叉验证：用 `proc_info[]` 数组自己数出来的状态分布 vs `proc_stat` 报告的值。

```
Cross-check:
R: proc_info=12    proc_stat=12    OK
S: proc_info=280   proc_stat=280   OK
D: proc_info=5     proc_stat=5     OK
Z: proc_info=3     proc_stat=2     MISMATCH!
I: proc_info=8     proc_stat=8     OK
```

**出现 MISMATCH 的原因**: `fetch_procs()` 和 `fetch_stat()` 是两个独立的系统调用，存在时间窗口。在这两次调用之间，可能有进程退出（僵尸减少）或新进程创建。轻微的偶发不一致是正常的；**系统性、持续的大量 MISMATCH 则提示状态分类有 bug**——最常见的原因是内核或用户态使用了精确值匹配而未掩码清除 `TASK_WAKEKILL` / `TASK_NOLOAD` 等标志位（详见 [§C.4](#c4-状态分类失配-state-classification-mismatch)）。

---

## 7. 编译与部署

### 7.1 内核侧

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

### 7.2 用户态

```bash
sudo apt install libncurses-dev    # ncurses 开发库

cd OScoursework/user_app
make                                # 编译 proc_monitor
make test                           # 运行 syscall 验证
sudo ./proc_monitor                 # 启动 TUI (需 root)
```

### 7.3 Makefile

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

## 8. 调试指南

### 8.1 验证 struct 大小一致性

启动 `proc_monitor` 后观察 stderr 输出：
```
[DEBUG] struct sizes: proc_info=80  proc_tree_node=28  proc_stat=36  clk_tck=100
```

或切换到 SYSCALL 视图 (按 `5`)，查看 Struct Sizes 区域。如果用户态 sizeof 与内核不一致，说明头文件不同步或 ABI 不匹配。

### 8.2 检查 state 位编码

1. 切换到 LIST 视图，观察 `S:HEX` 列 — 显示原始 state 的十六进制值
2. 选中一个进程，查看 `win_detail` 面板 Line 1 — 完整解码 `__state` 和 `exit_state`
3. 使用 `=0xNN` 过滤器精确匹配特定状态组合

### 8.3 检查 syscall 性能

切换到 SYSCALL 视图 (按 `5`)，查看每个 syscall 的延迟：
- `proc_collect` 通常 < 2000μs（取决于进程数）
- `proc_snapshot` 类似耗时
- `proc_stat` 通常 < 100μs（只做计数）

如果延迟异常高，检查是否有大量进程或内核锁竞争。

### 8.4 检查 struct 内存布局

切换到 HEX-DUMP 视图 (按 `4`)，对比字段偏移标注与实际字节内容：
- 检查 padding 是否正确（偏移 28 和 76 应为 4 字节填充）
- 检查 `unsigned long` 字段是否在 8 字节对齐的地址上（偏移 32/40/48/56）
- 如果布局不对，说明编译选项 (LP64 vs ILP32) 或 struct 定义不一致

### 8.5 导出原始数据离线分析

```bash
# CSV 导出 (LIST/DEBUG 视图中按 x)
cat proc_list.csv | head -5

# DOT 导出 (TREE 视图中按 x)
dot -Tpng proc_tree.dot -o tree.png

# 二进制 dump (HEX/SYSCALL 视图中按 x)
xxd proc_dump.bin | head -20
# Header: magic="PROC"(4B) + count(4B) + struct_size(4B) + g_procs[N]
```

### 8.6 交叉验证

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

## 附录 C: Bugs Fixed & Lessons Learned

本附录记录课程设计开发过程中已修复的 bug，每个条目包含症状、根因、修复、教训，
供后续开发者参考。

### C.1 CPU% 排序度量错误 (CPU Sort Metric)

| 属性 | 详情 |
|:---|:---|
| **提交** | `f76bcf6` |
| **文件** | `user_app/proc_monitor.c` (`cmp_cpu`) |
| **症状** | 按 Tab 切换到"按 CPU% 排序"，显示顺序与 CPU% 列不一致。gnome-terminal (CPU%=3.0) 排在 gnome-shell (CPU%=10.8) 前面 |
| **根因** | `cmp_cpu` 比较的是 `utime + stime` 累计滴答数（进程启动以来的总 CPU 时间），而非 `cpu_pct()` 计算的增量百分比。长运行时间的老进程自然有更高的累计值 |
| **修复** | `cmp_cpu` 改为调用 `cpu_pct(pid, utime + stime)`，比较两帧之间的 delta-based CPU% |
| **教训** | 当 UI 显示一个计算值（如百分比、速率）时，排序比较器必须使用相同的计算函数，而非原始累加值。排序度量必须与显示度量一致 |

### C.2 V 键视图切换失效 (V Key Not Switching)

| 属性 | 详情 |
|:---|:---|
| **提交** | `f76bcf6` |
| **文件** | `user_app/proc_monitor.c` |
| **症状** | 按 Shift+V 无法循环切换视图，但按 `1`-`5` 数字键可以。提示栏显示小写 `v`，按大写 V 无响应 |
| **根因** | 按键处理 switch 中只写了 `case 'v':`，未处理 `case 'V':`。ncurses 的 `wgetch()` 区分大小写 |
| **修复** | 添加 `case 'V':` 与 `case 'v':` 并列执行相同逻辑 |
| **教训** | 处理键盘输入时，对无修饰字母键应同时检查大小写，除非有明确理由区分。这是一个在 ncurses 程序中极易被忽略的细节 |

### C.3 HEX-DUMP 导航混乱 (HEX-DUMP Navigation)

| 属性 | 详情 |
|:---|:---|
| **提交** | `f76bcf6` |
| **文件** | `user_app/proc_monitor.c` (`render_hex_view`, `main`) |
| **症状** | 在 HEX-DUMP 视图中按上下键：期望滚动 hex dump 内容，实际效果是切换了正在被 dump 的进程。且只能查看前几个进程的 hex dump |
| **根因** | 全局变量 `g_selected` 被复用为两个正交概念：(a) 选中哪个进程（传给 `filtered_index()` 定位进程），(b) hex dump 中高亮哪一行。上下键改变 `g_selected` → 同时改变了进程选择 |
| **修复** | 引入独立变量 `g_hex_selected` 跟踪被 dump 的进程（在过滤后列表中的索引）。`g_selected` 在 HEX-DUMP 视图中仅表示 hex 行光标。添加 `n`/`p` 键切换进程。进入/离开 HEX-DUMP 时在两者间同步 |
| **教训** | 不同视图的"光标"有不同语义（进程索引 vs 字节偏移 vs 树节点）。绝不要用一个变量表示两个正交维度。视图切换时需显式管理状态变量的语义转换 |

### C.4 状态分类失配 (State Classification Mismatch)

| 属性 | 详情 |
|:---|:---|
| **提交** | `c463243` |
| **文件** | `kernel/proc_monitor.c` (`sys_proc_stat`), `user_app/proc_monitor.c` (cross-check) |
| **症状** | SYSCALL 视图交叉验证显示大量系统性 MISMATCH：`S: proc_info=224 proc_stat=211`（差 13），`D: proc_info=154 proc_stat=0`（差 154），`I: proc_stat=167 proc_info=0`。每次刷新都复现，不是偶发 |
| **根因** | 内核 `sys_proc_stat` 使用精确值 `switch(task->__state)` 分类。Linux 6.x 的 `__state` 常带有标志位：`TASK_INTERRUPTIBLE|TASK_WAKEKILL = 0x0081`、`TASK_IDLE = 0x0402`。这些值不等于任何 case 标签（`TASK_INTERRUPTIBLE = 0x0001`、`TASK_UNINTERRUPTIBLE = 0x0002`），全部落入 `default` → idle。同时用户态交叉验证也未处理 `TASK_NOLOAD`，把 `TASK_IDLE (0x0402)` 误判为 D (因为 `0x0402 & 0x03 == 2`) |
| **修复** | 内核：先检查 `TASK_NOLOAD` → idle，再 `s &= ~(TASK_WAKEKILL|TASK_WAKING)` 掩码，最后 switch 基础状态。用户态：同步增加 `0x0400` (NOLOAD) 检测和 `~0x0080` (WAKEKILL) 掩码 |
| **教训** | Linux 5.14+ 分离 `__state` 和 `exit_state` 后，`__state` 变为位掩码而非枚举——这是影响面极大的架构变更。**内核和用户态两侧必须用一致的掩码策略**，精确 `switch(state)` 在 6.x 内核上必然出错。`TASK_REPORT` 掩码 (0x007f) 可用于提取纯净的基础状态 |

### C.5 CPU% 时间戳记录位置错误 (CPU% Timestamp)

| 属性 | 详情 |
|:---|:---|
| **提交** | `623dfcc` |
| **文件** | `user_app/proc_monitor.c` (`save_prev_cpu`, `update_data`) |
| **症状** | 第一帧之后的 CPU% 值异常巨大（数百甚至数千百分比） |
| **根因** | 时间戳在 `save_prev_cpu()` 中记录（render 之前）。此时距 `cpu_pct()` 调用（render 期间）仅微秒级别，`elapsed ≈ 0` 导致 CPU% = `delta_ticks / (clk_tck * 0)` → 极大值 |
| **修复** | 将时间戳记录 (`gettimeofday`) 移到 `update_data()` 中。`elapsed` 现在反映两次 syscall 数据采集之间的真实间隔 |
| **教训** | CPU% = `delta_ticks / (clk_tck * delta_time)`。`delta_time` 必须是两次数据采集的间隔，不是两次渲染的间隔。时序逻辑须放在正确的时机——先采集数据（记录时间），再渲染（使用时间差） |


---

## 作者

Arch1mboldi · 2026年春季操作系统课程设计
