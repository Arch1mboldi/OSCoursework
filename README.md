# 实验三：Linux 系统调用 —— 进程全量监控与分析系统

> **操作系统课程设计 · 实验三**  
> Linux 6.18 内核 · 自定义系统调用  
> 2026年7月

---

## 项目概述

在 Linux 6.18 内核中新增 **3 个自定义系统调用**，并编写用户态 C 程序通过 `syscall()` 直接调用，
实现进程全量监控与分析系统。

### 3 个系统调用

| 系统调用 | 编号 | 功能 |
|:---|:---|:---|
| `sys_proc_collect` | 470 | 一次性收集所有进程的完整信息 (PID/PPID/状态/CPU时间/内存/线程数/UID) |
| `sys_proc_snapshot` | 471 | 返回进程树父子关系拓扑快照 (含层级深度) |
| `sys_proc_stat` | 472 | 返回系统整体进程统计 (按状态分类 + 内核线程/用户进程) |

### 用户态程序功能

| 功能    | 命令                                 | 说明                           |
| :---- | :--------------------------------- | :--------------------------- |
| 统计摘要  | `stat`                             | 进程总数、各状态分布、内核/用户线程数          |
| 进程列表  | `list`                             | 表格展示所有进程，支持排序与多条件过滤          |
| 进程树导出 | `tree <file>`                      | 导出为 Graphviz DOT 格式，可渲染为 PNG |
| 实时刷新  | `watch`                            | 每2秒自动刷新，类 `top` 体验           |
| 排序    | `sort pid/cpu/mem/name`            | 按指定字段排序                      |
| 过滤    | `filter pid=N` / `filter name=XXX` | 多条件联合过滤                      |

---

## 仓库结构

```
OSCoursework/
├── kernel/
│   ├── proc_monitor.c                   # [新建] 3个系统调用的内核实现
│   └── Makefile.patch                   # kernel/Makefile 修改说明
├── include/
│   └── linux/
│       └── proc_monitor.h               # [新建] 内核-用户态共享数据结构
├── arch/
│   └── x86/
│       └── entry/
│           └── syscalls/
│               └── syscall_64.tbl.patch # 系统调用表注册说明
├── user_app/
│   ├── proc_monitor.h                   # 用户态头文件 (数据结构 + 系统调用号)
│   ├── proc_monitor.c                   # 用户态监控主程序 (C 命令行交互版)
│   ├── test_syscall.c                   # 最小验证程序 (测试3个系统调用)
│   └── Makefile                         # 用户态编译脚本
├── user_app_bubbletea/
│   ├── go.mod                           # Go 模块定义
│   └── main.go                          # Go Bubble Tea TUI 监控程序
├── .gitignore
└── README.md                            # 本文件
```

---

## 快速开始

### 1. 内核侧：修改、编译、安装

```bash
# (1) 将本仓库文件放入 Linux 6.18 内核源码树
#     假设内核源码在 ~/linux-6.18/

cp OScoursework/include/linux/proc_monitor.h  ~/linux-6.18/include/linux/
cp OScoursework/kernel/proc_monitor.c         ~/linux-6.18/kernel/

# (2) 修改 kernel/Makefile
echo "obj-y += proc_monitor.o" >> ~/linux-6.18/kernel/Makefile

# (3) 注册系统调用号
#     编辑 ~/linux-6.18/arch/x86/entry/syscalls/syscall_64.tbl
#     在末尾添加:
#       470  common  proc_collect   sys_proc_collect
#       471  common  proc_snapshot  sys_proc_snapshot
#       472  common  proc_stat      sys_proc_stat

# (4) 编译内核 (推荐 localmodconfig，只编译用到的模块，5-15 分钟)
cd ~/linux-6.18
make localmodconfig
make -j$(nproc)
make modules -j$(nproc)

# (5) 安装并重启
sudo make modules_install
sudo make install
sudo update-grub
sudo reboot
```

### 2. 用户态：编译、验证、运行

```bash
cd OScoursework/user_app

# 编译
make

# 验证系统调用可用
make test

# 运行监控程序
sudo ./proc_monitor
```

### 3. Go TUI 版 (Bubble Tea)

更精美的终端 UI，支持实时刷新、交互式过滤、进程树可视化：

```bash
# 安装 Go 1.21+ (如未安装)
sudo apt install golang-go   # Ubuntu
# 或从 https://go.dev/dl/ 下载

cd OScoursework/user_app_bubbletea

# 下载依赖并编译
go mod tidy
go build -o procmon .

# 运行
sudo ./procmon
```

**Go TUI 版功能：**

| 功能 | 按键 | 说明 |
|:---|:---|:---|
| 实时刷新 | (自动) | 每秒自动刷新，状态栏实时更新 |
| 过滤 | `/` | 输入过滤条件后 Enter 确认 |
| 清除过滤 | `Esc` | |
| 排序切换 | `s` | PID → CPU → MEM → NAME 循环 |
| 排序方向 | `S` | 升序/降序切换 |
| 进程树 | `t` | 切换列表/树视图，Unicode 缩进线 |
| 滚动 | `↑↓` / `j k` | 逐行滚动 |
| 翻页 | `PgUp` / `PgDn` | |
| 跳转 | `g`=顶部 `G`=底部 | |
| 帮助 | `?` | |

**过滤语法：**

| 输入 | 含义 |
|:---|:---|
| `systemd` | 进程名包含 "systemd" (忽略大小写) |
| `=R` | 状态为 Running |
| `=Z` | 状态为 Zombie |
| `:1234` | PID 等于 1234 |

---

## 实现依据：Linux 内核文档与 API 参考

本节记录每个内核接口、数据结构、同步机制在 Linux 6.18 官方文档中的出处。

### 1. 系统调用注册机制

依据：**`Documentation/process/adding-syscalls.rst`**

| 实现要点 | 内核文档要求 | 对应代码 |
|:---|:---|:---|
| 使用 `SYSCALL_DEFINEn()` 宏 | L200-204: *"add this entry point with the appropriate `SYSCALL_DEFINEn()` macro"* | `kernel/proc_monitor.c` L48/168/223 |
| x86_64 系统调用表注册 | L300-309: *"a 'common' entry in `syscall_64.tbl`: `333 common xyzzy sys_xyzzy`"* | `syscall_64.tbl.patch` |
| 在 `include/linux/syscalls.h` 添加原型 | L207-211: *"a corresponding function prototype, marked as asmlinkage"* | 待补充 |
| 在 `kernel/sys_ni.c` 添加回退桩 | L225-229: *"Add your new system call here too: `COND_SYSCALL(xyzzy);`"* | 待补充 |
| 设计可扩展的数据结构 | L83-103: 推荐用结构体封装参数，含 `size` 字段支持未来扩展 | `proc_info` 等结构体经 `copy_to_user()` 传递 |

### 2. 进程链表遍历与 RCU 同步

依据：**`Documentation/RCU/listRCU.rst`**

| 实现要点 | 内核文档 | 对应代码 |
|:---|:---|:---|
| `for_each_process()` 基于 RCU 链表 | L29-36: `next_task(p)` → `list_entry_rcu(p->tasks.next, ...)` | `kernel/proc_monitor.c` L63/182/236 |
| 读者应使用 `rcu_read_lock()` | L40-44: `rcu_read_lock(); for_each_process(p) { ... } rcu_read_unlock();` | 当前使用 `read_lock(&tasklist_lock)`，待优化 |
| 写者使用 `write_lock(&tasklist_lock)` | L51-53: `write_lock(&tasklist_lock); list_del_rcu(...);` | 内核进程创建/销毁时自动处理 |
| RCU 宽限期保证遍历安全 | L53-68: *"deferring of destruction ensures that any readers ... will see valid pointers"* | 保证遍历中读取的 `task_struct*` 字段有效 |
| RCU 读者不可睡眠 | `Documentation/RCU/rcu.rst` L32-34: *"RCU readers are not permitted to block, switch to user-mode, or enter the idle loop"* | 因此 `copy_to_user()` 必须在释放锁之后调用 |

### 3. 进程状态编码

依据：**`include/linux/sched.h`**（内核头文件，Linux 6.x）

Linux 5.14+ 将 `task_struct` 的 `state` 重命名为 **`__state`**，并引入 `TASK_REPORT` 掩码。
退出状态（`EXIT_ZOMBIE`、`EXIT_DEAD`）存于独立的 **`task->exit_state`** 字段，不在 `__state` 中。

| 字段 | 含义 | 典型值 |
|:---|:---|:---|
| `task->__state` | 当前调度状态 | `TASK_RUNNING=0`, `TASK_INTERRUPTIBLE=1`, `TASK_UNINTERRUPTIBLE=2`, `TASK_STOPPED`, `TASK_TRACED` |
| `task->exit_state` | 进程退出状态 | `EXIT_ZOMBIE=0x10`, `EXIT_DEAD=0x20` |

> **注意**：当前 `sys_proc_stat` 在 `switch(task->__state)` 中检测 `EXIT_ZOMBIE`/`EXIT_DEAD`，
> 这两个值实际在 `exit_state` 中，`__state` 永远不包含它们。需修复（见审查报告 Issue 1）。

### 4. CPU 时间采集

依据：**`include/linux/sched/cputime.h`** + `include/linux/jiffies.h`

发行版内核（`CONFIG_VIRT_CPU_ACCOUNTING_GEN=y`）将 CPU 时间以**纳秒**精度存储在 `task_struct` 中。
直接读 `task->utime` 会得到纳秒值（如 56280000000），而非 jiffies。

| API | 说明 | 代码位置 |
|:---|:---|:---|
| `task_cputime(task, &ut_ns, &st_ns)` | 以纳秒精度读取 CPU 时间 | `proc_monitor.c` L83-84 |
| `nsec_to_clock_t(ns)` | 纳秒 → `USER_HZ` 时钟滴答（= `sysconf(_SC_CLK_TCK)`） | `proc_monitor.c` L85-86 |

依据：**`Documentation/filesystems/proc.rst`** Table 1-4（L338-339）：
> *"utime — user mode jiffies; stime — kernel mode jiffies"*

本实现输出的 `utime` / `stime` 是 **USER_HZ 时钟滴答（jiffies）**，与 `/proc/pid/stat` 语义一致。

### 5. 内存信息采集

依据：**`Documentation/filesystems/proc.rst`** Table 1-4（L347-348）：
> *"vsize — virtual memory size; rss — resident set memory size"*

| API | 说明 | 代码位置 |
|:---|:---|:---|
| `task->mm->total_vm << PAGE_SHIFT` | 虚拟内存大小（页数 → 字节） | `proc_monitor.c` L91 |
| `get_mm_rss(task->mm)` | 常驻内存集，返回页数 | `proc_monitor.c` L92 |

依据：**`Documentation/mm/active_mm.rst`**（L44-46）：
> *"tsk->mm points to the 'real address space'. For an anonymous process, tsk->mm will be NULL."*

内核线程（`[kthreadd]`、`[ksoftirqd]` 等）`mm == NULL`，无用户态地址空间，`vsize`/`rss` 填 0。

### 6. 用户身份映射

| API | 说明 | 文档来源 |
|:---|:---|:---|
| `get_task_comm(kinfo.comm, task)` | 安全复制进程名（最多 `TASK_COMM_LEN-1` 字符） | `include/linux/sched.h` |
| `task_tgid_nr(task)` | 获取线程组 ID，即用户态视角的 PID | `include/linux/sched.h` |
| `task_nice(task)` | 获取 nice 优先级（-20 ~ 19） | `include/linux/sched.h` |
| `from_kuid_munged(current_user_ns(), task_uid(task))` | 将内核 UID 映射到调用者的用户命名空间 | `include/linux/cred.h` |
| `copy_to_user()` | 内核→用户态数据传递的唯一安全方式 | `adding-syscalls.rst` 隐含要求：所有 `__user` 指针必须经此函数 |

### 7. 安全与权限

依据：**`Documentation/process/adding-syscalls.rst`** L166-167：
> *"If your new syscall manipulates a process other than the calling process, it should be restricted (using a call to `ptrace_may_access()`)"*

当前实现无权限检查，任何用户可查看全部进程信息。若需限制访问，应添加 `capable(CAP_SYS_PTRACE)` 或 `ptrace_may_access()`。

### 8. 头文件依赖对照表

`kernel/proc_monitor.c` 的 `#include` 及各自提供的 API：

| 头文件 | 提供的 API |
|:---|:---|
| `<linux/syscalls.h>` | `SYSCALL_DEFINEn()` 宏 |
| `<linux/sched.h>` | `task_struct`, `for_each_process()`, `get_task_comm()`, `task_tgid_nr()`, `task_nice()` |
| `<linux/sched/signal.h>` | `task->signal->nr_threads` |
| `<linux/sched/cputime.h>` | `task_cputime()` |
| `<linux/jiffies.h>` | `nsec_to_clock_t()` |
| `<linux/uaccess.h>` | `copy_to_user()` |
| `<linux/cred.h>` | `from_kuid_munged()`, `current_user_ns()`, `task_uid()` |
| `<linux/mm.h>` | `get_mm_rss()`, `PAGE_SHIFT` |
| `<linux/proc_monitor.h>` | 本项目自定义的共享数据结构 |

---

## 核心设计决策

### 为什么设计专门的数据结构？

内核的 `task_struct` 包含数百个字段和大量内部状态，直接暴露给用户态会导致：
- **安全风险**：泄露内核地址布局和内部状态
- **兼容性问题**：内核版本升级后结构体变化导致所有用户态程序崩溃
- **复杂性**：用户态不需要知道内核的全部实现细节

因此我们定义了扁平化的 `proc_info` / `proc_tree_node` / `proc_stat` 结构体作为**数据契约**。

### 为什么必须用 `copy_to_user()`？

内核空间和用户空间的虚拟地址是隔离的。直接把内核指针 (`task_struct*`) 传给用户态会导致 **段错误 (Segmentation Fault)**。`copy_to_user()` 是内核向用户态传递数据的**唯一合法方式**，它执行：
1. 地址合法性检查
2. 权限验证
3. 缺页异常处理

### 用户态为什么必须用 `syscall()`？

这是实验的**核心要求** — 禁止使用 `/proc`、`ps`、`eBPF`、`ptrace` 等旁路。`syscall()` 直接触发 `syscall` 指令陷入内核，本质上是**重写了一个极简版的 `/proc` 数据采集机制**。

---

## 技术要点

| 要点 | 说明 | 文档依据 |
|:---|:---|:---|
| 锁机制 | `read_lock(&tasklist_lock)` 保护进程链表遍历 | `RCU/listRCU.rst` — 推荐 `rcu_read_lock()` |
| 锁释放时机 | `copy_to_user()` 可能睡眠，必须在释放锁后调用 | `RCU/rcu.rst` — RCU 读者不可阻塞 |
| 内核线程处理 | `mm==NULL` 的内核线程无用户态内存，vsize/rss 填 0 | `mm/active_mm.rst` L44-46 |
| 状态编码 | Linux 6.x 使用 `task->__state`；`exit_state` 存退出状态（僵尸/死） | `include/linux/sched.h` |
| 逐个拷贝 | 每个条目单独 `copy_to_user()`，避免内核栈分配过大数组 | `adding-syscalls.rst` 安全要求 |
| CPU 时间 | `task_cputime()` + `nsec_to_clock_t()` → USER_HZ 滴答 | `sched/cputime.h` + `proc.rst` L338-339 |
| 内存信息 | `total_vm << PAGE_SHIFT` (字节) + `get_mm_rss()` (页数) | `proc.rst` L347-348 |

---

## 相关资源

- [Linux Kernel 6.18](https://kernel.org/)
- [Linux 内核源码在线](https://git.kernel.org/pub/scm/linux/kernel/git/stable/linux.git)
- 详细设计文档: 参见仓库同级的 `plan.md`

### 本项目引用的内核文档

| 文档 | 路径 | 内容 |
|:---|:---|:---|
| 添加系统调用规范 | `Documentation/process/adding-syscalls.rst` | `SYSCALL_DEFINEn`, syscall 表注册, `copy_to_user`, 原型声明, `COND_SYSCALL` |
| RCU 链表遍历 | `Documentation/RCU/listRCU.rst` | `for_each_process()`, `rcu_read_lock`, RCU 宽限期, 进程链表 |
| RCU 基本概念 | `Documentation/RCU/rcu.rst` | RCU 读者不可阻塞/切换上下文/进入空闲 |
| /proc 文件系统 | `Documentation/filesystems/proc.rst` | `/proc/pid/stat` 字段定义 (utime/stime/vsize/rss/nice/num_threads) |
| Active MM | `Documentation/mm/active_mm.rst` | `task->mm == NULL` 内核线程语义 |

---

## 作者

Arch1mboldi · 2026年春季操作系统课程设计

---

🤖 本 README 部分内容由 Claude Code 辅助生成
