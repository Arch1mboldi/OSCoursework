#include <stdio.h>
#include <unistd.h>
#include <sys/syscall.h>
#include "proc_monitor.h" // 确保这里的头文件和内核中的完全一致

int main() {
    struct proc_info buf[10];
    int count = 0;
    
    // 假设系统调用号是 470
    long ret = syscall(470, buf, 10, &count);
    
    if (ret == 0) {
        printf("Successfully collected %d processes\n", count);
        for(int i = 0; i < count; i++) {
            printf("PID: %d, Comm: %s, Utime: %lu\n", 
                   buf[i].pid, buf[i].comm, buf[i].utime);
        }
    } else {
        perror("Syscall failed");
    }
    return 0;
}