/*
 * aether_hal_config.h - L2 接口层唯一配置源
 *
 * 所属层：L2 接口层（HAL）
 * 依赖：无（仅 C 标准整数类型）
 * 对外接口：本文件全部宏
 * 不变量：
 *   - L2 工程参数（队列容量、限速阈值、事件载荷上限）只在此处定义；
 *     实现文件与业务层不得出现同类裸数值（§10.2 单一参数源 / §5.6 第五门）
 *   - 改任一宏必须附前后对比证据并写入 docs/ 变更记录
 * 对应验收项：§5.3 流控与背压、§5.6 机检第五门、§10.2 参数治理
 */
#ifndef AETHER_HAL_CONFIG_H
#define AETHER_HAL_CONFIG_H

#include <stdint.h>

/* 每个域的事件环形队列容量。达到上限后按流控策略处理，绝不无限增长。 */
#define AETHER_HAL_QUEUE_CAP       64u

/* 单次事件载荷上限（字节）。显示帧描述、注入轨迹、状态快照都装得下。 */
#define AETHER_HAL_EVENT_PAYLOAD   64u

/* 日志限速：单个域在连续调用中最多保留的告警条数，超出计数不打印，
 * 防止高频故障把系统写死（§5.3「日志」行 / §10.4）。 */
#define AETHER_HAL_LOG_RATE_MAX    8u

/* 故障注入「延迟」模式的模拟超时计数：连续多少次 submit 被视为超时。 */
#define AETHER_HAL_FAULT_DELAY_N   3u

/* 基带状态机可注入的状态数量（无卡/无信号/漫游/仅紧急/注册失败/已注册）。 */
#define AETHER_HAL_CELL_STATES     6u

/* 传感器降频上报的最小步进（采样率下调时必须上报，不许悄悄变低）。 */
#define AETHER_HAL_SENSOR_RATE_MIN 1u

/* 接口 API 版本，能力查询时返回，供上层做兼容性判断。 */
#define AETHER_HAL_API_VERSION     1u

#endif /* AETHER_HAL_CONFIG_H */
