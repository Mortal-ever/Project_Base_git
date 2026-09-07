/**
  * @file      coffee2_app_config.h
  * @brief     Define Coffee2 product communication and task parameters.
  * @author    WHong
  * @date      2026-07-30
  */

#ifndef COFFEE2_APP_CONFIG_H
#define COFFEE2_APP_CONFIG_H

#include "compiler_compat.h"

/** @brief Log event that exposes the actual product firmware version. */
#define COFFEE2_DEVICE_VERSION_EVENT         "FW_VERSION:Coffee2OpenV3.0.2"
/** @brief Last valid logical coffee recipe identifier. */
#define COFFEE2_COFFEE_RECIPE_MAX            0x0021U

/** @brief Place reviewed CPU-only static data in startup-cleared STM32F4 CCM. */
#define COFFEE2_CCM_DATA APP_CCM_DATA

/** @brief Static IPv4 address applied after CubeMX LwIP initialization. */
#define COFFEE2_IP_ADDRESS_0                 192U
#define COFFEE2_IP_ADDRESS_1                 168U
#define COFFEE2_IP_ADDRESS_2                 5U
#define COFFEE2_IP_ADDRESS_3                 10U
#define COFFEE2_NETMASK_0                    255U
#define COFFEE2_NETMASK_1                    255U
#define COFFEE2_NETMASK_2                    255U
#define COFFEE2_NETMASK_3                    0U
#define COFFEE2_GATEWAY_0                    192U
#define COFFEE2_GATEWAY_1                    168U
#define COFFEE2_GATEWAY_2                    5U
#define COFFEE2_GATEWAY_3                    1U

/** @brief Host-facing Modbus TCP server configuration. */
#define COFFEE2_SERVER_PORT                  6001U
#define COFFEE2_SERVER_UNIT_ID               1U
#define COFFEE2_SERVER_MAX_CLIENTS           4U
#define COFFEE2_SERVER_POLL_MS               20U
#define COFFEE2_SERVER_BYTE_TIMEOUT_MS       100U

/** @brief Robot Modbus TCP client configuration. */
#define COFFEE2_ROBOT_IP_0                   192U
#define COFFEE2_ROBOT_IP_1                   168U
#define COFFEE2_ROBOT_IP_2                   5U
#define COFFEE2_ROBOT_IP_3                   1U
#define COFFEE2_ROBOT_PORT                   502U
#define COFFEE2_ROBOT_UNIT_ID                1U
#define COFFEE2_ROBOT_CONNECT_TIMEOUT_MS     3000U
#define COFFEE2_ROBOT_IO_TIMEOUT_MS          1000U
#define COFFEE2_ROBOT_LOOP_MS                20U
#define COFFEE2_ROBOT_ACTION_POLL_MS        100U
#define COFFEE2_ROBOT_ACCEPT_LOG_INTERVAL_MS 5000U
#define COFFEE2_ROBOT_MOTION_TIMEOUT_MS     60000U
#define COFFEE2_ROBOT_EDGE_LOW_MS           50U
#define COFFEE2_ROBOT_RETRY_MAX_MS          30000U
#define COFFEE2_ROBOT_READY_SAMPLES         3U

/** @brief Select the validated Dobot product register contract. */
#define COFFEE2_ROBOT_PROTOCOL_1             0U
#define COFFEE2_ROBOT_PROTOCOL_2             1U
#define COFFEE2_ROBOT_PROTOCOL_3             2U
#define COFFEE2_ROBOT_PROTOCOL_VARIANT       COFFEE2_ROBOT_PROTOCOL_1

/** @brief Select one protocol owner for each physical UART bus. */
#define COFFEE2_BUS_PROTOCOL_MODBUS_RTU       1U
#define COFFEE2_BUS_PROTOCOL_F200_UART        3U
#define COFFEE2_BUS2_PROTOCOL                 COFFEE2_BUS_PROTOCOL_F200_UART
#define COFFEE2_BUS3_PROTOCOL                 COFFEE2_BUS_PROTOCOL_MODBUS_RTU
#define COFFEE2_BUS4_PROTOCOL                 COFFEE2_BUS_PROTOCOL_MODBUS_RTU
#define COFFEE2_BUS5_PROTOCOL                 COFFEE2_BUS_PROTOCOL_MODBUS_RTU
#define COFFEE2_MODBUS_BUS_COUNT              \
	(((COFFEE2_BUS2_PROTOCOL == COFFEE2_BUS_PROTOCOL_MODBUS_RTU) ? 1U : 0U) + \
	 ((COFFEE2_BUS3_PROTOCOL == COFFEE2_BUS_PROTOCOL_MODBUS_RTU) ? 1U : 0U) + \
	 ((COFFEE2_BUS4_PROTOCOL == COFFEE2_BUS_PROTOCOL_MODBUS_RTU) ? 1U : 0U) + \
	 ((COFFEE2_BUS5_PROTOCOL == COFFEE2_BUS_PROTOCOL_MODBUS_RTU) ? 1U : 0U))

/** @brief RTU ownership and queue parameters for UART2 through UART5. */
#define COFFEE2_RTU_BUS_COUNT                4U
#define COFFEE2_COMMAND_QUEUE_LENGTH         4U
#define COFFEE2_RTU_IO_TIMEOUT_MS            500U
#define COFFEE2_RTU_IDLE_MS                  20U

/** @brief UART defaults reapplied by Coffee2 before task creation. */
#define COFFEE2_BUS2_DEFAULT_BAUD            115200U
#define COFFEE2_BUS3_DEFAULT_BAUD            9600U
#define COFFEE2_BUS4_DEFAULT_BAUD            19200U
#define COFFEE2_BUS5_DEFAULT_BAUD            38400U
#define COFFEE2_LOG_BAUD                     115200U

/** @brief IO 模块命令之间的最小帧间隔，单位为毫秒。 */
#define COFFEE2_IO_MIN_FRAME_INTERVAL_MS     20U
#define COFFEE2_ICE_MIN_FRAME_INTERVAL_MS    100U
#define COFFEE2_EXTERNAL_IO_POINT_COUNT       16U

/** @brief FreeRTOS task stack sizes in StackType_t units. */
#define COFFEE2_LOG_TASK_STACK               256U
#define COFFEE2_SERVER_TASK_STACK            1536U
#define COFFEE2_ROBOT_TASK_STACK             1024U
#define COFFEE2_RTU_TASK_STACK               384U
#define COFFEE2_WORKFLOW_TASK_STACK          1024U

/** @brief Workflow timing parameters. */
#define COFFEE2_WORKFLOW_QUEUE_LENGTH        2U
#define COFFEE2_WORKFLOW_IO_REFRESH_MS       100U
#define COFFEE2_WORKFLOW_DEFAULT_TIMEOUT_MS  30000U
/** @brief Provisional syrup controller time units per host 0.1 ml unit. */
#define COFFEE2_SYRUP_TIME_PER_VOLUME_UNIT    1U
/** @brief Product IO workflow timings inherited from the validated baseline. */
#define COFFEE2_HOT_WATER_FILL_TIMEOUT_MS     120000U
#define COFFEE2_HOT_WATER_DEFAULT_HEAT_MIN    30U
#define COFFEE2_HOT_WATER_MAX_HEAT_MIN        120U
#define COFFEE2_FRUIT_MILK_MS_PER_ML          100U
#define COFFEE2_FRUIT_MILK_CLEAN_MS           15000U
#define COFFEE2_WORKFLOW_DEVICE_IO_TIMEOUT_MS  1000U
#define COFFEE2_WORKFLOW_IO_ACTION_TIMEOUT_MS 5000U

/** @brief Provisional linear ice calibration in 0.1 g and milliseconds. */
#define COFFEE2_ICE_SLOPE_MS_PER_GRAM         18L
#define COFFEE2_ICE_OFFSET_MS                 (-300L)
#define COFFEE2_ICE_COMPENSATION_FACTOR      1L
#define COFFEE2_ICE_MIN_PULSE_MS              200U
#define COFFEE2_ICE_MAX_PULSE_MS              2000U
#define COFFEE2_ICE_SETTLE_MS                 1000U
#define COFFEE2_ICE_TOLERANCE_DECIGRAM        20L
#define COFFEE2_ICE_MAX_CORRECTIONS           2U

#endif /* COFFEE2_APP_CONFIG_H */
