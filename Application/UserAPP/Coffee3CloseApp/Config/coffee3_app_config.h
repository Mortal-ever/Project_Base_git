/**
  * @file      coffee3_app_config.h
  * @brief     定义 Coffee3 产品通信、任务与工作流参数。
  * @author    WHong
  * @date      2026-09-24
  */

#ifndef COFFEE3_APP_CONFIG_H
#define COFFEE3_APP_CONFIG_H

#include "compiler_compat.h"

/** @brief 日志中发布的产品固件版本事件。 */
#define COFFEE3_DEVICE_VERSION_EVENT         "FW_VERSION:Coffee3CloseV3.0.0"
/** @brief 可接受的最大咖啡配方编号。 */
#define COFFEE3_COFFEE_RECIPE_MAX            0x0021U

/** @brief 把仅由 CPU 访问的已审查静态数据放入启动清零的 STM32F4 CCM。 */
#define COFFEE3_CCM_DATA APP_CCM_DATA

/** @brief CubeMX LwIP 初始化后应用的静态 IPv4 地址、掩码与网关。 */
#define COFFEE3_IP_ADDRESS_0                 192U
#define COFFEE3_IP_ADDRESS_1                 168U
#define COFFEE3_IP_ADDRESS_2                 5U
#define COFFEE3_IP_ADDRESS_3                 10U
#define COFFEE3_NETMASK_0                    255U
#define COFFEE3_NETMASK_1                    255U
#define COFFEE3_NETMASK_2                    255U
#define COFFEE3_NETMASK_3                    0U
#define COFFEE3_GATEWAY_0                    192U
#define COFFEE3_GATEWAY_1                    168U
#define COFFEE3_GATEWAY_2                    5U
#define COFFEE3_GATEWAY_3                    1U

/** @brief 面向主机的 Modbus TCP 服务参数。 */
#define COFFEE3_SERVER_PORT                  6001U
#define COFFEE3_SERVER_UNIT_ID               1U
#define COFFEE3_SERVER_MAX_CLIENTS           4U
#define COFFEE3_SERVER_POLL_MS               20U
#define COFFEE3_SERVER_BYTE_TIMEOUT_MS       100U

/** @brief 机器人 Modbus TCP 客户端连接、轮询与恢复参数。 */
#define COFFEE3_ROBOT_IP_0                   192U
#define COFFEE3_ROBOT_IP_1                   168U
#define COFFEE3_ROBOT_IP_2                   5U
#define COFFEE3_ROBOT_IP_3                   1U
#define COFFEE3_ROBOT_PORT                   502U
#define COFFEE3_ROBOT_UNIT_ID                1U
#define COFFEE3_ROBOT_CONNECT_TIMEOUT_MS    3000U
#define COFFEE3_ROBOT_IO_TIMEOUT_MS          1000U
#define COFFEE3_ROBOT_LOOP_MS                20U
#define COFFEE3_ROBOT_ACTION_POLL_MS        100U
#define COFFEE3_ROBOT_ACCEPT_LOG_INTERVAL_MS 5000U
#define COFFEE3_ROBOT_ACCEPT_TIMEOUT_MS     15000U
#define COFFEE3_ROBOT_MOTION_TIMEOUT_MS     60000U
#define COFFEE3_ROBOT_RECOVERY_TIMEOUT_MS   90000U
#define COFFEE3_ROBOT_PREPARE_RETRY_MS       3000U
#define COFFEE3_ROBOT_PREPARE_RETRY_LIMIT       5U
#define COFFEE3_ROBOT_EDGE_LOW_MS           50U
#define COFFEE3_ROBOT_RETRY_MAX_MS          20000U
#define COFFEE3_ROBOT_READY_SAMPLES         3U

/** @brief 选择经过验证的 Dobot 产品寄存器协议版本。 */
#define COFFEE3_ROBOT_PROTOCOL_1             0U
#define COFFEE3_ROBOT_PROTOCOL_2             1U
#define COFFEE3_ROBOT_PROTOCOL_3             2U
#define COFFEE3_ROBOT_PROTOCOL_VARIANT       COFFEE3_ROBOT_PROTOCOL_1

/** @brief 为每条物理串口总线选择唯一协议拥有者。 */
#define COFFEE3_BUS_PROTOCOL_MODBUS_RTU       1U
#define COFFEE3_BUS2_PROTOCOL                 COFFEE3_BUS_PROTOCOL_MODBUS_RTU
#define COFFEE3_BUS3_PROTOCOL                 COFFEE3_BUS_PROTOCOL_MODBUS_RTU
#define COFFEE3_BUS4_PROTOCOL                 COFFEE3_BUS_PROTOCOL_MODBUS_RTU
#define COFFEE3_BUS5_PROTOCOL                 COFFEE3_BUS_PROTOCOL_MODBUS_RTU
#define COFFEE3_MODBUS_BUS_COUNT              \
	(((COFFEE3_BUS2_PROTOCOL == COFFEE3_BUS_PROTOCOL_MODBUS_RTU) ? 1U : 0U) + \
	 ((COFFEE3_BUS3_PROTOCOL == COFFEE3_BUS_PROTOCOL_MODBUS_RTU) ? 1U : 0U) + \
	 ((COFFEE3_BUS4_PROTOCOL == COFFEE3_BUS_PROTOCOL_MODBUS_RTU) ? 1U : 0U) + \
	 ((COFFEE3_BUS5_PROTOCOL == COFFEE3_BUS_PROTOCOL_MODBUS_RTU) ? 1U : 0U))

/** @brief UART2 至 UART5 的 RTU 路由、队列与超时参数。 */
#define COFFEE3_RTU_BUS_COUNT                4U
#define COFFEE3_COMMAND_QUEUE_LENGTH         4U
#define COFFEE3_RTU_IO_TIMEOUT_MS            500U
#define COFFEE3_RTU_IDLE_MS                  20U
/** @brief 普通 RTU 设备后台健康刷新的周期。 */
#define COFFEE3_RTU_POLL_PERIOD_MS           5000U
/** @brief 同一 RTU 总线上普通设备启动轮询的相位间隔。 */
#define COFFEE3_RTU_POLL_STAGGER_MS          200U
/** @brief 第五路总线外部 IO 的刷新周期；两个设备错开轮询。 */
#define COFFEE3_RTU_IO_POLL_PERIOD_MS        100U
#define COFFEE3_RTU_IO_POLL_STAGGER_MS       50U
#define COFFEE3_RTU_POLL_TIMEOUT_MS          100U
#define COFFEE3_RTU_OFFLINE_MISS_LIMIT       2U
#define COFFEE3_RTU_IO_OFFLINE_MISS_LIMIT    3U

/** @brief Coffee3 创建任务前重新应用的串口默认波特率。 */
#define COFFEE3_BUS2_DEFAULT_BAUD            19200U
#define COFFEE3_BUS3_DEFAULT_BAUD            9600U
#define COFFEE3_BUS4_DEFAULT_BAUD            19200U
#define COFFEE3_BUS5_DEFAULT_BAUD            38400U
#define COFFEE3_LOG_BAUD                     115200U

/** @brief IO 模块命令之间的最小帧间隔，单位为毫秒。 */
#define COFFEE3_IO_MIN_FRAME_INTERVAL_MS     20U
/** @brief 制冰机连续命令之间的最小间隔，单位为毫秒。 */
#define COFFEE3_ICE_MIN_FRAME_INTERVAL_MS    100U
/** @brief 每个外部 IO 模块的物理点位数量。 */
#define COFFEE3_EXTERNAL_IO_POINT_COUNT       16U

/** @brief 各 FreeRTOS 任务栈大小，单位为 StackType_t。 */
#define COFFEE3_LOG_TASK_STACK               256U
#define COFFEE3_SERVER_TASK_STACK            1536U
#define COFFEE3_ROBOT_TASK_STACK             1024U
#define COFFEE3_RTU_TASK_STACK               384U
#define COFFEE3_WORKFLOW_TASK_STACK          1024U

/** @brief 工作流队列、刷新、动作与超时参数。 */
#define COFFEE3_WORKFLOW_QUEUE_LENGTH        2U
#define COFFEE3_WORKFLOW_IO_REFRESH_MS       100U
#define COFFEE3_WORKFLOW_DEFAULT_TIMEOUT_MS  30000U
#define COFFEE3_COFFEE_ACTION_TIMEOUT_MS      180000U
#define COFFEE3_OUTLET_EMPTY_HOLD_MS          30000U
#define COFFEE3_DOOR_MOTION_TIMEOUT_MS        5000U
#define COFFEE3_IO_STALE_MS                  2000U
/** @brief 主机每 0.1 毫升对应的暂定糖浆控制器时间单位。 */
#define COFFEE3_SYRUP_TIME_PER_VOLUME_UNIT    1U
/** @brief 热水、清洗及设备 IO 工作流的超时和持续时间参数。 */
#define COFFEE3_HOT_WATER_FILL_TIMEOUT_MS     120000U
#define COFFEE3_HOT_WATER_DEFAULT_HEAT_MIN    30U
#define COFFEE3_HOT_WATER_MAX_HEAT_MIN        120U
#define COFFEE3_FRUIT_MILK_CLEAN_MS           15000U
#define COFFEE3_WORKFLOW_DEVICE_IO_TIMEOUT_MS  1000U
#define COFFEE3_WORKFLOW_IO_ACTION_TIMEOUT_MS 5000U

/** @brief 出冰首轮脉冲、补冰、称重判定与安全范围参数；时间单位为毫秒，重量单位为克。 */
#define COFFEE3_ICE_INITIAL_SPLIT_GRAM        120U
#define COFFEE3_ICE_INITIAL_SMALL_MS          800U
#define COFFEE3_ICE_INITIAL_LARGE_MS          1200U
#define COFFEE3_ICE_CORRECTION_OFFSET_MS      100L
#define COFFEE3_ICE_MIN_PULSE_MS              200U
#define COFFEE3_ICE_MAX_PULSE_MS              2000U
#define COFFEE3_ICE_PULSE_STEP_MS              50U
#define COFFEE3_ICE_SETTLE_MS                 1600U
#define COFFEE3_ICE_BASELINE_SETTLE_MS         300U
#define COFFEE3_ICE_BASELINE_RETRY_MS         3000U
#define COFFEE3_ICE_BASELINE_TOLERANCE_GRAM    2L
#define COFFEE3_ICE_TOLERANCE_GRAM            15L
#define COFFEE3_ICE_CUP_DETECT_GRAM           6L
#define COFFEE3_ICE_CUP_MAX_GRAM              100L
#define COFFEE3_ICE_CUP_FIRST_SETTLE_MS       500U
#define COFFEE3_ICE_CUP_RETRY_MS              1000U
#define COFFEE3_ICE_CUP_DETECT_ATTEMPTS       3U
#define COFFEE3_ICE_CUP_COMM_RETRIES          10U
#define COFFEE3_ICE_SAFE_MIN_PERCENT          70U
#define COFFEE3_ICE_SAFE_MAX_PERCENT          130U
#define COFFEE3_ICE_MAX_CORRECTIONS           4U

#endif /* COFFEE3_APP_CONFIG_H */
