# 进程全量监控系统 —— 原理与实现

操作系统课程设计 · 实验三
Linux 6.18 内核 · 自定义系统调用 (470/471/472)
2026年7月

---

## 目录

1. [系统架构](#1-系统架构)
2. [数据结构 — ABI 约定](#2-数据结构--abi-约定)
3. [内核实现](#3-内核实现)
   - [3.1 内核 API 与自实现函数](#31-内核-api-与自实现函数)
   - [3.2 sys_proc_collect (470)](#32-sys_proc_collect-470)
   - [3.3 sys_proc_snapshot (471)](#33-sys_proc_snapshot-471)
   - [3.4 sys_proc_stat (472)](#34-sys_proc_stat-472)
   - [3.5 锁策略](#35-锁策略)
   - [3.6 进程状态编码](#36-进程状态编码)
   - [3.7 CPU 时间与内存采集](#37-cpu-时间与内存采集)
4. [用户态实现](#4-用户态实现)
   - [4.1 整体架构](#41-整体架构)
   - [4.2 数据采集与耗时测量](#42-数据采集与耗时测量)
   - [4.3 状态解码](#43-状态解码)
   - [4.4 CPU% 计算](#44-cpu-计算)
   - [4.5 排序与过滤](#45-排序与过滤)
   - [4.6 五视图模式](#46-五视图模式)
   - [4.7 数据导出](#47-数据导出)
   - [4.8 交叉验证](#48-交叉验证)
5. [内核概念要点](#5-内核概念要点)
   - [5.1 系统调用：Ring 3 到 Ring 0](#51-系统调用ring-3-到-ring-0)
   - [5.2 copy_to_user() 内部机制](#52-copy_to_user-内部机制)
   - [5.3 RCU 与 tasklist_lock](#53-rcu-与-tasklist_lock)
   - [5.4 指针安全与内核栈限制](#54-指针安全与内核栈限制)
6. [编译与部署](#6-编译与部署)
7. [调试指南](#7-调试指南)
附录 A. [内核状态位参考](#附录-a-内核状态位参考)
附录 B. [proc_info 字段与 /proc/pid/stat 对照](#附录-b-proc_info-字段与-procpidstat-对照)
附录 C. [已修复的 Bug](#附录-c-已修复的-bug)

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

数据流向: 内核遍历 `task_struct` 链表 → 提取字段填入 `proc_info` / `proc_tree_node` → `copy_to_user()` → 用户态 buffer → TUI 渲染。

---

## 2. 数据结构 — ABI 约定

三个结构体定义在 `include/linux/proc_monitor.h`（内核侧）和 `user_app/proc_monitor.h`（用户态侧）。
两边定义必须逐字节一致——字段名、类型、顺序、对齐方式完全相同。

### struct proc_info — 进程完整信息

```c
struct proc_info {
    pid_t         pid;          // 进程 ID (TGID, 线程组 ID)
    pid_t         ppid;         // 父进程 ID
    char          comm[16];     // 进程名 (最多 15 字符 + NUL)
    int           state;        // __state | exit_state (位掩码)
    unsigned long utime;        // 用户态 CPU 时间 (USER_HZ 时钟滴答)
    unsigned long stime;        // 内核态 CPU 时间 (USER_HZ 时钟滴答)
    unsigned long vsize;        // 虚拟内存大小 (字节)
    unsigned long rss;          // 常驻内存集 (页数, 4KB/页)
    int           nice;         // nice 优先级 (-20~19)
    int           num_threads;  // 线程组内线程数
    uid_t         uid;          // 真实用户 ID
};
```

x86_64 内存布局 (LP64 ABI, sizeof=80):

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

### struct proc_tree_node — 进程树节点

```c
struct proc_tree_node {
    pid_t pid;       // sizeof=28 on x86_64 (4+4+16+4)
    pid_t ppid;
    char  comm[16];
    int   level;     // 树深度 (init_task=0，向上追溯 real_parent)
};
```

### struct proc_stat — 聚合统计

```c
struct proc_stat {
    int total_processes;      // sizeof=36 on x86_64 (9 × int)
    int running_processes;    // TASK_RUNNING
    int sleeping_processes;   // TASK_INTERRUPTIBLE
    int uninterruptible;      // TASK_UNINTERRUPTIBLE
    int stopped_processes;    // TASK_STOPPED / TASK_TRACED
    int zombie_processes;     // EXIT_ZOMBIE
    int idle_processes;       // TASK_IDLE / 其他
    int kernel_threads;       // mm==NULL
    int user_threads;         // mm!=NULL
};
```

---

## 3. 内核实现

### 3.1 内核 API 与自实现函数

本节区分 Linux 内核提供的 API（通过 `#include` 引入后调用）与本项目自行编写的函数。

**本项目调用的 Linux 内核 API**:

`<linux/syscalls.h>`:
- `SYSCALL_DEFINEn(name, ...)` — 宏，展开为系统调用入口函数 `sys_name`，自动处理参数从用户态寄存器解码

`<linux/sched.h>`:
- `for_each_process(task)` — 宏，遍历 `task_struct` 链表
- `next_task(task)` — 宏，返回链表中 `task` 的下一个节点
- `read_lock(&tasklist_lock)` / `read_unlock(&tasklist_lock)` — 获取/释放 tasklist_lock 读锁
- `get_task_comm(buf, task)` — 安全复制进程名到 buf
- `task_tgid_nr(task)` — 获取线程组 ID，即用户态视角的 PID
- `task_nice(task)` — 获取 nice 值 (-20~19)

`<linux/sched/signal.h>`:
- `task->signal->nr_threads` — 线程组内线程数

`<linux/sched/cputime.h>`:
- `task_cputime(task, &ut_ns, &st_ns)` — 读取 CPU 时间，单位纳秒

`<linux/jiffies.h>`:
- `nsec_to_clock_t(ns)` — 纳秒转换为 USER_HZ 时钟滴答

`<linux/uaccess.h>`:
- `copy_to_user(dst, src, n)` — 将 n 字节从内核地址 src 拷贝到用户态地址 dst，返回未拷贝字节数

`<linux/cred.h>`:
- `from_kuid_munged(ns, kuid)` — 将内核 UID 映射到指定命名空间
- `current_user_ns()` — 获取当前进程的用户命名空间
- `task_uid(task)` — 获取 task 的内核 UID

`<linux/mm.h>`:
- `get_mm_rss(mm)` — 获取常驻内存集页数
- `PAGE_SHIFT` — 页大小对应的位移量 (12 → 4KB)

**本项目自实现的函数**:

内核侧 (`kernel/proc_monitor.c`):
- `sys_proc_collect()` — 系统调用 470。遍历进程链表，采集 11 个字段填入 `proc_info`，逐个 `copy_to_user()` 到用户态数组
- `sys_proc_snapshot()` — 系统调用 471。遍历进程链表，计算树深度，填入 `proc_tree_node` 并拷贝
- `sys_proc_stat()` — 系统调用 472。遍历一次，按状态分类计数，填 9 个计数器
- `compute_level(task)` — 从 `task` 沿 `real_parent` 向上走到 `init_task`，累计步数

用户态 (`user_app/proc_monitor.c`) — 见 §4。

### 3.2 sys_proc_collect (470)

签名: `long sys_proc_collect(struct proc_info *user_buf, int max_count, int *ret_count)`

遍历进程链表，为每个进程填充一个 `proc_info`。字段采集逻辑:

- `comm`: `get_task_comm()` — 最多 15 字符 + NUL
- `pid`: `task_tgid_nr(task)` — 线程组 ID
- `ppid`: `task_tgid_nr(task->real_parent)` — 真实父进程 TGID
- `state`: `task->__state | task->exit_state` — 合并调度状态和退出状态
- `utime` / `stime`: `task_cputime()` → 纳秒 → `nsec_to_clock_t()` → USER_HZ 滴答
- `vsize`: `task->mm->total_vm << PAGE_SHIFT` → 字节。内核线程 (`mm==NULL`) 填 0
- `rss`: `get_mm_rss(task->mm)` → 页数。内核线程填 0
- `nice`: `task_nice(task)` — -20 到 19
- `num_threads`: `task->signal->nr_threads`
- `uid`: `from_kuid_munged(current_user_ns(), task_uid(task))` — 映射到调用者命名空间

锁模式: lock-drop-copy-relock（§3.5）。

返回值: 0 成功; `-EINVAL` 参数无效; `-EFAULT` copy_to_user 失败（尽力返回已拷贝数量）。

### 3.3 sys_proc_snapshot (471)

签名: `long sys_proc_snapshot(struct proc_tree_node *user_buf, int max_count, int *ret_count)`

与 collect 相同的遍历和锁模式。每个节点记录 `pid`, `ppid`, `comm`, `level`。
`level` 由 `compute_level()` 计算：从 `task->real_parent` 向上追溯到 `init_task`，步数即深度。
用户态可基于 `pid/ppid` 重建树，也可直接用 `level` 按缩进渲染。

### 3.4 sys_proc_stat (472)

签名: `long sys_proc_stat(struct proc_stat *stat)`

一次遍历完成全部计数。循环内只做整数运算，不调用 `copy_to_user()`，因此全程持锁。

分类逻辑（当前版本，使用位掩码）:

```
for_each_process(task):
    total_processes++

    if (task->exit_state & EXIT_ZOMBIE)     → zombie_processes++
    else if (task->exit_state & EXIT_DEAD)  → skip
    else:
        if (task->__state & TASK_NOLOAD):
            idle_processes++       // TASK_IDLE = UNINTERRUPTIBLE | NOLOAD
        else:
            s = task->__state & ~(TASK_WAKEKILL | TASK_WAKING)
            switch (s):
                TASK_RUNNING         → running_processes++
                TASK_INTERRUPTIBLE   → sleeping_processes++
                TASK_UNINTERRUPTIBLE → uninterruptible++
                __TASK_STOPPED / __TASK_TRACED → stopped_processes++
                default              → idle_processes++

    task->mm == NULL → kernel_threads++
    else             → user_threads++
```

必须先用掩码清除 `TASK_WAKEKILL` 和 `TASK_WAKING`，再 `switch` 基础状态。
若用 `switch(task->__state)` 精确匹配，`0x0081` (INTERRUPTIBLE|WAKEKILL) 不等于
任何 case 标签，落入 `default` 被误判为 idle。`TASK_NOLOAD` 单独检测——`TASK_IDLE`
语义上就是 UNINTERRUPTIBLE|NOLOAD，归 idle。早期版本因未做掩码处理，交叉验证出现
系统性 MISMATCH（附录 C.4）。

### 3.5 锁策略

进程链表 (`task_struct->tasks`) 由 RCU 和 `tasklist_lock` 保护。

约束: RCU 读临界区内不能睡眠。若读者被调度，写者必须等该 CPU 经历 RCU 宽限期，
导致长时间阻塞。而 `copy_to_user()` 可能因缺页触发磁盘 I/O 而睡眠。

collect 和 snapshot 采用 lock-drop-copy-relock:

```
read_lock(&tasklist_lock);
task = next_task(&init_task);

while (task != &init_task) {
    从 task 读取字段填入 kinfo/knode
    next = next_task(task);        // 持锁保存后继指针
    read_unlock(&tasklist_lock);

    copy_to_user(&buf[i], &kinfo, sizeof(kinfo));  // 无锁，可能睡眠
    count++;

    read_lock(&tasklist_lock);
    task = next;
}

read_unlock(&tasklist_lock);
```

保存 `next_task(task)` 而非通过 `task->tasks.next` 获取后继：释放锁后 `task`
可能因进程退出被 RCU 回收，`task->tasks.next` 是 UAF。提前保存的 `next` 是独立
指针值，只要目标进程未退出就一直有效。

stat 例外: `sys_proc_stat` 循环中只做整数运算，唯一 `copy_to_user()` 在循环之后，
因此可以全程持锁。

### 3.6 进程状态编码

Linux 5.14+ 将 `task->state` 拆分为两部分:

- `task->__state` — 调度状态: `TASK_RUNNING(0x00)`, `TASK_INTERRUPTIBLE(0x01)`, `TASK_UNINTERRUPTIBLE(0x02)`, `__TASK_STOPPED(0x04)`, `__TASK_TRACED(0x08)`, `TASK_DEAD(0x40)`
- `task->exit_state` — 退出状态: `EXIT_DEAD(0x10)`, `EXIT_ZOMBIE(0x20)`

`__state` 可带有附加标志位:
- `TASK_WAKEKILL (0x0080)` — 可被信号唤醒
- `TASK_WAKING (0x0100)` — 正在被唤醒
- `TASK_NOLOAD (0x0400)` — 不计入负载 (TASK_IDLE = UNINTERRUPTIBLE|NOLOAD)

内核向用户态传递 `state = __state | exit_state`。

**为什么必须用位掩码而非精确匹配**:

```
TASK_INTERRUPTIBLE                    = 0x0001
TASK_INTERRUPTIBLE | TASK_WAKEKILL    = 0x0081  ← 不等于 0x0001

若 switch(state):  0x0081 → default → 误判为 idle
若 state & ~0x0080 & 0x03: 0x0081 → 0x01 → 正确识别为 sleeping
```

内核分类和用户态解码都必须先掩码清除标志位。早期版本两侧都未处理，交叉验证出现
系统性 MISMATCH（附录 C.4）。

### 3.7 CPU 时间与内存采集

CPU 时间: 发行版内核 (`CONFIG_VIRT_CPU_ACCOUNTING_GEN=y`) 以纳秒精度存储。
`task_cputime(task, &ut_ns, &st_ns)` 读纳秒 → `nsec_to_clock_t(ns)` 转 USER_HZ 滴答。
`sysconf(_SC_CLK_TCK)` 获取 USER_HZ（通常 100，即每滴答 10ms）。与 `/proc/pid/stat` 一致。

内存: `vsize = task->mm->total_vm << PAGE_SHIFT` (字节)，对应 `/proc/pid/stat` 字段 23。
`rss = get_mm_rss(task->mm)` (页数)，对应字段 24。内核线程 (`task->mm == NULL`) 无用户态地址空间，均填 0。

---

## 4. 用户态实现

### 4.1 整体架构

ncurses 终端界面，4 面板布局:

- `win_main` (主视图) — 5 种模式: 列表 / 原始数值表 / 进程树 / 十六进制 dump / 系统调用面板
- `win_detail` (2 行) — 选中进程的全部 11 字段 + `__state`/`exit_state` 位解码
- `win_syscall` (2 行) — 系统调用 ret/errno/延迟(μs) + proc_stat 聚合
- `win_hint` (1 行) — 按键提示

数据刷新: `wtimeout(win_main, 1000)` 设 1 秒超时。`wgetch()` 返回 `ERR` 时自动刷新:
`save_prev_cpu()` → `update_data()` → `sort_procs()` → 重绘。

### 4.2 数据采集与耗时测量

每个系统调用通过 `syscall()` 发起，`clock_gettime(CLOCK_MONOTONIC)` 测量 wall-clock 延迟:

```c
t0 = time_now_us();
g_sc_collect.ret = syscall(SYS_proc_collect, g_procs, MAX_PROCS, &count);
g_sc_collect.latency_us = time_now_us() - t0;
```

`update_data()` 按序调用 `fetch_procs()` → `fetch_tree()` → `fetch_stat()`。

时间戳 `g_tv_prev`/`g_tv_curr` 在 `update_data()` 中记录，用于 CPU% 计算的时间差。
早期版本在 `save_prev_cpu()`（渲染前）记录，导致 `elapsed ≈ 0`——附录 C.5。

### 4.3 状态解码

`state` 字段 = `__state | exit_state`，解码分步:

1. 退出状态: `state & 0x20` → Z (ZOMBIE), `state & 0x10` → X (DEAD)
2. 调度标志: `state & 0x08` → t (TRACED), `state & 0x04` → T (STOPPED)
3. 基础状态: `state & 0x03`: 0=R, 1=S, 2=D

`win_detail` 显示完整位解码。交叉验证（§4.8）使用与内核一致的掩码: 先检查 `0x0400` (NOLOAD)
→ idle，再 `st & ~0x0080` 清除 WAKEKILL，最后 `& 0x03`。

### 4.4 CPU% 计算

CPU% = 两次采集间的 CPU 滴答差 / wall-clock 时间差:

```
delta_ticks = cur_total - prev_total
elapsed     = g_tv_curr - g_tv_prev   (秒)
cpu%        = delta_ticks / (clk_tck × elapsed) × 100
```

第一帧 `g_has_prev=0`，不计算。`g_prev[]` 按 PID 排序以支持 `bsearch`。
边界: 新进程→0%, `cur_total < prev_total` (PID 复用)→0%, `elapsed ≤ 0`→0%。

### 4.5 排序与过滤

排序: `qsort(g_procs, ...)` 按 `g_sort_field` (0=PID, 1=CPU, 2=MEM, 3=NAME),
`g_sort_desc` 控制升降序。`cmp_cpu` 调用 `cpu_pct()` 比较 delta-based 百分比。

过滤: `/` 键进入过滤模式。支持 `bash` (名称子串), `=R` (状态字符),
`=0xNN` (精确 state 十六进制), `:1234` (精确 PID)。过滤影响所有视图。

### 4.6 五视图模式

按 `v`/`V` 循环，数字键 `1`-`5` 直达:

1. **LIST** — 紧凑表格。状态按颜色高亮 (R=绿, Z=红, D=黄, T/t=紫)
2. **DEBUG-TABLE** — 所有字段的原始数值，不做格式化/单位转换
3. **TREE** — 进程树层次。UTF-8 终端用 Unicode 树线 (`│ ├── └──`)，按深度着色
4. **HEX-DUMP** — 选中进程的原始字节 (16B/行 hex + ASCII) + 字段偏移标注。`n`/`p` 切换进程，上下键滚动 hex 行。使用独立的 `g_hex_selected` 变量跟踪被 dump 进程
5. **SYSCALL** — 系统调用诊断: ret/errno/latency/struct sizes + cross-validation

### 4.7 数据导出

按 `x` 键: LIST/DEBUG → CSV (`proc_list.csv`), TREE → DOT (`proc_tree.dot`),
HEX/SYSCALL → 二进制 dump (`proc_dump.bin`，magic 头 + count + struct_size + 原始字节)。

### 4.8 交叉验证

SYSCALL 视图底部对比 `proc_info[]` 自统计 vs `proc_stat` 报告值:

```
R: proc_info=3     proc_stat=3     OK
S: proc_info=210   proc_stat=210   OK
D: proc_info=0     proc_stat=0     OK
Z: proc_info=0     proc_stat=0     OK
I: proc_info=168   proc_stat=168   OK
```

两次系统调用存在时间窗口，偶发不一致正常。系统性的大量 MISMATCH 提示分类逻辑 bug
（通常是未掩码清除标志位所致，附录 C.4）。

---

## 5. 内核概念要点

### 5.1 系统调用：Ring 3 到 Ring 0

x86_64 上，用户态执行 `syscall` 指令触发 CPU 模式切换。硬件完成:
`RCX ← RIP` (保存返回地址), `R11 ← RFLAGS` (保存标志), `RIP ← MSR_LSTAR`
(跳转到 `entry_SYSCALL_64`), `CPL ← 0` (切换到 Ring 0)。

寄存器约定: `rax` = 系统调用号 (470=0x1D6), `rdi/rsi/rdx/r10/r8/r9` = 参数 1-6。

内核入口 `entry_SYSCALL_64` (汇编): `swapgs` → 保存用户栈指针 → 切换到内核栈
→ 构造 `pt_regs` 帧（保存全部用户寄存器）→ `do_syscall_64()` 查 `sys_call_table[rax]`
→ 跳转 `sys_proc_collect`。

返回: `sysretq` 恢复 RIP/RFLAGS/RSP，CPL 恢复 Ring 3。

### 5.2 copy_to_user() 内部机制

`copy_to_user()` 执行以下步骤:

1. `access_ok(to, n)` — 验证目标地址在用户态地址空间 (x86_64: `< 0x800000000000`)
2. `stac()` — 临时关闭 SMAP。SMAP 是硬件特性: Ring 0 默认禁止访问用户态页，必须显式放行
3. `copy_user_generic()` — 逐字节拷贝，每条指令可能触发缺页
4. `clac()` — 重新启用 SMAP

第 3 步可能因 swap 换页、Copy-on-Write、mmap 文件映射等触发缺页 → 磁盘 I/O → 睡眠。
因此 `copy_to_user()` 绝对不能在持有自旋锁或 RCU 读锁时调用。

### 5.3 RCU 与 tasklist_lock

进程链表是 read-mostly 结构。RCU 优化读者（零同步开销），写者延迟回收旧版本。

`read_lock(&tasklist_lock)` 等价 `preempt_disable()` + 读者计数 +1。读者睡眠导致
写者阻塞直至宽限期结束——因此 `copy_to_user()` 必须在锁外。

collect/snapshot: lock-drop-copy-relock。stat: 全程持锁（循环内无 `copy_to_user()`）。
381 进程正常耗时: collect ~300-2000μs, stat ~50-100μs。

### 5.4 指针安全与内核栈限制

内核绝不向用户态暴露指针。`proc_info` 全部为值类型。`from_kuid_munged()` 将内核 UID
映射为调用者命名空间的数值，不泄露内核结构。

内核栈大小: `THREAD_SIZE = 16KB`，扣除 `pt_regs` 和中断帧实际可用约 12KB。
`struct proc_info[8192]` = 640KB 远超限制，会导致栈溢出 → 内核 panic。
逐个 `copy_to_user()` 避免了在内核栈上分配大数组。

---

## 6. 编译与部署

### 内核侧

```bash
cp include/linux/proc_monitor.h  ~/linux-6.18/include/linux/
cp kernel/proc_monitor.c         ~/linux-6.18/kernel/
echo "obj-y += proc_monitor.o" >> ~/linux-6.18/kernel/Makefile

# 在 syscall_64.tbl 末尾添加:
# 470  common  proc_collect   sys_proc_collect
# 471  common  proc_snapshot  sys_proc_snapshot
# 472  common  proc_stat      sys_proc_stat

cd ~/linux-6.18
make localmodconfig && make -j$(nproc) && make modules -j$(nproc)
sudo make modules_install && sudo make install
sudo update-grub && sudo reboot
```

### 用户态

```bash
sudo apt install libncurses-dev
cd user_app
make              # 编译 proc_monitor
make test         # 验证系统调用
sudo ./proc_monitor
```

---

## 7. 调试指南

启动时 stderr 输出 struct 大小，切换到 SYSCALL 视图（按 `5`）查看详细信息和交叉验证。
HEX-DUMP 视图（按 `4`）验证 struct 内存布局: padding 应在偏移 28 和 76，`unsigned long`
字段应在 8 字节对齐地址（偏移 32/40/48/56）。`=0xNN` 过滤器可精确匹配特定状态组合。

性能参考: collect 300-2000μs, snapshot 300-1500μs, stat 50-100μs（381 进程）。

导出: `x` 键 → CSV/DOT/二进制 dump。DOT 可用 `dot -Tpng proc_tree.dot -o tree.png` 渲染。

交叉验证系统性 MISMATCH → 检查状态分类的位掩码（附录 C.4）。

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

## 附录 C: 已修复的 Bug

### C.1 CPU% 排序度量错误

提交 `f76bcf6`。症状: 按 CPU% 列排序，实际顺序与显示值不一致。
根因: `cmp_cpu` 比较 `utime+stime` 累计滴答（进程启动以来的总 CPU 时间），
而非 `cpu_pct()` 计算的增量百分比。修复: `cmp_cpu` 改为调用 `cpu_pct()`。
教训: 排序比较器必须使用与显示列相同的计算函数。

### C.2 V 键视图切换失效

提交 `f76bcf6`。症状: 按 Shift+V 无法切换视图。根因: 按键处理只写了 `case 'v':`，
ncurses 区分大小写。修复: 添加 `case 'V':`。教训: ncurses 中字母键须同时处理大小写。

### C.3 HEX-DUMP 导航混乱

提交 `f76bcf6`。症状: HEX-DUMP 视图中按上下键切换了进程而非滚动 hex 行。
根因: `g_selected` 同时表示进程索引和 hex 行号。修复: 引入 `g_hex_selected`
解耦，新增 `n`/`p` 键切换进程。教训: 不同视图的光标有不同的语义维度，需用独立变量。

### C.4 状态分类失配

提交 `c463243`。症状: 交叉验证系统性 MISMATCH (`S: 224 vs 211`, `D: 154 vs 0`,
`I: 0 vs 167`)。根因: 内核 `switch(task->__state)` 精确匹配，带标志位的值
(如 `0x0081`) 落入 default → idle。用户态交叉验证也未处理 `TASK_NOLOAD`。
修复: 内核先检查 TASK_NOLOAD → idle，再掩码清除 WAKEKILL|WAKING 后 switch。
用户态同步增加 NOLOAD 检测和 WAKEKILL 掩码。教训: Linux 6.x 的 `__state` 是
位掩码，不是枚举。精确 `switch(state)` 必然出错。两侧须用一致的掩码策略。

### C.5 CPU% 时间戳位置错误

提交 `623dfcc`。症状: 第一帧后 CPU% 异常巨大（数百到数千百分比）。根因: 时间戳
在 `save_prev_cpu()`（渲染前）记录，`elapsed ≈ 0`。修复: 时间戳记录移至
`update_data()` 中。教训: CPU% 的时间差必须基于数据采集点，而非渲染点。

---

## 作者

Arch1mboldi · 2026年春季操作系统课程设计
