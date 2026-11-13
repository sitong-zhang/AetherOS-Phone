/*
 * AetherOS 用户态 API（ring3）
 * 与内核共享 aether_kconfig.h 的系统调用号；不链接 libc。
 */
#ifndef AETHER_UAPI_H
#define AETHER_UAPI_H
#include "aether_types.h"
#include "aether_kconfig.h"

extern long usyscall(long nr, long a1, long a2, long a3, long a4, long a5);

static inline long u_write(const char *s) {
    unsigned long n = 0; while (s[n]) n++;
    return usyscall(K_SYS_WRITE, (long)s, (long)n, 0, 0, 0);
}
static inline long u_get_ticks(void) { return usyscall(K_SYS_GET_TICKS, 0,0,0,0,0); }
static inline long u_boot_magic(void) { return usyscall(K_SYS_BOOT_MAGIC, 0,0,0,0,0); }
static inline long u_sysinfo(char *b, long n) { return usyscall(K_SYS_SYSINFO,(long)b,n,0,0,0); }
static inline long u_login(const char *pw, const char *duress) {
    return usyscall(K_SYS_LOGIN, (long)pw, (long)duress, 0,0,0);
}
static inline long u_unlock(const char *pw, char *msg, long n) {
    return usyscall(K_SYS_UNLOCK, (long)pw, (long)msg, n, 0,0);
}
static inline long u_lock(void) { return usyscall(K_SYS_LOCK,0,0,0,0,0); }
static inline long u_put(const char *name, const void *data, long len) {
    return usyscall(K_SYS_VAULT_PUT, (long)name, (long)data, len, 0,0);
}
static inline long u_get(const char *name, void *buf, long n) {
    return usyscall(K_SYS_VAULT_GET, (long)name, (long)buf, n, 0,0);
}
static inline long u_list(char *b, long n) { return usyscall(K_SYS_VAULT_LIST,(long)b,n,0,0,0); }
static inline long u_wipe_status(char *b, long n) { return usyscall(K_SYS_WIPE_STATUS,(long)b,n,0,0,0); }
static inline long u_audit(char *b, long n) { return usyscall(K_SYS_AUDIT_DUMP,(long)b,n,0,0,0); }
static inline long u_selftest(char *b, long n) { return usyscall(K_SYS_SELFTEST,(long)b,n,0,0,0); }
static inline long u_read(char *b, long n) { return usyscall(K_SYS_READ,(long)b,n,0,0,0); }

/* 用户态极简字符串工具（不依赖 libc） */
static inline unsigned long ustrlen(const char *s) { unsigned long n=0; while(s[n])n++; return n; }
static inline int ustrcmp(const char *a, const char *b) {
    unsigned long i=0; while(a[i]&&a[i]==b[i])i++;
    return (int)(unsigned char)a[i]-(int)(unsigned char)b[i];
}
static inline void ustrcpy(char *d, const char *s) { unsigned long i=0; while((d[i]=s[i]))i++; }
static inline char *uintoa(char *b, unsigned long v) {
    char t[32]; int n=0;
    do { t[n++]=(char)('0'+(v%10)); v/=10; } while(v&&n<32);
    int k=0; while(n>0) b[k++]=t[--n];
    b[k]=0; return b;
}

#endif /* AETHER_UAPI_H */
