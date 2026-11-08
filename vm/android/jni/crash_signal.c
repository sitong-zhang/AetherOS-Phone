/* Aether VM — native 崩溃兜底（自研，极简）
 *
 * 目标：native 层发生致命信号时，用 async-signal-safe 的方式写一行简报到文件，
 * 然后「链式转发」给原有 signal handler —— 不改变进程原本的崩溃行为。
 *
 * 刻意不注册 SIGSEGV：ART 用 SIGSEGV 做隐式空指针/栈溢出检查，覆盖会破坏 JVM。
 * 只注册 SIGABRT / SIGBUS / SIGILL / SIGFPE。
 * Java 层冷启动时读取 native_crash.txt 并合并进崩溃报告弹窗。
 */
#include <jni.h>
#include <signal.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>

#define MAX_SIG 64
static char g_path[512];
static volatile sig_atomic_t g_ready = 0;
static struct sigaction g_old[MAX_SIG];

static void wr(int fd, const char *s, int n)
{
    while (n > 0) {
        ssize_t k = write(fd, s, (size_t)n);
        if (k <= 0) break;
        s += k; n -= (int)k;
    }
}

static int put_str(char *p, const char *s)
{
    int n = 0;
    while (*s) { *p++ = *s++; ++n; }
    return n;
}

static int put_dec(char *p, unsigned long v)
{
    char t[32]; int i = 0, n = 0;
    if (!v) { *p = '0'; return 1; }
    while (v) { t[i++] = (char)('0' + (v % 10)); v /= 10; }
    while (i) { *p++ = t[--i]; ++n; }
    return n;
}

static int put_hex(char *p, unsigned long v)
{
    static const char h[] = "0123456789abcdef";
    char t[32]; int i = 0, n = 0;
    if (!v) { *p = '0'; return 1; }
    while (v) { t[i++] = h[v & 15]; v >>= 4; }
    while (i) { *p++ = t[--i]; ++n; }
    return n;
}

static void on_fatal(int sig, siginfo_t *si, void *uc)
{
    (void)uc;
    if (g_ready) {
        int fd = open(g_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (fd >= 0) {
            char b[512]; int n = 0;
            n += put_str(b + n, "AETHER_NATIVE_CRASH signal=");
            n += put_dec(b + n, (unsigned long)sig);
            n += put_str(b + n, " code=");
            n += put_dec(b + n, (unsigned long)(si ? si->si_code : 0));
            n += put_str(b + n, " fault_addr=0x");
            n += put_hex(b + n, (unsigned long)(uintptr_t)(si ? si->si_addr : 0));
            n += put_str(b + n, " pid=");
            n += put_dec(b + n, (unsigned long)getpid());
            n += put_str(b + n, " tid=");
            n += put_dec(b + n, (unsigned long)gettid());
            b[n++] = '\n';
            wr(fd, b, n);
            close(fd);
        }
    }
    /* 恢复原有 handler 再重新投递：与未注册本 handler 时行为一致 */
    if (sig >= 0 && sig < MAX_SIG)
        sigaction(sig, &g_old[sig], NULL);
    else
        signal(sig, SIG_DFL);
    raise(sig);
    _exit(128 + sig);
}

JNIEXPORT void JNICALL
Java_dev_aether_vm_CrashGuard_nativeInstall(JNIEnv *env, jclass cls, jstring jdir)
{
    (void)cls;
    const char *dir = (*env)->GetStringUTFChars(env, jdir, NULL);
    if (!dir) return;
    snprintf(g_path, sizeof(g_path), "%s/native_crash.txt", dir);
    (*env)->ReleaseStringUTFChars(env, jdir, dir);
    g_ready = 1;

    static const int fatal[] = { SIGABRT, SIGBUS, SIGILL, SIGFPE };
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = on_fatal;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    for (size_t i = 0; i < sizeof(fatal) / sizeof(fatal[0]); ++i) {
        int s = fatal[i];
        if (s >= 0 && s < MAX_SIG) sigaction(s, &sa, &g_old[s]);
    }
}
