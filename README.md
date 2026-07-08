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
│   ├── proc_monitor.c                   # 用户态监控主程序
│   ├── test_syscall.c                   # 最小验证程序 (测试3个系统调用)
│   └── Makefile                         # 用户态编译脚本
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

# (4) 编译内核
cd ~/linux-6.18
make olddefconfig
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

| 要点 | 说明 |
|:---|:---|
| 锁机制 | `read_lock(&tasklist_lock)` 保护进程链表遍历 |
| 锁释放时机 | `copy_to_user()` 可能睡眠，必须在释放锁后调用 |
| 内核线程处理 | `mm==NULL` 的内核线程无用户态内存，vsize/rss 填 0 |
| 状态编码 | Linux 6.x 使用 `task->__state`，编码值与旧版 `state` 不同 |
| 逐个拷贝 | 每个条目单独 `copy_to_user()`，避免内核栈分配过大数组 |

---

## 相关资源

- [Linux Kernel 6.18](https://kernel.org/)
- [Linux 内核源码在线](https://git.kernel.org/pub/scm/linux/kernel/git/stable/linux.git)
- 详细设计文档: 参见仓库同级的 `plan.md`

---

## 作者

Arch1mboldi · 2026年春季操作系统课程设计

---

🤖 本 README 部分内容由 Claude Code 辅助生成
