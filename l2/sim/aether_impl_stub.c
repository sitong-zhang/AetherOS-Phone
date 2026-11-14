/*
 * aether_impl_stub.c - L2 桩实现（机检第四门「实现可替换」用）
 *
 * 所属层：L2 接口层（HAL）· 桩实现分支
 * 依赖：aether_dev_hal.h / stdlib / string
 * 对外接口：aether_impl_stub（aether_hal_impl_t 常量）
 * 不变量：
 *   1. 与 sim 实现接口完全一致，仅行为不同；上层目标文件编译两次哈希必须一致（§5.6 第四门）
 *   2. 桩实现返回 NOT_SUPPORTED / NO_IMPL，用于验证「缺实现时上层明确降级，不白屏不死机」
 *   3. 能力查询必须如实返回 PRESENT=0，不许假装能力存在（§11.2 第 5 类静默失败）
 * 对应验收项：§5.4 实现切换开关、§5.6 机检第四门、§15.2 切换实现
 */
#include "aether_dev_hal.h"
#include <stdlib.h>
#include <string.h>

static aether_err_t stub_init(aether_dom_t dom, void **out_ctx)
{
    (void)dom;
    *out_ctx = NULL;   /* 桩实现不需要上下文，也不假装分配成功 */
    return AETHER_OK;
}

static void stub_fini(aether_dom_t dom, void *ctx)
{
    (void)dom; (void)ctx;
}

static aether_err_t stub_caps(aether_dom_t dom, void *ctx, aether_caps_t *out)
{
    (void)dom; (void)ctx;
    if (!out) return AETHER_ERR_BAD_ARG;
    out->flags = 0;   /* 如实：无可用能力 */
    strncpy(out->impl_name, "stub", sizeof out->impl_name - 1);
    return AETHER_OK;
}

static aether_err_t stub_submit(aether_dom_t dom, void *ctx, const aether_event_t *evt)
{
    (void)dom; (void)ctx; (void)evt;
    return AETHER_ERR_NO_IMPL;
}

static aether_err_t stub_poll(aether_dom_t dom, void *ctx, aether_event_t *out)
{
    (void)dom; (void)ctx; (void)out;
    return AETHER_ERR_NO_IMPL;
}

static aether_err_t stub_set_state(aether_dom_t dom, void *ctx, const void *buf, size_t len)
{
    (void)dom; (void)ctx; (void)buf; (void)len;
    return AETHER_ERR_NOT_SUPPORTED;
}

static aether_err_t stub_get_state(aether_dom_t dom, void *ctx, void *buf, size_t len)
{
    (void)dom; (void)ctx; (void)buf; (void)len;
    return AETHER_ERR_NOT_SUPPORTED;
}

const aether_hal_impl_t aether_impl_stub = {
    .name      = "stub",
    .init      = stub_init,
    .caps      = stub_caps,
    .submit    = stub_submit,
    .poll      = stub_poll,
    .set_state = stub_set_state,
    .get_state = stub_get_state,
    .fini      = stub_fini,
};
