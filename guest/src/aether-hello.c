/*
 * aether-hello.c - AetherOS 用户态自检演示程序
 * 简单打印欢迎语，可作为服务或手动运行，验证用户态可执行环境。
 */
#include <stdio.h>
#include <time.h>
#include <unistd.h>

int main(void)
{
    printf("Hello from AetherOS Phone — 内置于 Android APK 的手机操作系统用户态。\n");
    printf("PID=%d  UID=%d\n", (int)getpid(), (int)getuid());
    return 0;
}
