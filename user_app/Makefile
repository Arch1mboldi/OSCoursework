# user_app/Makefile — 用户态进程监控程序编译脚本
#
# 用法:
#   make          — 编译 proc_monitor
#   make clean    — 清理编译产物
#   make run      — 编译并以 sudo 运行
#   make test     — 编译并运行最小验证程序

CC      = gcc
CFLAGS  = -Wall -Wextra -O2 -std=c11
LDFLAGS =

TARGET  = proc_monitor
TEST    = test_syscall
SRCS    = proc_monitor.c
OBJS    = $(SRCS:.c=.o)

.PHONY: all clean run test help

all: $(TARGET)

$(TARGET): $(OBJS) proc_monitor.h
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LDFLAGS)

%.o: %.c proc_monitor.h
	$(CC) $(CFLAGS) -c -o $@ $<

$(TEST): test_syscall.c proc_monitor.h
	$(CC) $(CFLAGS) -o $@ test_syscall.c $(LDFLAGS)

test: $(TEST)
	@echo "=== 测试系统调用是否可用 ==="
	./$(TEST)

run: $(TARGET)
	@echo "=== 启动进程监控系统 (需要 root 权限) ==="
	sudo ./$(TARGET)

clean:
	rm -f $(TARGET) $(TEST) $(OBJS) proc_tree.dot

help:
	@echo "可用目标:"
	@echo "  make         编译 proc_monitor"
	@echo "  make clean   清理编译产物"
	@echo "  make run     以 sudo 运行监控程序"
	@echo "  make test    编译并运行系统调用验证"
