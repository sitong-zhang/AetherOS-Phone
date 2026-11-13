/*
 * aether_store.c - 隔离存储后端实现
 */
#define _GNU_SOURCE
#include "aether_store.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <limits.h>
#include <sys/stat.h>
#include <sys/types.h>

struct ae_store { char root[PATH_MAX]; };

static int valid_zone(const char *z)
{
    return z && (strcmp(z, AE_ZONE_SYS)==0 || strcmp(z, AE_ZONE_DATA)==0 ||
                 strcmp(z, AE_ZONE_JOURNAL)==0 || strcmp(z, AE_ZONE_RECEIPT)==0);
}
static int valid_name(const char *n)
{
    if (!n || !*n || strlen(n) >= AE_STORE_NAME_MAX) return 0;
    if (strchr(n, '/') || strstr(n, "..")) return 0;
    return 1;
}
static int mkdir_p(const char *p)
{
    char tmp[PATH_MAX];
    size_t n = strlen(p);
    if (n >= sizeof tmp) return -1;
    memcpy(tmp, p, n+1);
    if (tmp[n-1]=='/') tmp[n-1]=0;
    for (char *q = tmp+1; *q; q++) {
        if (*q=='/') { *q=0; if (mkdir(tmp,0755) && errno!=EEXIST) return -1; *q='/'; }
    }
    if (mkdir(tmp,0755) && errno!=EEXIST) return -1;
    return 0;
}
static void fsync_dir(const char *dirpath)
{
    int fd = open(dirpath, O_RDONLY | O_DIRECTORY);
    if (fd >= 0) { (void)fsync(fd); close(fd); }
}

int ae_store_open(const char *root, ae_store_t **out)
{
    if (!root || !*root || !out) return -1;
    *out = NULL;
    if (mkdir_p(root) != 0) return -1;
    char z[PATH_MAX];
    static const char *zones[] = {AE_ZONE_SYS, AE_ZONE_DATA, AE_ZONE_JOURNAL, AE_ZONE_RECEIPT};
    size_t i;
    for (i = 0; i < 4; i++) {
        if (snprintf(z,sizeof z,"%s/%s",root,zones[i]) >= (int)sizeof z) return -1;
        if (mkdir_p(z) != 0) return -1;
    }
    ae_store_t *s = (ae_store_t*)calloc(1,sizeof *s);
    if (!s) return -1;
    if (!realpath(root, s->root)) { free(s); return -1; }
    *out = s;
    return 0;
}
void ae_store_close(ae_store_t *s) { free(s); }
const char *ae_store_root(ae_store_t *s) { return s ? s->root : ""; }

int ae_store_path(ae_store_t *s, char *buf, size_t n,
                  const char *zone, const char *name)
{
    if (!s || !buf || !valid_zone(zone) || !valid_name(name)) return -1;
    int r = snprintf(buf, n, "%s/%s/%s", s->root, zone, name);
    return (r < 0 || (size_t)r >= n) ? -1 : 0;
}

int ae_store_atomic_write(ae_store_t *s, const char *zone, const char *name,
                          const void *buf, size_t len)
{
    char path[PATH_MAX], tmp[PATH_MAX];
    if (ae_store_path(s, path, sizeof path, zone, name) != 0) return -1;
    if (snprintf(tmp,sizeof tmp,"%s.tmp",path) >= (int)sizeof tmp) return -1;
    int fd = open(tmp, O_WRONLY|O_CREAT|O_TRUNC, 0600);
    if (fd < 0) return -1;
    const uint8_t *p = (const uint8_t*)buf;
    size_t off = 0;
    while (off < len) {
        ssize_t w = write(fd, p+off, len-off);
        if (w < 0) { if (errno==EINTR) continue; close(fd); unlink(tmp); return -1; }
        off += (size_t)w;
    }
    if (fsync(fd) != 0) { close(fd); unlink(tmp); return -1; }
    close(fd);
    if (rename(tmp, path) != 0) { unlink(tmp); return -1; }
    /* 目录 fsync 让 rename 本身持久化 */
    char dir[PATH_MAX];
    if (snprintf(dir,sizeof dir,"%s/%s",s->root,zone) < (int)sizeof dir) fsync_dir(dir);
    return 0;
}

int ae_store_read(ae_store_t *s, const char *zone, const char *name,
                  uint8_t **out, size_t *len)
{
    if (!out || !len) return -1;
    *out = NULL; *len = 0;
    char path[PATH_MAX];
    if (ae_store_path(s, path, sizeof path, zone, name) != 0) return -1;
    FILE *f = fopen(path,"rb");
    if (!f) return -1;
    if (fseek(f,0,SEEK_END)!=0) { fclose(f); return -1; }
    long sz = ftell(f);
    if (sz < 0) { fclose(f); return -1; }
    rewind(f);
    /* +1 并补 '\0'：上层会把文本类文件（审计链/基线）当 C 字符串解析，
       缺少终止符会导致 strtok_r/sscanf 越界读，进而把完好的链误判为篡改。*/
    uint8_t *b = (uint8_t*)malloc((size_t)sz + 1);
    if (!b) { fclose(f); return -1; }
    if (sz > 0 && fread(b,1,(size_t)sz,f) != (size_t)sz) { free(b); fclose(f); return -1; }
    fclose(f);
    b[(size_t)sz] = 0;
    *out = b; *len = (size_t)sz;
    return 0;
}

int ae_store_exists(ae_store_t *s, const char *zone, const char *name)
{
    char path[PATH_MAX];
    if (ae_store_path(s, path, sizeof path, zone, name) != 0) return 0;
    struct stat st;
    return stat(path,&st)==0 && S_ISREG(st.st_mode);
}
int ae_store_remove(ae_store_t *s, const char *zone, const char *name)
{
    char path[PATH_MAX];
    if (ae_store_path(s, path, sizeof path, zone, name) != 0) return -1;
    return unlink(path)==0 ? 0 : (errno==ENOENT ? 0 : -1);
}

static int cmp_str(const void *a, const void *b)
{
    return strcmp((const char*)a,(const char*)b);
}

int ae_store_list(ae_store_t *s, const char *zone,
                  char (*names)[AE_STORE_NAME_MAX], int max, int *count)
{
    if (!count) return -1;
    *count = 0;
    if (!names || max <= 0) return -1;
    char dir[PATH_MAX];
    if (!valid_zone(zone)) return -1;
    if (snprintf(dir,sizeof dir,"%s/%s",s->root,zone) >= (int)sizeof dir) return -1;
    DIR *d = opendir(dir);
    if (!d) return -1;
    struct dirent *de;
    while ((de = readdir(d)) && *count < max) {
        if (!strcmp(de->d_name,".")||!strcmp(de->d_name,"..")) continue;
        char full[PATH_MAX];
        /* 复用带边界检查与穿越防护的路径构造，杜绝手工拼接的截断/越界 */
        if (ae_store_path(s, full, sizeof full, zone, de->d_name) != 0) continue;
        struct stat st;
        if (stat(full,&st)==0 && S_ISREG(st.st_mode)) {
            size_t l = strlen(de->d_name);
            if (l >= AE_STORE_NAME_MAX) l = AE_STORE_NAME_MAX - 1;
            memcpy(names[*count], de->d_name, l);
            names[*count][l] = 0;
            (*count)++;
        }
    }
    closedir(d);
    if (*count > 1) qsort(names,(size_t)*count,AE_STORE_NAME_MAX,cmp_str);
    return 0;
}

int ae_store_zone_digest(ae_store_t *s, const char *zone, uint8_t out[AE_SHA256_LEN])
{
    char (*names)[AE_STORE_NAME_MAX] = NULL;
    int max = 256, count = 0;
    names = (char(*)[AE_STORE_NAME_MAX])calloc((size_t)max, AE_STORE_NAME_MAX);
    if (!names) return -1;
    if (ae_store_list(s, zone, names, max, &count) != 0) { free(names); return -1; }
    ae_sha256_ctx c; ae_sha256_init(&c);
    int i;
    for (i = 0; i < count; i++) {
        ae_sha256_update(&c, names[i], strlen(names[i]));
        char path[PATH_MAX];
        if (ae_store_path(s, path, sizeof path, zone, names[i]) != 0) continue;
        FILE *f = fopen(path,"rb");
        if (!f) continue;
        uint8_t b[8192]; size_t n;
        while ((n=fread(b,1,sizeof b,f))>0) ae_sha256_update(&c,b,n);
        fclose(f);
    }
    ae_sha256_final(&c,out);
    free(names);
    return 0;
}

/* ------------------------------------------------------------------ 定长卷 */
int ae_store_volume_create(ae_store_t *s, const char *name, size_t size)
{
    char path[PATH_MAX];
    if (ae_store_path(s,path,sizeof path,AE_ZONE_DATA,name)!=0) return -1;
    int fd = open(path,O_WRONLY|O_CREAT|O_TRUNC,0600);
    if (fd<0) return -1;
    if (ftruncate(fd,(off_t)size)!=0) { close(fd); return -1; }
    if (fsync(fd)!=0) { close(fd); return -1; }
    close(fd);
    return 0;
}
int ae_store_volume_size(ae_store_t *s, const char *name, size_t *size)
{
    char path[PATH_MAX];
    if (ae_store_path(s,path,sizeof path,AE_ZONE_DATA,name)!=0) return -1;
    struct stat st;
    if (stat(path,&st)!=0) return -1;
    if (size) *size=(size_t)st.st_size;
    return 0;
}
static int vol_open_rw(ae_store_t *s, const char *name, int *fd)
{
    char path[PATH_MAX];
    if (ae_store_path(s,path,sizeof path,AE_ZONE_DATA,name)!=0) return -1;
    *fd = open(path,O_RDWR);
    return *fd<0 ? -1 : 0;
}
int ae_store_volume_write(ae_store_t *s, const char *name,
                          const void *buf, size_t len, size_t offset)
{
    int fd;
    if (vol_open_rw(s,name,&fd)!=0) return -1;
    const uint8_t *p=(const uint8_t*)buf; size_t off=0;
    while (off<len) {
        ssize_t w=pwrite(fd,p+off,len-off,(off_t)(offset+off));
        if (w<0){ if(errno==EINTR)continue; close(fd); return -1; }
        off+=(size_t)w;
    }
    if (fsync(fd)!=0){ close(fd); return -1; }
    close(fd); return 0;
}
int ae_store_volume_read(ae_store_t *s, const char *name,
                         void *buf, size_t len, size_t offset)
{
    int fd;
    if (vol_open_rw(s,name,&fd)!=0) return -1;
    uint8_t *p=(uint8_t*)buf; size_t off=0;
    while (off<len) {
        ssize_t r=pread(fd,p+off,len-off,(off_t)(offset+off));
        if (r<0){ if(errno==EINTR)continue; close(fd); return -1; }
        if (r==0) break;
        off+=(size_t)r;
    }
    close(fd);
    return off==len ? 0 : -1;
}

int ae_store_volume_overwrite(ae_store_t *s, const char *name, int passes,
                              uint8_t out_hash[AE_SHA256_LEN])
{
    if (passes < 2) passes = 2;
    char path[PATH_MAX];
    if (ae_store_path(s,path,sizeof path,AE_ZONE_DATA,name)!=0) return -1;
    struct stat st;
    if (stat(path,&st)!=0) return -1;
    size_t size=(size_t)st.st_size;
    int fd=open(path,O_RDWR);
    if (fd<0) return -1;
    uint8_t *blk=(uint8_t*)malloc(65536);
    if (!blk){ close(fd); return -1; }
    int p;
    for (p=0;p<passes;p++) {
        /* 最后一轮固定 0x00（干净出厂态）；其余按顺序 0x00/0xFF/随机循环 */
        uint8_t pattern = 0x00;
        int is_random = 0;
        if (p == passes-1) pattern = 0x00;
        else if (p%3==0) pattern = 0x00;
        else if (p%3==1) pattern = 0xFF;
        else is_random = 1;
        size_t off=0;
        while (off<size) {
            size_t n = size-off<65536 ? size-off : 65536;
            if (is_random) { if (ae_random(blk,n)!=0){ free(blk); close(fd); return -1; } }
            else memset(blk,pattern,n);
            size_t w=0;
            while (w<n) {
                ssize_t r=pwrite(fd,blk+w,n-w,(off_t)(off+w));
                if (r<0){ if(errno==EINTR)continue; free(blk); close(fd); return -1; }
                w+=(size_t)r;
            }
            off+=n;
        }
        if (fsync(fd)!=0){ free(blk); close(fd); return -1; }
    }
    /* 末轮卷哈希（读回真实落盘内容，不是内存副本）；固定 blk[0] 起始，长度有上界 */
    ae_sha256_ctx c; ae_sha256_init(&c);
    size_t off=0;
    while (off<size) {
        size_t want = (size-off) < 65536 ? (size-off) : 65536;
        ssize_t q = pread(fd, blk, want, (off_t)off);
        if (q<0){ if(errno==EINTR) continue; break; }
        if (q==0) break;
        ae_sha256_update(&c, blk, (size_t)q);
        off += (size_t)q;
    }
    uint8_t discard[AE_SHA256_LEN];
    ae_sha256_final(&c, out_hash ? out_hash : discard);   /* 允许调用方不关心哈希 */
    free(blk); close(fd);
    return 0;
}
