/*
 * aether_integrity.c - 系统区完整性基线
 */
#define _GNU_SOURCE
#include "aether_integrity.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define BASE_FILE "integrity.baseline"

static void unhex(const char *h, uint8_t out[AE_SHA256_LEN])
{
    size_t i;
    for (i = 0; i < AE_SHA256_LEN; i++) {
        unsigned v=0;
        if (sscanf(h+2*i,"%2x",&v)!=1) v=0;
        out[i]=(uint8_t)v;
    }
}

int ae_integrity_seal(ae_store_t *s, ae_audit_t *a, uint8_t baseline[AE_SHA256_LEN])
{
    if (!s) return -1;
    if (ae_store_zone_digest(s,AE_ZONE_SYS,baseline)!=0) return -1;
    char hex[65], buf[128];
    ae_hex(baseline,AE_SHA256_LEN,hex);
    snprintf(buf,sizeof buf,"aether-integrity-v1\n%s\n",hex);
    if (ae_store_atomic_write(s,AE_ZONE_JOURNAL,BASE_FILE,buf,strlen(buf))!=0) return -1;
    if (a) (void)ae_audit_append(a,"INTEGRITY_SEAL",hex);
    return 0;
}

int ae_integrity_check(ae_store_t *s, int *ok,
                       uint8_t current[AE_SHA256_LEN],
                       uint8_t baseline[AE_SHA256_LEN])
{
    if (!s) return -1;
    if (ok) *ok=0;
    uint8_t *b=NULL; size_t n=0;
    if (ae_store_read(s,AE_ZONE_JOURNAL,BASE_FILE,&b,&n)!=0) return -1;
    char hex[65]; hex[0]=0;
    if (sscanf((const char*)b,"%*63s %64s",hex)!=1){ free(b); return -1; }
    unhex(hex,baseline);
    free(b);
    if (ae_store_zone_digest(s,AE_ZONE_SYS,current)!=0) return -1;
    if (ok) *ok = ae_ct_eq(current,baseline,AE_SHA256_LEN) ? 1 : 0;
    return 0;
}
