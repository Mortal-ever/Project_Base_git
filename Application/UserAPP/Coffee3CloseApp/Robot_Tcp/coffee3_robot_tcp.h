/**
  * @file      coffee3_robot_tcp.h
  * @brief     声明 Coffee3 机器人 Modbus TCP 所有者任务接口。
  * @author    WHong
  * @date      2026-09-24
  */

#ifndef COFFEE3_ROBOT_TCP_H
#define COFFEE3_ROBOT_TCP_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "FreeRTOS.h"
#include "coffee3_app_config.h"
#include "coffee3_device.h"

#if (COFFEE3_ROBOT_PROTOCOL_VARIANT == \
	COFFEE3_ROBOT_PROTOCOL_2) || \
	(COFFEE3_ROBOT_PROTOCOL_VARIANT == COFFEE3_ROBOT_PROTOCOL_3)
/** @brief 协议二和协议三的控制及结果线圈快照长度。 */
#define COFFEE3_ROBOT_CONTROL_COIL_COUNT  61U
#else
/** @brief 协议一的控制及结果线圈快照长度。 */
#define COFFEE3_ROBOT_CONTROL_COIL_COUNT  40U
#endif

/** @brief 保存机器人 TCP 生命周期和命令所有者状态。 */
typedef struct {
	uint32_t ulConnectAttemptCount; /*!< 累计 TCP 建连尝试次数。 */
	uint32_t ulConnectSuccessCount; /*!< 累计 TCP 建连成功次数。 */
	uint32_t ulDisconnectCount; /*!< 已观察到的连接关闭次数。 */
	uint32_t ulCommandCount; /*!< 主执行路径累计处理的命令数；本体控制和准备订单的早退路径不计入。 */
	uint32_t ulErrorCount; /*!< 主执行路径失败及会话退避累计次数；不覆盖所有命令错误。 */
	uint32_t ulConsecutiveFailures; /*!< 当前连续失败次数，用于恢复诊断。 */
	uint32_t ulNextRetryDelayMs; /*!< 已发布的下次重连等待时间，单位为毫秒。 */
	int32_t lLastResult; /*!< 主执行、周期健康刷新或会话迁移最近写入的结果；不覆盖所有命令。 */
	uint8_t ucConnected; /*!< TCP 传输连接状态，不代表机器人可运动。 */
	uint8_t ucReady; /*!< 机器人严格就绪状态，命令前应以此判断。 */
} Coffee3RobotTcpStatus_t;

/** @brief 保存用于安全和动作判断的最近一次控制器快照。 */
typedef struct {
	uint8_t aucBaseInputs[16]; /*!< 按协议顺序排列的 16 个本体离散输入。 */
	uint8_t aucControlCoils[COFFEE3_ROBOT_CONTROL_COIL_COUNT]; /*!< 当前协议版本的控制和结果线圈快照。 */
} Coffee3RobotData_t;

/** @brief 机器人 TCP 生命周期与命令执行状态。 */
extern Coffee3RobotTcpStatus_t g_xCoffee3RobotTcpStatus;
/** @brief 最近一次机器人本体输入和控制线圈快照。 */
extern Coffee3RobotData_t g_xCoffee3RobotData;

BaseType_t xCoffee3RobotTcpInitialize(void);

BaseType_t xCoffee3RobotTcpSubmitManualMotion(Coffee3Command_t *pxCommand);

void vCoffee3RobotTcpTask(void *pvArgument);

void vCoffee3RobotTcpRequestShutdown(void);
uint8_t ucCoffee3RobotTcpShutdownComplete(void);

#ifdef __cplusplus
}
#endif

#endif /* COFFEE3_ROBOT_TCP_H */


