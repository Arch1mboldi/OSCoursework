/*
 * user_app/test_syscall.c — 系统调用最小验证程序
 *
 * 用于在新内核启动后快速验证 3 个自定义系统调用是否注册成功。
 * 不依赖 proc_monitor.c，可独立编译运行。
 *
 * 编译: gcc -Wall -O2 -o test_syscall test_syscall.c
 * 运行: ./test_syscall
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <errno.h>
#include "proc_monitor.h"

#define TEST_COUNT 64

static void test_proc_collect(void)
{
	struct proc_info buf[TEST_COUNT];
	int count = 0;
	long ret;

	printf("[TEST 1] sys_proc_collect (%d) ... ", SYS_proc_collect);

	ret = syscall(SYS_proc_collect, buf, TEST_COUNT, &count);

	if (ret < 0) {
		printf("FAILED (ret=%ld, errno=%d: %s)\n",
		       ret, errno, strerror(errno));
		return;
	}

	printf("OK (收集到 %d 个进程)\n", count);
	if (count > 0) {
		printf("  示例: PID=%d, NAME=%s, STATE=%d\n",
		       buf[0].pid, buf[0].comm, buf[0].state);
	}
}

static void test_proc_snapshot(void)
{
	struct proc_tree_node buf[TEST_COUNT];
	int count = 0;
	long ret;

	printf("[TEST 2] sys_proc_snapshot (%d) ... ", SYS_proc_snapshot);

	ret = syscall(SYS_proc_snapshot, buf, TEST_COUNT, &count);

	if (ret < 0) {
		printf("FAILED (ret=%ld, errno=%d: %s)\n",
		       ret, errno, strerror(errno));
		return;
	}

	printf("OK (收集到 %d 个树节点)\n", count);
	if (count > 0) {
		printf("  示例: PID=%d, PPID=%d, NAME=%s, LEVEL=%d\n",
		       buf[0].pid, buf[0].ppid, buf[0].comm, buf[0].level);
	}
}

static void test_proc_stat(void)
{
	struct proc_stat stat;
	long ret;

	printf("[TEST 3] sys_proc_stat (%d) ... ", SYS_proc_stat);

	ret = syscall(SYS_proc_stat, &stat);

	if (ret < 0) {
		printf("FAILED (ret=%ld, errno=%d: %s)\n",
		       ret, errno, strerror(errno));
		return;
	}

	printf("OK\n");
	printf("  进程总数: %d | 运行: %d | 睡眠: %d | 僵尸: %d\n"
	       "  内核线程: %d | 用户进程: %d\n",
	       stat.total_processes, stat.running_processes,
	       stat.sleeping_processes, stat.zombie_processes,
	       stat.kernel_threads, stat.user_threads);
}

static void test_syscall_existence(void)
{
	long ret;
	printf("\n[TEST 0] 系统调用存在性检查\n");

	/* 用 NULL 参数调用，如果返回 -EINVAL 说明系统调用存在 */
	ret = syscall(SYS_proc_collect, NULL, 0, NULL);
	printf("  proc_collect(NULL): ret=%ld (期望 -EINVAL=-22)\n", ret);

	ret = syscall(SYS_proc_snapshot, NULL, 0, NULL);
	printf("  proc_snapshot(NULL): ret=%ld (期望 -EINVAL=-22)\n", ret);

	ret = syscall(SYS_proc_stat, NULL);
	printf("  proc_stat(NULL):     ret=%ld (期望 -EINVAL=-22)\n", ret);
}

int main(void)
{
	printf("=== 自定义系统调用验证程序 ===\n");
	printf("系统调用号: collect=%d, snapshot=%d, stat=%d\n",
	       SYS_proc_collect, SYS_proc_snapshot, SYS_proc_stat);

	test_syscall_existence();
	test_proc_stat();
	test_proc_collect();
	test_proc_snapshot();

	printf("\n=== 测试完成 ===\n");
	return 0;
}
