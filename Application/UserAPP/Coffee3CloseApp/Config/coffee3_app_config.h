/**
  * @file      coffee3_app_config.h
  * @brief     Define Coffee3 product communication and task parameters.
  * @author    WHong
  * @date      2026-07-30
  */

#ifndef COFFEE3_APP_CONFIG_H
#define COFFEE3_APP_CONFIG_H

#include "compiler_compat.h"

/** @brief Log event that exposes the actual product firmware version. */
#define COFFEE3_DEVICE_VERSION_EVENT         "FW_VERSION:Coffee3CloseV3.0.0"
/** @brief Last valid logical coffee recipe identifier. */
#define COFFEE3_COFFEE_RECIPE_MAX            0x0021U

/** @brief Place reviewed CPU-only static data in startup-cleared STM32F4 CCM. */
#define COFFEE3_CCM_DATA APP_CCM_DATA

/** @brief Static IPv4 address applied after CubeMX LwIP initialization. */
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

/** @brief Host-facing Modbus TCP server configuration. */
#define COFFEE3_SERVER_PORT                  6001U
#define COFFEE3_SERVER_UNIT_ID               1U
#define COFFEE3_SERVER_MAX_CLIENTS           4U
#define COFFEE3_SERVER_POLL_MS               20U
#define COFFEE3_SERVER_BYTE_TIMEOUT_MS       100U

/** @brief Robot Modbus TCP client configuration. */
#define COFFEE3_ROBOT_IP_0                   192U
#define COFFEE3_ROBOT_IP_1                   168U
#define COFFEE3_ROBOT_IP_2                   5U
#define COFFEE3_ROBOT_IP_3                   1U
#define COFFEE3_ROBOT_PORT                   502U
#define COFFEE3_ROBOT_UNIT_ID                1U
#define COFFEE3_ROBOT_CONNECT_TIMEOUT_MS     3000U
#define COFFEE3_ROBOT_IO_TIMEOUT_MS          1000U
#define COFFEE3_ROBOT_LOOP_MS                20U
#define COFFEE3_ROBOT_ACTION_POLL_MS        100U
#define COFFEE3_ROBOT_ACCEPT_LOG_INTERVAL_MS 5000U
#define COFFEE3_ROBOT_MOTION_TIMEOUT_MS     60000U
#define COFFEE3_ROBOT_EDGE_LOW_MS           50U
#define COFFEE3_ROBOT_RETRY_MAX_MS          30000U
#define COFFEE3_ROBOT_READY_SAMPLES         3U

/** @brief Select the validated Dobot product register contract. */
#define COFFEE3_ROBOT_PROTOCOL_1             0U
#define COFFEE3_ROBOT_PROTOCOL_2             1U
#define COFFEE3_ROBOT_PROTOCOL_3             2U
#define COFFEE3_ROBOT_PROTOCOL_VARIANT       COFFEE3_ROBOT_PROTOCOL_1

/** @brief Select one protocol owner for each physical UART bus. */
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

/** @brief RTU ownership and queue parameters for UART2 through UART5. */
#define COFFEE3_RTU_BUS_COUNT                4U
#define COFFEE3_COMMAND_QUEUE_LENGTH         4U
#define COFFEE3_RTU_IO_TIMEOUT_MS            500U
#define COFFEE3_RTU_IDLE_MS                  20U

/** @brief UART defaults reapplied by Coffee3 before task creation. */
#define COFFEE3_BUS2_DEFAULT_BAUD            115200U
#define COFFEE3_BUS3_DEFAULT_BAUD            9600U
#define COFFEE3_BUS4_DEFAULT_BAUD            19200U
#define COFFEE3_BUS5_DEFAULT_BAUD            38400U
#define COFFEE3_LOG_BAUD                     115200U

/** @brief IO 模块命令之间的最小帧间隔，单位为毫秒。 */
#define COFFEE3_IO_MIN_FRAME_INTERVAL_MS     20U
#define COFFEE3_ICE_MIN_FRAME_INTERVAL_MS    100U
#define COFFEE3_EXTERNAL_IO_POINT_COUNT       16U

/** @brief FreeRTOS task stack sizes in StackType_t units. */
#define COFFEE3_LOG_TASK_STACK               256U
#define COFFEE3_SERVER_TASK_STACK            1536U
#define COFFEE3_ROBOT_TASK_STACK             1024U
#define COFFEE3_RTU_TASK_STACK               384U
#define COFFEE3_WORKFLOW_TASK_STACK          1024U

/** @brief Workflow timing parameters. */
#define COFFEE3_WORKFLOW_QUEUE_LENGTH        2U
#define COFFEE3_WORKFLOW_IO_REFRESH_MS       100U
#define COFFEE3_WORKFLOW_DEFAULT_TIMEOUT_MS  30000U
#define COFFEE3_COFFEE_ACTION_TIMEOUT_MS      180000U
#define COFFEE3_OUTLET_EMPTY_HOLD_MS          30000U
#define COFFEE3_DOOR_MOTION_TIMEOUT_MS        5000U
#define COFFEE3_IO_STALE_MS                  2000U
/** @brief Provisional syrup controller time units per host 0.1 ml unit. */
#define COFFEE3_SYRUP_TIME_PER_VOLUME_UNIT    1U
/** @brief Product IO workflow timings inherited from the validated baseline. */
#define COFFEE3_HOT_WATER_FILL_TIMEOUT_MS     120000U
#define COFFEE3_HOT_WATER_DEFAULT_HEAT_MIN    30U
#define COFFEE3_HOT_WATER_MAX_HEAT_MIN        120U
#define COFFEE3_FRUIT_MILK_MS_PER_ML          100U
#define COFFEE3_FRUIT_MILK_CLEAN_MS           15000U
#define COFFEE3_WORKFLOW_DEVICE_IO_TIMEOUT_MS  1000U
#define COFFEE3_WORKFLOW_IO_ACTION_TIMEOUT_MS 5000U

/** @brief Provisional linear ice calibration in 0.1 g and milliseconds. */
#define COFFEE3_ICE_SLOPE_MS_PER_GRAM         18L
#define COFFEE3_ICE_OFFSET_MS                 (-300L)
#define COFFEE3_ICE_COMPENSATION_FACTOR      1L
#define COFFEE3_ICE_MIN_PULSE_MS              200U
#define COFFEE3_ICE_MAX_PULSE_MS              2000U
#define COFFEE3_ICE_SETTLE_MS                 1000U
#define COFFEE3_ICE_TOLERANCE_DECIGRAM        20L
#define COFFEE3_ICE_MAX_CORRECTIONS           2U

#endif /* COFFEE3_APP_CONFIG_H */
