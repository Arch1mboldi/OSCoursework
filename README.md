# 实验三：Linux 系统调用 —— 进程全量监控与分析系统

操作系统课程设计 · 实验三
Linux 6.18 内核 · 自定义系统调用 (470/471/472)
2026年7月

---

## 项目概述

在 Linux 6.18 内核中新增 3 个自定义系统调用，并编写用户态 ncurses 终端程序，
通过 `syscall()` 直接调用，实现进程全量监控。

**3 个系统调用**:
- `sys_proc_collect` (470) — 采集所有进程的 PID、PPID、状态、CPU 时间、内存、线程数、UID
- `sys_proc_snapshot` (471) — 返回进程树父子关系拓扑，含层级深度
- `sys_proc_stat` (472) — 按状态分类的进程计数

**用户态程序**: ncurses 终端界面，4 面板布局（主视图、详情、系统调用诊断、按键提示），
5 种视图模式（列表、原始数值表、进程树、十六进制内存 dump、系统调用诊断面板），
支持排序、过滤、导出。

详细设计文档参见 [`principles.md`](principles.md)。

---

## 仓库结构

```
OSCoursework/
├── kernel/proc_monitor.c          # 3 个系统调用的内核实现
├── include/linux/proc_monitor.h   # 内核-用户态共享数据结构
├── user_app/
│   ├── proc_monitor.h             # 用户态头文件
│   ├── proc_monitor.c             # ncurses TUI 主程序
│   ├── test_syscall.c             # 系统调用验证程序
│   └── Makefile
├── README.md
└── principles.md                  # 详细原理与实现文档
```

---

## 快速开始

### 内核侧

将文件放入内核源码树，注册系统调用号，编译安装：

```bash
cp include/linux/proc_monitor.h  ~/linux-6.18/include/linux/
cp kernel/proc_monitor.c         ~/linux-6.18/kernel/
echo "obj-y += proc_monitor.o" >> ~/linux-6.18/kernel/Makefile

# 编辑 arch/x86/entry/syscalls/syscall_64.tbl，末尾添加:
#   470  common  proc_collect   sys_proc_collect
#   471  common  proc_snapshot  sys_proc_snapshot
#   472  common  proc_stat      sys_proc_stat

cd ~/linux-6.18
make localmodconfig && make -j$(nproc) && make modules -j$(nproc)
sudo make modules_install && sudo make install
sudo update-grub && sudo reboot
```

### 用户态

```bash
sudo apt install libncurses-dev
cd user_app
make
make test        # 验证系统调用
sudo ./proc_monitor
```

---

## 关键设计

**为什么定义独立的数据结构**:
`task_struct` 包含数百个字段和内核内部状态。直接暴露会导致安全风险（泄露内核地址）、
兼容性问题（内核升级破坏用户态）。因此定义 `proc_info` / `proc_tree_node` / `proc_stat`
作为内核与用户态之间的数据格式约定，两边头文件逐字节一致。

**为什么必须用 `copy_to_user()`**:
内核和用户态的地址空间隔离。不能把内核指针传给用户态（段错误），也不能在内核态直接
解引用用户态指针（SMAP 硬件保护）。`copy_to_user()` 是唯一安全通道：它执行地址合法性
检查、临时关闭 SMAP、处理缺页异常。

**锁策略**:
进程链表由 RCU 和 `tasklist_lock` 保护。关键约束——RCU 读者不能睡眠，而 `copy_to_user()`
可能因缺页触发磁盘 I/O 而睡眠。因此必须采用 lock-drop-copy-relock 模式：持锁读取数据
并保存 `next_task()` 指针，释放锁后调用 `copy_to_user()`，再重新加锁用保存的指针继续遍历。

**状态编码**:
Linux 5.14+ 将进程状态拆分为 `task->__state`（调度状态）和 `task->exit_state`（退出状态）。
内核通过 `state = __state | exit_state` 合并传递。`__state` 可能带有标志位
（TASK_WAKEKILL=0x80, TASK_NOLOAD=0x400），分类时必须先用位掩码清除标志位，不能用精确值匹配。

---

## 内核 API 参考

本项目使用的 Linux 内核函数：

- `SYSCALL_DEFINEn()` (`<linux/syscalls.h>`) — 定义系统调用入口
- `for_each_process()`, `next_task()`, `get_task_comm()`, `task_tgid_nr()`, `task_nice()` (`<linux/sched.h>`) — 进程遍历与字段读取
- `read_lock()`, `read_unlock()` — tasklist_lock 读写锁
- `task_cputime()` (`<linux/sched/cputime.h>`) — 读取 CPU 时间（纳秒）
- `nsec_to_clock_t()` (`<linux/jiffies.h>`) — 纳秒转换为 USER_HZ 滴答
- `get_mm_rss()`, `PAGE_SHIFT` (`<linux/mm.h>`) — 读取内存信息
- `from_kuid_munged()` (`<linux/cred.h>`) — UID 命名空间映射
- `copy_to_user()` (`<linux/uaccess.h>`) — 内核到用户态数据拷贝

本项目自实现的函数：
- 内核侧：`sys_proc_collect()` (470), `sys_proc_snapshot()` (471), `sys_proc_stat()` (472), `compute_level()`
- 用户态：`fetch_procs()`, `update_data()`, `cpu_pct()`, `state_decode()`, 排序/过滤/渲染/导出函数

---

## 相关文档

- Linux 内核: `Documentation/process/adding-syscalls.rst` — 系统调用注册规范
- Linux 内核: `Documentation/RCU/listRCU.rst` — RCU 链表遍历
- Linux 内核: `Documentation/filesystems/proc.rst` — `/proc/pid/stat` 字段定义
- Linux 内核: `Documentation/mm/active_mm.rst` — 内核线程 (`mm==NULL`) 语义
- 本项目: [`principles.md`](principles.md) — 完整原理说明、内存布局、调试指南、bug 记录

---

## 作者

Arch1mboldi · 2026年春季操作系统课程设计
