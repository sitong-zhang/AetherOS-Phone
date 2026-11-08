/*
 * aether-hello.c - AetherOS PC 演示程序
 * 简单打印欢迎语，可作为服务或手动运行，验证用户态可执行环境。
 */
#include <stdio.h>
#include <time.h>
#include <unistd.h>

int main(void)
{
    printf("Hello from AetherOS (PC) — 一个从零构建的 x86_64 电脑操作系统。\n");
    printf("PID=%d  UID=%d\n", (int)getpid(), (int)getuid());
    return 0;
}
