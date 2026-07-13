# supplementary.md — 核心代码与图表

本文档作为《操作系统课程设计实验报告》的补充材料，包含系统核心代码、架构图、数据结构布局和测试截图说明。

---

## 附录 1：内核模块核心代码

### 1.1 共享数据结构定义 (include/linux/proc_monitor.h)

```c
#ifndef _LINUX_PROC_MONITOR_H
#define _LINUX_PROC_MONITOR_H

#include <linux/types.h>

#ifndef TASK_COMM_LEN
#define TASK_COMM_LEN 16
#endif

struct proc_info {
    pid_t         pid;          /* 进程ID (TGID)              */
    pid_t         ppid;         /* 父进程ID                    */
    char          comm[TASK_COMM_LEN];  /* 进程名称 (15字符+\0) */
    int           state;        /* 进程状态 (__state | exit_state) */
    unsigned long utime;        /* 用户态CPU时间 (时钟滴答)    */
    unsigned long stime;        /* 内核态CPU时间 (时钟滴答)    */
    unsigned long vsize;        /* 虚拟内存大小 (字节)         */
    unsigned long rss;          /* 常驻内存集 (页数)           */
    int           nice;         /* nice 优先级 (-20 ~ 19)      */
    int           num_threads;  /* 线程组内线程数              */
    uid_t         uid;          /* 真实用户ID                  */
};

struct proc_tree_node {
    pid_t pid;                  /* 进程ID                      */
    pid_t ppid;                 /* 父进程ID                    */
    char  comm[TASK_COMM_LEN];  /* 进程名称                    */
    int   level;                /* 树深度 (init_task=0)        */
};

struct proc_stat {
    int total_processes;        /* 进程总数                    */
    int running_processes;      /* TASK_RUNNING                */
    int sleeping_processes;     /* TASK_INTERRUPTIBLE          */
    int uninterruptible;        /* TASK_UNINTERRUPTIBLE        */
    int stopped_processes;      /* TASK_STOPPED / TASK_TRACED  */
    int zombie_processes;       /* EXIT_ZOMBIE                 */
    int idle_processes;         /* TASK_IDLE / TASK_NEW        */
    int kernel_threads;         /* mm==NULL 的内核线程         */
    int user_threads;           /* mm!=NULL 的用户线程         */
};

#endif /* _LINUX_PROC_MONITOR_H */
```

### 1.2 sys_proc_collect (470) — 进程信息采集

```c
SYSCALL_DEFINE3(proc_collect,
        struct proc_info __user *, user_buf,
        int, max_count,
        int __user *, ret_count)
{
    struct proc_info kinfo;
    struct task_struct *task, *next;
    int count = 0;
    unsigned long nr_threads;

    if (!user_buf || max_count <= 0 || !ret_count)
        return -EINVAL;

    read_lock(&tasklist_lock);
    task = next_task(&init_task);  /* 跳过头结点 init_task */

    while (task != &init_task) {
        if (count >= max_count)
            break;

        memset(&kinfo, 0, sizeof(kinfo));

        /* 安全获取进程名 */
        get_task_comm(kinfo.comm, task);

        /* PID / PPID */
        kinfo.pid  = task_tgid_nr(task);
        kinfo.ppid = task_tgid_nr(task->real_parent);

        /* 进程状态: __state 与 exit_state 合并 */
        kinfo.state = task->__state | task->exit_state;

        /* CPU time: 纳秒 → USER_HZ 时钟滴答 */
        {
            u64 ut_ns, st_ns;
            task_cputime(task, &ut_ns, &st_ns);
            kinfo.utime = nsec_to_clock_t(ut_ns);
            kinfo.stime = nsec_to_clock_t(st_ns);
        }

        /* 内存信息: 内核线程 mm==NULL */
        if (task->mm) {
            kinfo.vsize = task->mm->total_vm << PAGE_SHIFT;
            kinfo.rss   = get_mm_rss(task->mm);
        } else {
            kinfo.vsize = 0;
            kinfo.rss   = 0;
        }

        kinfo.nice = task_nice(task);

        /* 线程数 */
        if (task->signal)
            nr_threads = task->signal->nr_threads;
        else
            nr_threads = 1;
        kinfo.num_threads = (int)nr_threads;

        /* UID: 从内核命名空间映射 */
        kinfo.uid = from_kuid_munged(current_user_ns(), task_uid(task));

        /*
         * 保存 next 指针后再释放锁:
         *   - copy_to_user() 可能睡眠, 不能在持锁时调用
         *   - task 在释放锁期间可能因进程退出而被 RCU 回收
         *   - 必须用保存的 next 指针继续遍历
         */
        next = next_task(task);
        read_unlock(&tasklist_lock);

        if (copy_to_user(&user_buf[count], &kinfo, sizeof(kinfo))) {
            /* 拷贝失败，尽力返回已成功拷贝的数量 */
            if (copy_to_user(ret_count, &count, sizeof(int)))
                return -EFAULT;
            return -EFAULT;
        }

        count++;
        read_lock(&tasklist_lock);
        task = next;  /* 使用保存的 next，而非 task->tasks.next */
    }

    read_unlock(&tasklist_lock);

    if (copy_to_user(ret_count, &count, sizeof(int)))
        return -EFAULT;

    return 0;
}
```

### 1.3 compute_level — 进程树深度计算

```c
static int compute_level(struct task_struct *task)
{
    int level = 0;
    struct task_struct *parent = task->real_parent;

    while (parent && parent != &init_task) {
        level++;
        parent = parent->real_parent;
    }
    return level;
}
```

### 1.4 sys_proc_stat (472) — 进程状态分类统计（核心分类逻辑）

```c
SYSCALL_DEFINE1(proc_stat, struct proc_stat __user *, stat)
{
    struct proc_stat kstat;
    struct task_struct *task;

    if (!stat)
        return -EINVAL;

    memset(&kstat, 0, sizeof(kstat));

    read_lock(&tasklist_lock);

    for_each_process(task) {
        kstat.total_processes++;

        /*
         * 先检查退出状态 (exit_state):
         *   EXIT_ZOMBIE — 僵尸进程
         *   EXIT_DEAD   — 进程正被回收, 不计数
         */
        if (task->exit_state & EXIT_ZOMBIE) {
            kstat.zombie_processes++;
        } else if (task->exit_state & EXIT_DEAD) {
            /* skip */
        } else {
            /*
             * __state 可能带有标志位:
             *   TASK_WAKEKILL (0x0080)
             *   TASK_NOLOAD   (0x0400)
             *   TASK_WAKING   (0x0100)
             * 必须先用掩码清除标志位, 再对基础状态分类
             */
            unsigned int s = task->__state;

            if (s & TASK_NOLOAD) {
                /* TASK_IDLE = UNINTERRUPTIBLE | NOLOAD */
                kstat.idle_processes++;
            } else {
                s &= ~(TASK_WAKEKILL | TASK_WAKING);
                switch (s) {
                case TASK_RUNNING:
                    kstat.running_processes++;
                    break;
                case TASK_INTERRUPTIBLE:
                    kstat.sleeping_processes++;
                    break;
                case TASK_UNINTERRUPTIBLE:
                    kstat.uninterruptible++;
                    break;
                case __TASK_STOPPED:
                case __TASK_TRACED:
                    kstat.stopped_processes++;
                    break;
                default:
                    kstat.idle_processes++;
                    break;
                }
            }
        }

        /* 内核线程 vs 用户线程 */
        if (task->mm == NULL)
            kstat.kernel_threads++;
        else
            kstat.user_threads++;
    }

    read_unlock(&tasklist_lock);

    if (copy_to_user(stat, &kstat, sizeof(kstat)))
        return -EFAULT;

    return 0;
}
```

---

## 附录 2：用户态程序核心代码

### 2.1 系统调用封装与耗时测量

```c
static long time_now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000000L + ts.tv_nsec / 1000L;
}

static int fetch_procs(void)
{
    long t0, t1;
    int count = 0;

    t0 = time_now_us();
    g_sc_collect.ret = syscall(SYS_proc_collect, g_procs, MAX_PROCS, &count);
    t1 = time_now_us();

    g_sc_collect.latency_us = t1 - t0;
    g_sc_collect.err = (g_sc_collect.ret < 0) ? errno : 0;
    g_sc_collect.count = count;

    if (g_sc_collect.ret < 0) {
        snprintf(g_last_msg, sizeof(g_last_msg),
                 "proc_collect: ret=%ld errno=%d (%s)",
                 g_sc_collect.ret, errno, strerror(errno));
        return -1;
    }
    g_proc_count = count;
    return 0;
}
```

### 2.2 进程状态解码（位掩码方式）

```c
static char state_char(int state)
{
    if (state & 0x20) return 'Z';  /* EXIT_ZOMBIE   */
    if (state & 0x10) return 'X';  /* EXIT_DEAD     */
    if (state & 0x08) return 't';  /* __TASK_TRACED  */
    if (state & 0x04) return 'T';  /* __TASK_STOPPED */
    switch (state & 0x03) {
    case 0:  return 'R';  /* RUNNING */
    case 1:  return 'S';  /* INTERRUPTIBLE */
    case 2:  return 'D';  /* UNINTERRUPTIBLE */
    default: return '?';
    }
}

static const char *state_name(int state)
{
    if (state & 0x20) return "ZOMBIE";
    if (state & 0x10) return "DEAD";
    if (state & 0x08) return "TRACED";
    if (state & 0x04) return "STOPPED";
    switch (state & 0x03) {
    case 0:  return "RUNNING";
    case 1:  return "SLEEPING";
    case 2:  return "DISK_WAIT";
    default: return "IDLE/OTHER";
    }
}
```

### 2.3 CPU% 差值计算

```c
static double cpu_pct(pid_t pid, unsigned long cur_total)
{
    struct prev_cpu key = { .pid = pid };
    struct prev_cpu *found;
    unsigned long prev_total;
    double elapsed;

    if (!g_has_prev || g_prev_count <= 0)
        return 0.0;

    found = bsearch(&key, g_prev, g_prev_count,
                    sizeof(struct prev_cpu), cmp_prev_pid);
    if (!found)
        return 0.0;

    prev_total = found->total;
    if (cur_total < prev_total)
        return 0.0;  /* PID 复用: 新进程 CPU 更少 */

    /* elapsed = 当前帧采集时间 - 上一帧采集时间 (秒) */
    elapsed = (g_tv_curr.tv_sec  - g_tv_prev.tv_sec) +
              (g_tv_curr.tv_usec - g_tv_prev.tv_usec) / 1000000.0;

    if (elapsed <= 0.0)
        return 0.0;

    return (double)(cur_total - prev_total)
           / (double)clk_tck / elapsed * 100.0;
}
```

### 2.4 交叉验证（用户态自统计 vs proc_stat）

```c
/* 在 SYSCALL 视图底部执行交叉验证 */
{
    int local_r = 0, local_s = 0, local_d = 0;
    int local_t = 0, local_z = 0, local_i = 0;

    for (int i = 0; i < g_proc_count; i++) {
        int st = g_procs[i].state;

        if (st & 0x20) {        /* EXIT_ZOMBIE */
            local_z++;
        } else if (!(st & 0x10)) {  /* not EXIT_DEAD */
            /* 与内核 proc_stat 保持一致的分类逻辑:
             * TASK_NOLOAD (0x0400) → idle
             * 掩码 TASK_WAKEKILL (0x0080) */
            if (st & 0x0400) {
                local_i++;
            } else {
                int base = (st & ~0x0080) & 0x03;
                switch (base) {
                case 0: local_r++; break;
                case 1: local_s++; break;
                case 2: local_d++; break;
                }
            }
        }
        if (st & 0x04) local_t++;
    }

    /* 对比显示:
     * R: proc_info=3     proc_stat=3     OK
     * S: proc_info=210   proc_stat=210   OK
     * D: proc_info=0     proc_stat=0     OK
     * Z: proc_info=0     proc_stat=0     OK
     * I: proc_info=168   proc_stat=168   OK
     */
}
```

### 2.5 三格式导出

```c
static void do_export(void)
{
    char base[256] = "proc_monitor";

    /* 交互式输入文件名前缀 (需要临时退出 ncurses) */
    def_prog_mode();
    endwin();
    printf("Export all formats (CSV + DOT + BIN) to [%s]: ", base);
    fflush(stdout);
    {
        char buf[256];
        if (fgets(buf, sizeof(buf), stdin) && buf[0] != '\n') {
            size_t l = strlen(buf);
            if (l > 0 && buf[l-1] == '\n') buf[l-1] = '\0';
            snprintf(base, sizeof(base), "%s", buf);
        }
    }
    reset_prog_mode();
    refresh();

    update_data();  /* 确保拿到最新数据 */

    /* 一次性导出全部三种格式 */
    char fname[280];
    snprintf(fname, sizeof(fname), "%s.csv", base);
    export_csv(fname);
    snprintf(fname, sizeof(fname), "%s.dot", base);
    export_tree_dot(fname);
    snprintf(fname, sizeof(fname), "%s.bin", base);
    export_raw(fname);
}
```

---

## 附录 3：图表

### 3.1 系统架构图

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

**数据流向**: 内核遍历 task_struct 链表 → 提取字段填入 proc_info/proc_tree_node → copy_to_user() → 用户态 buffer → TUI 渲染。

### 3.2 x86_64 系统调用完整流程

```
用户态 (Ring 3, CPL=3)              内核态 (Ring 0, CPL=0)
========================            ==========================

user_program()
  │
  ├─ syscall(SYS_proc_collect, ...)
  │    │
  │    └─ glibc 封装:
  │         mov $470, %rax    ← 系统调用号
  │         mov $buf,  %rdi   ← 参数1
  │         mov $cnt,  %rsi   ← 参数2
  │         mov $ret,  %rdx   ← 参数3
  │         syscall           ← CPU 指令
  │
  └────────────┬──────────────────────┐
               │  硬件自动:            │
               │  RCX ← RIP           │
               │  R11 ← RFLAGS        │
               │  RIP ← MSR_LSTAR     │
               │  CPL ← 0             │
               │                      ▼
               │              entry_SYSCALL_64 (asm)
               │                │
               │                ├─ swapgs
               │                ├─ mov %rsp, per-CPU(usr_rsp)
               │                ├─ mov per-CPU(kernel_rsp), %rsp
               │                ├─ push pt_regs frame
               │                ├─ sti (开中断)
               │                │
               │                ▼
               │              do_syscall_64(regs)
               │                │
               │                ├─ nr = regs->ax  (= 470)
               │                ├─ if (nr < NR_syscalls)
               │                │    f = sys_call_table[nr]
               │                │    regs->ax = f(regs->di,
               │                │                 regs->si,
               │                │                 regs->dx)
               │                │
               │                ▼
               │              ┌─────────────────────────┐
               │              │ sys_proc_collect(        │
               │              │   user_buf, max_count,   │
               │              │   ret_count)             │
               │              │                          │
               │              │ read_lock(&tasklist_lock)│
               │              │ for_each_process():      │
               │              │   提取 proc_info 字段    │
               │              │   copy_to_user()         │
               │              │ read_unlock(..)          │
               │              │ return 0                 │
               │              └─────────────────────────┘
               │                │
               │                ▼
               │              sysretq  ← CPU 指令
               │              ┌──────────────────────┐
               │              │ RIP ← RCX            │
               │              │ RFLAGS ← R11         │
               │              │ CPL ← 3              │
               └──────────────┴──────────────────────┘
                              │
  (回到用户态，继续执行) ◄─────┘
```

### 3.3 struct proc_info 在 x86_64 LP64 ABI 下的内存布局

```
Offset  Size  Content                                 ASCII Preview
──────  ────  ──────────────────────────────────────  ────────────
0x00    4     pid (int, 4B)                           [LSB ... MSB]
0x04    4     ppid (int, 4B)                          [LSB ... MSB]
0x08    16    comm[16] (char[], 最多15字符+\0)          "systemd\0\0\0\0..."
0x18    4     state (int, __state | exit_state)       [LSB ... MSB]
0x1C    4     (padding, 对齐 unsigned long)            [00 00 00 00]
0x20    8     utime (unsigned long, 8B)               [LSB ... MSB]
0x28    8     stime (unsigned long, 8B)               [LSB ... MSB]
0x30    8     vsize (unsigned long, 8B)               [LSB ... MSB]
0x38    8     rss (unsigned long, 8B)                 [LSB ... MSB]
0x40    4     nice (int, 4B)                          [LSB ... MSB]
0x44    4     num_threads (int, 4B)                   [LSB ... MSB]
0x48    4     uid (uid_t, 4B)                         [LSB ... MSB]
0x4C    4     (padding, 对齐到 8B 边界)                [00 00 00 00]
──────  ────  ──────────────────────────────────────  ────────────
Total: 80 bytes (0x50)

对齐规则 (x86_64 LP64 ABI):
  - int / pid_t / uid_t:  4 字节对齐 (偏移 0,4,24,64,68,72)
  - unsigned long:         8 字节对齐 (偏移 32,40,48,56)
  - char[16]:              1 字节对齐 (偏移 8)
  - 编译器自动插入 2 个 4 字节 padding (偏移 28, 76)
```

### 3.4 lock-drop-copy-relock 锁模式示意图

```
时间线
────────────────────────────────────────────────────►

  task1 (PID=1)        task2 (PID=10)       task3 (PID=50)
  ┌──────────┐        ┌──────────┐        ┌──────────┐
  │init_task │───→────│ task1    │───→────│ task2    │───→ ...
  └──────────┘        └──────────┘        └──────────┘

第 1 次迭代:
  read_lock()  ──→  read_lock()
  task=task1        kinfo ← 从 task1 提取
                    next = next_task(task1) = task2  ← 持锁保存
  ──────────────────────────────────────────
  read_unlock()
  copy_to_user(&buf[0], &kinfo, 80)  ← 可睡眠 (缺页→磁盘 I/O)
  ──────────────────────────────────────────
  read_lock()  ──→  read_lock()
  task = next = task2  ← 使用保存的指针

第 2 次迭代:
                    kinfo ← 从 task2 提取
                    next = next_task(task2) = task3
  ──────────────────────────────────────────
  read_unlock()
  copy_to_user(&buf[1], &kinfo, 80)
  ──────────────────────────────────────────
  read_lock()

  ... 重复直至遍历完链表 ...

关键约束:
  - RCU 读者不能睡眠: copy_to_user() 在 read_unlock() 之后调用
  - UAF 防护: next 值持锁时保存, task 被释放不影响后续遍历
  - task->tasks.next 不可用: task 被 RCU 回收后, 通过 task 解引用是 UAF
```

### 3.5 进程状态位掩码表

```
__state 调度状态位:
  ┌────────┬──────┬──────────────────────────┐
  │ 值     │ 符号 │ 含义                     │
  ├────────┼──────┼──────────────────────────┤
  │ 0x0000 │ R    │ TASK_RUNNING — 可运行    │
  │ 0x0001 │ S    │ TASK_INTERRUPTIBLE — 可中断睡眠 │
  │ 0x0002 │ D    │ TASK_UNINTERRUPTIBLE — 不可中断 │
  │ 0x0004 │ T    │ __TASK_STOPPED — 已停止  │
  │ 0x0008 │ t    │ __TASK_TRACED — 被跟踪   │
  │ 0x0040 │      │ TASK_DEAD — 调度中死亡   │
  └────────┴──────┴──────────────────────────┘

__state 标志位 (可与基础状态位组合):
  ┌────────┬────────────────────────────┐
  │ 值     │ 含义                       │
  ├────────┼────────────────────────────┤
  │ 0x0080 │ TASK_WAKEKILL — 可被信号唤醒 │
  │ 0x0100 │ TASK_WAKING — 正在被唤醒    │
  │ 0x0200 │ TASK_PARKED — 已停驻       │
  │ 0x0400 │ TASK_NOLOAD — 不计入负载    │
  │ 0x0800 │ TASK_NEW — 新创建           │
  └────────┴────────────────────────────┘

exit_state 退出状态位:
  ┌────────┬──────┬──────────────────────────┐
  │ 值     │ 符号 │ 含义                     │
  ├────────┼──────┼──────────────────────────┤
  │ 0x0010 │ X    │ EXIT_DEAD — 完全销毁     │
  │ 0x0020 │ Z    │ EXIT_ZOMBIE — 僵尸进程   │
  │ 0x0040 │      │ EXIT_TRACE — 退出跟踪中  │
  └────────┴──────┴──────────────────────────┘

组合示例:
  ┌─────────┬────────────────────────────────┐
  │ state   │ 含义                           │
  ├─────────┼────────────────────────────────┤
  │ 0x0000  │ RUNNING                        │
  │ 0x0001  │ S (INTERRUPTIBLE)              │
  │ 0x0081  │ S (INTERRUPTIBLE | WAKEKILL)   │
  │ 0x0002  │ D (UNINTERRUPTIBLE)            │
  │ 0x0402  │ IDLE (TASK_IDLE = D | NOLOAD)  │
  │ 0x0022  │ Z (ZOMBIE, exit_state)         │
  │ 0x0004  │ T (STOPPED)                    │
  └─────────┴────────────────────────────────┘

位掩码分类 vs 精确匹配的错误示例:
  精确 switch(0x0081): default → 误判为 IDLE
  位掩码 (0x0081 & ~0x80 & 0x03): → 0x01 → 正确识别为 SLEEPING
```

### 3.6 proc_info 字段与 /proc/pid/stat 的对应关系

```
proc_info 字段      /proc/pid/stat 字段索引   单位
────────────────    ─────────────────────     ────────
pid                 1 (pid)                   —
comm                2 (comm)                  字符串
state               3 (state)                 __state | exit_state 位掩码
ppid                4 (ppid)                  —
utime               14 (utime)                USER_HZ 时钟滴答
stime               15 (stime)                USER_HZ 时钟滴答
nice                19 (nice)                 -20 到 19
num_threads         20 (num_threads)          —
vsize               23 (vsize)                字节
rss                 24 (rss)                  页数
uid                 来自 /proc/pid/status     —

获取方式对比:
  /proc/pid/stat:  open() + read() → 文本解析
  自定义 syscall:  一次 syscall(470) → 直接填充二进制 struct proc_info[]

  自定义 syscall 优势:
    - 无文件系统开销 (不需要 open/read/close 系统调用)
    - 无文本解析开销 (直接二进制填充)
    - 无格式不确定性 (/proc 字段语义可能跨内核版本变化)
```

### 3.7 TUI 界面布局

```
┌────────────────────── win_main (rows-6) ──────────────────────┐
│ ProcMon [LIST] | Sort:CPU ^ | FILT | 381 procs |             │
│                                                               │
│ PID     PPID    NAME           S:HEX  CPU%   UTIME   STIME   │
│ ───────────────────────────────────────────────────────────── │
│ 1       0       systemd        S:0001   0.0    1.2     0.8   │
│ 289     1       systemd-journ  S:0001   0.0    0.3     0.1   │  ← 选中行反色
│ 514     1       systemd-udevd  S:0001   0.0    0.1     0.0   │
│ ...     ...     ...            ...     ...    ...     ...    │
│                                                               │
└───────────────────────────────────────────────────────────────┘
┌─────────────────────── win_detail (2 rows) ───────────────────┐
│ PID=289   PPID=1    comm="systemd-journal" state=0x0001(S:.. │
│ utime=30   stime=12   vsize=128.0M(134217728B) rss=8.0K(2pg)│
└───────────────────────────────────────────────────────────────┘
┌────────────────────── win_syscall (2 rows) ───────────────────┐
│ collect(470): ret=0 err=0 312us/381 | snapshot(471): ret=0   │
│ T:381 R:3 S:210 D:0 T:0 Z:0 I:168 | Kthr:171 Uthr:210 |     │
└───────────────────────────────────────────────────────────────┘
┌───────────────────────── win_hint (1 row) ────────────────────┐
│ v:view[LIST] ↑↓:sel Tab:sort s:rev /:filter r:clear x:export │
└───────────────────────────────────────────────────────────────┘
```

### 3.8 交叉验证结果示例

SYSCALL 视图底部显示的交叉验证输出（正常情况）：

```
--- Cross-check (proc_info[] vs proc_stat) ---
R: proc_info=3      proc_stat=3      OK
S: proc_info=210    proc_stat=210    OK
D: proc_info=0      proc_stat=0      OK
Z: proc_info=0      proc_stat=0      OK
I: proc_info=168    proc_stat=168    OK
Note: mismatches expected due to race between collect & stat syscalls
```

### 3.9 系统调用耗时参考数据

在 381 个进程的测试环境中测得：

```
系统调用            典型延迟 (μs)    说明
──────────────      ────────────     ──────────────────
proc_collect (470)  300 - 2000      遍历 381 个进程 + 381 次 copy_to_user
proc_snapshot (471) 300 - 1500      遍历 381 个进程 + level 计算
proc_stat (472)     50  - 100       一次遍历, 仅整数运算, 全程持锁
```

---

*本文件为实验报告补充材料，与 实验报告正文.md 配合使用。*
