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
9. [深入：一次系统调用的内核之旅 (Syscall Deep Dive)](#9-深入一次系统调用的内核之旅-syscall-deep-dive)
   - [9.1 前置概念速查](#91-前置概念速查)
   - [9.2 CPU 视角：从 Ring 3 到 Ring 0](#92-cpu-视角从-ring-3-到-ring-0)
   - [9.3 内存视角：关键数据结构布局](#93-内存视角关键数据结构布局)
   - [9.4 锁机制详解：RCU + tasklist_lock](#94-锁机制详解rcu--tasklist_lock)
   - [9.5 copy_to_user() 内部机制](#95-copy_to_user-内部机制)
   - [9.6 完整执行轨迹：sys_proc_collect (470)](#96-完整执行轨迹sys_proc_collect-470)
   - [9.7 三个系统调用的差异对比](#97-三个系统调用的差异对比)
   - [9.8 关键指针与内存安全](#98-关键指针与内存安全)
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

| 模式              | 编号  | 用途                                                   | 数据来源                  |
| :-------------- | :-- | :--------------------------------------------------- | :-------------------- |
| **LIST**        | 1   | 紧凑表格, 含 state hex 原始值, CPU%, 11 字段                   | proc_info             |
| **DEBUG-TABLE** | 2   | 所有字段原始数值, 不做任何格式化                                    | proc_info             |
| **TREE**        | 3   | 进程树层次结构, Unicode 树线, 按深度着色                           | proc_tree_node        |
| **HEX-DUMP**    | 4   | 选中进程 proc_info 的原始字节 + 字段偏移标注                        | proc_info (raw bytes) |
| **SYSCALL**     | 5   | 系统调用诊断: ret/errno/latency/struct 大小/cross-validation | sc_result + proc_stat |

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

| 语法      | 含义                         | 实现                                      |
| :------ | :------------------------- | :-------------------------------------- |
| `bash`  | 进程名包含 "bash" (大小写不敏感)      | `strstr(lower(name), lower(filter))`    |
| `=R`    | state 字符匹配 (R/S/D/T/Z/X/t) | `state_char(p->state) == filter[1]`     |
| `=0x22` | state 位掩码精确匹配 (调试用)        | `p->state == strtol("=0x22", NULL, 16)` |
| `:1234` | PID 精确匹配                   | `p->pid == atoi(":1234")`               |
|         |                            |                                         |

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

## 9. 深入：一次系统调用的内核之旅 (Syscall Deep Dive)

> 本章将 §4-§5 中分散的概念串联起来，以 `sys_proc_collect(470)` 为主线，
> 从 CPU、内存、锁三个视角追踪一次系统调用的完整执行过程。

### 9.1 前置概念速查

在开始追踪之前，快速回顾涉及的每个内核概念：

| 概念 | 一句话定义 | 详见 |
|:---|:---|:---|
| **用户态 (Ring 3)** | CPU 的低权限模式，不能执行特权指令，不能直接访问内核内存 | x86_64 保护模式 |
| **内核态 (Ring 0)** | CPU 的高权限模式，可访问所有内存和硬件，执行特权指令 | x86_64 保护模式 |
| **`syscall` 指令** | x86_64 专用指令，原子地完成：Ring 3→Ring 0、保存 RIP/RFLAGS、跳转到内核入口 | `entry_SYSCALL_64` |
| **MSR (Model-Specific Register)** | CPU 配置寄存器。`MSR_LSTAR` 存内核入口地址，`MSR_GS_BASE` 存 per-CPU 数据基址 | x86_64 ABR |
| **内核栈** | 每个任务在内核态使用的独立栈（通常 16KB，`task_struct->stack`），与用户态栈分离 | `thread_union` |
| **`task_struct`** | 内核描述一个进程/线程的结构体（~7KB），含 PID、state、mm、tasks 链表节点等 | `include/linux/sched.h` |
| **`tasklist_lock`** | 保护进程链表 (`task_struct->tasks`) 的读写自旋锁，读者用 `read_lock()` | [§5.1](#51-锁策略lock-drop-copy-relock) |
| **RCU (Read-Copy-Update)** | 一种"读写不对称"的同步机制：读者几乎零开销，写者延迟回收旧数据。进程链表 RCU 保护 | `Documentation/RCU` |
| **`copy_to_user()`** | 内核函数，将内核数据安全拷贝到用户态地址空间，包含权限检查和缺页处理 | [§9.5](#95-copy_to_user-内部机制) |
| **SMAP (Supervisor Mode Access Prevention)** | x86 硬件特性：Ring 0 默认**禁止**访问用户态页。必须用 `stac` 指令临时放行 | CR4.SMAP 位 |
| **UAF (Use-After-Free)** | 访问已释放内存的 bug。本项目通过保存 `next_task()` 指针避免 UAF | [§5.1](#51-锁策略lock-drop-copy-relock) |

### 9.2 CPU 视角：从 Ring 3 到 Ring 0

当用户程序调用 `syscall(SYS_proc_collect, buf, max_count, &count)` 时，CPU 执行以下步骤：

**第 1 步 — glibc 封装**

```c
// glibc 的 syscall() 函数将参数放入寄存器，执行 syscall 指令
// x86_64 调用约定 (System V AMD64 ABI, Appendix A):
//   rax = 系统调用号 (470 = 0x1D6)
//   rdi = 第1个参数 (user_buf)
//   rsi = 第2个参数 (max_count)
//   rdx = 第3个参数 (ret_count)
```

```asm
mov    $470, %rax        # 系统调用号 → rax
mov    %rdi, %r10        # 第4参数用 r10 (syscall 指令会破坏 rcx)
syscall                  # ─── 从这里开始进入内核 ───
```

**第 2 步 — CPU 硬件动作 (`syscall` 指令)**

`syscall` 是一条**原子**指令，CPU 硬件在同一时钟周期内完成：

```
1. RCX ← RIP_next          (保存用户态返回地址)
2. R11 ← RFLAGS            (保存用户态标志寄存器)
3. CS  ← MSR_STAR[47:32]   (加载内核代码段选择子 → Ring 0)
4. SS  ← MSR_STAR[47:32]+8 (加载内核栈段选择子)
5. RIP ← MSR_LSTAR         (跳转到 entry_SYSCALL_64)
6. CPL ← 0                 (切换到 Ring 0)
```

关键 MSR 值（内核启动时设置）：

| MSR | 存储内容 | 本项目中对应的值 |
|:---|:---|:---|
| `MSR_LSTAR` (0xC0000082) | 系统调用入口地址 | `entry_SYSCALL_64` 的虚拟地址 |
| `MSR_STAR` (0xC0000081) | 用户/内核段选择子 | `__USER32_CS`, `__KERNEL_CS` 等 |
| `MSR_GS_BASE` (0xC0000101) | per-CPU 数据区基址 | 指向当前 CPU 的 `struct tss_struct` 等 |

**第 3 步 — 内核入口 `entry_SYSCALL_64`**

内核在 `arch/x86/entry/entry_64.S` 中定义入口：

```asm
entry_SYSCALL_64:
    swapgs                      # ① 交换 GS 基址: 用户GS↔内核GS
    mov    %rsp, PER_CPU(cpu_tss_rw + TSS_sp2)  # ② 保存用户栈指针
    mov    PER_CPU(cpu_top_of_stack), %rsp      # ③ 切换到内核栈
    pushq  $__USER_DS           # ④ 构造 pt_regs 帧
    pushq  PER_CPU(cpu_tss_rw + TSS_sp2)        #    (保存用户 SS)
    pushq  %r11                 #    (保存用户 RFLAGS)
    pushq  $__USER_CS           #    (保存用户 CS)
    pushq  %rcx                 #    (保存用户 RIP)
    pushq  %rax                 #    (保存 RAX=470, 也用作返回值)
    ... (保存其余寄存器) ...
    mov    %rsp, %rdi           # ⑤ rdi = pt_regs 指针 (传给 do_syscall_64)
    call   do_syscall_64        # ⑥ 进入 C 代码
```

此时 CPU 状态：

```
Ring:     0 (内核态)
RSP:      指向内核栈上的 pt_regs 帧
RAX:      470 (系统调用号)
RDI:      user_buf   (来自用户态的 proc_info* 指针)
RSI:      max_count  (来自用户态的 int)
RDX:      ret_count  (来自用户态的 int*)
GS_BASE:  per-CPU 内核数据结构
```

**第 4 步 — `do_syscall_64()` 分发**

```c
// arch/x86/entry/common.c (简化)
__visible void do_syscall_64(struct pt_regs *regs) {
    long nr = regs->ax;                        // 系统调用号 = 470
    if (nr < NR_syscalls) {
        sys_call_ptr_t fn = sys_call_table[nr]; // fn = sys_proc_collect
        regs->ax = fn(regs);                    // 调用实际处理函数
    }
}
```

`sys_call_table[470]` 在编译时由 `SYSCALL_DEFINE3(proc_collect, ...)` 宏填充——宏展开时自动生成 `sys_proc_collect` 并将函数指针填入表项。

### 9.3 内存视角：关键数据结构布局

当 `copy_to_user()` 执行时，以下是各数据结构在地址空间中的位置：

```
                    x86_64 虚拟地址空间 (48-bit)
    ┌────────────────────────────────────────────────────────────┐
    │                    用户态 (Ring 3)                          │
    │                                                            │
    │  proc_monitor 进程虚拟地址空间:                              │
    │  ┌──────────────────────────┐                               │
    │  │ [stack]                  │ ← 用户态栈 (RSP_usr)          │
    │  │ ...                      │                               │
    │  │ g_procs[MAX_PROCS]       │ ← .bss 段，~640KB             │
    │  │   = buf[] (proc_info*)   │   syscall 参数 rdi 指向这里    │
    │  │ ...                      │                               │
    │  │ &count (int*)            │ ← syscall 参数 rdx 指向这里    │
    │  └──────────────────────────┘                               │
    │                                                            │
    ├────────────────── syscall boundary ─────────────────────────┤
    │                                                            │
    │                    内核态 (Ring 0)                           │
    │                                                            │
    │  ┌──────────────────────────┐                               │
    │  │ [kernel stack]           │ ← task_struct->stack (16KB)   │
    │  │   pt_regs frame (~1KB)   │   entry_SYSCALL_64 压入       │
    │  │   local variables:       │                               │
    │  │     kinfo (80B)          │ ← 内核栈上的临时 proc_info     │
    │  │     next (*task_struct)  │ ← 保存的下一个 task 指针       │
    │  │     count (int)          │                               │
    │  │   ...                    │                               │
    │  ├──────────────────────────┤                               │
    │  │ direct mapping           │ ← 所有物理内存的 1:1 映射      │
    │  │ (0xffff888000000000+)    │   物理地址 N → 虚拟地址 N+基址 │
    │  │                          │                               │
    │  │  task_struct (PID=1)     │ ← 内核堆 (kmalloc-* 分配)     │
    │  │    ├─ tasks.next ────────┼──→ task_struct (PID=2)        │
    │  │    ├─ mm*                │                               │
    │  │    ├─ __state            │                               │
    │  │    ├─ utime, stime       │                               │
    │  │    └─ ...                │                               │
    │  │                          │                               │
    │  │  task_struct (PID=2)     │                               │
    │  │    ├─ tasks.next ────────┼──→ task_struct (PID=3)        │
    │  │    └─ ...                │                               │
    │  │                          │                               │
    │  │  ... (共 381 个节点)     │                               │
    │  │                          │                               │
    │  │  init_task               │ ← 链表哨兵 (静态分配)          │
    │  └──────────────────────────┘                               │
    └────────────────────────────────────────────────────────────┘
```

关键观察：

1. **用户态 `buf` 和内核 `kinfo` 在不同的地址空间**：`buf` 在用户态低地址（如 `0x7fff...`），`kinfo` 在内核栈高地址（如 `0xffffc900...`）。`copy_to_user()` 跨越这个边界。
2. **`task_struct` 在内核 direct mapping 区域**：地址形如 `0xffff8880...`，内核代码可以直接解引用（Ring 0 权限）。
3. **内核栈很小（16KB）**：这也是为什么不分配 `proc_info[8192]` 大数组在栈上——80B × 8192 = 640KB，远超 16KB。所以我们逐个拷贝。

### 9.4 锁机制详解：RCU + tasklist_lock

#### 为什么用 RCU

进程链表 (`task_struct->tasks`) 是典型的 **read-mostly** 数据结构：
- 读者极频繁：`ps`, `top`, `/proc` 每时每刻在遍历
- 写者极稀少：只有 `fork()` / `exit()` 时修改

RCU 优化读者——读者几乎零同步开销。写者则需等待"宽限期"后释放旧版本。

#### `read_lock(&tasklist_lock)` 内部

```c
// 简化：在 x86_64 上等价于
preempt_disable();           // 禁止内核抢占
atomic_inc(&lock->readers);  // 读者计数 +1
// 此时写者必须等待 readers 降为 0 才能获取写锁
```

`preempt_disable()` 是关键——读者不能被调度走，因为：
- 如果读者被抢占后睡眠 1 秒，写者必须等待 1 秒
- RCU 宽限期依赖"所有 CPU 都经历一次调度"——被禁抢占的 CPU 不会调度

#### 为什么 RCU 读者不能睡眠

```
假设读者持锁时睡眠:
  read_lock(&tasklist_lock);
  ... 睡眠 (等待 I/O) ...
  [其他 CPU 上的写者调用 write_lock(&tasklist_lock) — 死等!]
  ... 1秒, 2秒, 10秒 ...
  [系统基本卡死]
```

`copy_to_user()` 可能因缺页触发 I/O → 睡眠。这就是 **lock-drop-copy-relock** 模式的必要性。

#### Lock-Drop-Copy-Relock 逐步推演

以遍历 381 个进程中的第 N 个为例：

```
时刻 T0: read_lock(&tasklist_lock);
          ┌────── 当前: 持锁, task = PID_N ──────┐
          │                                        │
          │  kinfo.pid  = task->pid;    // 读字段   │
          │  kinfo.state = task->__state |          │
          │                task->exit_state;        │
          │  kinfo.utime = nsec_to_clock_t(...);    │
          │  ... (读其余字段) ...                   │
          │                                        │
T1:       │  next = next_task(task);   // 保存后继!  │
          │  // 关键: 此刻 task 和 next 都有效       │
          │  // 因为持锁, RCU 保证两者不过期         │
          └────────────────────────────────────────┘
T2:       read_unlock(&tasklist_lock);
          ┌────── 当前: 无锁 ──────────────────┐
          │                                     │
          │  // 危险期: task 可能被 free        │
          │  // 因为 exit() → RCU 回收!         │
          │  // 但 next 仍有效 (未退出或        │
          │  // RCU 宽限期未过)                  │
          │                                     │
T3:       │  copy_to_user(&buf[N], &kinfo, 80); │
          │  // 可能缺页 → 换入 swap → 睡眠     │
          │  // 睡眠安全: 因为我们没持锁!        │
          │  // 醒来后 kinfo (栈上) 仍然有效     │
          └─────────────────────────────────────┘
T4:       read_lock(&tasklist_lock);
          ┌────── 当前: 重新持锁 ──────────┐
          │                                 │
          │  task = next;  // 用保存的指针   │
          │  // 绝不: task = task->tasks.next│
          │  // 因为原 task 可能已被 free!   │
          │  count++;                       │
          └─────────────────────────────────┘
T5:       // 下一轮循环: task = PID_{N+1}
```

**为什么 `next = next_task(task)` 安全，`task->tasks.next` 不安全？**

```
read_lock 期间:
  task ──► PID_N (有效) ──.tasks.next──► PID_{N+1} (有效)
  next ──► 仍指向 PID_{N+1} ← 这是一个 task_struct*，单独保存

read_unlock 之后:
  task ──► PID_N (可能已退出! 内存可能被回收!)
  
  如果此时访问 task->tasks.next:
    = PID_N->tasks.next = UAF! ← 访问已释放的内存
  
  但我们用的是 next:
    next = PID_{N+1} ← 保存的是指针值，独立于 PID_N
    只要 PID_{N+1} 还没退出，next 就仍然有效
    RCU 宽限期保证 PID_{N+1} 不会毫无预警地消失
```

#### `sys_proc_stat` 为什么可以全程持锁

```c
SYSCALL_DEFINE1(proc_stat, struct proc_stat __user *, stat) {
    struct proc_stat kstat;
    memset(&kstat, 0, sizeof(kstat));
    
    read_lock(&tasklist_lock);
    for_each_process(task) {         // 持锁遍历
        kstat.total_processes++;     // 纯内存操作
        switch (...) { ... }         // 纯内存操作
    }
    read_unlock(&tasklist_lock);
    
    copy_to_user(stat, &kstat, sizeof(kstat));  // ← 唯一的拷贝，在锁外!
    return 0;
}
```

对比：`proc_collect` 每进程拷贝一次（N 次 × `copy_to_user`），必须反复释放锁。`proc_stat` 全遍历完只拷贝一次（1 次 × `copy_to_user`），全程持锁即可。

### 9.5 copy_to_user() 内部机制

`copy_to_user()` 不像名字暗示的那样只是一个 `memcpy`。它实际上做了：

```
copy_to_user(void __user *to, const void *from, unsigned long n)
  │
  ├─ 1. might_fault()            ← 标记"可能缺页", 调试用
  │
  ├─ 2. access_ok(to, n)         ← 权限检查 (见图解)
  │      └─ 验证 [to, to+n) 完全落在用户态地址空间
  │         (x86_64: to < TASK_SIZE_MAX = 0x800000000000)
  │
  ├─ 3. stac() / user_access_begin()  ← 临时关闭 SMAP
  │      └─ SMAP: Ring 0 默认禁止访问用户页, 必须显式放行
  │         stac = Set AC flag in RFLAGS
  │
  ├─ 4. copy_user_generic(to, from, n)  ← 实际 memcpy
  │      └─ 使用 REP MOVSB 或类似指令
  │      └─ 每条指令都可能触发缺页异常
  │
  ├─ 5. clac() / user_access_end() ← 重新启用 SMAP
  │
  └─ 6. return (n - bytes_copied)  ← 返回未拷贝的字节数 (0=成功)
```

#### `access_ok()` — 为什么需要权限检查

```
用户态地址空间 (x86_64):
  0x0000000000000000 ─ 0x00007FFFFFFFFFFF  ← 用户可访问
  0x0000800000000000 ─ 0xFFFFFFFFFFFFFFFF  ← 内核专用 ("非规范地址" 分隔)
  
access_ok(ptr, size) 检查:
  1. ptr 不是内核地址 (无 RCE 攻击 — 不允许用户说"帮我写到内核地址")
  2. ptr + size 不溢出 48 位地址空间
  3. ptr 指向的实际 VMA 有写权限 (某些架构在 access_ok 中做, x86_64 推迟到缺页时)
```

#### 为什么 copy_to_user() 可能睡眠

```
copy_user_generic() 执行中:
  
  MOV [rdi], rsi    ← rdi = 用户态 buf 地址, rsi = 内核 kinfo 数据
    │
    ├─ TLB 查询: rdi 对应的物理页?
    │    └─ TLB 未命中 → 页表遍历 (可能缺页)
    │
    ├─ 页表项 Present=0?
    │    ├─ 被 swap 换出 → do_swap_page() → 磁盘 I/O → 睡眠!
    │    ├─ Copy-on-Write → do_wp_page() → 分配新页
    │    └─ mmap 文件映射 → filemap_fault() → 磁盘 I/O → 睡眠!
    │
    └─ 页表项 Present=1, 但 Write=0?
         └─ 写保护 → do_wp_page() 处理
```

缺页处理路径 (`do_page_fault → handle_mm_fault → ...`) 可能：
- 分配物理页 (`alloc_pages`) — 不睡眠
- 磁盘 I/O (`swap_readpage`, `filemap_read`) — **睡眠**
- 等待内存回收 (`__GFP_DIRECT_RECLAIM`) — **睡眠**

这就是为什么内核文档反复强调：

> **"Do not call copy_to_user() while holding a spinlock or RCU read lock."**

### 9.6 完整执行轨迹：sys_proc_collect (470)

以下是一次 `syscall(470, buf, 8192, &count)` 的完整 20 步执行轨迹。
假设系统有 381 个进程。

```
┌──── 步骤 ────┬──── CPU/锁状态 ────┬──── 内存活动 ────────────────────┐
│              │                    │                                  │
│ 1. 用户态    │ Ring 3             │ 栈上准备 syscall 参数             │
│    glibc     │ 无锁               │ rax=470, rdi=buf, rsi=8192,      │
│              │                    │ rdx=&count                       │
│              │                    │                                  │
│ 2. syscall   │ Ring 3 → Ring 0    │ HW: RCX←RIP, R11←RFLAGS,        │
│    指令      │ 无锁               │ RIP←MSR_LSTAR, CPL←0             │
│              │                    │                                  │
│ 3. entry_    │ Ring 0             │ swapgs, 切换内核栈               │
│    SYSCALL_64│ 无锁               │ 压入 pt_regs (保存所有用户寄存器) │
│              │                    │ rsp 指向内核栈                   │
│              │                    │                                  │
│ 4. do_       │ Ring 0             │ 查 sys_call_table[470]           │
│    syscall_64│ 无锁               │ → 调用 sys_proc_collect()        │
│              │                    │                                  │
│ 5. sys_proc_ │ Ring 0             │ 校验参数: user_buf!=NULL,        │
│    collect   │ 无锁               │ max_count>0, ret_count!=NULL     │
│    入口      │                    │                                  │
│              │                    │                                  │
│ 6. 持锁      │ Ring 0             │ task = next_task(&init_task)     │
│    开始遍历  │ read_lock ✓        │ → 跳过哨兵, 拿到 PID=1           │
│              │ 禁用抢占           │                                  │
│              │                    │                                  │
│ 7. 读取      │ Ring 0             │ get_task_comm(kinfo.comm, task)  │
│    task_struct│ read_lock ✓       │ kinfo.pid = task_tgid_nr(task)   │
│    (PID=1)   │                    │ task_cputime() → u64 ut/st       │
│              │                    │ nsec_to_clock_t() → 滴答         │
│              │                    │ total_vm, rss, nice, threads, uid│
│              │                    │ kinfo.state = __state | exit_state│
│              │                    │ 共读取 ~12 个 task_struct 字段    │
│              │                    │                                  │
│ 8. 保存 next │ Ring 0             │ next = next_task(task)           │
│    释放锁    │ read_lock ✓        │ → task->tasks.next               │
│              │                    │ read_unlock(&tasklist_lock)      │
│              │                    │                                  │
│ 9. 拷贝到    │ Ring 0             │ access_ok(&buf[0], 80) → pass    │
│    用户态    │ 无锁 (!!)          │ stac()  ← 禁用 SMAP              │
│              │ 可被抢占           │ copy 80B: kinfo → buf[0]         │
│              │ 可能睡眠           │ clac()  ← 启用 SMAP              │
│              │                    │                                  │
│10. 重新持锁  │ Ring 0             │ read_lock(&tasklist_lock)        │
│    继续遍历  │ read_lock ✓        │ task = next (=PID=2)             │
│              │                    │ count++                          │
│              │                    │                                  │
│11-17.        │ ... 重复步骤 7-10 对 PID=2 到 PID=381 ...              │
│    循环      │ 每进程切换锁状态    │ 每进程 1 次 copy_to_user()       │
│    ×380次    │ 共 381 次          │ 共 381 × 80B = 30,480B 拷贝      │
│    更多      │ read_lock/unlock   │ 至多 381 个 task 遍历            │
│              │                    │                                  │
│18. 循环结束  │ Ring 0             │ task == &init_task (回到哨兵)    │
│              │ read_lock ✓        │ read_unlock(&tasklist_lock)      │
│              │                    │                                  │
│19. 写回 count│ Ring 0             │ copy_to_user(ret_count,          │
│              │ 无锁               │             &count, sizeof(int)) │
│              │                    │ → 写回用户态 &count              │
│              │                    │                                  │
│20. 返回      │ Ring 0 → Ring 3    │ sysretq: 恢复 RIP/RFLAGS/RSP     │
│    用户态    │ 无锁               │ rax = 0 (返回值)                 │
│              │                    │ buf[] 现已填入 381 个 proc_info  │
│              │                    │ count 已写入 381                 │
└──────────────┴────────────────────┴──────────────────────────────────┘
```

**时间估算**（假定 1GHz CPU, 381 个进程）：

| 阶段 | 每次耗时 | 381次总耗时 |
|:---|:---|:---|
| 读 task_struct 字段 (~12 次指针解引用) | ~0.5μs | ~190μs |
| `read_unlock` + `read_lock` 对 | ~0.1μs | ~38μs |
| `copy_to_user()` (80B, L1 cache) | ~0.2μs | ~76μs |
| `copy_to_user()` (80B, 缺页) | ~10ms | — (罕见) |
| **正常总耗时** | | **~300-500μs** |
| 如果每个进程都缺页 | | **~3.8 秒** (极罕见) |

这就是 SYSCALL 视图中 `proc_collect` 延迟通常在 300-2000μs 的原因。

### 9.7 三个系统调用的差异对比

| 维度 | collect (470) | snapshot (471) | stat (472) |
|:---|:---|:---|:---|
| **输出** | `struct proc_info[N]` | `struct proc_tree_node[N]` | `struct proc_stat` (1个) |
| **每进程拷贝量** | 80B | 28B | 0 (聚合后一次 36B) |
| **锁模式** | lock-drop-copy-relock | lock-drop-copy-relock | 全程持锁 |
| **`copy_to_user` 调用次数** | N+1 次 | N+1 次 | 1 次 |
| **遍历中能否睡眠** | 是 (锁外) | 是 (锁外) | 否 (全程持锁) |
| **内核栈峰值使用** | ~200B (kinfo + 局部变量) | ~200B (knode + 局部变量) | ~50B (仅计数器) |
| **典型延迟 (381进程)** | 300-2000μs | 300-1500μs | 50-100μs |
| **最大风险** | 遍历期间进程数变化 | 同 collect | 基本无风险 |

为什么 snapshot 比 collect 快：
- 输出结构更小 (28B vs 80B → `copy_to_user` 更快)
- 字段采集更简单 (不需要 `task_cputime`, `get_mm_rss` 等耗时函数)
- 不需要合并 `__state | exit_state`

为什么 stat 快得多：
- 只做整数计数，不拷贝内存
- 最后一次 `copy_to_user` 只拷贝 36B
- 全程持锁无需反复加锁/解锁

### 9.8 关键指针与内存安全

#### 用户态 buf 指针：信任但验证

```c
// 用户传入:
struct proc_info *user_buf = malloc(sizeof(struct proc_info) * 8192);
syscall(470, user_buf, 8192, &count);

// 内核收到的是一个"裸"指针值 (如 0x7f1234560000)
// 内核不能直接假设它有效!
```

内核安全处理链条：

```
user_buf (用户态指针, 值=0x7f1234560000)
  │
  ├─ 1. 不直接解引用 ← 硬件 SMAP 阻止
  │
  ├─ 2. access_ok(user_buf, max_count * 80)
  │      └─ 验证: 地址在用户空间 (≤ 0x800000000000)
  │      └─ 验证: 地址 + 大小不溢出
  │
  ├─ 3. copy_to_user(&user_buf[i], &kinfo, 80)
  │      └─ stac() 临时禁用 SMAP
  │      └─ 逐字节拷贝 (缺页时内核代为处理)
  │      └─ clac() 重新启用 SMAP
  │
  └─ 4. 如果 user_buf[i] 的页没有映射?
         → 内核缺页处理 → 检查 VMA → 
           如果是合法 VMA: 分配物理页, 继续拷贝
           如果是非法地址: 发送 SIGSEGV 给进程
```

#### 内核指针绝不泄露到用户态

```c
// 什么内核不做的 (对比错误做法):
kinfo.internal_ptr = task;               // ❌ task_struct* 泄露!
kinfo.mm_ptr       = task->mm;           // ❌ mm_struct* 泄露!
kinfo.stack_ptr    = task->stack;        // ❌ 内核栈地址泄露!

// 什么内核做的:
kinfo.pid  = task_tgid_nr(task);         // ✓ 整数 PID
kinfo.uid  = from_kuid_munged(...);      // ✓ 整数 UID (经过命名空间映射)
kinfo.utime = nsec_to_clock_t(ut_ns);    // ✓ 整数 CPU 时间
kinfo.vsize = task->mm ? task->mm->total_vm << PAGE_SHIFT : 0;  // ✓ 整数大小
```

所有给用户态的数据都是**值类型**（整数、字符数组），不包含任何指针。

#### 内核栈限制：为什么不分配大数组

```c
// 错误做法 (本项目绝不这样写!):
SYSCALL_DEFINE3(proc_collect, ...) {
    struct proc_info local_array[8192];  // 80 × 8192 = 655,360 字节
    // 内核栈只有 16KB! → 栈溢出 → 内核 panic!
    ...
}

// 正确做法:
SYSCALL_DEFINE3(proc_collect, ...) {
    struct proc_info kinfo;  // 80 字节 — 刚好
    // 循环: 填充 kinfo → copy_to_user → 下一个
    ...
}
```

x86_64 内核栈限制：
- 总大小：`THREAD_SIZE = 16KB` (4 pages)
- 可用空间：~12KB（`pt_regs` 和中断帧占约 4KB）
- 超过限制 → `stack overflow` → 内核 panic (不可恢复)
- 即使递归也要用 `CONFIG_STACKPROTECTOR` 保护

#### 反 UAF 的 next 指针保存

这是在 §9.4 已详述的，这里用代码对比形式总结：

```c
// ❌ 错误 — UAF
read_lock(&tasklist_lock);
task = next_task(&init_task);
while (task != &init_task) {
    fill_kinfo(&kinfo, task);
    read_unlock(&tasklist_lock);
    copy_to_user(&buf[i++], &kinfo, sizeof(kinfo));
    read_lock(&tasklist_lock);
    task = list_next_entry(task, tasks);  // ← UAF! task 可能已被释放
}

// ✓ 正确 — 无 UAF
read_lock(&tasklist_lock);
task = next_task(&init_task);
while (task != &init_task) {
    fill_kinfo(&kinfo, task);
    next = next_task(task);               // ← 持锁时保存
    read_unlock(&tasklist_lock);
    copy_to_user(&buf[i++], &kinfo, sizeof(kinfo));
    read_lock(&tasklist_lock);
    task = next;                          // ← 使用保存的, 不做指针追踪
}
```


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
