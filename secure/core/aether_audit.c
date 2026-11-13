/*
 * aether_audit.c - 只追加防篡改哈希链
 */
#define _GNU_SOURCE
#include "aether_audit.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

#define CHAIN_FILE "chain.log"
#define HEAD_FILE  "head.bin"

struct ae_audit {
    ae_store_t *store;
    uint8_t     head[AE_SHA256_LEN];
    uint64_t    seq;
};

static void sanitize(const char *in, char *out, size_t n)
{
    size_t i;
    for (i = 0; i+1 < n && in[i]; i++) {
        char c = in[i];
        out[i] = (c=='|'||c=='\n'||c=='\r') ? '_' : c;
    }
    out[i] = '\0';
}
static void unhex(const char *h, uint8_t out[AE_SHA256_LEN])
{
    size_t i;
    for (i = 0; i < AE_SHA256_LEN; i++) {
        unsigned v = 0;
        if (sscanf(h + 2*i, "%2x", &v) != 1) v = 0;
        out[i] = (uint8_t)v;
    }
}
static int save_head(ae_audit_t *a)
{
    char buf[160], hex[65];
    ae_hex(a->head, AE_SHA256_LEN, hex);
    snprintf(buf,sizeof buf,"%llu\n%s\n",(unsigned long long)a->seq,hex);
    return ae_store_atomic_write(a->store, AE_ZONE_JOURNAL, HEAD_FILE, buf, strlen(buf));
}
static int load_head(ae_audit_t *a)
{
    uint8_t *b=NULL; size_t n=0;
    if (ae_store_read(a->store,AE_ZONE_JOURNAL,HEAD_FILE,&b,&n)!=0) return -1;
    char hex[65]; unsigned long long sq=0;
    if (sscanf((const char*)b,"%llu\n%64s",&sq,hex)!=2){ free(b); return -1; }
    a->seq=(uint64_t)sq;
    unhex(hex,a->head);
    free(b);
    return 0;
}

int ae_audit_open(ae_store_t *s, ae_audit_t **out)
{
    if (!s||!out) return -1;
    *out=NULL;
    ae_audit_t *a=(ae_audit_t*)calloc(1,sizeof *a);
    if (!a) return -1;
    a->store=s;
    if (load_head(a)!=0) {
        /* 创世：空链 + 全 0 head */
        a->seq=0; memset(a->head,0,AE_SHA256_LEN);
        (void)ae_store_atomic_write(s,AE_ZONE_JOURNAL,CHAIN_FILE,"",0);
        if (save_head(a)!=0){ free(a); return -1; }
    }
    *out=a;
    return 0;
}
void ae_audit_close(ae_audit_t *a){ free(a); }
uint64_t ae_audit_seq(ae_audit_t *a){ return a?a->seq:0; }
const uint8_t *ae_audit_head(ae_audit_t *a){ return a?a->head:NULL; }

int ae_audit_append(ae_audit_t *a, const char *event, const char *detail)
{
    if (!a||!event) return -1;
    char ev[128], de[256], prevhex[65], hashhex[65], body[640], line[720];
    sanitize(event,ev,sizeof ev);
    sanitize(detail?detail:"",de,sizeof de);
    if (!de[0]) snprintf(de,sizeof de,"-");   /* 保证 5 个字段都非空，避免解析时丢段 */
    ae_hex(a->head,AE_SHA256_LEN,prevhex);
    uint64_t seq=a->seq+1;
    snprintf(body,sizeof body,"%llu|%ld|%s|%s|%s",
             (unsigned long long)seq,(long)time(NULL),ev,de,prevhex);
    /* hash = SHA256(prev_raw || body) */
    ae_sha256_ctx c; ae_sha256_init(&c);
    ae_sha256_update(&c,a->head,AE_SHA256_LEN);
    ae_sha256_update(&c,body,strlen(body));
    uint8_t h[AE_SHA256_LEN]; ae_sha256_final(&c,h);
    ae_hex(h,AE_SHA256_LEN,hashhex);
    snprintf(line,sizeof line,"%s|%s\n",body,hashhex);
    /* 追加写 + fsync */
    char path[4096];
    if (ae_store_path(a->store,path,sizeof path,AE_ZONE_JOURNAL,CHAIN_FILE)!=0) return -1;
    int fd=open(path,O_WRONLY|O_APPEND|O_CREAT,0600);
    if (fd<0) return -1;
    size_t off=0,n=strlen(line);
    while(off<n){ ssize_t w=write(fd,line+off,n-off); if(w<0){if(errno==EINTR)continue;close(fd);return -1;} off+=(size_t)w; }
    if (fsync(fd)!=0){ close(fd); return -1; }
    close(fd);
    memcpy(a->head,h,AE_SHA256_LEN);
    a->seq=seq;
    return save_head(a);
}

int ae_audit_verify(ae_audit_t *a, uint64_t *count, uint8_t head[AE_SHA256_LEN])
{
    if (!a) return -1;
    if (count) *count=0;
    uint8_t *b=NULL; size_t n=0;
    if (ae_store_read(a->store,AE_ZONE_JOURNAL,CHAIN_FILE,&b,&n)!=0) return -1;
    uint8_t prev[AE_SHA256_LEN]; memset(prev,0,sizeof prev);
    char expected_prev[65]; ae_hex(prev,AE_SHA256_LEN,expected_prev);
    uint64_t lines=0;
    char *save=NULL;
    char *ln=strtok_r((char*)b,"\n",&save);
    int bad=0;
    while (ln) {
        lines++;
        char *last=strrchr(ln,'|');
        if (!last){ bad=1; break; }
        *last='\0';
        char *body=ln, *hashhex=last+1;
        /* body 的 5 段：seq|ts|event|detail|prevhex */
        char fields[5][256]; char *sv2=NULL; int nf=0;
        char *tok=strtok_r(body,"|",&sv2);
        while (tok && nf<5){ snprintf(fields[nf],sizeof fields[nf],"%s",tok); nf++; tok=strtok_r(NULL,"|",&sv2); }
        if (nf!=5){ bad=1; break; }
        char seqbuf[64]; snprintf(seqbuf,sizeof seqbuf,"%llu",(unsigned long long)lines);
        if (strcmp(fields[0],seqbuf)!=0) { bad=1; break; }          /* 序号被插/删 */
        if (strcmp(fields[4],expected_prev)!=0) { bad=1; break; }    /* 链断裂 */
        /* 重算需原始 body（含分隔符），用 fields 拼回 */
        char rebuilt[1400];
        snprintf(rebuilt,sizeof rebuilt,"%s|%s|%s|%s|%s",
                 fields[0],fields[1],fields[2],fields[3],fields[4]);
        ae_sha256_ctx c; ae_sha256_init(&c);
        ae_sha256_update(&c,prev,AE_SHA256_LEN);
        ae_sha256_update(&c,rebuilt,strlen(rebuilt));
        uint8_t calc[AE_SHA256_LEN]; ae_sha256_final(&c,calc);
        char calchex[65]; ae_hex(calc,AE_SHA256_LEN,calchex);
        if (strcmp(calchex,hashhex)!=0){ bad=1; break; }             /* 内容被改 */
        memcpy(prev,calc,AE_SHA256_LEN);
        ae_hex(prev,AE_SHA256_LEN,expected_prev);
        ln=strtok_r(NULL,"\n",&save);
    }
    free(b);
    if (!bad && lines!=a->seq) bad=1;
    /* 链重算出的 head 必须与 head.bin 中持久化的一致（head 参数仅作输出） */
    if (!bad && !ae_ct_eq(prev,a->head,AE_SHA256_LEN)) bad=1;
    if (count) *count=lines;
    if (head) memcpy(head,prev,AE_SHA256_LEN);
    return bad ? -1 : 0;
}

int ae_audit_dump(ae_audit_t *a, char *buf, size_t n)
{
    if (!a||!buf||!n) return -1;
    uint8_t *b=NULL; size_t m=0;
    if (ae_store_read(a->store,AE_ZONE_JOURNAL,CHAIN_FILE,&b,&m)!=0){ buf[0]=0; return -1; }
    size_t cp=m<n?m:n-1;
    memcpy(buf,b,cp); buf[cp]=0;
    free(b);
    return 0;
}
