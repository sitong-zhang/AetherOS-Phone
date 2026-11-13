/*
 * test_hal.c - L2 接口层自测（§5.4，不启动整机界面）
 *
 * 所属层：L2 接口层（HAL）· 测试入口
 * 依赖：aether_hal.h / stdio / string
 * 对外接口：main()；退出码 0=全过，非 0=有失败
 * 不变量：
 *   1. 断言只检查实现行为，不修改任何验收期望值（§11.3 红线）
 *   2. 十五域逐一覆盖，缺一即报失败（§5.6 第三门「接口覆盖」）
 *   3. 失败要能复现：每条断言打印 EXPECT/GOT 与域名
 * 对应验收项：§5.3 流控、§5.4 接口自证、§6.2 Gate S2、§15.2 接口层
 */
#include "aether_hal.h"
#include <stdio.h>
#include <string.h>

static int g_pass, g_fail;

#define CHECK(cond, fmt, ...)                                              \
    do {                                                                   \
        if (cond) { g_pass++; }                                            \
        else { g_fail++;                                                   \
            printf("FAIL [%s:%d] " fmt "\n", __func__, __LINE__, ##__VA_ARGS__); \
        }                                                                  \
    } while (0)

/* 复用 sim 实现的状态块布局做注入（与 aether_impl_sim.c 一致） */
typedef struct {
    uint32_t mode, value, aux, pending_key, seq;
} test_state_t;

static aether_event_t mk_evt(uint32_t flags, uint32_t val)
{
    aether_event_t e;
    memset(&e, 0, sizeof e);
    e.flags = flags;
    e.size = sizeof(uint32_t);
    memcpy(e.payload, &val, sizeof val);
    return e;
}

/* --- 用例 1：十五域三件套齐备 --- */
static void test_all_domains_caps(void)
{
    CHECK(AETHER_DOM_COUNT == 15, "域数量应为 15, got %d", AETHER_DOM_COUNT);
    for (int i = 0; i < AETHER_DOM_COUNT; i++) {
        aether_caps_t c;
        aether_flow_t f;
        aether_dom_t d = (aether_dom_t)i;
        aether_err_t e1 = aether_hal_caps(d, &c);
        aether_err_t e2 = aether_hal_flow(d, &f);
        aether_err_t e3 = aether_hal_last_error(d);
        CHECK(e1 == AETHER_OK, "%s caps err=%s", aether_hal_dom_name(d), aether_hal_err_name(e1));
        CHECK(e2 == AETHER_OK, "%s flow err=%s", aether_hal_dom_name(d), aether_hal_err_name(e2));
        CHECK(e3 == AETHER_OK, "%s last_error=%s", aether_hal_dom_name(d), aether_hal_err_name(e3));
        CHECK(c.api_version == AETHER_HAL_API_VERSION, "%s api_version", aether_hal_dom_name(d));
        CHECK(c.queue_capacity == AETHER_HAL_QUEUE_CAP, "%s queue_capacity", aether_hal_dom_name(d));
        CHECK((c.flags & AETHER_CAP_PRESENT) != 0, "%s 缺 PRESENT 能力", aether_hal_dom_name(d));
        CHECK(strcmp(c.impl_name, "sim") == 0, "%s impl_name=%s", aether_hal_dom_name(d), c.impl_name);
        CHECK(f.queue_capacity == AETHER_HAL_QUEUE_CAP, "%s flow.queue_capacity", aether_hal_dom_name(d));
    }
}

/* --- 用例 2：正常提交/消费，计数可对账 --- */
static void test_submit_poll_accounting(void)
{
    aether_dom_t d = AETHER_DOM_DISPLAY;
    for (int i = 0; i < 10; i++) {
        aether_event_t e = mk_evt(0, (uint32_t)i);
        CHECK(aether_hal_submit(d, &e) == AETHER_OK, "submit #%d", i);
    }
    for (int i = 0; i < 10; i++) {
        aether_event_t out;
        uint32_t val = 0;
        CHECK(aether_hal_poll(d, &out) == AETHER_OK, "poll #%d", i);
        memcpy(&val, out.payload, sizeof val);
        CHECK(val == (uint32_t)i, "顺序错: expect %d got %u", i, val);
    }
    aether_flow_t f;
    aether_hal_flow(d, &f);
    CHECK(f.produced == 10, "produced=%llu expect 10", (unsigned long long)f.produced);
    CHECK(f.consumed == 10, "consumed=%llu expect 10", (unsigned long long)f.consumed);
    CHECK(f.queued == 0, "queued=%u expect 0", f.queued);
}

/* --- 用例 3：背压有上限、有计数，禁止无声丢 --- */
static void test_backpressure_counted(void)
{
    aether_dom_t d = AETHER_DOM_AUDIO;
    aether_hal_set_policy(d, AETHER_FLOW_DROP);
    /* 提交超过队列容量的事件，不消费 */
    for (uint32_t i = 0; i < AETHER_HAL_QUEUE_CAP * 2; i++) {
        aether_event_t e = mk_evt(0, i);
        aether_hal_submit(d, &e);
    }
    aether_flow_t f;
    aether_hal_flow(d, &f);
    CHECK(f.queued <= AETHER_HAL_QUEUE_CAP, "队列深度越界 %u > %u",
          f.queued, AETHER_HAL_QUEUE_CAP);
    CHECK(f.produced == f.consumed + f.dropped + f.rejected + f.queued,
          "账不平: produced=%llu consumed=%llu dropped=%llu rejected=%llu queued=%u",
          (unsigned long long)f.produced, (unsigned long long)f.consumed,
          (unsigned long long)f.dropped, (unsigned long long)f.rejected, f.queued);
    CHECK(f.dropped > 0, "背压丢弃未计数（无声丢数据）");
    aether_hal_set_policy(d, AETHER_FLOW_QUEUE);
}

/* --- 用例 4：关键事件在队列满时优先保留（按下/抬起、注册失败、掉电绝不丢） --- */
static void test_key_events_preserved(void)
{
    aether_dom_t d = AETHER_DOM_POWER;
    /* 用非关键事件填满队列 */
    for (uint32_t i = 0; i < AETHER_HAL_QUEUE_CAP; i++) {
        aether_event_t e = mk_evt(0, i);
        aether_hal_submit(d, &e);
    }
    /* 关键事件（掉电）必须能进队列 */
    aether_event_t key = mk_evt(AETHER_EVT_KEY, 0xA0DE0000u);
    aether_err_t e = aether_hal_submit(d, &key);
    CHECK(e == AETHER_OK, "关键事件被拒 err=%s", aether_hal_err_name(e));

    aether_flow_t f;
    aether_hal_flow(d, &f);
    CHECK(f.key_preserved > 0, "关键事件保留次数=%llu，应为正",
          (unsigned long long)f.key_preserved);

    /* 把队列消费完，确认关键事件确实在里面 */
    int found = 0;
    for (uint32_t i = 0; i < AETHER_HAL_QUEUE_CAP + 2; i++) {
        aether_event_t out;
        if (aether_hal_poll(d, &out) != AETHER_OK) break;
        if (out.flags & AETHER_EVT_KEY) found = 1;
    }
    CHECK(found == 1, "关键事件未出现在消费流中");
}

/* --- 用例 5：故障注入四种模式，上层能拿到明确错误码并降级 --- */
static void test_fault_injection(void)
{
    aether_dom_t d = AETHER_DOM_WIFI;
    aether_event_t e = mk_evt(0, 1);

    aether_hal_set_fault(d, AETHER_FAULT_FAIL);
    CHECK(aether_hal_submit(d, &e) == AETHER_ERR_FAULT, "FAIL 模式应返回 FAULT");

    aether_hal_set_fault(d, AETHER_FAULT_DISCONNECT);
    CHECK(aether_hal_submit(d, &e) == AETHER_ERR_DISCONNECTED, "DISCONNECT 模式");

    aether_hal_set_fault(d, AETHER_FAULT_NO_IMPL);
    CHECK(aether_hal_submit(d, &e) == AETHER_ERR_NO_IMPL, "NO_IMPL 模式（降级不静默）");

    aether_hal_set_fault(d, AETHER_FAULT_DELAY);
    int timeouts = 0;
    for (uint32_t i = 0; i < AETHER_HAL_FAULT_DELAY_N + 2; i++)
        if (aether_hal_submit(d, &e) == AETHER_ERR_TIMEOUT) timeouts++;
    CHECK(timeouts == AETHER_HAL_FAULT_DELAY_N,
          "DELAY 超时次数=%d expect %d", timeouts, AETHER_HAL_FAULT_DELAY_N);

    aether_hal_set_fault(d, AETHER_FAULT_NONE);
    CHECK(aether_hal_submit(d, &e) == AETHER_OK, "清除故障后应恢复");

    aether_flow_t f;
    aether_hal_flow(d, &f);
    CHECK(f.fault_injected > 0, "故障注入次数未计数");
}

/* --- 用例 6：状态注入/导出（脚本设定坐标、电量、基带状态机） --- */
static void test_state_injection(void)
{
    /* 基带完整状态机：注入「注册失败」，应产出关键事件 */
    aether_dom_t cell = AETHER_DOM_CELLULAR;
    test_state_t st;
    memset(&st, 0, sizeof st);
    CHECK(aether_hal_get_state(cell, &st, sizeof st) == AETHER_OK, "get_state 初始");
    st.mode = 4;         /* CELL_REG_FAILED */
    st.value = 0;
    CHECK(aether_hal_set_state(cell, &st, sizeof st) == AETHER_OK, "set_state 注册失败");
    aether_event_t out;
    int got_key = 0;
    for (int i = 0; i < 4; i++) {
        if (aether_hal_poll(cell, &out) == AETHER_OK && (out.flags & AETHER_EVT_KEY))
            got_key = 1;
    }
    CHECK(got_key == 1, "基带注册失败未产生关键事件（关键事件不许丢）");

    /* 传感器降频必须上报 */
    aether_dom_t sen = AETHER_DOM_SENSOR;
    test_state_t s0, s1;
    memset(&s0, 0, sizeof s0);
    s0.value = 100;
    aether_hal_set_state(sen, &s0, sizeof s0);
    s1 = s0; s1.value = 25;
    aether_hal_set_state(sen, &s1, sizeof s1);
    test_state_t back;
    CHECK(aether_hal_get_state(sen, &back, sizeof back) == AETHER_OK, "get_state 传感器");
    CHECK(back.aux == 100, "降频未上报：aux=%u expect 100", back.aux);
}

/* --- 用例 7：实现可替换，上层调用路径不变、错误码明确、能力如实 --- */
static void test_impl_switch(void)
{
    aether_dom_t d = AETHER_DOM_CAMERA;
    CHECK(strcmp(aether_hal_impl_name(), "sim") == 0, "默认实现应为 sim");

    CHECK(aether_hal_switch_impl(&aether_impl_stub) == AETHER_OK, "切换到 stub");
    CHECK(strcmp(aether_hal_impl_name(), "stub") == 0, "切换后名字");

    aether_caps_t c;
    CHECK(aether_hal_caps(d, &c) == AETHER_OK, "stub caps 可查");
    CHECK(c.flags == 0, "stub 不得假装能力存在（静默失败红线）");
    CHECK(strcmp(c.impl_name, "stub") == 0, "stub impl_name");

    aether_event_t e = mk_evt(0, 1);
    CHECK(aether_hal_submit(d, &e) == AETHER_ERR_NO_IMPL,
          "stub submit 应明确返回 NO_IMPL 而非静默");

    CHECK(aether_hal_switch_impl(&aether_impl_sim) == AETHER_OK, "切回 sim");
    CHECK(strcmp(aether_hal_impl_name(), "sim") == 0, "切回后名字");
    CHECK(aether_hal_submit(d, &e) == AETHER_OK, "sim 恢复可用");
}

/* --- 用例 8：日志限速可观测（§5.3「日志不许把系统写死」/ §10.4） --- */
static void test_log_rate_limit(void)
{
    /* 用未被前面用例使用过的域，避免静态限速计数器受其他用例影响 */
    aether_dom_t d = AETHER_DOM_BLUETOOTH;
    aether_event_t e = mk_evt(0, 1);
    aether_hal_set_fault(d, AETHER_FAULT_FAIL);
    for (uint32_t i = 0; i < AETHER_HAL_LOG_RATE_MAX * 3; i++)
        (void)aether_hal_submit(d, &e);
    aether_hal_set_fault(d, AETHER_FAULT_NONE);

    aether_flow_t f;
    aether_hal_flow(d, &f);
    CHECK(f.fault_injected == AETHER_HAL_LOG_RATE_MAX * 3,
          "故障计数应等于提交数, got %llu",
          (unsigned long long)f.fault_injected);
    CHECK(f.log_suppressed > 0, "日志限速未生效或未上报 log_suppressed");
    CHECK(f.log_suppressed == f.fault_injected - AETHER_HAL_LOG_RATE_MAX,
          "限速对账不平: suppressed=%llu injected=%llu",
          (unsigned long long)f.log_suppressed,
          (unsigned long long)f.fault_injected);
}

/* --- 用例 9：参数校验与边界 --- */
static void test_bad_args(void)
{
    aether_event_t e = mk_evt(0, 1);
    e.size = AETHER_HAL_EVENT_PAYLOAD + 1;
    CHECK(aether_hal_submit(AETHER_DOM_RTC, &e) == AETHER_ERR_BAD_ARG, "超大载荷应拒绝");
    CHECK(aether_hal_caps((aether_dom_t)999, NULL) == AETHER_ERR_BAD_ARG, "非法域");
    CHECK(aether_hal_set_fault(AETHER_DOM_RTC, (aether_fault_t)999) == AETHER_ERR_BAD_ARG,
          "非法故障模式");
    CHECK(aether_hal_err_name(AETHER_ERR_DISCONNECTED) != NULL, "错误码名称可查");
    CHECK(strlen(aether_hal_dom_name(AETHER_DOM_RTC)) > 0, "域名可查");
}

int main(void)
{
    printf("=== AetherOS L2 接口层自测 (§5.4) ===\n");
    if (aether_hal_init(NULL) != 0) {
        printf("FAIL 初始化失败\n");
        return 1;
    }
    printf("实现: %s / 域数: %d / 队列容量: %u\n",
           aether_hal_impl_name(), AETHER_DOM_COUNT, AETHER_HAL_QUEUE_CAP);

    test_all_domains_caps();
    test_submit_poll_accounting();
    test_backpressure_counted();
    test_key_events_preserved();
    test_fault_injection();
    test_state_injection();
    test_impl_switch();
    test_log_rate_limit();
    test_bad_args();

    aether_hal_fini();
    printf("\n=== 结果: PASS=%d FAIL=%d ===\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
