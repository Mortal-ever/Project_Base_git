/**
  * @file      coffee3_robot_tcp.c
  * @brief     实现机器人 Modbus TCP 连接、命令执行与断线恢复。
  * @author    WHong
  * @date      2026-09-24
  */

#include "coffee3_robot_tcp.h"
#include "coffee3_server.h"

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "coffee3_manager.h"
#include "coffee3_app_config.h"
#include "coffee3_device.h"
#include "coffee3_log.h"
#include "dobot_robot_device.h"
#include "modbus_port.h"
#include "queue.h"
#include "task.h"
#include "TcpClientSession/tcp_client_session.h"
#include "transport_tcp.h"
#include "stm32f4xx_hal.h"
#include "Ota/app_ota_flash.h"

/* 本机端口日志专用扇区为 10 和 11，位于应用程序及 OTA 暂存区之外。
 * 每条已提交记录都在发送 SYN 前预留一个端口；轮换擦除时，另一扇区仍
 * 保留上一条有效记录。 */
#define C3_PORT_BANK0 0x080C0000UL
#define C3_PORT_BANK1 0x080E0000UL
#define C3_PORT_END   0x08100000UL
#define C3_PORT_MAGIC 0x43535054UL
static uint8_t s_ucFreshReadyReset; /*!< 新连接已完成一次就绪状态复位。 */
static uint8_t s_ucConnectionStartupPending; /*!< 当前连接仍需执行启动检查。 */
static uint8_t s_ucRobotProtocolFailureCount; /*!< 连续协议帧异常计数。 */
static uint8_t s_ucRobotNoResponseCount; /*!< 连续无响应或超时计数。 */

/**
  * @brief  在 Flash 双扇区日志中预留下一次 TCP 本机端口。
  * @retval 49152 至 65535 本次连接已持久化的本机端口。
  * @retval 0 Flash 容量、OTA 互斥或写入校验失败，本次连接应延后。
  * @note   先持久化序号再允许发送 SYN，避免复位后复用结果不明连接的端口。
  */
static uint16_t prvReserveRobotPort(void)
{
	uint32_t ulAddress, ulLatest = 0U, ulSequence = 0U; /*!< 扫描地址、最新记录地址及序号。 */
	uint32_t ulBank, ulEnd, ulSlot, ulError; /*!< 当前扇区、边界、写槽和擦除错误。 */
	const volatile uint32_t *pulRecord; /*!< 指向 Flash 中四字记录的只读指针。 */
	FLASH_EraseInitTypeDef xErase; /*!< 扇区轮换时使用的 HAL 擦除参数。 */
	HAL_StatusTypeDef xResult = HAL_OK; /*!< Flash 解锁、擦写和上锁的综合结果。 */
	uint16_t usPort; /*!< 由持久化序号映射出的动态本机端口。 */

	/* 第一步：确认芯片容量，再扫描两个扇区找出最新有效序号。 */
	if (*(const volatile uint16_t *)FLASHSIZE_BASE != 1024U) {
		(void)xCoffee3LogWrite(COFFEE3_LOG_LEVEL_ERROR,
			COFFEE3_LOG_SOURCE_ROBOT, "Source port journal requires 1MB Flash", -1);
		return 0U;
	}
	for (ulAddress = C3_PORT_BANK0; ulAddress < C3_PORT_END; ulAddress += 16U) {
		pulRecord = (const volatile uint32_t *)ulAddress;
		if ((pulRecord[0] == C3_PORT_MAGIC) &&
			(pulRecord[2] == ~pulRecord[1]) && (pulRecord[3] == 0U) &&
			((ulLatest == 0U) || ((int32_t)(pulRecord[1] - ulSequence) > 0))) {
			ulSequence = pulRecord[1];
			ulLatest = ulAddress;
		}
	}
	ulBank = (ulLatest >= C3_PORT_BANK1) ? C3_PORT_BANK1 : C3_PORT_BANK0;
	ulEnd = ulBank + 0x20000UL;
	ulSlot = (ulLatest == 0U) ? ulBank : ulLatest + 16U;
	for (; ulSlot < ulEnd; ulSlot += 16U) {
		pulRecord = (const volatile uint32_t *)ulSlot;
		if ((pulRecord[0] == 0xFFFFFFFFUL) && (pulRecord[1] == 0xFFFFFFFFUL) &&
			(pulRecord[2] == 0xFFFFFFFFUL) && (pulRecord[3] == 0xFFFFFFFFUL)) {
			break;
		}
	}
	/* 第二步：暂停任务调度，与任务上下文中的 OTA Flash 写入互斥。 */
	vTaskSuspendAll();
	if (ucAppOtaFlashIsActive() != 0U) {
		(void)xTaskResumeAll();
		return 0U;
	}
	xResult = HAL_FLASH_Unlock();
	if ((xResult == HAL_OK) && (ulSlot >= ulEnd)) {
		ulSlot = (ulBank == C3_PORT_BANK0) ? C3_PORT_BANK1 : C3_PORT_BANK0;
		memset(&xErase, 0, sizeof(xErase));
		xErase.TypeErase = FLASH_TYPEERASE_SECTORS;
		xErase.Sector = (ulSlot == C3_PORT_BANK0) ? FLASH_SECTOR_10 : FLASH_SECTOR_11;
		xErase.NbSectors = 1U;
		xErase.VoltageRange = FLASH_VOLTAGE_RANGE_3;
		xResult = HAL_FLASHEx_Erase(&xErase, &ulError);
	}
	/* 第三步：按“标记、序号、反码、提交字”顺序写入并回读校验。 */
	ulSequence++;
	if (xResult == HAL_OK) xResult = HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, ulSlot, C3_PORT_MAGIC);
	if (xResult == HAL_OK) xResult = HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, ulSlot + 4U, ulSequence);
	if (xResult == HAL_OK) xResult = HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, ulSlot + 8U, ~ulSequence);
	if (xResult == HAL_OK) xResult = HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, ulSlot + 12U, 0U);
	if (HAL_FLASH_Lock() != HAL_OK) xResult = HAL_ERROR;
	pulRecord = (const volatile uint32_t *)ulSlot;
	if ((xResult == HAL_OK) && ((pulRecord[0] != C3_PORT_MAGIC) ||
		(pulRecord[1] != ulSequence) || (pulRecord[2] != ~ulSequence) ||
		(pulRecord[3] != 0U))) xResult = HAL_ERROR;
	(void)xTaskResumeAll();
	if (xResult != HAL_OK) {
		(void)xCoffee3LogWrite(COFFEE3_LOG_LEVEL_ERROR,
			COFFEE3_LOG_SOURCE_ROBOT, "Source port persistence failed; connect deferred", (int32_t)xResult);
		return 0U;
	}
	/* 第四步：只在记录持久化成功后发布端口供 TCP 建连使用。 */
	usPort = (uint16_t)(49152UL + ((ulSequence - 1U) % 16384UL));
	(void)xCoffee3LogPrintfOrder(COFFEE3_LOG_LEVEL_INFO,
		COFFEE3_LOG_SOURCE_ROBOT, COFFEE3_LOG_ORDER_SYSTEM,
		"TCP source port reserved=%u destination=502 sequence=%lu", (unsigned int)usPort,
		(unsigned long)ulSequence);
	return usPort;
}

/** @brief 已连接状态下刷新机器人快照的周期，单位为毫秒。 */
#define COFFEE3_ROBOT_HEALTH_MS              2000U
#define COFFEE3_ROBOT_STARTUP_RETRY_MS        3000U
#define COFFEE3_ROBOT_STARTUP_STEP_DELAY_MS  200U
#define COFFEE3_ROBOT_STARTUP_FINAL_WAIT_MS  8000U
#define COFFEE3_ROBOT_STARTUP_POLL_MS        100U
#define COFFEE3_ROBOT_PROTOCOL_FAILURE_LIMIT 3U
#define COFFEE3_ROBOT_NO_RESPONSE_LIMIT      5U
/** @brief 机器人任务栈上连接事件文本的最大字节数。 */
#define COFFEE3_ROBOT_CONNECTION_EVENT_LENGTH 64U

#define COFFEE3_ROBOT_STARTUP_STATE_DRAG       0x0001U
#define COFFEE3_ROBOT_STARTUP_STATE_POWER      0x0002U
#define COFFEE3_ROBOT_STARTUP_STATE_ENABLE     0x0004U
#define COFFEE3_ROBOT_STARTUP_STATE_ALARM      0x0008U
#define COFFEE3_ROBOT_STARTUP_STATE_COLLISION  0x0010U
#define COFFEE3_ROBOT_STARTUP_STATE_SAFETY     0x0020U
#define COFFEE3_ROBOT_STARTUP_STATE_RECOVERY   0x0040U
#define COFFEE3_ROBOT_STARTUP_STATE_READY      0x0080U
#define COFFEE3_ROBOT_STARTUP_STATE_MOTION     0x0100U

#if (COFFEE3_ROBOT_PROTOCOL_VARIANT == \
	COFFEE3_ROBOT_PROTOCOL_2)
#define COFFEE3_DOBOT_PROTOCOL_VARIANT \
	DOBOT_ROBOT_PROTOCOL_2
#define COFFEE3_ROBOT_LOG_DRIVER_ID \
	DEVICE_DRIVER_ROBOT_DOBOT_PROTOCOL_2
#elif (COFFEE3_ROBOT_PROTOCOL_VARIANT == \
	COFFEE3_ROBOT_PROTOCOL_3)
#define COFFEE3_DOBOT_PROTOCOL_VARIANT \
	DOBOT_ROBOT_PROTOCOL_3
#define COFFEE3_ROBOT_LOG_DRIVER_ID \
	DEVICE_DRIVER_ROBOT_DOBOT_PROTOCOL_3
#else
#define COFFEE3_DOBOT_PROTOCOL_VARIANT \
	DOBOT_ROBOT_PROTOCOL_1
#define COFFEE3_ROBOT_LOG_DRIVER_ID \
	DEVICE_DRIVER_ROBOT_DOBOT_PROTOCOL_1
#endif

/** @brief 定义机器人连接后启动检查的业务结果。 */
typedef enum {
	COFFEE3_ROBOT_STARTUP_OK = 0, /*!< 启动检查通过并达到要求状态。 */
	COFFEE3_ROBOT_STARTUP_NOT_READY = 1 /*!< 通信成功但机器人尚未就绪。 */
} Coffee3RobotStartupOutcome_e;

/** @brief 区分机器人本体控制命令与点位动作命令。 */
typedef enum {
	COFFEE3_DOBOT_COMMAND_NONE = 0, /*!< 产品动作无法映射到机器人协议。 */
	COFFEE3_DOBOT_COMMAND_BODY = 1, /*!< 通过本体控制线圈发送的脉冲命令。 */
	COFFEE3_DOBOT_COMMAND_ACTION = 2 /*!< 使用命令和结果线圈握手的点位动作。 */
} Coffee3DobotCommandKind_e;

/** @brief 保存一次产品动作解析后的机器人协议目标。 */
typedef struct {
	Coffee3DobotCommandKind_e xKind; /*!< 已解析的命令类别。 */
	DobotRobotBodyCommand_e xBodyCommand; /*!< 本体控制命令枚举。 */
	uint16_t usCommandCoil; /*!< 点位动作命令线圈地址。 */
	uint16_t usResultCoil; /*!< 点位动作完成线圈地址。 */
} Coffee3DobotCommand_t;

static const DobotRobotPoint_t s_axCoffee3DobotPoints[] = { /*!< 产品动作到协议线圈的映射表。 */
	{ COFFEE3_ACTION_ROBOT_HOME, DOBOT_ROBOT_SELECTOR_ANY, DOBOT_ROBOT_P1_COMMAND_HOME, DOBOT_ROBOT_P1_RESULT_HOME },
	{ COFFEE3_ACTION_ROBOT_TAKE_HOT_CUP, DOBOT_ROBOT_SELECTOR_ANY, DOBOT_ROBOT_P1_COMMAND_HOT_CUP, DOBOT_ROBOT_P1_RESULT_HOT_CUP },
	{ COFFEE3_ACTION_ROBOT_TAKE_COLD_CUP, DOBOT_ROBOT_SELECTOR_ANY, DOBOT_ROBOT_P1_COMMAND_COLD_CUP, DOBOT_ROBOT_P1_RESULT_COLD_CUP },
	{ COFFEE3_ACTION_ROBOT_TO_COFFEE, 0U, DOBOT_ROBOT_P1_COMMAND_COFFEE_FRONT, DOBOT_ROBOT_P1_RESULT_COFFEE_FRONT },
	{ COFFEE3_ACTION_ROBOT_TO_COFFEE, 1U, DOBOT_ROBOT_P1_COMMAND_COFFEE_INSIDE, DOBOT_ROBOT_P1_RESULT_COFFEE_INSIDE },
	{ COFFEE3_ACTION_ROBOT_TO_ICE, DOBOT_ROBOT_SELECTOR_ANY, DOBOT_ROBOT_P1_COMMAND_ICE, DOBOT_ROBOT_P1_RESULT_ICE },
	{ COFFEE3_ACTION_ROBOT_TO_LID, 0U, DOBOT_ROBOT_P1_COMMAND_LID_1, DOBOT_ROBOT_P1_RESULT_LID_1 },
	{ COFFEE3_ACTION_ROBOT_TO_LID, 1U, DOBOT_ROBOT_P1_COMMAND_LID_2, DOBOT_ROBOT_P1_RESULT_LID_2 },
	{ COFFEE3_ACTION_ROBOT_TAKE_LID, DOBOT_ROBOT_SELECTOR_ANY, DOBOT_ROBOT_P1_COMMAND_TAKE_LID, DOBOT_ROBOT_P1_RESULT_TAKE_LID },
	{ COFFEE3_ACTION_ROBOT_COVER_LID, DOBOT_ROBOT_SELECTOR_ANY, DOBOT_ROBOT_P1_COMMAND_COVER_LID, DOBOT_ROBOT_P1_RESULT_COVER_LID },
	{ COFFEE3_ACTION_ROBOT_PUT_OUTPUT, 1U, DOBOT_ROBOT_P1_COMMAND_PUT_OUTPUT, DOBOT_ROBOT_P1_RESULT_PUT_OUTPUT },
	{ COFFEE3_ACTION_ROBOT_PUT_STORAGE, DOBOT_ROBOT_SELECTOR_ANY, DOBOT_ROBOT_P1_COMMAND_PUT_STORAGE, DOBOT_ROBOT_P1_RESULT_PUT_STORAGE },
	{ COFFEE3_ACTION_ROBOT_TO_PRINTER, DOBOT_ROBOT_SELECTOR_ANY, DOBOT_ROBOT_P1_COMMAND_PRINTER, DOBOT_ROBOT_P1_RESULT_PRINTER },
	{ COFFEE3_ACTION_ROBOT_TAKE_OUTPUT_1, DOBOT_ROBOT_SELECTOR_ANY, DOBOT_ROBOT_P1_COMMAND_OUTPUT_1, DOBOT_ROBOT_P1_RESULT_OUTPUT_1 },
	{ COFFEE3_ACTION_ROBOT_TAKE_COFFEE, DOBOT_ROBOT_SELECTOR_ANY, DOBOT_ROBOT_P1_COMMAND_TAKE_COFFEE, DOBOT_ROBOT_P1_RESULT_TAKE_COFFEE },
	{ COFFEE3_ACTION_ROBOT_TAKE_STORAGE, DOBOT_ROBOT_SELECTOR_ANY, DOBOT_ROBOT_P1_COMMAND_TAKE_STORAGE, DOBOT_ROBOT_P1_RESULT_TAKE_STORAGE },
	{ COFFEE3_ACTION_ROBOT_TO_FRUIT_SYRUP, DOBOT_ROBOT_SELECTOR_ANY, DOBOT_ROBOT_P1_COMMAND_FRUIT_SYRUP, DOBOT_ROBOT_P1_RESULT_FRUIT_SYRUP }
};

#if (COFFEE3_ROBOT_PROTOCOL_VARIANT == COFFEE3_ROBOT_PROTOCOL_1)
static const DobotRobotDriverConfig_t s_xCoffee3DobotConfig1 = { /*!< 协议一的驱动和点位表配置。 */
	&g_xDobotRobotProtocol1, s_axCoffee3DobotPoints,
	(uint16_t)(sizeof(s_axCoffee3DobotPoints) / sizeof(s_axCoffee3DobotPoints[0])),
	DEVICE_ROLE_ROBOT_1
};
#elif (COFFEE3_ROBOT_PROTOCOL_VARIANT == COFFEE3_ROBOT_PROTOCOL_2)
static const DobotRobotDriverConfig_t s_xCoffee3DobotConfig2 = { /*!< 协议二的驱动和点位表配置。 */
	&g_xDobotRobotProtocol2, s_axCoffee3DobotPoints,
	(uint16_t)(sizeof(s_axCoffee3DobotPoints) / sizeof(s_axCoffee3DobotPoints[0])),
	DEVICE_ROLE_ROBOT_1
};
#elif (COFFEE3_ROBOT_PROTOCOL_VARIANT == COFFEE3_ROBOT_PROTOCOL_3)
static const DobotRobotDriverConfig_t s_xCoffee3DobotConfig3 = { /*!< 协议三的驱动和点位表配置。 */
	&g_xDobotRobotProtocol3, s_axCoffee3DobotPoints,
	(uint16_t)(sizeof(s_axCoffee3DobotPoints) / sizeof(s_axCoffee3DobotPoints[0])),
	DEVICE_ROLE_ROBOT_1
};
#endif

/**
  * @brief  获取编译期选定的 Dobot 协议驱动配置。
  * @retval 非空 当前协议版本对应的只读驱动配置。
  */
static const DobotRobotDriverConfig_t *prvCoffee3DobotConfig(void)
{
	#if (COFFEE3_ROBOT_PROTOCOL_VARIANT == COFFEE3_ROBOT_PROTOCOL_2)
	return &s_xCoffee3DobotConfig2;
	#elif (COFFEE3_ROBOT_PROTOCOL_VARIANT == COFFEE3_ROBOT_PROTOCOL_3)
	return &s_xCoffee3DobotConfig3;
	#else
	return &s_xCoffee3DobotConfig1;
	#endif
}

/**
  * @brief  将 Coffee3 产品动作解析为本体命令或点位线圈对。
  * @param[in] pxCommand 待解析命令，不得为空。
  * @param[out] pxResolved 接收解析结果，不得为空。
  * @retval 1 解析成功。
  * @retval 0 参数无效、动作不支持或选择参数不合法。
  */
static uint8_t prvResolveCoffee3Dobot(const Coffee3Command_t *pxCommand,
	Coffee3DobotCommand_t *pxResolved)
{
	uint16_t usSelector; /*!< 区分咖啡机内外、杯盖位或出杯位的点位选择值。 */
	if ((pxCommand == NULL) || (pxResolved == NULL)) return 0U;
	pxResolved->xKind = COFFEE3_DOBOT_COMMAND_NONE;
	pxResolved->xBodyCommand = DOBOT_ROBOT_BODY_COMMAND_START;
	pxResolved->usCommandCoil = 0U;
	pxResolved->usResultCoil = 0U;
	switch ((Coffee3Action_e)pxCommand->usAction) {
	case COFFEE3_ACTION_ROBOT_START: pxResolved->xBodyCommand = DOBOT_ROBOT_BODY_COMMAND_START; break;
	case COFFEE3_ACTION_ROBOT_STOP: case COFFEE3_ACTION_CANCEL: pxResolved->xBodyCommand = DOBOT_ROBOT_BODY_COMMAND_STOP; break;
	case COFFEE3_ACTION_ROBOT_PAUSE: pxResolved->xBodyCommand = DOBOT_ROBOT_BODY_COMMAND_PAUSE; break;
	case COFFEE3_ACTION_ROBOT_ENABLE: pxResolved->xBodyCommand = DOBOT_ROBOT_BODY_COMMAND_ENABLE; break;
	case COFFEE3_ACTION_ROBOT_DISABLE: pxResolved->xBodyCommand = DOBOT_ROBOT_BODY_COMMAND_DISABLE; break;
	case COFFEE3_ACTION_ROBOT_CLEAR_ALARM: case COFFEE3_ACTION_RESET: pxResolved->xBodyCommand = DOBOT_ROBOT_BODY_COMMAND_CLEAR_ALARM; break;
	case COFFEE3_ACTION_ROBOT_ENTER_DRAG: pxResolved->xBodyCommand = DOBOT_ROBOT_BODY_COMMAND_ENTER_DRAG; break;
	case COFFEE3_ACTION_ROBOT_EXIT_DRAG: pxResolved->xBodyCommand = DOBOT_ROBOT_BODY_COMMAND_EXIT_DRAG; break;
	case COFFEE3_ACTION_ROBOT_AUTO_MODE: pxResolved->xBodyCommand = DOBOT_ROBOT_BODY_COMMAND_AUTO_MODE; break;
	case COFFEE3_ACTION_ROBOT_MANUAL_MODE: pxResolved->xBodyCommand = DOBOT_ROBOT_BODY_COMMAND_MANUAL_MODE; break;
	default:
		usSelector = DOBOT_ROBOT_SELECTOR_ANY;
		if (pxCommand->usAction == COFFEE3_ACTION_ROBOT_TO_COFFEE) usSelector = (pxCommand->ausParameter[0] == 0U) ? 0U : 1U;
		else if (pxCommand->usAction == COFFEE3_ACTION_ROBOT_TO_LID) usSelector = (pxCommand->ausParameter[0] <= 1U) ? 0U : 1U;
		else if (pxCommand->usAction == COFFEE3_ACTION_ROBOT_PUT_OUTPUT) {
			if (pxCommand->ausParameter[0] != 1U) return 0U;
			usSelector = pxCommand->ausParameter[0];
		}
		if (ucDobotRobotResolvePoint(prvCoffee3DobotConfig(), pxCommand->usAction,
			usSelector, &pxResolved->usCommandCoil, &pxResolved->usResultCoil) == 0U) return 0U;
		pxResolved->xKind = COFFEE3_DOBOT_COMMAND_ACTION;
		return 1U;
	}
	pxResolved->xKind = COFFEE3_DOBOT_COMMAND_BODY;
	return 1U;
}

/*-----------------------------------------------------------*/
/**
  * @brief  将产品动作编号转换为日志中可读的固定名称。
  * @param[in] usAction 产品动作编号。
  * @retval 字符串常量 已知动作名称或通用 ROBOT_ACTION 名称。
  */
static const char *prvRobotActionName(uint16_t usAction)
{
	switch ((Coffee3Action_e)usAction) {
	case COFFEE3_ACTION_ROBOT_START: return "ROBOT_START";
	case COFFEE3_ACTION_ROBOT_STOP: return "ROBOT_STOP";
	case COFFEE3_ACTION_ROBOT_ENABLE: return "ROBOT_ENABLE";
	case COFFEE3_ACTION_ROBOT_CLEAR_ALARM: return "ROBOT_CLEAR_ALARM";
	case COFFEE3_ACTION_ROBOT_PAUSE: return "ROBOT_PAUSE";
	case COFFEE3_ACTION_ROBOT_DISABLE: return "ROBOT_DISABLE";
	case COFFEE3_ACTION_ROBOT_ENTER_DRAG: return "ROBOT_ENTER_DRAG";
	case COFFEE3_ACTION_ROBOT_EXIT_DRAG: return "ROBOT_EXIT_DRAG";
	case COFFEE3_ACTION_ROBOT_AUTO_MODE: return "ROBOT_AUTO_MODE";
	case COFFEE3_ACTION_ROBOT_MANUAL_MODE: return "ROBOT_MANUAL_MODE";
	case COFFEE3_ACTION_ROBOT_HOME: return "ROBOT_HOME";
	case COFFEE3_ACTION_ROBOT_TAKE_HOT_CUP: return "ROBOT_TAKE_HOT_CUP";
	case COFFEE3_ACTION_ROBOT_TAKE_COLD_CUP: return "ROBOT_TAKE_COLD_CUP";
	case COFFEE3_ACTION_ROBOT_TO_COFFEE: return "ROBOT_TO_COFFEE";
	case COFFEE3_ACTION_ROBOT_TO_ICE: return "ROBOT_TO_ICE";
	case COFFEE3_ACTION_ROBOT_TO_LID: return "ROBOT_TO_LID";
	case COFFEE3_ACTION_ROBOT_TAKE_LID: return "ROBOT_TAKE_LID";
	case COFFEE3_ACTION_ROBOT_COVER_LID: return "ROBOT_COVER_LID";
	case COFFEE3_ACTION_ROBOT_PUT_OUTPUT: return "ROBOT_PUT_OUTPUT";
	case COFFEE3_ACTION_ROBOT_PUT_STORAGE: return "ROBOT_PUT_STORAGE";
	case COFFEE3_ACTION_ROBOT_TO_PRINTER: return "ROBOT_TO_PRINTER";
	case COFFEE3_ACTION_ROBOT_TAKE_OUTPUT_1: return "ROBOT_TAKE_OUTPUT_1";
	case COFFEE3_ACTION_ROBOT_TAKE_OUTPUT_2: return "ROBOT_TAKE_OUTPUT_2";
	case COFFEE3_ACTION_ROBOT_TAKE_COFFEE: return "ROBOT_TAKE_COFFEE";
	case COFFEE3_ACTION_ROBOT_TAKE_STORAGE: return "ROBOT_TAKE_STORAGE";
	case COFFEE3_ACTION_ROBOT_START_SIGNAL: return "ROBOT_START_SIGNAL";
	case COFFEE3_ACTION_ROBOT_TO_FRUIT_SYRUP: return "ROBOT_TO_FRUIT_SYRUP";
	default: return "ROBOT_ACTION";
	}
}

/** @brief 保存一个点位动作从准备、接受、运动到结果确认的完整事务。 */
typedef struct {
	Coffee3Command_t xCommand; /*!< 当前事务拥有的业务命令副本。 */
	uint16_t usCommandCoil; /*!< 本次动作的命令线圈地址。 */
	uint16_t usResultCoil; /*!< 本次动作的结果线圈地址。 */
	TickType_t xRecoveryStart; /*!< 进入断线恢复的系统节拍，0 表示未恢复。 */
	TickType_t xAcceptDeadline; /*!< 等待机器人清除命令线圈的绝对期限。 */
	TickType_t xMotionDeadline; /*!< 动作接受后的建议完成期限。 */
	TickType_t xNextPrepareRetryTick; /*!< 准备阶段下一次重试的系统节拍。 */
	TickType_t xAcceptedTick; /*!< 首次确认机器人接受命令的系统节拍。 */
	TickType_t xLastAcceptLogTick; /*!< 上次输出接受等待日志的节拍。 */
	TickType_t xNextPollTick; /*!< 下一次推进动作状态机的节拍。 */
	uint8_t ucActive; /*!< 非零表示事务仍由机器人任务持有。 */
	uint8_t ucRecovering; /*!< 非零表示事务正在断线或协议异常恢复。 */
	uint8_t ucAmbiguous; /*!< 命令线圈已清但本地未见接受，结果仍不确定。 */
	uint8_t ucAccepted; /*!< 已观察到机器人清除命令线圈。 */
	uint8_t ucResultWhileCommandHigh; /*!< 已记录命令与结果同时为高的时序异常。 */
	uint8_t ucCommandWriteAttempted; /*!< 已开始写动作命令，禁止恢复时重发。 */
	uint8_t ucCommandWriteConfirmed; /*!< 命令线圈写高已收到成功返回。 */
	uint8_t ucCompletionObserved; /*!< 已观察到结果线圈为高。 */
	uint8_t ucPrepareRetryCount; /*!< 命令发布前准备阶段的重试次数。 */
	uint8_t ucOverdue; /*!< 动作已超过接受或运动期限但仍继续等待。 */
	uint8_t ucOverdueLogged; /*!< 已输出一次逾期日志，防止重复刷屏。 */
	uint8_t ucTerminalLogged; /*!< 已保存最终结果日志的标志。 */
	int32_t lTerminalResult; /*!< 归档事务的最终结果码。 */
	Coffee3RobotPhase_e xPhase; /*!< 当前动作阶段。 */
} Coffee3RobotTransaction_t;

/* 动作事务从等待接受、运动到结果应答均由本任务独占管理。 */

/** @brief 向 TCP 会话回调提供 Modbus 端口和传输上下文。 */
typedef struct {
	ModbusPort_t *pxPort; /*!< 会话探测所用的机器人 Modbus 客户端端口。 */
	TransportTcpContext_t *pxTransport; /*!< 提供实际本机端口等 TCP 状态。 */
} Coffee3RobotSessionContext_t;

static void prvLogRobotConnected(const uint8_t pucIpv4[4], uint16_t usPort,
	uint32_t ulAttempt);

COFFEE3_CCM_DATA
Coffee3RobotTcpStatus_t g_xCoffee3RobotTcpStatus; /*!< 机器人连接和命令运行状态。 */
COFFEE3_CCM_DATA
Coffee3RobotData_t g_xCoffee3RobotData; /*!< 最近一次机器人控制器快照。 */

COFFEE3_CCM_DATA
static StaticQueue_t s_xRobotQueueStorage; /*!< 机器人命令静态队列的控制块。 */
COFFEE3_CCM_DATA
static uint8_t s_aucRobotQueueStorage[
	COFFEE3_COMMAND_QUEUE_LENGTH * sizeof(Coffee3Command_t)]; /*!< 队列项目存储区。 */
COFFEE3_CCM_DATA
static QueueHandle_t s_xRobotQueue; /*!< 机器人任务接收正式命令的队列句柄。 */
/* 0 表示空闲，1 表示已有待执行动作，2 表示正在申请工作流预留位。 */
COFFEE3_CCM_DATA
static volatile uint8_t s_ucManualMotionPending; /*!< 手动动作槽的三态占用标志。 */
COFFEE3_CCM_DATA
static Coffee3Command_t s_xManualMotionPending; /*!< 可被最新调试请求覆盖的手动动作。 */
COFFEE3_CCM_DATA
static volatile Coffee3RobotTransaction_t s_xLastTransaction; /*!< 最近归档事务的诊断快照。 */

static ModbusPortResult_e prvRefresh(ModbusPort_t *pxPort,
	uint32_t ulTimeoutMs);
static uint8_t prvRobotSessionNetworkReady(void *pvOwnerContext);
static int32_t prvRobotSessionProbe(void *pvOwnerContext,
	uint32_t ulTimeoutMs);
static void prvRobotSessionEvent(void *pvOwnerContext,
	TcpClientSessionState_e xPreviousState,
	TcpClientSessionState_e xCurrentState, int32_t lReason,
	uint32_t ulAttempt, uint32_t ulRetryDelayMs);
static ModbusPortResult_e prvExecute(ModbusPort_t *pxPort,
	const Coffee3Command_t *pxCommand,
	Coffee3RobotTransaction_t *pxTransaction,
	uint8_t *pucActionTimedOut);
static uint8_t prvRobotBasicAction(uint16_t usAction);
static ModbusPortResult_e prvWriteRisingEdge(ModbusPort_t *pxPort,
	uint16_t usCoil, uint32_t ulTimeoutMs);
static ModbusPortResult_e prvAdvanceAction(ModbusPort_t *pxPort,
	Coffee3RobotTransaction_t *pxTransaction, uint8_t *pucActionTimedOut);
static ModbusPortResult_e prvStartup(ModbusPort_t *pxPort,
	uint8_t ucRecoverySafe,
	Coffee3RobotStartupOutcome_e *pxOutcome);
static ModbusPortResult_e prvReconcile(ModbusPort_t *pxPort,
	Coffee3RobotTransaction_t *pxTransaction,
	uint8_t *pucDone);
static void prvBeginActionTransaction(Coffee3RobotTransaction_t *pxTransaction,
	const Coffee3Command_t *pxCommand, uint16_t usCommandCoil,
	uint16_t usResultCoil);
static ModbusPortResult_e prvSchedulePrepareRetry(
	Coffee3RobotTransaction_t *pxTransaction,
	ModbusPortResult_e xFailure);
static void prvArchiveAndResetTransaction(
	Coffee3RobotTransaction_t *pxTransaction, int32_t lTerminalResult);
static uint8_t prvRobotDebugMotion(const Coffee3Command_t *pxCommand);
static void prvMarkActionOverdue(Coffee3RobotTransaction_t *pxTransaction,
	const char *pcReason);
static uint8_t prvRobotOperational(void);
static uint8_t prvRobotStrictReady(void);
static ModbusPortResult_e prvClearActionCoils(ModbusPort_t *pxPort,
	uint8_t *pucStateMismatch, uint16_t usOrderId);
static ModbusPortResult_e prvClearActionRange(ModbusPort_t *pxPort,
	uint16_t usStart, uint16_t usCount, uint8_t *pucStateMismatch);
static uint16_t prvRobotStartupStateMask(void);
static ModbusPortResult_e prvWriteControlValue(ModbusPort_t *pxPort,
	uint16_t usCoil, bool bValue, uint32_t ulTimeoutMs);
/** @brief 清零连续协议错误与无响应计数。 */
static void prvResetRobotLinkHealth(void)
{
	s_ucRobotProtocolFailureCount = 0U;
	s_ucRobotNoResponseCount = 0U;
}

/*-----------------------------------------------------------*/
/**
  * @brief 结合原始结果、故障详情和主动探测判断 TCP 链路是否确已失效。
  * @param[in,out] pxPort 机器人 Modbus 端口，不得为空。
  * @param[in] xResult 触发判断的原始 Modbus 结果。
  * @param[out] pucProbeAttempted 可为空；非空时返回是否执行了主动探测。
  * @retval 1 传输层已断开，或连续协议/无响应达到重连阈值。
  * @retval 0 链路仍可用、探测恢复或尚未达到确认阈值。
  */
static uint8_t prvRobotLinkFailureConfirmed(ModbusPort_t *pxPort,
	ModbusPortResult_e xResult, uint8_t *pucProbeAttempted)
{
	ModbusPortFault_t xFault; /*!< 原始失败对应的传输与协议故障详情。 */
	ModbusPortFault_t xProbeFault; /*!< 主动探测失败后的故障详情。 */
	ModbusPortResult_e xProbeResult; /*!< 读取固定探测线圈的结果。 */
	bool bProbe; /*!< 探测线圈接收值，业务值本身不参与判定。 */

	if (pucProbeAttempted != NULL) {
		*pucProbeAttempted = 0U;
	}
	if (pxPort == NULL) {
		return 0U;
	}
	if (xResult == MODBUS_PORT_RESULT_OK) {
		prvResetRobotLinkHealth();
		return 0U;
	}
	/* 第一步：明确的套接字断开类错误立即确认链路丢失。 */
	memset(&xFault, 0, sizeof(xFault));
	vModbusPortGetLastFault(pxPort, &xFault);
	if ((xFault.xTransportResult == TRANSPORT_RESULT_DISCONNECTED) ||
		(xFault.xTransportResult == TRANSPORT_RESULT_NOT_OPEN) ||
		(xFault.xTransportResult == TRANSPORT_RESULT_NOT_READY) ||
		(xFault.xTransportResult == TRANSPORT_RESULT_IO_ERROR)) {
		(void)xCoffee3LogWriteField(COFFEE3_LOG_LEVEL_WARNING,
			COFFEE3_LOG_SOURCE_ROBOT, "ROBOT_LINK_CONFIRMED_LOST",
			(int32_t)xResult, "transport_result",
			(int32_t)xFault.xTransportResult);
		prvResetRobotLinkHealth();
		return 1U;
	}
	if ((xResult != MODBUS_PORT_RESULT_TIMEOUT) &&
		(xResult != MODBUS_PORT_RESULT_PROTOCOL) &&
		(xFault.xTransportResult != TRANSPORT_RESULT_TIMEOUT)) {
		return 0U;
	}
	if (xResult == MODBUS_PORT_RESULT_PROTOCOL) {
		if (s_ucRobotProtocolFailureCount < UINT8_MAX) {
			s_ucRobotProtocolFailureCount++;
		}
		(void)xCoffee3LogPrintfOrder(COFFEE3_LOG_LEVEL_WARNING,
			COFFEE3_LOG_SOURCE_ROBOT, COFFEE3_LOG_ORDER_SYSTEM,
			"ROBOT_FRAME_DROPPED result=%d protocol_code=%ld count=%u",
			(int)xResult, (long)xFault.lProtocolCode,
			(unsigned int)s_ucRobotProtocolFailureCount);
	} else if (s_ucRobotNoResponseCount < UINT8_MAX) {
		s_ucRobotNoResponseCount++;
	}
	if (pucProbeAttempted != NULL) {
		*pucProbeAttempted = 1U;
	}
	/* 第二步：对超时或协议异常补做一次读探测，避免误判瞬时帧错误。 */
	bProbe = false;
	xProbeResult = xModbusPortReadCoils(pxPort,
		COFFEE3_ROBOT_UNIT_ID, 3100U, 1U, &bProbe,
		COFFEE3_ROBOT_IO_TIMEOUT_MS);
	if (xProbeResult == MODBUS_PORT_RESULT_OK) {
		prvResetRobotLinkHealth();
		(void)xCoffee3LogWrite(COFFEE3_LOG_LEVEL_INFO,
			COFFEE3_LOG_SOURCE_ROBOT, "ROBOT_LINK_PROBE_OK", 0);
		return 0U;
	}
	memset(&xProbeFault, 0, sizeof(xProbeFault));
	vModbusPortGetLastFault(pxPort, &xProbeFault);
	(void)xCoffee3LogPrintfOrder(COFFEE3_LOG_LEVEL_WARNING,
		COFFEE3_LOG_SOURCE_ROBOT, COFFEE3_LOG_ORDER_SYSTEM,
		"ROBOT_LINK_PROBE_FAILED result=%d transport=%d protocol=%ld",
		(int)xProbeResult, (int)xProbeFault.xTransportResult,
		(long)xProbeFault.lProtocolCode);
	if ((xProbeFault.xTransportResult == TRANSPORT_RESULT_DISCONNECTED) ||
		(xProbeFault.xTransportResult == TRANSPORT_RESULT_NOT_OPEN) ||
		(xProbeFault.xTransportResult == TRANSPORT_RESULT_NOT_READY) ||
		(xProbeFault.xTransportResult == TRANSPORT_RESULT_IO_ERROR)) {
		prvResetRobotLinkHealth();
		return 1U;
	}
	if (xProbeResult == MODBUS_PORT_RESULT_PROTOCOL) {
		if (s_ucRobotProtocolFailureCount < UINT8_MAX) {
			s_ucRobotProtocolFailureCount++;
		}
	} else if ((xProbeResult == MODBUS_PORT_RESULT_TIMEOUT) ||
		(xProbeFault.xTransportResult == TRANSPORT_RESULT_TIMEOUT)) {
		if (s_ucRobotNoResponseCount < UINT8_MAX) {
			s_ucRobotNoResponseCount++;
		}
	}
	/* 第三步：只有累计达到阈值才强制会话重连。 */
	if (s_ucRobotProtocolFailureCount >=
		COFFEE3_ROBOT_PROTOCOL_FAILURE_LIMIT) {
		(void)xCoffee3LogWriteField(COFFEE3_LOG_LEVEL_ERROR,
			COFFEE3_LOG_SOURCE_ROBOT, "ROBOT_PROTOCOL_DESYNC_RESET",
			(int32_t)xProbeResult, "frames",
			(int32_t)s_ucRobotProtocolFailureCount);
		prvResetRobotLinkHealth();
		return 1U;
	}
	if (s_ucRobotNoResponseCount >= COFFEE3_ROBOT_NO_RESPONSE_LIMIT) {
		(void)xCoffee3LogWriteField(COFFEE3_LOG_LEVEL_ERROR,
			COFFEE3_LOG_SOURCE_ROBOT, "ROBOT_SESSION_UNRESPONSIVE",
			(int32_t)xProbeResult, "timeouts",
			(int32_t)s_ucRobotNoResponseCount);
		prvResetRobotLinkHealth();
		return 1U;
	}
	return 0U;
}

/*-----------------------------------------------------------*/
/**
  * @brief 将队首服务器命令保持为当前待处理命令。
  * @param[in] pxFirst 已从正式队列取得的命令，不得为空。
  * @param[out] pxLatest 接收命令副本，不得为空。
  * @note 服务器命令保持 FIFO 所有权，不会覆盖已接受的运动事务。
  */
static void prvFoldServerCommands(const Coffee3Command_t *pxFirst,
	Coffee3Command_t *pxLatest)
{
	if ((pxFirst == NULL) || (pxLatest == NULL)) {
		return;
	}
	/* 服务器命令保持 FIFO 所有权，已接受动作不能被后续手动请求静默替换。 */
	*pxLatest = *pxFirst;
}

/*-----------------------------------------------------------*/
static void prvDisconnectRobot(TcpClientSession_t *pxSession,
	int32_t lReason);
static void prvSetRobotReady(uint8_t ucReady);

static volatile uint8_t s_ucRobotShutdownRequested; /*!< 非零表示外部已请求任务关闭 TCP。 */
static volatile uint8_t s_ucRobotShutdownComplete; /*!< 非零表示任务已实际关闭 TCP 通道。 */

/**
  * @brief 请求机器人任务异步关闭当前 TCP 通道。
  * @note 返回时套接字可能仍未关闭，应轮询完成接口确认。
  */
void vCoffee3RobotTcpRequestShutdown(void)
{
	s_ucRobotShutdownComplete = 0U;
	s_ucRobotShutdownRequested = 1U;
}

/**
  * @brief 查询机器人 TCP 通道是否已由所有者任务关闭。
  * @retval 1 通道已实际关闭。
  * @retval 0 尚未请求关闭或任务仍在处理关闭请求。
  */
uint8_t ucCoffee3RobotTcpShutdownComplete(void)
{
	return s_ucRobotShutdownComplete;
}

static const uint32_t s_aulCoffee3RobotRetryDelayMs[] = { /*!< 连续建连失败的退避阶梯，单位为毫秒。 */
	1000U, 2000U, 5000U, 10000U, COFFEE3_ROBOT_RETRY_MAX_MS
};

static const TcpClientSessionConfig_t s_xCoffee3RobotSessionConfig = { /*!< TCP 会话回调、退避和探测配置。 */
	prvRobotSessionNetworkReady,
	prvRobotSessionProbe,
	prvRobotSessionEvent,
	s_aulCoffee3RobotRetryDelayMs,
	(uint8_t)(sizeof(s_aulCoffee3RobotRetryDelayMs) /
		sizeof(s_aulCoffee3RobotRetryDelayMs[0])),
	COFFEE3_ROBOT_IO_TIMEOUT_MS
};

static const uint8_t s_aucCoffee3RobotIp[4] = { /*!< 配置中的机器人 IPv4 地址四个字节。 */
	COFFEE3_ROBOT_IP_0, COFFEE3_ROBOT_IP_1,
	COFFEE3_ROBOT_IP_2, COFFEE3_ROBOT_IP_3
};

/*-----------------------------------------------------------*/
/**
  * @brief 初始化机器人状态、手动动作槽和静态命令队列。
  * @retval pdPASS 队列创建并完成设备路由注册。
  * @retval pdFAIL 静态队列创建失败。
  */
BaseType_t xCoffee3RobotTcpInitialize(void)
{
	s_ucFreshReadyReset = 0U;
	s_ucConnectionStartupPending = 0U;
	prvResetRobotLinkHealth();
	memset(&g_xCoffee3RobotTcpStatus, 0,
		sizeof(g_xCoffee3RobotTcpStatus));
	memset(&g_xCoffee3RobotData, 0, sizeof(g_xCoffee3RobotData));
	s_ucManualMotionPending = 0U;
	memset(&s_xManualMotionPending, 0, sizeof(s_xManualMotionPending));
	{
		Coffee3RobotTransaction_t xEmptyTransaction; /*!< 用于原子赋值的全零事务快照。 */
		memset(&xEmptyTransaction, 0, sizeof(xEmptyTransaction));
		s_xLastTransaction = xEmptyTransaction;
	}
	s_xRobotQueue = xQueueCreateStatic(COFFEE3_COMMAND_QUEUE_LENGTH,
		sizeof(Coffee3Command_t), s_aucRobotQueueStorage,
		&s_xRobotQueueStorage);
	if (s_xRobotQueue == NULL) {
		return pdFAIL;
	}
	vCoffee3DeviceRegisterRoute(0U, s_xRobotQueue);
	return pdPASS;
}

/*-----------------------------------------------------------*/
/**
  * @brief 将服务器调试动作提交到单个可覆盖的延后执行槽。
  * @param[in,out] pxCommand 调试动作；成功时写入手动预留标志。
  * @retval pdPASS 已替换待执行动作或成功取得工作流预留位并提交。
  * @retval pdFAIL 参数不符、槽位正在预留或工作流拒绝预留。
  */
BaseType_t xCoffee3RobotTcpSubmitManualMotion(Coffee3Command_t *pxCommand)
{
	uint16_t usPreviousAction; /*!< 被最新调试动作替换的旧动作编号。 */

	if ((pxCommand == NULL) ||
		(pxCommand->ucDeviceId != (uint8_t)COFFEE3_DEVICE_ROBOT) ||
		(pxCommand->ucSource != (uint8_t)COFFEE3_COMMAND_SOURCE_SERVER) ||
		((pxCommand->ucFlags & COFFEE3_COMMAND_FLAG_DEBUG) == 0U)) {
		return pdFAIL;
	}
	usPreviousAction = 0U;
	/* 手动调试槽保存最新值而非 FIFO；替换时沿用已有工作流预留位。 */
	taskENTER_CRITICAL();
	if (s_ucManualMotionPending == 1U) {
		usPreviousAction = s_xManualMotionPending.usAction;
		pxCommand->ucFlags |= COFFEE3_COMMAND_FLAG_MANUAL_RESERVED;
		s_xManualMotionPending = *pxCommand;
		taskEXIT_CRITICAL();
		(void)xCoffee3LogPrintfOrder(COFFEE3_LOG_LEVEL_INFO,
			COFFEE3_LOG_SOURCE_SERVER, COFFEE3_LOG_ORDER_DEBUG,
			"Debug pending replaced: device=Robot previous=%s next=%s",
			prvRobotActionName(usPreviousAction),
			prvRobotActionName(pxCommand->usAction));
		return pdPASS;
	}
	if (s_ucManualMotionPending != 0U) {
		taskEXIT_CRITICAL();
		return pdFAIL;
	}
	s_ucManualMotionPending = 2U;
	taskEXIT_CRITICAL();
	if (xCoffee3WorkflowAcquireDeferredManual() != pdPASS) {
		taskENTER_CRITICAL();
		s_ucManualMotionPending = 0U;
		taskEXIT_CRITICAL();
		return pdFAIL;
	}
	pxCommand->ucFlags |= COFFEE3_COMMAND_FLAG_MANUAL_RESERVED;
	taskENTER_CRITICAL();
	s_xManualMotionPending = *pxCommand;
	s_ucManualMotionPending = 1U;
	taskEXIT_CRITICAL();
	return pdPASS;
}

/*-----------------------------------------------------------*/
/**
  * @brief 在工作流允许时原子取出一个待执行手动动作。
  * @param[out] pxCommand 接收动作的缓冲区，不得为空。
  * @retval 1 已取出动作并释放手动槽。
  * @retval 0 无待执行动作、参数为空或工作流暂不允许派发。
  */
static uint8_t prvTakePendingManualMotion(Coffee3Command_t *pxCommand)
{
	if ((pxCommand == NULL) ||
		(ucCoffee3WorkflowManualDispatchAllowed() == 0U)) {
		return 0U;
	}
	taskENTER_CRITICAL();
	if (s_ucManualMotionPending != 1U) {
		taskEXIT_CRITICAL();
		return 0U;
	}
	*pxCommand = s_xManualMotionPending;
	memset(&s_xManualMotionPending, 0, sizeof(s_xManualMotionPending));
	s_ucManualMotionPending = 0U;
	taskEXIT_CRITICAL();
	return 1U;
}

/*-----------------------------------------------------------*/
/**
  * @brief 记录一次成功连接的机器人端点和尝试次数。
  * @param[in] pucIpv4 机器人 IPv4 地址四个字节。
  * @param[in] usPort 机器人 TCP 端口。
  * @param[in] ulAttempt 本轮累计连接尝试次数。
  */
static void prvLogRobotConnected(const uint8_t pucIpv4[4], uint16_t usPort,
	uint32_t ulAttempt)
{
	static const char acPrefix[] = "ROBOT_TCP_CONNECTED peer="; /*!< 端点事件固定前缀。 */
	char acEvent[COFFEE3_ROBOT_CONNECTION_EVENT_LENGTH]; /*!< 完整连接事件的栈缓冲区。 */
	uint16_t usPrefixLength; /*!< 固定前缀长度，单位为字节。 */
	const char *pcEvent; /*!< 最终交给日志系统的事件文本。 */

	usPrefixLength = (uint16_t)(sizeof(acPrefix) - 1U);
	memcpy(acEvent, acPrefix, usPrefixLength);
	if (ucTransportTcpFormatIpv4Endpoint(pucIpv4, usPort,
		&acEvent[usPrefixLength],
		(uint16_t)(sizeof(acEvent) - usPrefixLength)) != 0U) {
		pcEvent = acEvent;
	} else {
		pcEvent = "ROBOT_TCP_CONNECTED";
	}
	(void)xCoffee3LogWriteField(COFFEE3_LOG_LEVEL_INFO,
		COFFEE3_LOG_SOURCE_ROBOT, pcEvent, 0, "attempt",
		(int32_t)ulAttempt);
}

/*-----------------------------------------------------------*/
/**
  * @brief 独占管理机器人 TCP 会话、启动检查、命令和动作恢复。
  * @param[in] pvArgument 未使用的任务参数。
  * @note 命令写入一旦尝试，重连后只对账，不会重发结果不明的运动。
  */
void vCoffee3RobotTcpTask(void *pvArgument)
{

	/* 本任务是机器人 TCP、启动、恢复和命令执行的唯一所有者。 */
	/* ==================== 通信资源对象 (底层与协议层) ==================== */
	TransportChannel_t xChannel;          /* 传输层通道对象：抽象了底层的 TCP Socket 收发操作 */
	TransportTcpContext_t xTransport;     /* TCP 上下文：存储 TCP 特有的运行时状态（Socket句柄、连接状态等） */
	TransportTcpConfig_t xConfig;         /* TCP 配置参数：调用前由本任务填充（目标IP、端口、超时等），作为创建通道的输入 */
	ModbusPort_t xPort;                   /* Modbus 协议端口：将 xChannel 包装为标准 Modbus 协议接口，供业务层调用 */
	TcpClientSession_t xSession;          /* 客户端会话管理器：负责 TCP 建连、断开与退避重连；周期健康刷新由本任务执行。 */
	Coffee3RobotSessionContext_t xSessionContext; /* 自定义上下文：将 xPort 和 xTransport 的指针打包，供会话层回调时逆向寻找通信对象 */

	/* ==================== 业务数据对象 (指令与事务) ==================== */
	Coffee3Command_t xCommand;            /* 当前指令：存放从消息队列(s_xRobotQueue)最新接收到的业务指令 */
	Coffee3Command_t xDeferredCommand;    /* 暂存指令：当网络未就绪但收到指令时，先存于此，待网络恢复后优先执行 */
	Coffee3RobotTransaction_t xTransaction; /* 事务状态机：记录当前正在执行的指令进度、恢复状态、超时时间等，跨时钟周期跟踪动作 */

	/* ==================== 返回值与结果枚举 ==================== */
	TransportResult_e xTransportResult;    /* 传输层结果：记录本任务创建 TCP 通道的结果。 */
	ModbusPortResult_e xResult;           /* Modbus 层结果：捕获协议交互（读写线圈、对账、启动）的执行结果 */
	Coffee3RobotStartupOutcome_e xStartupOutcome; /* 启动结果：记录机器人启动业务判定 OK 或 NOT_READY；通信错误由 xResult 表示。 */

	/* ==================== 定时器与计数器 ==================== */
	TickType_t xNextStartupRetryTick;      /* 下次允许执行启动检查的绝对节拍。 */
	TickType_t xNextHealthTick;           /* 下次健康刷新节拍：到期读取机器人输入与控制线圈快照。 */
	uint32_t ulStartupFailures;            /* 当前连接内连续启动失败次数。 */

	/* ==================== 状态机与流程控制标志 ==================== */
	uint8_t ucCreated;                    /* 资源就绪标志：底层通信积木(xChannel/xPort等)是否成功初始化。为0则任务空转 */
	uint8_t ucSessionReady;               /* 会话处理入口已开放，不等同于点位动作严格就绪。 */
	uint8_t ucDone;                       /* 对账完成标志：在恢复流程中，标识之前未完成的动作是否已在机械臂侧执行完毕 */
	uint8_t ucWorkflowTransaction;        /* 事务来源标志：当前指令是否来自 Workflow。Workflow 指令通常需要更严格的恢复对账 */
	uint8_t ucReconciledSession;          /* 已对账标志：防止在单次网络恢复期间，对同一个未完成的事务进行重复对账操作 */
	uint8_t ucRecoveryWaitingLogged;       /* 恢复等待日志标志：确保在等待恢复期间，同一事务的“正在等待”日志只输出一次，防止刷屏 */
	uint8_t ucDeferredCommand;            /* 暂存有效标志：为1时表示 xDeferredCommand 里有指令，需在网络恢复后优先处理 */
	uint8_t ucSessionCommandPending;      /* 待执行命令标志：网络刚恢复且有暂存指令时置1，指示流程立即下发该指令 */
	uint8_t ucWarmAttachLogged;           /* 热挂接日志标志：确保“机械臂热挂接成功”的日志只输出一次，直到会话断开重置 */
	uint8_t ucActionTimedOut;             /* 准备阶段重试耗尽标志，不代表运动等待超时。 */
	uint8_t ucLinkProbeAttempted;         /* 链路探测已试标志：在判断链路是否彻底丢失时，标识是否已主动发过探测包 */
	uint8_t ucLinkConfirmed;              /* 链路断开确认标志：由链路探测函数填充，为1时表明网络确实断了，需断开连接并降级 */
	
	BaseType_t xCommandReceived;          /* 命令取得标志：pdPASS 表示 xCommand 已从队列、手动槽、延后槽或预取位置取得。 */


	(void)pvArgument;
	(void)xCoffee3LogWrite(COFFEE3_LOG_LEVEL_INFO,
		COFFEE3_LOG_SOURCE_ROBOT, "TASK_RUNNING:C3Robot", 0);
	vAppTaskManagerWaitNetworkStackReady();
	/* 第一步：准备远端地址、端口和连接超时配置。 */
	memset(&xConfig, 0, sizeof(xConfig));
	xConfig.xMode = TRANSPORT_TCP_MODE_CLIENT;
	memcpy(xConfig.aucRemoteIp, s_aucCoffee3RobotIp,
		sizeof(s_aucCoffee3RobotIp));
	xConfig.usPort = COFFEE3_ROBOT_PORT;
	xConfig.ulConnectTimeoutMs = COFFEE3_ROBOT_CONNECT_TIMEOUT_MS;
	xConfig.ulIoTimeoutMs = COFFEE3_ROBOT_IO_TIMEOUT_MS;
	/* 第二步：创建 TCP 通道，再包装为 Modbus 客户端端口。 */
	xTransportResult = xTransportTcpCreate(&xChannel, &xTransport,
		"coffee3_robot_tcp", &xConfig);
	ucCreated = 0U;
	if (xTransportResult == TRANSPORT_RESULT_OK) {
		xTransport.usReserveLocalPort = prvReserveRobotPort;
		xResult = xModbusPortClientInit(&xPort, &xChannel,
			MODBUS_PORT_TRANSPORT_TCP,
			COFFEE3_ROBOT_IO_TIMEOUT_MS);
		ucCreated = (xResult == MODBUS_PORT_RESULT_OK) ? 1U : 0U;
	}
	memset(&xSessionContext, 0, sizeof(xSessionContext));
	/* 第三步：把端口与传输上下文交给会话回调，并初始化会话。 */
	if (ucCreated != 0U) {
		xSessionContext.pxPort = &xPort; 			 /* 保存 Modbus 端口地址。 */
		xSessionContext.pxTransport = &xTransport;  /* 保存 TCP 上下文地址。 */
		if (xTcpClientSessionInit(&xSession,		/* 用通道和回调上下文初始化会话。 */
			&s_xCoffee3RobotSessionConfig, &xChannel,
			&xSessionContext) != pdPASS) {
			ucCreated = 0U;
		}
	}
	(void)xCoffee3LogWriteField(
		(ucCreated != 0U) ? COFFEE3_LOG_LEVEL_INFO :
			COFFEE3_LOG_LEVEL_ERROR,
		COFFEE3_LOG_SOURCE_ROBOT, "ROBOT_ENDPOINT_INIT",
		(ucCreated != 0U) ? 0 : -1, "init_code",
		(ucCreated != 0U) ? 0 : (int32_t)xTransportResult);
	if (ucCreated != 0U) {
#if COFFEE3_ROBOT_PROTOCOL_VARIANT == COFFEE3_ROBOT_PROTOCOL_2
		(void)xCoffee3LogWriteField(COFFEE3_LOG_LEVEL_INFO,
			COFFEE3_LOG_SOURCE_ROBOT, "DEVICE_PROTOCOL:DOBOT_P2", 0,
			"driver", (int32_t)COFFEE3_ROBOT_LOG_DRIVER_ID);
#elif COFFEE3_ROBOT_PROTOCOL_VARIANT == COFFEE3_ROBOT_PROTOCOL_3
		(void)xCoffee3LogWriteField(COFFEE3_LOG_LEVEL_INFO,
			COFFEE3_LOG_SOURCE_ROBOT, "DEVICE_PROTOCOL:DOBOT_P3", 0,
			"driver", (int32_t)COFFEE3_ROBOT_LOG_DRIVER_ID);
#else
		(void)xCoffee3LogWriteField(COFFEE3_LOG_LEVEL_INFO,
			COFFEE3_LOG_SOURCE_ROBOT, "DEVICE_PROTOCOL:DOBOT_P1", 0,
			"driver", (int32_t)COFFEE3_ROBOT_LOG_DRIVER_ID);
#endif
		(void)xCoffee3LogWriteField(COFFEE3_LOG_LEVEL_INFO,
			COFFEE3_LOG_SOURCE_ROBOT, "DEVICE_LINK:ROBOT_TCP", 0,
			"unit", (int32_t)COFFEE3_ROBOT_UNIT_ID);
	}
	xNextStartupRetryTick = 0U;
	xNextHealthTick = 0U;
	ulStartupFailures = 0U;
	ucSessionReady = 0U;
	ucReconciledSession = 0U;
	ucRecoveryWaitingLogged = 0U;
	ucWarmAttachLogged = 0U;
	memset(&xTransaction, 0, sizeof(xTransaction));
	memset(&xDeferredCommand, 0, sizeof(xDeferredCommand));
	ucDeferredCommand = 0U;
	prvSetRobotReady(0U);
	vCoffee3DeviceSetRecovering(COFFEE3_DEVICE_ROBOT, 0U);
	vTaskDelay(pdMS_TO_TICKS(20000U)); /* 等待服务器任务完成启动资源回收。 */

	/* 第四步：循环处理关闭请求、连接状态、恢复对账、命令和健康刷新。 */
	for (;;) {
		/* 关闭请求由本任务异步落实，完成标志在通道实际关闭后置位。 */
		if (s_ucRobotShutdownRequested != 0U) {
			(void)xTransportClose(&xChannel);
			prvSetRobotReady(0U);
			s_ucRobotShutdownRequested = 0U;
			s_ucRobotShutdownComplete = 1U;
			(void)xCoffee3LogWrite(COFFEE3_LOG_LEVEL_INFO,
				COFFEE3_LOG_SOURCE_ROBOT, "ROBOT_TCP_CLOSED_FOR_RESET", 0);
			for (;;) {
				vTaskDelay(pdMS_TO_TICKS(20U));
			}
		}
		xCommandReceived = pdFAIL;
		ucSessionCommandPending = 0U;
		/* 逾期只用于诊断；订单动作只能由上位业务取消，不能按本地期限放弃。 */
		if ((xTransaction.ucActive != 0U) &&
			(xTransaction.ucCommandWriteAttempted != 0U) &&
			((int32_t)(xTaskGetTickCount() -
				((xTransaction.ucAccepted != 0U) ?
				 xTransaction.xMotionDeadline : xTransaction.xAcceptDeadline)) >= 0)) {
			prvMarkActionOverdue(&xTransaction,
				(xTransaction.ucAccepted != 0U) ? "motion" : "accept");
		}
		if ((xTransaction.ucActive != 0U) &&
			(xTransaction.xRecoveryStart != 0U) &&
			((xTaskGetTickCount() - xTransaction.xRecoveryStart) >=
				pdMS_TO_TICKS(COFFEE3_ROBOT_RECOVERY_TIMEOUT_MS))) {
			prvMarkActionOverdue(&xTransaction, "recovery");
		}
		if ((xTransaction.ucActive != 0U) &&
			(xTransaction.xRecoveryStart != 0U)) {
			if (ucCoffee3CommandIsCanceled(
				&xTransaction.xCommand) != 0U) {
				vCoffee3DeviceSetRecovering(COFFEE3_DEVICE_ROBOT, 0U);
				vCoffee3DeviceCommandCompleted(&xTransaction.xCommand,
					COFFEE3_COMMAND_RESULT_CANCELED, 0U);
				prvArchiveAndResetTransaction(&xTransaction,
					COFFEE3_COMMAND_RESULT_CANCELED);
				ucRecoveryWaitingLogged = 0U;
			}
		}
		if (ucCreated == 0U) {
			vTaskDelay(pdMS_TO_TICKS(100U));
			continue;
		}
		/* 推进连接/退避会话，再处理 TCP 在线即可执行的本体控制。 */
		vTcpClientSessionProcess(&xSession);
		/* 本体控制只要求 TCP 在线，可在程序就绪探测或事务恢复完成前受理。 */
		if ((g_xCoffee3RobotTcpStatus.ucConnected != 0U) &&
			(xQueuePeek(s_xRobotQueue, &xCommand, 0U) == pdPASS) &&
			(xCommand.usAction >= COFFEE3_ACTION_ROBOT_START) &&
			(xCommand.usAction <= COFFEE3_ACTION_ROBOT_MANUAL_MODE) &&
			(xQueueReceive(s_xRobotQueue, &xCommand, 0U) == pdPASS)) {
			if (xTransaction.ucActive == 0U) {
				vCoffee3DeviceCommandStarted(&xCommand);
			}
			xResult = prvExecute(&xPort, &xCommand, &xTransaction, &ucActionTimedOut);
			/* 本体控制不得覆盖仍在执行的运动事务标识和阶段。 */
			if (xTransaction.ucActive == 0U) {
				vCoffee3DeviceCommandCompleted(&xCommand, (int32_t)xResult,
					(xResult == MODBUS_PORT_RESULT_TIMEOUT) ? 1U : 0U);
			} else if ((xCommand.ucFlags & COFFEE3_COMMAND_FLAG_MANUAL_RESERVED) != 0U) {
				vCoffee3WorkflowReleaseManual();
			}
			(void)xCoffee3LogWriteFieldOrder(COFFEE3_LOG_LEVEL_INFO,
				COFFEE3_LOG_SOURCE_ROBOT, (uint16_t)xCommand.ulOrderId,
				"Robot body control completed", (int32_t)xResult,
				"action", (int32_t)xCommand.usAction);
		}
		if (ucTcpClientSessionIsOnline(&xSession) == 0U) {
			ucSessionReady = 0U;
			ucReconciledSession = 0U;
			ucWarmAttachLogged = 0U;
			xNextStartupRetryTick = 0U;
			ulStartupFailures = 0U;
			vTaskDelay(pdMS_TO_TICKS(50U));
			continue;
		}

		/* 新连接先对账在途动作；没有结果不明运动时才运行启动检查。 */
		if ((ucSessionReady == 0U) || (s_ucConnectionStartupPending != 0U)) {
			/* 重连后必须先核对在途动作，再执行本体 START 或清理序列，
			 * 以免破坏机器人仍在运动时的完成证据。 */
			if ((xTransaction.ucActive != 0U) &&
				(ucReconciledSession == 0U)) {
				(void)xCoffee3LogWriteField(COFFEE3_LOG_LEVEL_INFO,
					COFFEE3_LOG_SOURCE_ROBOT,
					"ROBOT_RECOVERY_RECONCILE", 0,
					"command_id", (int32_t)
						xTransaction.xCommand.ulCommandId);
				xResult = prvReconcile(&xPort, &xTransaction, &ucDone);
				if (xResult != MODBUS_PORT_RESULT_OK) {
					if (prvRobotLinkFailureConfirmed(&xPort, xResult,
						&ucLinkProbeAttempted) != 0U) {
						prvDisconnectRobot(&xSession, (int32_t)xResult);
					}
					vTaskDelay(pdMS_TO_TICKS(50U));
					continue;
				}
				ucReconciledSession = 1U;
				if (ucDone != 0U) {
					(void)xCoffee3LogWriteField(COFFEE3_LOG_LEVEL_INFO,
						COFFEE3_LOG_SOURCE_ROBOT,
						"ROBOT_RECOVERY_COMPLETED", 0,
						"command_id", (int32_t)
							xTransaction.xCommand.ulCommandId);
					vCoffee3DeviceSetRecovering(COFFEE3_DEVICE_ROBOT, 0U);
					vCoffee3DeviceCommandCompleted(&xTransaction.xCommand,
						0, 0U);
					prvArchiveAndResetTransaction(&xTransaction, 0);
				} else {
					xTransaction.xRecoveryStart = 0U;
					xTransaction.ucRecovering = 0U;
					vCoffee3DeviceSetRecovering(COFFEE3_DEVICE_ROBOT, 0U);
					vCoffee3DeviceSetRobotPhase(xTransaction.xPhase);
					/* 当前事务终结前持续拥有本次连接的恢复处理权。 */
					s_ucConnectionStartupPending = 0U;
					ucSessionReady = 1U;
					prvSetRobotReady(prvRobotStrictReady());
				}
			}
			if ((s_ucConnectionStartupPending != 0U) &&
				(xTransaction.ucActive == 0U)) {
				if ((ulStartupFailures != 0U) &&
					((int32_t)(xTaskGetTickCount() -
					xNextStartupRetryTick) < 0)) {
					vTaskDelay(pdMS_TO_TICKS(50U));
					continue;
				}
				xResult = prvRefresh(&xPort,
					COFFEE3_ROBOT_IO_TIMEOUT_MS);
				if ((xResult == MODBUS_PORT_RESULT_OK) &&
					(prvRobotStrictReady() != 0U)) {
					s_ucConnectionStartupPending = 0U;
					ulStartupFailures = 0U;
					xNextStartupRetryTick = 0U;
					(void)xCoffee3LogWrite(COFFEE3_LOG_LEVEL_INFO,
						COFFEE3_LOG_SOURCE_ROBOT,
						"ROBOT_STARTUP_FRESH_READY", 0);
				} else {
					(void)xCoffee3LogWriteField(COFFEE3_LOG_LEVEL_INFO,
						COFFEE3_LOG_SOURCE_ROBOT,
						"ROBOT_STARTUP_ATTEMPT", 0,
						"attempt", (int32_t)(ulStartupFailures + 1U));
					xStartupOutcome = COFFEE3_ROBOT_STARTUP_OK;
					if (xResult == MODBUS_PORT_RESULT_OK) {
						xResult = prvStartup(&xPort,
							xTransaction.ucActive, &xStartupOutcome);
					}
					if ((xResult == MODBUS_PORT_RESULT_OK) &&
						(xStartupOutcome == COFFEE3_ROBOT_STARTUP_OK) &&
						(prvRobotStrictReady() != 0U)) {
						s_ucConnectionStartupPending = 0U;
						ulStartupFailures = 0U;
						xNextStartupRetryTick = 0U;
					} else {
						ulStartupFailures++;
						xNextStartupRetryTick = xTaskGetTickCount() +
							pdMS_TO_TICKS(COFFEE3_ROBOT_STARTUP_RETRY_MS);
						if (prvRobotLinkFailureConfirmed(&xPort, xResult,
							&ucLinkProbeAttempted) != 0U) {
							prvDisconnectRobot(&xSession, (int32_t)xResult);
						} else {
							vCoffee3DeviceSetOnline(COFFEE3_DEVICE_ROBOT, 1U);
							vCoffee3DeviceSetReady(COFFEE3_DEVICE_ROBOT, 0U);
							prvSetRobotReady(0U);
							(void)xCoffee3LogWriteField(COFFEE3_LOG_LEVEL_INFO,
								COFFEE3_LOG_SOURCE_ROBOT,
								"ROBOT_STARTUP_RETRY_SCHEDULED", 0,
								"delay_ms", COFFEE3_ROBOT_STARTUP_RETRY_MS);
						}
						vTaskDelay(pdMS_TO_TICKS(50U));
						continue;
					}
				}
			}
			if ((xTransaction.ucActive == 0U) &&
				(ucDeferredCommand == 0U) &&
				(xQueuePeek(s_xRobotQueue, &xCommand, 0U) == pdPASS) &&
				(prvRobotBasicAction(xCommand.usAction) != 0U) &&
				(xQueueReceive(s_xRobotQueue, &xCommand, 0U) == pdPASS)) {
				if (xCommand.ucSource ==
					(uint8_t)COFFEE3_COMMAND_SOURCE_SERVER) {
					prvFoldServerCommands(&xCommand, &xCommand);
				}
				xDeferredCommand = xCommand;
				ucDeferredCommand = 1U;
				ucSessionCommandPending = 1U;
			}
			/* 活动运动发布最终结果前保持队列所有权，手动运动留在专用槽中。 */
			if ((xTransaction.ucActive != 0U) &&
				(ucReconciledSession == 0U)) {
				if (ucReconciledSession == 0U) {
					(void)xCoffee3LogWriteField(COFFEE3_LOG_LEVEL_INFO,
						COFFEE3_LOG_SOURCE_ROBOT,
						"ROBOT_RECOVERY_RECONCILE", 0,
						"command_id", (int32_t)
							xTransaction.xCommand.ulCommandId);
				}
				xResult = prvReconcile(&xPort, &xTransaction,
					&ucDone);
				if (xResult != MODBUS_PORT_RESULT_OK) {
					if (prvRobotLinkFailureConfirmed(&xPort, xResult,
						&ucLinkProbeAttempted) != 0U) {
						prvDisconnectRobot(&xSession, (int32_t)xResult);
					}
					continue;
				}
				ucReconciledSession = 1U;
				if ((xTransaction.ucActive != 0U) &&
					(xTransaction.xRecoveryStart != 0U) &&
					(ucDone == 0U)) {
					xTransaction.xRecoveryStart = 0U;
					xTransaction.ucRecovering = 0U;
					vCoffee3DeviceSetRecovering(
						COFFEE3_DEVICE_ROBOT, 0U);
					vCoffee3DeviceSetRobotPhase(
						xTransaction.xPhase);
					ucSessionReady = 1U;
					prvSetRobotReady(prvRobotStrictReady());
				}
				if (ucDone != 0U) {
					(void)xCoffee3LogWriteField(COFFEE3_LOG_LEVEL_INFO,
						COFFEE3_LOG_SOURCE_ROBOT,
						"ROBOT_RECOVERY_COMPLETED", 0,
						"command_id", (int32_t)
							xTransaction.xCommand.ulCommandId);
					vCoffee3DeviceSetRecovering(COFFEE3_DEVICE_ROBOT, 0U);
					vCoffee3DeviceCommandCompleted(&xTransaction.xCommand,
						0, 0U);
					prvArchiveAndResetTransaction(&xTransaction, 0);
				}
			}
			if (xTransaction.ucActive != 0U) {
				if ((ucRecoveryWaitingLogged == 0U) &&
					(xTransaction.xRecoveryStart != 0U)) {
					(void)xCoffee3LogWriteField(COFFEE3_LOG_LEVEL_INFO,
						COFFEE3_LOG_SOURCE_ROBOT,
						"ROBOT_RECOVERY_WAITING", 0,
						"command_id", (int32_t)
							xTransaction.xCommand.ulCommandId);
					ucRecoveryWaitingLogged = 1U;
				}
				if (ucCoffee3CommandIsCanceled(
					&xTransaction.xCommand) != 0U) {
					vCoffee3DeviceSetRecovering(
						COFFEE3_DEVICE_ROBOT, 0U);
					vCoffee3DeviceCommandCompleted(
					&xTransaction.xCommand,
					COFFEE3_COMMAND_RESULT_CANCELED, 0U);
					prvArchiveAndResetTransaction(&xTransaction,
						COFFEE3_COMMAND_RESULT_CANCELED);
					ucRecoveryWaitingLogged = 0U;
				}
				if (xTransaction.ucActive != 0U) {
					vTaskDelay(pdMS_TO_TICKS(
						COFFEE3_ROBOT_ACTION_POLL_MS));
					continue;
				}
			}
			if ((xTransaction.ucActive == 0U) &&
				(prvRobotOperational() != 0U)) {
				ucSessionReady = 1U;
				if (ucWarmAttachLogged == 0U) {
					(void)xCoffee3LogWriteField(COFFEE3_LOG_LEVEL_INFO,
						COFFEE3_LOG_SOURCE_ROBOT, "ROBOT_WARM_ATTACH", 0,
						"state_mask", (int32_t)prvRobotStartupStateMask());
					ucWarmAttachLogged = 1U;
				}
				prvSetRobotReady(prvRobotStrictReady());
			}
			/* 手动 STOP 后即使本体不再运行，会话仍可处理；点位运动继续受
			 * 严格就绪条件限制。 */
			ucSessionReady = 1U;
			prvSetRobotReady(prvRobotStrictReady());
			if (prvRobotOperational() != 0U) {
				ucSessionReady = 1U;
				prvSetRobotReady(prvRobotStrictReady());
				(void)xCoffee3LogWrite(COFFEE3_LOG_LEVEL_INFO,
					COFFEE3_LOG_SOURCE_ROBOT, "ROBOT_SERVICE_READY", 0);
				g_xCoffee3RobotTcpStatus.ulConsecutiveFailures = 0U;
				g_xCoffee3RobotTcpStatus.ulNextRetryDelayMs = 0U;
			}
			if (ucSessionCommandPending != 0U) {
				ucSessionReady = 1U;
			}
			continue;
		}
		/* 调试运动允许被最新工程请求替换，订单拥有的运动不会在此被取代。 */
		/* 活动事务按固定轮询周期推进，不因接受或运动期限到达而重发。 */
		if ((xTransaction.ucActive != 0U) &&
			(prvRobotDebugMotion(&xTransaction.xCommand) != 0U) &&
			(prvTakePendingManualMotion(&xCommand) != 0U)) {
			(void)xCoffee3LogPrintfOrder(COFFEE3_LOG_LEVEL_WARNING,
				COFFEE3_LOG_SOURCE_ROBOT, COFFEE3_LOG_ORDER_DEBUG,
				"Debug interrupted: device=Robot previous=%s next=%s",
				prvRobotActionName(xTransaction.xCommand.usAction),
				prvRobotActionName(xCommand.usAction));
			vCoffee3DeviceCommandCompleted(&xTransaction.xCommand,
				COFFEE3_COMMAND_RESULT_SUPERSEDED, 0U);
			prvArchiveAndResetTransaction(&xTransaction,
				COFFEE3_COMMAND_RESULT_SUPERSEDED);
			xCommandReceived = pdPASS;
			ucRecoveryWaitingLogged = 0U;
		}
		if ((xTransaction.ucActive != 0U) &&
			(xTransaction.xPhase != COFFEE3_ROBOT_PHASE_IDLE) &&
			(xTransaction.xRecoveryStart == 0U)) {
			if ((int32_t)(xTaskGetTickCount() -
				xTransaction.xNextPollTick) >= 0) {
				xTransaction.xNextPollTick = xTaskGetTickCount() +
					pdMS_TO_TICKS(COFFEE3_ROBOT_ACTION_POLL_MS);
				xResult = prvAdvanceAction(&xPort, &xTransaction,
					&ucActionTimedOut);
				if (xResult == MODBUS_PORT_RESULT_OK) {
					vCoffee3DeviceCommandCompleted(
						&xTransaction.xCommand, 0, 0U);
					prvArchiveAndResetTransaction(&xTransaction, 0);
				} else if (xResult == MODBUS_PORT_RESULT_CANCELED) {
					vCoffee3DeviceCommandCompleted(
						&xTransaction.xCommand,
						COFFEE3_COMMAND_RESULT_CANCELED, 0U);
					prvArchiveAndResetTransaction(&xTransaction,
						COFFEE3_COMMAND_RESULT_CANCELED);
				} else if (ucActionTimedOut != 0U) {
					vCoffee3DeviceCommandCompleted(
						&xTransaction.xCommand,
						MODBUS_PORT_RESULT_TIMEOUT, 1U);
					prvArchiveAndResetTransaction(&xTransaction,
						MODBUS_PORT_RESULT_TIMEOUT);
				} else if (xResult != MODBUS_PORT_RESULT_BUSY) {
					ucLinkConfirmed = prvRobotLinkFailureConfirmed(
						&xPort, xResult, &ucLinkProbeAttempted);
					if (ucLinkConfirmed != 0U) {
						xTransaction.xRecoveryStart =
							xTaskGetTickCount();
						xTransaction.ucRecovering = 1U;
						xTransaction.xPhase =
							COFFEE3_ROBOT_PHASE_RECOVERING;
						vCoffee3DeviceSetRecovering(
							COFFEE3_DEVICE_ROBOT, 1U);
						vCoffee3DeviceSetRobotPhase(
							COFFEE3_ROBOT_PHASE_RECOVERING);
						ucSessionReady = 0U;
						prvDisconnectRobot(&xSession, (int32_t)xResult);
					} else if ((xResult == MODBUS_PORT_RESULT_TIMEOUT) ||
						(xResult == MODBUS_PORT_RESULT_PROTOCOL) ||
						(ucLinkProbeAttempted != 0U)) {
						xTransaction.xRecoveryStart =
							xTaskGetTickCount();
						xTransaction.ucRecovering = 1U;
						xTransaction.xPhase =
							COFFEE3_ROBOT_PHASE_RECOVERING;
						vCoffee3DeviceSetRecovering(
							COFFEE3_DEVICE_ROBOT, 1U);
						vCoffee3DeviceSetRobotPhase(
							COFFEE3_ROBOT_PHASE_RECOVERING);
						ucSessionReady = 0U;
					} else {
						vCoffee3DeviceCommandCompleted(
							&xTransaction.xCommand,
							(int32_t)xResult, 0U);
						prvArchiveAndResetTransaction(&xTransaction,
							(int32_t)xResult);
					}
				}
			}
			if (xTransaction.ucActive != 0U) {
				vTaskDelay(pdMS_TO_TICKS(COFFEE3_ROBOT_ACTION_POLL_MS));
				continue;
			}
		}

		/* 空闲时优先取手动槽，其次取断线期间暂存命令，最后阻塞读队列。 */
		if ((xTransaction.ucActive == 0U) && (ucDeferredCommand == 0U) &&
			(prvTakePendingManualMotion(&xCommand) != 0U)) {
			xCommandReceived = pdPASS;
			(void)xCoffee3LogWriteFieldOrder(COFFEE3_LOG_LEVEL_INFO,
				COFFEE3_LOG_SOURCE_ROBOT, COFFEE3_LOG_ORDER_DEBUG,
				"MANUAL_ROBOT_DISPATCH", 0,
				"action", (int32_t)xCommand.usAction);
		}
		if ((xCommandReceived == pdFAIL) && (ucDeferredCommand != 0U)) {
			xCommand = xDeferredCommand;
			ucDeferredCommand = 0U;
			xCommandReceived = pdPASS;
		}
		if ((xCommandReceived == pdPASS) ||
			(xQueueReceive(s_xRobotQueue, &xCommand,
			pdMS_TO_TICKS(COFFEE3_ROBOT_LOOP_MS)) == pdPASS)) {
			if ((xCommandReceived == pdFAIL) &&
				(xCommand.ucSource ==
					(uint8_t)COFFEE3_COMMAND_SOURCE_SERVER)) {
				prvFoldServerCommands(&xCommand, &xCommand);
			}
			vCoffee3DeviceCommandStarted(&xCommand);
			if (xCommand.usAction == COFFEE3_ACTION_ROBOT_PREPARE_ORDER) {
				xResult = MODBUS_PORT_RESULT_CANCELED;
				if ((xCommand.ucSource == COFFEE3_COMMAND_SOURCE_WORKFLOW) &&
					(ucCoffee3CommandIsCanceled(&xCommand) == 0U)) {
					xResult = prvRefresh(&xPort, COFFEE3_ROBOT_IO_TIMEOUT_MS);
					if ((xResult == MODBUS_PORT_RESULT_OK) &&
						((prvRobotOperational() == 0U) ||
						 (g_xCoffee3RobotData.aucBaseInputs[DOBOT_ROBOT_BODY_STATUS_POWERED] == 0U))) {
						xStartupOutcome = COFFEE3_ROBOT_STARTUP_OK;
						(void)xCoffee3LogPrintfOrder(COFFEE3_LOG_LEVEL_INFO,
							COFFEE3_LOG_SOURCE_ROBOT, (uint16_t)xCommand.ulOrderId,
			"Robot start required: power/enable/run/alarm not ready");
						xResult = prvStartup(&xPort, 0U, &xStartupOutcome);
						if ((xResult == MODBUS_PORT_RESULT_OK) &&
							(xStartupOutcome != COFFEE3_ROBOT_STARTUP_OK)) {
							xResult = MODBUS_PORT_RESULT_NOT_READY;
						}
					}
					if ((xResult == MODBUS_PORT_RESULT_OK) &&
						((prvRobotStrictReady() == 0U) ||
						 (g_xCoffee3RobotData.aucBaseInputs[DOBOT_ROBOT_BODY_STATUS_POWERED] == 0U))) {
						xResult = MODBUS_PORT_RESULT_NOT_READY;
					}
				}
				if (ucCoffee3CommandIsCanceled(&xCommand) != 0U) {
					xResult = MODBUS_PORT_RESULT_CANCELED;
				}
				prvSetRobotReady(prvRobotStrictReady());
				(void)xCoffee3LogPrintfOrder(
					(xResult == MODBUS_PORT_RESULT_OK) ? COFFEE3_LOG_LEVEL_INFO :
					COFFEE3_LOG_LEVEL_WARNING, COFFEE3_LOG_SOURCE_ROBOT,
					(uint16_t)xCommand.ulOrderId,
					"Order robot preparation finished: result=%ld", (long)xResult);
				vCoffee3DeviceCommandCompleted(&xCommand, (int32_t)xResult, 0U);
				continue;
			}
			ucWorkflowTransaction =
				(xCommand.ucSource ==
					(uint8_t)COFFEE3_COMMAND_SOURCE_WORKFLOW) ? 1U : 0U;
			(void)xCoffee3LogPrintfOrder(COFFEE3_LOG_LEVEL_INFO,
				COFFEE3_LOG_SOURCE_ROBOT, (uint16_t)xCommand.ulOrderId,
				"ROBOT_ACTION_START=%s",
				prvRobotActionName(xCommand.usAction));
			ucActionTimedOut = 0U;
			xResult = prvExecute(&xPort, &xCommand,
			&xTransaction, &ucActionTimedOut);
			g_xCoffee3RobotTcpStatus.lLastResult = (int32_t)xResult;
			g_xCoffee3RobotTcpStatus.ulCommandCount++;
			if (xResult != MODBUS_PORT_RESULT_OK) {
				g_xCoffee3RobotTcpStatus.ulErrorCount++;
			}
			ucLinkConfirmed = 0U;
			if ((xResult == MODBUS_PORT_RESULT_BUSY) &&
				(xTransaction.ucActive != 0U)) {
				/* 点位动作由所有者循环后续推进状态机。 */
			} else if (xResult == MODBUS_PORT_RESULT_OK) {
				vCoffee3DeviceCommandCompleted(&xCommand, 0, 0U);
				prvArchiveAndResetTransaction(&xTransaction, 0);
				if ((prvRobotBasicAction(xCommand.usAction) != 0U) &&
					(prvRobotOperational() == 0U)) {
					ucSessionReady = 0U;
				}
			} else if ((ucActionTimedOut != 0U) &&
				(ucWorkflowTransaction != 0U)) {
				if (xTransaction.ucRecovering == 0U) {
					(void)xCoffee3LogWriteFieldOrder(
						COFFEE3_LOG_LEVEL_WARNING,
						COFFEE3_LOG_SOURCE_ROBOT,
						(uint16_t)xCommand.ulOrderId,
						"ROBOT_ACTION_TIMEOUT_RECONCILE",
						MODBUS_PORT_RESULT_TIMEOUT, "command_id",
						(int32_t)xCommand.ulCommandId);
				}
				if (xTransaction.xRecoveryStart == 0U) {
					xTransaction.xRecoveryStart = xTaskGetTickCount();
				}
				xTransaction.ucRecovering = 1U;
				vCoffee3DeviceSetRecovering(COFFEE3_DEVICE_ROBOT, 1U);
				ucSessionReady = 0U;
			} else if ((ucWorkflowTransaction != 0U) ||
				(xTransaction.ucActive != 0U)) {
				ucLinkConfirmed = prvRobotLinkFailureConfirmed(&xPort,
					xResult, &ucLinkProbeAttempted);
				if (ucLinkConfirmed != 0U) {
					if (xTransaction.xRecoveryStart == 0U) {
						xTransaction.xRecoveryStart = xTaskGetTickCount();
					}
					xTransaction.ucRecovering = 1U;
					vCoffee3DeviceSetRecovering(COFFEE3_DEVICE_ROBOT, 1U);
					(void)xCoffee3LogWriteField(
						COFFEE3_LOG_LEVEL_WARNING,
						COFFEE3_LOG_SOURCE_ROBOT,
						"ROBOT_COMMAND_LINK_LOST", (int32_t)xResult,
						"command_id", (int32_t)xCommand.ulCommandId);
					vCoffee3DeviceSetOnline(COFFEE3_DEVICE_ROBOT, 0U);
					vCoffee3DeviceSetReady(COFFEE3_DEVICE_ROBOT, 0U);
					ucSessionReady = 0U;
					prvDisconnectRobot(&xSession, (int32_t)xResult);
				} else if ((xResult == MODBUS_PORT_RESULT_TIMEOUT) ||
					(xResult == MODBUS_PORT_RESULT_PROTOCOL) ||
					(ucLinkProbeAttempted != 0U)) {
					if (xTransaction.xRecoveryStart == 0U) {
						xTransaction.xRecoveryStart = xTaskGetTickCount();
						(void)xCoffee3LogWriteField(
							COFFEE3_LOG_LEVEL_WARNING,
							COFFEE3_LOG_SOURCE_ROBOT,
							"ROBOT_COMMAND_RECONCILE", (int32_t)xResult,
							"command_id", (int32_t)xCommand.ulCommandId);
					}
					xTransaction.ucRecovering = 1U;
					vCoffee3DeviceSetRecovering(COFFEE3_DEVICE_ROBOT, 1U);
					ucSessionReady = 0U;
				} else {
					vCoffee3DeviceCommandCompleted(&xCommand,
						(int32_t)xResult, 0U);
					prvArchiveAndResetTransaction(&xTransaction,
						(int32_t)xResult);
				}
			} else {
				vCoffee3DeviceCommandCompleted(&xCommand,
					(int32_t)xResult,
					(ucActionTimedOut != 0U) ? 1U : 0U);
					prvArchiveAndResetTransaction(&xTransaction,
						(int32_t)xResult);
				if ((prvRobotBasicAction(xCommand.usAction) != 0U) &&
					(prvRobotOperational() == 0U)) {
					ucSessionReady = 0U;
				}
				if (ucActionTimedOut == 0U) {
					ucLinkConfirmed = prvRobotLinkFailureConfirmed(&xPort,
						xResult, &ucLinkProbeAttempted);
				}
				if (ucLinkConfirmed != 0U) {
					vCoffee3DeviceSetOnline(COFFEE3_DEVICE_ROBOT, 0U);
					vCoffee3DeviceSetReady(COFFEE3_DEVICE_ROBOT, 0U);
					ucSessionReady = 0U;
					prvDisconnectRobot(&xSession, (int32_t)xResult);
				}
			}
		/* 无命令时按周期刷新在线和严格就绪状态。 */
		} else if ((int32_t)(xTaskGetTickCount() -
			xNextHealthTick) >= 0) {
			xResult = prvRefresh(&xPort, COFFEE3_ROBOT_IO_TIMEOUT_MS);
			g_xCoffee3RobotTcpStatus.lLastResult = (int32_t)xResult;
			if (xResult == MODBUS_PORT_RESULT_OK) {
				vCoffee3DeviceSetOnline(COFFEE3_DEVICE_ROBOT, 1U);
				vCoffee3DeviceSetReady(COFFEE3_DEVICE_ROBOT,
					prvRobotStrictReady());
				prvSetRobotReady(prvRobotStrictReady());
			} else if (prvRobotLinkFailureConfirmed(&xPort, xResult,
				&ucLinkProbeAttempted) != 0U) {
				vCoffee3DeviceSetOnline(COFFEE3_DEVICE_ROBOT, 0U);
				vCoffee3DeviceSetReady(COFFEE3_DEVICE_ROBOT, 0U);
				prvSetRobotReady(0U);
				ucSessionReady = 0U;
				prvDisconnectRobot(&xSession, (int32_t)xResult);
			}
			xNextHealthTick = xTaskGetTickCount() + pdMS_TO_TICKS(
				COFFEE3_ROBOT_HEALTH_MS);
		}
	}
}

/*-----------------------------------------------------------*/
/**
  * @brief 刷新机器人本体输入和控制线圈快照。
  * @param[in,out] pxPort 已初始化的机器人 Modbus 端口。
  * @param[in] ulTimeoutMs 每次 Modbus 事务超时，单位为毫秒。
  * @retval MODBUS_PORT_RESULT_OK 两段快照均读取成功。
  * @retval 其他值 任一 Modbus 读取失败。
  */
static ModbusPortResult_e prvRefresh(ModbusPort_t *pxPort,
	uint32_t ulTimeoutMs)
{
	/* 每次就绪或动作判断前都读取控制器，避免依据陈旧快照决策。 */
	const DobotRobotProtocolConfig_t *pxProtocol; /*!< 当前协议的地址和数量配置。 */
	ModbusPortResult_e xResult; /*!< 最近一次 Modbus 读取结果。 */
	bool abBaseValues[DOBOT_ROBOT_BASE_INPUT_COUNT]; /*!< 本体离散输入临时快照。 */
	bool abValues[COFFEE3_ROBOT_CONTROL_COIL_COUNT]; /*!< 控制和结果线圈临时快照。 */
	uint8_t ucIndex; /*!< 将布尔快照复制到字节镜像的索引。 */

	pxProtocol = prvCoffee3DobotConfig()->pxProtocol;
	xResult = xModbusPortReadDiscreteInputs(pxPort,
		COFFEE3_ROBOT_UNIT_ID, pxProtocol->usBaseInputStart,
		pxProtocol->usBaseInputCount, abBaseValues, ulTimeoutMs);
	if (xResult == MODBUS_PORT_RESULT_OK) {
		for (ucIndex = 0U;
			ucIndex < pxProtocol->usBaseInputCount; ucIndex++) {
			g_xCoffee3RobotData.aucBaseInputs[ucIndex] =
				abBaseValues[ucIndex] ? 1U : 0U;
		}
		xResult = xModbusPortReadCoils(pxPort,
			COFFEE3_ROBOT_UNIT_ID, pxProtocol->usControlStart,
			pxProtocol->usControlCount,
			abValues, ulTimeoutMs);
	}
	if (xResult == MODBUS_PORT_RESULT_OK) {
		for (ucIndex = 0U;
			ucIndex < pxProtocol->usControlCount; ucIndex++) {
			g_xCoffee3RobotData.aucControlCoils[ucIndex] =
				abValues[ucIndex] ? 1U : 0U;
		}
	}
	return xResult;
}

/*-----------------------------------------------------------*/
/**
  * @brief 查询网络栈是否允许机器人会话尝试连接。
  * @param[in] pvOwnerContext 未使用的会话所有者上下文。
  * @retval 1 网络栈已就绪。
  * @retval 0 网络栈尚未就绪。
  */
static uint8_t prvRobotSessionNetworkReady(void *pvOwnerContext)
{
	(void)pvOwnerContext;
	return ucAppTaskManagerIsNetworkReady();
}

/*-----------------------------------------------------------*/
/**
  * @brief 清除机器人 3100 就绪线圈并验证已连接会话的协议通信。
  * @param[in] pvOwnerContext 指向机器人会话上下文。
  * @param[in] ulTimeoutMs 探测事务超时，单位为毫秒。
  * @retval 0 线圈清零写入成功，后续仍需重新观察新的就绪信号。
  * @retval 非零 Modbus 结果码或参数错误。
  * @note 成功时置新就绪信号等待标志，并清零本地控制线圈快照的就绪位。
  */
static int32_t prvRobotSessionProbe(void *pvOwnerContext,
	uint32_t ulTimeoutMs)
{
	Coffee3RobotSessionContext_t *pxContext; /*!< 会话持有的端口和传输上下文。 */
	ModbusPortResult_e xResult; /*!< 3100 就绪线圈清零写入的协议结果。 */
	ModbusPortFault_t xFault; /*!< 协议帧异常时取得的故障详情。 */
	uint8_t ucAttempt; /*!< 同一连接内丢弃异常帧后的探测重试次数。 */

	pxContext = (Coffee3RobotSessionContext_t *)pvOwnerContext;
	if ((pxContext == NULL) || (pxContext->pxPort == NULL)) {
		return (int32_t)MODBUS_PORT_RESULT_INVALID_ARG;
	}
	/* TCP 可达性独立于机器人程序 RUNNING 状态和 3100 就绪线圈。 */
	xResult = MODBUS_PORT_RESULT_PROTOCOL;
	for (ucAttempt = 0U;
		ucAttempt < COFFEE3_ROBOT_PROTOCOL_FAILURE_LIMIT; ucAttempt++) {
		xResult = xModbusPortWriteCoil(pxContext->pxPort,
			COFFEE3_ROBOT_UNIT_ID, 3100U, false, ulTimeoutMs);
		if (xResult != MODBUS_PORT_RESULT_PROTOCOL) {
			break;
		}
		memset(&xFault, 0, sizeof(xFault));
		vModbusPortGetLastFault(pxContext->pxPort, &xFault);
		(void)xCoffee3LogPrintfOrder(COFFEE3_LOG_LEVEL_WARNING,
			COFFEE3_LOG_SOURCE_ROBOT, COFFEE3_LOG_ORDER_SYSTEM,
			"ROBOT_PROTOCOL_CHECK_FRAME_DROPPED protocol=%ld attempt=%u",
			(long)xFault.lProtocolCode, (unsigned int)(ucAttempt + 1U));
	}
	s_ucFreshReadyReset = (xResult == MODBUS_PORT_RESULT_OK) ? 1U : 0U;
	g_xCoffee3RobotData.aucControlCoils[0U] = 0U;
	(void)xCoffee3LogWrite(COFFEE3_LOG_LEVEL_INFO, COFFEE3_LOG_SOURCE_ROBOT,
		"Reconnect: clear 3100; wait for fresh program ready signal", (int32_t)xResult);
	return (int32_t)xResult;
}

/*-----------------------------------------------------------*/
/**
  * @brief 处理 TCP 会话迁移并同步连接、设备在线和退避状态。
  * @param[in] pvOwnerContext 指向机器人会话上下文。
  * @param[in] xPreviousState 迁移前会话状态。
  * @param[in] xCurrentState 迁移后会话状态。
  * @param[in] lReason 状态迁移原因或错误码。
  * @param[in] ulAttempt 当前累计连接尝试次数。
  * @param[in] ulRetryDelayMs 下一次重试等待时间，单位为毫秒。
  */
static void prvRobotSessionEvent(void *pvOwnerContext,
	TcpClientSessionState_e xPreviousState,
	TcpClientSessionState_e xCurrentState, int32_t lReason,
	uint32_t ulAttempt, uint32_t ulRetryDelayMs)
{
	Coffee3RobotSessionContext_t *pxContext; /*!< 回调关联的机器人通信对象。 */
	ModbusPortFault_t xFault; /*!< 协议检查失败时的完整故障信息。 */
	int32_t lNativeError; /*!< TCP 或 Modbus 故障中的本地错误码。 */

	pxContext = (Coffee3RobotSessionContext_t *)pvOwnerContext;
	lNativeError = 0;
	memset(&xFault, 0, sizeof(xFault));
	if ((pxContext != NULL) && (pxContext->pxTransport != NULL)) {
		lNativeError = pxContext->pxTransport->lLastNativeError;
	}
	if ((pxContext != NULL) && (pxContext->pxPort != NULL)) {
		vModbusPortGetLastFault(pxContext->pxPort, &xFault);
	}
	g_xCoffee3RobotTcpStatus.ulConnectAttemptCount = ulAttempt;
	g_xCoffee3RobotTcpStatus.lLastResult = lReason;

	if ((xPreviousState == TCP_CLIENT_SESSION_NETWORK_WAIT) &&
		(xCurrentState == TCP_CLIENT_SESSION_BACKOFF)) {
		return;
	}
	if ((xPreviousState == TCP_CLIENT_SESSION_BACKOFF) &&
		(xCurrentState == TCP_CLIENT_SESSION_CONNECTING)) {
		(void)xCoffee3LogWriteField(COFFEE3_LOG_LEVEL_INFO,
			COFFEE3_LOG_SOURCE_ROBOT, "ROBOT_CONNECT_BEGIN", 0,
			"attempt", (int32_t)ulAttempt);
		return;
	}
	if ((xPreviousState == TCP_CLIENT_SESSION_CONNECTING) &&
		(xCurrentState == TCP_CLIENT_SESSION_PROTOCOL_CHECK)) {
		g_xCoffee3RobotTcpStatus.ucConnected = 1U;
		s_ucConnectionStartupPending = 1U;
		s_ucFreshReadyReset = 0U;
		g_xCoffee3RobotTcpStatus.ulConnectSuccessCount++;
		prvSetRobotReady(0U);
		prvLogRobotConnected(s_aucCoffee3RobotIp, COFFEE3_ROBOT_PORT,
			ulAttempt);
		(void)xCoffee3LogWriteField(COFFEE3_LOG_LEVEL_INFO,
			COFFEE3_LOG_SOURCE_ROBOT, "ROBOT_PROTOCOL_CHECK_BEGIN", 0,
			"timeout_ms", (int32_t)COFFEE3_ROBOT_IO_TIMEOUT_MS);
		return;
	}
	if ((xPreviousState == TCP_CLIENT_SESSION_PROTOCOL_CHECK) &&
		(xCurrentState == TCP_CLIENT_SESSION_ONLINE)) {
		prvResetRobotLinkHealth();
		g_xCoffee3RobotTcpStatus.ulConsecutiveFailures = 0U;
		g_xCoffee3RobotTcpStatus.ulNextRetryDelayMs = 0U;
		vCoffee3DeviceSetOnline(COFFEE3_DEVICE_ROBOT, 1U);
		(void)xCoffee3LogWriteField(COFFEE3_LOG_LEVEL_INFO,
			COFFEE3_LOG_SOURCE_ROBOT, "ROBOT_PROTOCOL_CHECK_OK", 0,
			"state_mask", (int32_t)prvRobotStartupStateMask());
		return;
	}
	if (xCurrentState == TCP_CLIENT_SESSION_BACKOFF) {
		s_ucConnectionStartupPending = 0U;
		s_ucFreshReadyReset = 0U;
		g_xCoffee3RobotTcpStatus.ulErrorCount++;
		g_xCoffee3RobotTcpStatus.ulConsecutiveFailures++;
		g_xCoffee3RobotTcpStatus.ulNextRetryDelayMs = ulRetryDelayMs;
		if (g_xCoffee3RobotTcpStatus.ucConnected != 0U) {
			g_xCoffee3RobotTcpStatus.ulDisconnectCount++;
		}
		g_xCoffee3RobotTcpStatus.ucConnected = 0U;
		prvSetRobotReady(0U);
		vCoffee3DeviceSetOnline(COFFEE3_DEVICE_ROBOT, 0U);
		vCoffee3DeviceSetReady(COFFEE3_DEVICE_ROBOT, 0U);
		if (xPreviousState == TCP_CLIENT_SESSION_CONNECTING) {
			(void)xCoffee3LogWriteField(COFFEE3_LOG_LEVEL_WARNING,
				COFFEE3_LOG_SOURCE_ROBOT, "ROBOT_CONNECT_FAILED", lReason,
				"native_error", lNativeError);
			if (lReason == (int32_t)TRANSPORT_RESULT_TIMEOUT) {
				(void)xCoffee3LogWriteField(COFFEE3_LOG_LEVEL_WARNING,
					COFFEE3_LOG_SOURCE_ROBOT, "ROBOT_CONNECT_TIMEOUT",
					lReason, "attempt", (int32_t)ulAttempt);
			}
		} else if (xPreviousState ==
			TCP_CLIENT_SESSION_PROTOCOL_CHECK) {
			(void)xCoffee3LogWriteField(COFFEE3_LOG_LEVEL_WARNING,
				COFFEE3_LOG_SOURCE_ROBOT, "ROBOT_PROTOCOL_CHECK_FAILED",
				lReason, "transport_result",
				(int32_t)xFault.xTransportResult);
			(void)xCoffee3LogWriteField(COFFEE3_LOG_LEVEL_WARNING,
				COFFEE3_LOG_SOURCE_ROBOT, "ROBOT_PROTOCOL_FAULT_PROTOCOL",
				lReason, "protocol_code", xFault.lProtocolCode);
			(void)xCoffee3LogWriteField(COFFEE3_LOG_LEVEL_WARNING,
				COFFEE3_LOG_SOURCE_ROBOT, "ROBOT_PROTOCOL_FAULT_EXCEPTION",
				lReason, "exception_code", (int32_t)xFault.ucExceptionCode);
			(void)xCoffee3LogWriteField(COFFEE3_LOG_LEVEL_WARNING,
				COFFEE3_LOG_SOURCE_ROBOT, "ROBOT_PROTOCOL_FAULT_NATIVE",
				lReason, "native_error", xFault.lNativeError);
		} else if (xPreviousState == TCP_CLIENT_SESSION_ONLINE) {
			(void)xCoffee3LogWriteField(COFFEE3_LOG_LEVEL_WARNING,
				COFFEE3_LOG_SOURCE_ROBOT, "ROBOT_LINK_LOST", lReason,
				"attempt", (int32_t)ulAttempt);
		}
		if (lNativeError != 0) {
			vCoffee3LogLwipResourceFailure(COFFEE3_LOG_SOURCE_ROBOT,
				lNativeError);
		}
		(void)xCoffee3LogWriteField(COFFEE3_LOG_LEVEL_INFO,
			COFFEE3_LOG_SOURCE_ROBOT, "ROBOT_RETRY_SCHEDULED", lReason,
			"delay_ms", (int32_t)ulRetryDelayMs);
		return;
	}
	if (xCurrentState == TCP_CLIENT_SESSION_NETWORK_WAIT) {
		s_ucConnectionStartupPending = 0U;
		s_ucFreshReadyReset = 0U;
		if (g_xCoffee3RobotTcpStatus.ucConnected != 0U) {
			g_xCoffee3RobotTcpStatus.ulDisconnectCount++;
		}
		g_xCoffee3RobotTcpStatus.ucConnected = 0U;
		prvSetRobotReady(0U);
		vCoffee3DeviceSetOnline(COFFEE3_DEVICE_ROBOT, 0U);
		vCoffee3DeviceSetReady(COFFEE3_DEVICE_ROBOT, 0U);
		(void)xCoffee3LogWriteField(COFFEE3_LOG_LEVEL_WARNING,
			COFFEE3_LOG_SOURCE_ROBOT, "ROBOT_NETWORK_WAITING", lReason,
			"attempt", (int32_t)ulAttempt);
	}
}

/*-----------------------------------------------------------*/
/**
  * @brief 判断机器人本体是否允许继续执行点位运动。
  * @retval 1 已使能、无报警且明确处于运行状态。
  * @retval 0 任一条件未满足。
  */
static uint8_t prvRobotOperational(void)
{
	/* READY 和 IDLE 只表示可用性；物理安全规则要求明确报告 RUNNING
	 * 才允许运动。 */
	if ((g_xCoffee3RobotData.aucBaseInputs[
		DOBOT_ROBOT_BODY_STATUS_ENABLED] == 0U) ||
		(g_xCoffee3RobotData.aucBaseInputs[
		DOBOT_ROBOT_BODY_STATUS_ALARM] != 0U)) {
		return 0U;
	}
	return (g_xCoffee3RobotData.aucBaseInputs[
		DOBOT_ROBOT_BODY_STATUS_RUNNING] != 0U) ? 1U : 0U;
}

/*-----------------------------------------------------------*/
/**
  * @brief 判断动作是否属于无需点位握手的机器人本体控制。
  * @param[in] usAction 产品动作编号。
  * @retval 1 本体控制、取消或复位动作。
  * @retval 0 点位动作或其他动作。
  */
static uint8_t prvRobotBasicAction(uint16_t usAction)
{
	return (((usAction >= (uint16_t)COFFEE3_ACTION_ROBOT_START) &&
		(usAction <= (uint16_t)COFFEE3_ACTION_ROBOT_MANUAL_MODE)) ||
		(usAction == (uint16_t)COFFEE3_ACTION_CANCEL) ||
		(usAction == (uint16_t)COFFEE3_ACTION_RESET)) ? 1U : 0U;
}

/*-----------------------------------------------------------*/
/**
  * @brief 检查新连接复位、就绪线圈、本体运行和安全状态是否全部满足。
  * @retval 1 允许发送点位动作。
  * @retval 0 尚未达到严格就绪条件。
  */
static uint8_t prvRobotStrictReady(void)
{
	uint8_t ucReady; /*!< 协议控制区中的机器人就绪线圈。 */
	uint8_t ucRunning; /*!< 本体输入中的程序运行状态。 */

	ucReady = g_xCoffee3RobotData.aucControlCoils[0U];
	ucRunning = g_xCoffee3RobotData.aucBaseInputs[
		DOBOT_ROBOT_BODY_STATUS_RUNNING];
	if ((s_ucFreshReadyReset == 0U) || (ucReady == 0U) || (ucRunning == 0U) ||
		(prvRobotOperational() == 0U)) {
		return 0U;
	}
	return 1U;
}

/*-----------------------------------------------------------*/
/**
  * @brief 更新机器人模块与设备镜像中的就绪状态，并记录边沿。
  * @param[in] ucReady 非零表示就绪，函数内部归一化为 0 或 1。
  */
static void prvSetRobotReady(uint8_t ucReady)
{
	uint8_t ucPrevious; /*!< 更新前的模块就绪值，用于检测状态边沿。 */

	ucReady = (ucReady != 0U) ? 1U : 0U;
	ucPrevious = g_xCoffee3RobotTcpStatus.ucReady;
	if (ucPrevious == ucReady) {
		vCoffee3DeviceSetReady(COFFEE3_DEVICE_ROBOT, ucReady);
		return;
	}
	g_xCoffee3RobotTcpStatus.ucReady = ucReady;
	vCoffee3DeviceSetReady(COFFEE3_DEVICE_ROBOT, ucReady);
	(void)xCoffee3LogWriteField(COFFEE3_LOG_LEVEL_INFO,
		COFFEE3_LOG_SOURCE_ROBOT,
		(ucReady != 0U) ? "ROBOT_READY_RISE" : "ROBOT_READY_FALL",
		(int32_t)ucReady, "previous", (int32_t)ucPrevious);
}

/*-----------------------------------------------------------*/
/**
  * @brief 将当前本体和协议状态压缩为启动诊断位掩码。
  * @retval 位掩码 各位表示拖拽、电源、使能、报警、安全及运动状态。
  */
static uint16_t prvRobotStartupStateMask(void)
{
	uint16_t usMask; /*!< 汇总当前启动相关状态的诊断位掩码。 */

	usMask = 0U;
	if (g_xCoffee3RobotData.aucBaseInputs[
		DOBOT_ROBOT_BODY_STATUS_DRAG] != 0U) {
		usMask |= COFFEE3_ROBOT_STARTUP_STATE_DRAG;
	}
	if (g_xCoffee3RobotData.aucBaseInputs[
		DOBOT_ROBOT_BODY_STATUS_POWERED] != 0U) {
		usMask |= COFFEE3_ROBOT_STARTUP_STATE_POWER;
	}
	if (g_xCoffee3RobotData.aucBaseInputs[
		DOBOT_ROBOT_BODY_STATUS_ENABLED] != 0U) {
		usMask |= COFFEE3_ROBOT_STARTUP_STATE_ENABLE;
	}
	if (g_xCoffee3RobotData.aucBaseInputs[
		DOBOT_ROBOT_BODY_STATUS_ALARM] != 0U) {
		usMask |= COFFEE3_ROBOT_STARTUP_STATE_ALARM;
	}
	if (g_xCoffee3RobotData.aucBaseInputs[
		DOBOT_ROBOT_BODY_STATUS_COLLISION] != 0U) {
		usMask |= COFFEE3_ROBOT_STARTUP_STATE_COLLISION;
	}
	if (g_xCoffee3RobotData.aucBaseInputs[
		DOBOT_ROBOT_BODY_STATUS_SAFETY_PAUSED] != 0U) {
		usMask |= COFFEE3_ROBOT_STARTUP_STATE_SAFETY;
	}
	if (g_xCoffee3RobotData.aucBaseInputs[
		DOBOT_ROBOT_BODY_STATUS_RECOVERY] != 0U) {
		usMask |= COFFEE3_ROBOT_STARTUP_STATE_RECOVERY;
	}
	if (g_xCoffee3RobotData.aucControlCoils[0U] != 0U) {
		usMask |= COFFEE3_ROBOT_STARTUP_STATE_READY;
	}
	if ((g_xCoffee3RobotData.aucBaseInputs[
		DOBOT_ROBOT_BODY_STATUS_RUNNING] != 0U) ||
		(g_xCoffee3RobotData.aucBaseInputs[
		DOBOT_ROBOT_BODY_STATUS_IDLE] != 0U)) {
		usMask |= COFFEE3_ROBOT_STARTUP_STATE_MOTION;
	}
	return usMask;
}

/*-----------------------------------------------------------*/
/**
  * @brief 清零协议配置的全部动作线圈范围并校验结果。
  * @param[in,out] pxPort 已初始化的机器人 Modbus 端口。
  * @param[out] pucStateMismatch 可为空；非空时报告仍有线圈未清零。
  * @param[in] usOrderId 写入日志的订单编号。
  * @retval MODBUS_PORT_RESULT_OK 清零完成，或通过输出参数报告状态不符。
  * @retval 其他值 Modbus 操作失败或无输出参数时校验失败。
  */
static ModbusPortResult_e prvClearActionCoils(ModbusPort_t *pxPort,
	uint8_t *pucStateMismatch, uint16_t usOrderId)
{
	const DobotRobotProtocolConfig_t *pxProtocol; /*!< 当前协议定义的两个清零区间。 */
	ModbusPortResult_e xResult; /*!< 最近一次区间清零结果。 */

	if (pucStateMismatch != NULL) {
		*pucStateMismatch = 0U;
	}
	pxProtocol = prvCoffee3DobotConfig()->pxProtocol;
	xResult = prvClearActionRange(pxPort,
		pxProtocol->usActionClearStart,
		pxProtocol->usActionClearCount, pucStateMismatch);
	if (xResult != MODBUS_PORT_RESULT_OK) {
		return xResult;
	}
	if ((pucStateMismatch != NULL) && (*pucStateMismatch != 0U)) {
		return MODBUS_PORT_RESULT_OK;
	}
	xResult = prvClearActionRange(pxPort,
		pxProtocol->usActionClearSecondStart,
		pxProtocol->usActionClearSecondCount, pucStateMismatch);
	if (xResult != MODBUS_PORT_RESULT_OK) {
		return xResult;
	}
	(void)xCoffee3LogWriteFieldOrder(COFFEE3_LOG_LEVEL_INFO,
		COFFEE3_LOG_SOURCE_ROBOT, usOrderId,
		"ROBOT_ACTION_COILS_CLEARED", 0,
		"count", (int32_t)(pxProtocol->usActionClearCount +
			pxProtocol->usActionClearSecondCount));
	return MODBUS_PORT_RESULT_OK;
}

/*-----------------------------------------------------------*/
/**
  * @brief 将一段动作线圈写零并回读验证。
  * @param[in,out] pxPort 已初始化的机器人 Modbus 端口。
  * @param[in] usStart 首个线圈地址。
  * @param[in] usCount 线圈数量，范围为 1 至 10。
  * @param[out] pucStateMismatch 可为空；非空时用 1 报告回读不为零。
  * @retval MODBUS_PORT_RESULT_OK 写入和回读成功。
  * @retval MODBUS_PORT_RESULT_INVALID_ARG 数量超出本地缓冲区范围。
  * @retval 其他值 Modbus 操作失败或回读状态不符。
  */
static ModbusPortResult_e prvClearActionRange(ModbusPort_t *pxPort,
	uint16_t usStart, uint16_t usCount, uint8_t *pucStateMismatch)
{
	bool abClear[10]; /*!< 待批量写入的全零线圈值。 */
	bool abRead[10]; /*!< 写入后回读的线圈状态。 */
	uint16_t usIndex; /*!< 初始化和验证线圈数组的索引。 */
	ModbusPortResult_e xResult; /*!< 批量写入或回读结果。 */

	if ((usCount == 0U) || (usCount > 10U)) {
		return MODBUS_PORT_RESULT_INVALID_ARG;
	}
	for (usIndex = 0U; usIndex < usCount; usIndex++) {
		abClear[usIndex] = false;
	}
	xResult = xModbusPortWriteCoils(pxPort, COFFEE3_ROBOT_UNIT_ID,
		usStart, usCount, abClear, COFFEE3_ROBOT_IO_TIMEOUT_MS);
	if (xResult != MODBUS_PORT_RESULT_OK) {
		return xResult;
	}
	xResult = xModbusPortReadCoils(pxPort, COFFEE3_ROBOT_UNIT_ID,
		usStart, usCount, abRead, COFFEE3_ROBOT_IO_TIMEOUT_MS);
	if (xResult != MODBUS_PORT_RESULT_OK) {
		return xResult;
	}
	for (usIndex = 0U; usIndex < usCount; usIndex++) {
		if (abRead[usIndex]) {
			if (pucStateMismatch != NULL) {
				*pucStateMismatch = 1U;
				return MODBUS_PORT_RESULT_OK;
			}
			return MODBUS_PORT_RESULT_PROTOCOL;
		}
	}
	return MODBUS_PORT_RESULT_OK;
}

/*-----------------------------------------------------------*/
/**
  * @brief 直接写入一个机器人控制线圈值。
  * @param[in,out] pxPort 已初始化的机器人 Modbus 端口。
  * @param[in] usCoil 目标线圈地址。
  * @param[in] bValue 要写入的布尔值。
  * @param[in] ulTimeoutMs 写操作超时，单位为毫秒。
  * @retval ModbusPortResult_e Modbus 单线圈写入结果。
  */
static ModbusPortResult_e prvWriteControlValue(ModbusPort_t *pxPort,
	uint16_t usCoil, bool bValue, uint32_t ulTimeoutMs)
{
	return xModbusPortWriteCoil(pxPort, COFFEE3_ROBOT_UNIT_ID,
		usCoil, bValue, ulTimeoutMs);
}

/*-----------------------------------------------------------*/
/**
  * @brief 执行机器人清报警、退出拖拽、使能和启动序列并等待运行。
  * @param[in,out] pxPort 已初始化的机器人 Modbus 端口。
  * @param[in] ucRecoverySafe 非零表示存在在途事务，跳过可能破坏证据的 STOP。
  * @param[out] pxOutcome 返回通信成功后的业务就绪结果，不得为空。
  * @retval MODBUS_PORT_RESULT_OK 通信序列完成，业务就绪见 pxOutcome。
  * @retval 其他值 任一步 Modbus 读写失败。
  */
static ModbusPortResult_e prvStartup(ModbusPort_t *pxPort,
	uint8_t ucRecoverySafe,
	Coffee3RobotStartupOutcome_e *pxOutcome)
{
	/* 清除旧本体脉冲，清报警、退出拖拽、使能并启动，最后等待 RUNNING。 */
	bool abBaseClear[10]; /*!< 启动前批量清零的本体控制线圈值。 */
	uint8_t ucIndex; /*!< 初始化本体控制数组的索引。 */
	ModbusPortResult_e xResult; /*!< 当前启动步骤的 Modbus 结果。 */
	uint8_t ucOperational; /*!< 已使能、无报警且运行时为 1。 */
	uint8_t ucPowerObserved; /*!< 最终快照中是否观察到本体上电信号。 */
	TickType_t xWaitStartTick; /*!< 等待机器人运行的起始系统节拍。 */

	*pxOutcome = COFFEE3_ROBOT_STARTUP_OK;
	(void)xCoffee3LogWriteField(COFFEE3_LOG_LEVEL_INFO,
		COFFEE3_LOG_SOURCE_ROBOT, "ROBOT_STARTUP_BEGIN",
		(int32_t)ucRecoverySafe, "state_mask",
		(int32_t)prvRobotStartupStateMask());
	(void)xCoffee3LogWriteField(COFFEE3_LOG_LEVEL_INFO,
		COFFEE3_LOG_SOURCE_ROBOT, "ROBOT_STARTUP_STEP", 0,
		"state_mask", (int32_t)prvRobotStartupStateMask());
	/* 第一步：清除本体控制脉冲，避免沿用上次连接的命令电平。 */
	for (ucIndex = 0U; ucIndex < 10U; ucIndex++) {
		abBaseClear[ucIndex] = false;
	}
	xResult = xModbusPortWriteCoils(pxPort,
		COFFEE3_ROBOT_UNIT_ID, 0U, 10U, abBaseClear,
		COFFEE3_ROBOT_IO_TIMEOUT_MS);
	if (xResult != MODBUS_PORT_RESULT_OK) {
		return xResult;
	}
	vTaskDelay(pdMS_TO_TICKS(COFFEE3_ROBOT_STARTUP_STEP_DELAY_MS));
	(void)xCoffee3LogWriteField(COFFEE3_LOG_LEVEL_INFO,
		COFFEE3_LOG_SOURCE_ROBOT, "ROBOT_STARTUP_STEP", 1,
		"state_mask", (int32_t)prvRobotStartupStateMask());
	/* 第二步：清报警；无在途动作时允许额外 STOP 整理初始状态。 */
	xResult = prvWriteRisingEdge(pxPort,
		DOBOT_ROBOT_BODY_COMMAND_CLEAR_ALARM,
		COFFEE3_ROBOT_IO_TIMEOUT_MS);
	if (xResult != MODBUS_PORT_RESULT_OK) {
		return xResult;
	}
	vTaskDelay(pdMS_TO_TICKS(COFFEE3_ROBOT_STARTUP_STEP_DELAY_MS));
	(void)xCoffee3LogWriteField(COFFEE3_LOG_LEVEL_INFO,
		COFFEE3_LOG_SOURCE_ROBOT, "ROBOT_STARTUP_STEP", 2,
		"state_mask", (int32_t)prvRobotStartupStateMask());
	if (ucRecoverySafe == 0U) {
		xResult = prvWriteRisingEdge(pxPort,
			DOBOT_ROBOT_BODY_COMMAND_STOP,
			COFFEE3_ROBOT_IO_TIMEOUT_MS);
		if (xResult != MODBUS_PORT_RESULT_OK) {
			return xResult;
		}
	}
	vTaskDelay(pdMS_TO_TICKS(COFFEE3_ROBOT_STARTUP_STEP_DELAY_MS));
	(void)xCoffee3LogWriteField(COFFEE3_LOG_LEVEL_INFO,
		COFFEE3_LOG_SOURCE_ROBOT, "ROBOT_STARTUP_STEP", 3,
		"state_mask", (int32_t)prvRobotStartupStateMask());
	xResult = prvWriteControlValue(pxPort,
		DOBOT_ROBOT_BODY_COMMAND_CLEAR_ALARM, false,
		COFFEE3_ROBOT_IO_TIMEOUT_MS);
	if (xResult != MODBUS_PORT_RESULT_OK) {
		return xResult;
	}
	vTaskDelay(pdMS_TO_TICKS(COFFEE3_ROBOT_STARTUP_STEP_DELAY_MS));
	(void)xCoffee3LogWriteField(COFFEE3_LOG_LEVEL_INFO,
		COFFEE3_LOG_SOURCE_ROBOT, "ROBOT_STARTUP_STEP", 4,
		"state_mask", (int32_t)prvRobotStartupStateMask());
	/* 第三步：再次清报警、退出拖拽并使能机器人。 */
	xResult = prvWriteRisingEdge(pxPort,
		DOBOT_ROBOT_BODY_COMMAND_CLEAR_ALARM,
		COFFEE3_ROBOT_IO_TIMEOUT_MS);
	if (xResult != MODBUS_PORT_RESULT_OK) {
		return xResult;
	}
	vTaskDelay(pdMS_TO_TICKS(COFFEE3_ROBOT_STARTUP_STEP_DELAY_MS));
	(void)xCoffee3LogWriteField(COFFEE3_LOG_LEVEL_INFO,
		COFFEE3_LOG_SOURCE_ROBOT, "ROBOT_STARTUP_STEP", 5,
		"state_mask", (int32_t)prvRobotStartupStateMask());
	/* 第三步继续：退出拖拽模式并使能机器人。 */
	xResult = prvWriteRisingEdge(pxPort,
		DOBOT_ROBOT_BODY_COMMAND_EXIT_DRAG,
		COFFEE3_ROBOT_IO_TIMEOUT_MS);
	if (xResult != MODBUS_PORT_RESULT_OK) {
		return xResult;
	}
	vTaskDelay(pdMS_TO_TICKS(COFFEE3_ROBOT_STARTUP_STEP_DELAY_MS));
	(void)xCoffee3LogWriteField(COFFEE3_LOG_LEVEL_INFO,
		COFFEE3_LOG_SOURCE_ROBOT, "ROBOT_STARTUP_STEP", 6,
		"state_mask", (int32_t)prvRobotStartupStateMask());
	/* 第三步继续：使能机器人，无在途动作时再发送一次 STOP 整理状态。 */
	xResult = prvWriteRisingEdge(pxPort,
		DOBOT_ROBOT_BODY_COMMAND_ENABLE,
		COFFEE3_ROBOT_IO_TIMEOUT_MS);
	if (xResult != MODBUS_PORT_RESULT_OK) {
		return xResult;
	}
	vTaskDelay(pdMS_TO_TICKS(COFFEE3_ROBOT_STARTUP_STEP_DELAY_MS));
	if (ucRecoverySafe == 0U) {
		(void)xCoffee3LogWriteField(COFFEE3_LOG_LEVEL_INFO,
			COFFEE3_LOG_SOURCE_ROBOT, "ROBOT_STARTUP_STEP", 7,
			"state_mask", (int32_t)prvRobotStartupStateMask());
		xResult = prvWriteRisingEdge(pxPort,
			DOBOT_ROBOT_BODY_COMMAND_STOP,
			COFFEE3_ROBOT_IO_TIMEOUT_MS);
		if (xResult != MODBUS_PORT_RESULT_OK) {
			return xResult;
		}
	}
	vTaskDelay(pdMS_TO_TICKS(COFFEE3_ROBOT_STARTUP_STEP_DELAY_MS));
	(void)xCoffee3LogWriteField(COFFEE3_LOG_LEVEL_INFO,
		COFFEE3_LOG_SOURCE_ROBOT, "ROBOT_STARTUP_STEP", 8,
		"state_mask", (int32_t)prvRobotStartupStateMask());
	/* 第四步：发出 START 后轮询状态，超时只返回“尚未就绪”业务结果。 */
	xResult = prvWriteRisingEdge(pxPort,
		DOBOT_ROBOT_BODY_COMMAND_START,
		COFFEE3_ROBOT_IO_TIMEOUT_MS);
	if (xResult != MODBUS_PORT_RESULT_OK) {
		return xResult;
	}
	vTaskDelay(pdMS_TO_TICKS(COFFEE3_ROBOT_STARTUP_STEP_DELAY_MS));
	xWaitStartTick = xTaskGetTickCount();
	(void)xCoffee3LogWriteField(COFFEE3_LOG_LEVEL_INFO,
		COFFEE3_LOG_SOURCE_ROBOT, "ROBOT_STARTUP_WAIT", 0,
		"timeout_ms", (int32_t)COFFEE3_ROBOT_STARTUP_FINAL_WAIT_MS);
	for (;;) {
		xResult = prvRefresh(pxPort, COFFEE3_ROBOT_IO_TIMEOUT_MS);
		if (xResult != MODBUS_PORT_RESULT_OK) {
			return xResult;
		}
		ucOperational = ((g_xCoffee3RobotData.aucBaseInputs[
			DOBOT_ROBOT_BODY_STATUS_ENABLED] != 0U) &&
			(g_xCoffee3RobotData.aucBaseInputs[
			DOBOT_ROBOT_BODY_STATUS_ALARM] == 0U) &&
			(g_xCoffee3RobotData.aucBaseInputs[
			DOBOT_ROBOT_BODY_STATUS_RUNNING] != 0U)) ? 1U : 0U;
		if (ucOperational != 0U) {
			break;
		}
		if ((xTaskGetTickCount() - xWaitStartTick) >= pdMS_TO_TICKS(
			COFFEE3_ROBOT_STARTUP_FINAL_WAIT_MS)) {
			break;
		}
		vTaskDelay(pdMS_TO_TICKS(COFFEE3_ROBOT_STARTUP_POLL_MS));
	}
	ucPowerObserved = g_xCoffee3RobotData.aucBaseInputs[
		DOBOT_ROBOT_BODY_STATUS_POWERED];
	if (ucOperational == 0U) {
		*pxOutcome = COFFEE3_ROBOT_STARTUP_NOT_READY;
		(void)xCoffee3LogWriteField(COFFEE3_LOG_LEVEL_WARNING,
			COFFEE3_LOG_SOURCE_ROBOT, "ROBOT_STARTUP_NOT_READY", 0,
			"state_mask", (int32_t)prvRobotStartupStateMask());
		return MODBUS_PORT_RESULT_OK;
	}
	if (ucPowerObserved == 0U) {
		(void)xCoffee3LogWriteField(COFFEE3_LOG_LEVEL_WARNING,
			COFFEE3_LOG_SOURCE_ROBOT,
			"ROBOT_POWER_SIGNAL_NOT_OBSERVED", 0,
			"state_mask", (int32_t)prvRobotStartupStateMask());
	}
	(void)xCoffee3LogWriteField(COFFEE3_LOG_LEVEL_INFO,
		COFFEE3_LOG_SOURCE_ROBOT, "ROBOT_STARTUP_DONE", 0,
		"state_mask", (int32_t)prvRobotStartupStateMask());
	return MODBUS_PORT_RESULT_OK;
}

/*-----------------------------------------------------------*/
/**
  * @brief 为一个尚未活动的点位动作建立本地事务。
  * @param[in,out] pxTransaction 待初始化事务，不得为空且必须空闲。
  * @param[in] pxCommand 要由事务持有的命令，不得为空。
  * @param[in] usCommandCoil 动作命令线圈地址。
  * @param[in] usResultCoil 动作完成线圈地址。
  */
static void prvBeginActionTransaction(Coffee3RobotTransaction_t *pxTransaction,
	const Coffee3Command_t *pxCommand, uint16_t usCommandCoil,
	uint16_t usResultCoil)
{
	TickType_t xNow; /*!< 事务建立时的系统节拍。 */

	if ((pxTransaction == NULL) || (pxCommand == NULL) ||
		(pxTransaction->ucActive != 0U)) {
		return;
	}
	xNow = xTaskGetTickCount();
	memset(pxTransaction, 0, sizeof(*pxTransaction));
	pxTransaction->xCommand = *pxCommand;
	pxTransaction->usCommandCoil = usCommandCoil;
	pxTransaction->usResultCoil = usResultCoil;
	pxTransaction->xAcceptDeadline = 0U;
	pxTransaction->xNextPollTick = xNow;
	pxTransaction->ucActive = 1U;
	pxTransaction->xPhase = COFFEE3_ROBOT_PHASE_WAIT_ACCEPT;
	vCoffee3DeviceSetRobotPhase(COFFEE3_ROBOT_PHASE_WAIT_ACCEPT);
	vCoffee3DeviceSetRobotAccepted(0U);
}

/*-----------------------------------------------------------*/
/**
  * @brief 在命令尚未写出时安排一次有上限的准备阶段重试。
  * @param[in,out] pxTransaction 当前动作事务，可为空。
  * @param[in] xFailure 本次准备失败原因。
  * @retval MODBUS_PORT_RESULT_BUSY 已安排下一次准备重试。
  * @retval MODBUS_PORT_RESULT_TIMEOUT 准备重试次数已耗尽。
  * @retval 其他值 无可重试事务或命令已尝试写出时保留原失败。
  * @note 重试仅发生在准备阶段；写动作命令后不再重发。
  */
static ModbusPortResult_e prvSchedulePrepareRetry(
	Coffee3RobotTransaction_t *pxTransaction,
	ModbusPortResult_e xFailure)
{
	if ((pxTransaction == NULL) || (pxTransaction->ucActive == 0U) ||
		(pxTransaction->ucCommandWriteAttempted != 0U)) {
		return xFailure;
	}
	if (pxTransaction->ucPrepareRetryCount >=
		COFFEE3_ROBOT_PREPARE_RETRY_LIMIT) {
		(void)xCoffee3LogWriteFieldOrder(COFFEE3_LOG_LEVEL_WARNING,
			COFFEE3_LOG_SOURCE_ROBOT,
			(uint16_t)pxTransaction->xCommand.ulOrderId,
			"ROBOT_ACTION_PREPARE_RETRY_EXHAUSTED", (int32_t)xFailure,
			"action", (int32_t)pxTransaction->xCommand.usAction);
		return MODBUS_PORT_RESULT_TIMEOUT;
	}
	pxTransaction->ucPrepareRetryCount++;
	pxTransaction->xNextPrepareRetryTick = xTaskGetTickCount() +
		pdMS_TO_TICKS(COFFEE3_ROBOT_PREPARE_RETRY_MS);
	(void)xCoffee3LogWriteFieldOrder(COFFEE3_LOG_LEVEL_WARNING,
		COFFEE3_LOG_SOURCE_ROBOT,
		(uint16_t)pxTransaction->xCommand.ulOrderId,
		"ROBOT_ACTION_PREPARE_RETRY", (int32_t)xFailure,
		"retry", (int32_t)pxTransaction->ucPrepareRetryCount);
	return MODBUS_PORT_RESULT_BUSY;
}

/*-----------------------------------------------------------*/
/**
  * @brief 归档活动事务的最终结果并清空当前事务。
  * @param[in,out] pxTransaction 要归档和复位的事务，可为空。
  * @param[in] lTerminalResult 事务最终结果码。
  */
static void prvArchiveAndResetTransaction(
	Coffee3RobotTransaction_t *pxTransaction, int32_t lTerminalResult)
{
	if (pxTransaction == NULL) {
		return;
	}
	if (pxTransaction->ucActive != 0U) {
		pxTransaction->ucTerminalLogged = 1U;
		pxTransaction->lTerminalResult = lTerminalResult;
		s_xLastTransaction = *pxTransaction;
	}
	memset(pxTransaction, 0, sizeof(*pxTransaction));
}

/*-----------------------------------------------------------*/
/**
  * @brief 判断命令是否为服务器发起的可替换调试运动。
  * @param[in] pxCommand 待判断命令，可为空。
  * @retval 1 来源为服务器且带调试标志。
  * @retval 0 其他命令或空指针。
  */
static uint8_t prvRobotDebugMotion(const Coffee3Command_t *pxCommand)
{
	if (pxCommand == NULL) {
		return 0U;
	}
	return ((pxCommand->ucSource ==
		(uint8_t)COFFEE3_COMMAND_SOURCE_SERVER) &&
		((pxCommand->ucFlags & COFFEE3_COMMAND_FLAG_DEBUG) != 0U)) ?
		1U : 0U;
}

/*-----------------------------------------------------------*/
/**
  * @brief 将活动动作标记为逾期并只记录一次等待日志。
  * @param[in,out] pxTransaction 当前活动事务，可为空。
  * @param[in] pcReason 逾期阶段名称，可为空。
  * @note 逾期不会终止动作；所有者继续等待结果或上位业务取消。
  */
static void prvMarkActionOverdue(Coffee3RobotTransaction_t *pxTransaction,
	const char *pcReason)
{
	const char *pcOwner; /*!< 日志中的动作所有者名称。 */

	if ((pxTransaction == NULL) || (pxTransaction->ucActive == 0U)) {
		return;
	}
	pxTransaction->ucOverdue = 1U;
	if (pxTransaction->ucOverdueLogged != 0U) {
		return;
	}
	pxTransaction->ucOverdueLogged = 1U;
	pcOwner = (prvRobotDebugMotion(&pxTransaction->xCommand) != 0U) ?
		"debug" : "order";
	(void)xCoffee3LogPrintfOrder(COFFEE3_LOG_LEVEL_WARNING,
		COFFEE3_LOG_SOURCE_ROBOT,
		(uint16_t)pxTransaction->xCommand.ulOrderId,
		"Overdue: device=Robot owner=%s action=%s phase=%s; waiting",
		pcOwner, prvRobotActionName(pxTransaction->xCommand.usAction),
		(pcReason != NULL) ? pcReason : "deadline");
}

/*-----------------------------------------------------------*/
/**
  * @brief 重连后读取命令和结果线圈，对账在途动作且不重复发布运动。
  * @param[in,out] pxPort 已恢复通信的机器人 Modbus 端口。
  * @param[in,out] pxTransaction 断线前仍活动的事务。
  * @param[out] pucDone 返回动作是否已完成且结果线圈已确认清零。
  * @retval MODBUS_PORT_RESULT_OK 对账完成或仍需继续等待。
  * @retval 其他值 地址非法、读写失败或结果清零校验失败。
  */
static ModbusPortResult_e prvReconcile(ModbusPort_t *pxPort,
	Coffee3RobotTransaction_t *pxTransaction,
	uint8_t *pucDone)
{
	/* TCP 恢复后重读命令和结果状态，以免重复触发物理运动。 */
	const DobotRobotProtocolConfig_t *pxProtocol; /*!< 当前协议的控制线圈区间。 */
	uint32_t ulControlEnd; /*!< 控制线圈区间的开区间末地址。 */
	uint8_t ucCommand; /*!< 快照中的事务命令线圈值。 */
	uint8_t ucResult; /*!< 快照中的事务结果线圈值。 */
	bool bResult; /*!< 清除结果线圈后的回读值。 */
	ModbusPortResult_e xResult; /*!< 对账刷新或写入的 Modbus 结果。 */

	*pucDone = 0U;
	/* 第一步：命令尚未发布时只允许回到准备重试路径。 */
	if (pxTransaction->ucCommandWriteAttempted == 0U) {
		return MODBUS_PORT_RESULT_OK;
	}
	if (pxTransaction->usCommandCoil == 0xFFFFU) {
		return MODBUS_PORT_RESULT_OK;
	}
	/* 第二步：验证事务线圈仍属于当前协议，再刷新机器人端状态。 */
	pxProtocol = prvCoffee3DobotConfig()->pxProtocol;
	ulControlEnd = (uint32_t)pxProtocol->usControlStart +
		(uint32_t)pxProtocol->usControlCount;
	if (((uint32_t)pxTransaction->usCommandCoil <
		(uint32_t)pxProtocol->usControlStart) ||
		((uint32_t)pxTransaction->usCommandCoil >= ulControlEnd) ||
		((uint32_t)pxTransaction->usResultCoil <
		(uint32_t)pxProtocol->usControlStart) ||
		((uint32_t)pxTransaction->usResultCoil >= ulControlEnd)) {
		return MODBUS_PORT_RESULT_PROTOCOL;
	}
	xResult = prvRefresh(pxPort, COFFEE3_ROBOT_IO_TIMEOUT_MS);
	if (xResult != MODBUS_PORT_RESULT_OK) {
		return xResult;
	}
	ucCommand = g_xCoffee3RobotData.aucControlCoils[
		pxTransaction->usCommandCoil - pxProtocol->usControlStart];
	ucResult = g_xCoffee3RobotData.aucControlCoils[
		pxTransaction->usResultCoil - pxProtocol->usControlStart];
	/* 第三步：按命令/结果组合恢复等待接受、运动或完成阶段。 */
	if ((ucCommand != 0U) && (ucResult != 0U) &&
		(pxTransaction->ucAccepted == 0U)) {
		pxTransaction->xPhase = COFFEE3_ROBOT_PHASE_WAIT_ACCEPT;
		vCoffee3DeviceSetRobotPhase(COFFEE3_ROBOT_PHASE_WAIT_ACCEPT);
		if (pxTransaction->ucResultWhileCommandHigh == 0U) {
			pxTransaction->ucResultWhileCommandHigh = 1U;
			(void)xCoffee3LogWriteFieldOrder(COFFEE3_LOG_LEVEL_WARNING,
			COFFEE3_LOG_SOURCE_ROBOT,
			(uint16_t)pxTransaction->xCommand.ulOrderId,
			"ROBOT_ACTION_PROTOCOL_SEQUENCE",
			MODBUS_PORT_RESULT_PROTOCOL, "action",
			(int32_t)pxTransaction->xCommand.usAction);
		}
		return MODBUS_PORT_RESULT_OK;
	}
	if (ucCommand != 0U) {
		pxTransaction->xPhase = COFFEE3_ROBOT_PHASE_WAIT_ACCEPT;
		vCoffee3DeviceSetRobotPhase(COFFEE3_ROBOT_PHASE_WAIT_ACCEPT);
		return MODBUS_PORT_RESULT_OK;
	}
	if (ucCommand == 0U) {
		if ((ucResult == 0U) &&
			(pxTransaction->ucCompletionObserved != 0U)) {
			/* TCP 失败前结果应答可能已经到达机器人，本地完成证据优先。 */
			*pucDone = 1U;
			return MODBUS_PORT_RESULT_OK;
		}
		if ((ucResult == 0U) && (pxTransaction->ucAccepted == 0U)) {
			/* 命令已清但本地未观察到接受属于结果不明状态：保留原接受期限，
			 * 不凭空认定已接受，也绝不重新发布动作。 */
			pxTransaction->ucAmbiguous = 1U;
			pxTransaction->xPhase = COFFEE3_ROBOT_PHASE_WAIT_ACCEPT;
			vCoffee3DeviceSetRobotPhase(COFFEE3_ROBOT_PHASE_WAIT_ACCEPT);
			return MODBUS_PORT_RESULT_OK;
		}
		if (pxTransaction->ucAccepted == 0U) {
			pxTransaction->ucAccepted = 1U;
			pxTransaction->xAcceptedTick = xTaskGetTickCount();
			pxTransaction->xMotionDeadline = pxTransaction->xAcceptedTick +
				pdMS_TO_TICKS(COFFEE3_ROBOT_MOTION_TIMEOUT_MS);
			vCoffee3DeviceSetRobotAccepted(1U);
			vCoffee3DeviceSetRobotPhase(COFFEE3_ROBOT_PHASE_MOVING);
			(void)xCoffee3LogWriteFieldOrder(COFFEE3_LOG_LEVEL_INFO,
				COFFEE3_LOG_SOURCE_ROBOT,
				(uint16_t)pxTransaction->xCommand.ulOrderId,
				"ROBOT_ACTION_ACCEPTED", 0,
				"coil", (int32_t)pxTransaction->usCommandCoil);
		}
		/* 第四步：若完成信号存在，则清零并回读确认后才宣布完成。 */
		if (ucResult != 0U) {
			pxTransaction->ucCompletionObserved = 1U;
			pxTransaction->xPhase = COFFEE3_ROBOT_PHASE_CLEAR_RESULT;
			xResult = xModbusPortWriteCoil(pxPort,
				COFFEE3_ROBOT_UNIT_ID, pxTransaction->usResultCoil,
				false, COFFEE3_ROBOT_IO_TIMEOUT_MS);
			if (xResult != MODBUS_PORT_RESULT_OK) {
				return xResult;
			}
			xResult = xModbusPortReadCoils(pxPort,
				COFFEE3_ROBOT_UNIT_ID, pxTransaction->usResultCoil,
				1U, &bResult, COFFEE3_ROBOT_IO_TIMEOUT_MS);
			if (xResult != MODBUS_PORT_RESULT_OK) {
				return xResult;
			}
			if (bResult) {
				return MODBUS_PORT_RESULT_PROTOCOL;
			}
			*pucDone = 1U;
			(void)xCoffee3LogWriteFieldOrder(COFFEE3_LOG_LEVEL_INFO,
				COFFEE3_LOG_SOURCE_ROBOT,
				(uint16_t)pxTransaction->xCommand.ulOrderId,
				"ROBOT_ACTION_RESULT_CLEARED", 0,
				"coil", (int32_t)pxTransaction->usResultCoil);
			(void)xCoffee3LogWriteFieldOrder(COFFEE3_LOG_LEVEL_INFO,
				COFFEE3_LOG_SOURCE_ROBOT,
				(uint16_t)pxTransaction->xCommand.ulOrderId,
				"ROBOT_ACTION_COMPLETE", 0,
				"action", (int32_t)pxTransaction->xCommand.usAction);
			return MODBUS_PORT_RESULT_OK;
		}
		pxTransaction->xPhase = COFFEE3_ROBOT_PHASE_MOVING;
		vCoffee3DeviceSetRobotPhase(COFFEE3_ROBOT_PHASE_MOVING);
	}
	return MODBUS_PORT_RESULT_OK;
}

/*-----------------------------------------------------------*/
/**
  * @brief 清除链路健康计数并要求 TCP 会话进入重连。
  * @param[in,out] pxSession 机器人 TCP 会话。
  * @param[in] lReason 触发重连的错误原因。
  */
static void prvDisconnectRobot(TcpClientSession_t *pxSession,
	int32_t lReason)
{
	prvResetRobotLinkHealth();
	vTcpClientSessionForceReconnect(pxSession, lReason);
}

/*-----------------------------------------------------------*/
/**
  * @brief 执行刷新、本体控制或点位动作的准备与首次发布。
  * @param[in,out] pxPort 已初始化的机器人 Modbus 端口。
  * @param[in] pxCommand 要执行的业务命令，不得为空。
  * @param[in,out] pxTransaction 点位动作事务，可为空。
  * @param[out] pucActionTimedOut 可为空；准备重试耗尽时由调用方设置。
  * @retval MODBUS_PORT_RESULT_BUSY 点位命令已发送或已安排准备重试。
  * @retval MODBUS_PORT_RESULT_OK 刷新或本体控制已完成。
  * @retval 其他值 参数、就绪、协议或 Modbus 操作失败。
  */
static ModbusPortResult_e prvExecute(ModbusPort_t *pxPort,
	const Coffee3Command_t *pxCommand,
	Coffee3RobotTransaction_t *pxTransaction,
	uint8_t *pucActionTimedOut)
{
	Coffee3DobotCommand_t xResolvedCommand; /*!< 产品动作解析后的协议目标。 */
	ModbusPortResult_e xResult; /*!< 当前准备或发布步骤的 Modbus 结果。 */
	bool bResult; /*!< 目标结果线圈的回读值。 */
	uint16_t usCommandCoil; /*!< 已解析的动作命令线圈。 */
	uint16_t usResultCoil; /*!< 已解析的动作结果线圈。 */
	uint8_t ucActionResolved; /*!< 非零表示已解析为点位动作。 */
	uint8_t ucStrictReady; /*!< 当前快照是否满足点位动作严格就绪条件。 */

	if (pucActionTimedOut != NULL) {
		*pucActionTimedOut = 0U;
	}

	/* 第一步：刷新命令直接更新快照，本体控制不受程序运行状态限制。 */
	if (pxCommand->usAction == COFFEE3_ACTION_REFRESH) {
		return prvRefresh(pxPort, COFFEE3_ROBOT_IO_TIMEOUT_MS);
	}
	/* 本体控制必须在自定义协议程序停止时仍可使用。 */
	if ((prvResolveCoffee3Dobot(pxCommand, &xResolvedCommand) != 0U) &&
		(xResolvedCommand.xKind == COFFEE3_DOBOT_COMMAND_BODY)) {
		return prvWriteRisingEdge(pxPort, xResolvedCommand.xBodyCommand,
			COFFEE3_ROBOT_IO_TIMEOUT_MS);
	}
	/* 第二步：解析点位线圈并建立事务，但此时尚未向机器人发布动作。 */
	ucActionResolved = 0U;
	if (prvResolveCoffee3Dobot(
		pxCommand, &xResolvedCommand) != 0U) {
		if (xResolvedCommand.xKind == COFFEE3_DOBOT_COMMAND_ACTION) {
			usCommandCoil = xResolvedCommand.usCommandCoil;
			usResultCoil = xResolvedCommand.usResultCoil;
			ucActionResolved = 1U;
		}
	}
	if (ucActionResolved != 0U) {
		prvBeginActionTransaction(pxTransaction, pxCommand, usCommandCoil,
			usResultCoil);
	}
	/* 第三步：刷新状态并检查严格就绪；失败只在准备阶段有限重试。 */
	xResult = prvRefresh(pxPort, COFFEE3_ROBOT_IO_TIMEOUT_MS);
	if (xResult != MODBUS_PORT_RESULT_OK) {
		return prvSchedulePrepareRetry(pxTransaction, xResult);
	}
	ucStrictReady = prvRobotStrictReady();
	if (ucStrictReady == 0U) {
		(void)xCoffee3LogWriteFieldOrder(COFFEE3_LOG_LEVEL_WARNING,
			COFFEE3_LOG_SOURCE_ROBOT, (uint16_t)pxCommand->ulOrderId,
			"ROBOT_COMMAND_NOT_READY",
			MODBUS_PORT_RESULT_PROTOCOL, "action",
			(int32_t)pxCommand->usAction);
		return prvSchedulePrepareRetry(pxTransaction, MODBUS_PORT_RESULT_PROTOCOL);
	}
	if (ucActionResolved == 0U) {
		if (pxCommand->usAction == COFFEE3_ACTION_ROBOT_START_SIGNAL) {
			return prvWriteControlValue(pxPort, DOBOT_ROBOT_P1_COMMAND_START_SIGNAL,
				(pxCommand->ausParameter[0U] != 0U), COFFEE3_ROBOT_IO_TIMEOUT_MS);
		}
		return MODBUS_PORT_RESULT_NOT_SUPPORTED;
	}
	/* 第四步：在清线圈前发布出杯口或暂存位选择。 */
	if (pxCommand->usAction == COFFEE3_ACTION_ROBOT_PUT_OUTPUT) {
		vCoffee3ServerSelectOutlet();
		(void)xCoffee3LogPrintfOrder(COFFEE3_LOG_LEVEL_INFO,
			COFFEE3_LOG_SOURCE_ROBOT,
			(uint16_t)pxCommand->ulOrderId,
			"ROBOT_OUTPUT_ROUTE_PUBLISHED output=1 route_reg=0x000A selector_reg=0x0033");
	}
	if ((pxCommand->usAction == COFFEE3_ACTION_ROBOT_PUT_STORAGE) ||
		(pxCommand->usAction == COFFEE3_ACTION_ROBOT_TAKE_STORAGE)) {
		if ((pxCommand->ausParameter[0] < 1U) ||
			(pxCommand->ausParameter[0] > 2U)) {
			return MODBUS_PORT_RESULT_INVALID_ARG;
		}
		vCoffee3ServerSelectStorage(pxCommand->ausParameter[0]);
	}
	/* 第五步：清除旧动作和旧结果，回读确认后才允许写新命令。 */
	xResult = prvClearActionCoils(pxPort, NULL,
		(uint16_t)pxCommand->ulOrderId);
	if (xResult != MODBUS_PORT_RESULT_OK) {
		return prvSchedulePrepareRetry(pxTransaction, xResult);
	}
	xResult = xModbusPortReadCoils(pxPort, COFFEE3_ROBOT_UNIT_ID,
		usResultCoil, 1U, &bResult, COFFEE3_ROBOT_IO_TIMEOUT_MS);
	if (xResult != MODBUS_PORT_RESULT_OK) {
		return prvSchedulePrepareRetry(pxTransaction, xResult);
	}
	if (bResult) {
		xResult = xModbusPortWriteCoil(pxPort, COFFEE3_ROBOT_UNIT_ID,
			usResultCoil, false, COFFEE3_ROBOT_IO_TIMEOUT_MS);
		if (xResult != MODBUS_PORT_RESULT_OK) {
			return prvSchedulePrepareRetry(pxTransaction, xResult);
		}
		xResult = xModbusPortReadCoils(pxPort, COFFEE3_ROBOT_UNIT_ID,
			usResultCoil, 1U, &bResult, COFFEE3_ROBOT_IO_TIMEOUT_MS);
		if (xResult != MODBUS_PORT_RESULT_OK) {
			return prvSchedulePrepareRetry(pxTransaction, xResult);
		}
		if (bResult) {
			return prvSchedulePrepareRetry(pxTransaction,
				MODBUS_PORT_RESULT_PROTOCOL);
		}
	}
	(void)xCoffee3LogWriteFieldOrder(COFFEE3_LOG_LEVEL_INFO,
		COFFEE3_LOG_SOURCE_ROBOT, (uint16_t)pxCommand->ulOrderId,
		"ROBOT_ACTION_PREPARED", 0,
		"coil", (int32_t)usCommandCoil);
	/* 第六步：写命令前锁定接受期限；写入尝试后禁止任何重发。 */
	if (pxTransaction != NULL) {
		pxTransaction->ucCommandWriteAttempted = 1U;
		pxTransaction->xAcceptDeadline = xTaskGetTickCount() +
			pdMS_TO_TICKS(COFFEE3_ROBOT_ACCEPT_TIMEOUT_MS);
	}
	xResult = xModbusPortWriteCoil(pxPort, COFFEE3_ROBOT_UNIT_ID,
		usCommandCoil, true, COFFEE3_ROBOT_IO_TIMEOUT_MS);
	if (xResult == MODBUS_PORT_RESULT_OK) {
		(void)xCoffee3LogWriteFieldOrder(COFFEE3_LOG_LEVEL_INFO,
			COFFEE3_LOG_SOURCE_ROBOT, (uint16_t)pxCommand->ulOrderId,
			"ROBOT_ACTION_SENT", 0,
			"coil", (int32_t)usCommandCoil);
		if (pxTransaction != NULL) {
			pxTransaction->ucCommandWriteConfirmed = 1U;
			pxTransaction->xNextPollTick = xTaskGetTickCount();
			pxTransaction->xPhase = COFFEE3_ROBOT_PHASE_WAIT_ACCEPT;
			vCoffee3DeviceSetRobotPhase(
				COFFEE3_ROBOT_PHASE_WAIT_ACCEPT);
			vCoffee3DeviceSetRobotAccepted(0U);
			return MODBUS_PORT_RESULT_BUSY;
		}
	}
	return xResult;
}

/*-----------------------------------------------------------*/
/**
  * @brief 在机器人控制线圈上产生一次低到高的触发沿。
  * @param[in,out] pxPort 已初始化的机器人 Modbus 端口。
  * @param[in] usCoil 要脉冲触发的线圈地址。
  * @param[in] ulTimeoutMs 每次写操作超时，单位为毫秒。
  * @retval ModbusPortResult_e 写低或写高步骤的结果。
  */
static ModbusPortResult_e prvWriteRisingEdge(ModbusPort_t *pxPort,
	uint16_t usCoil, uint32_t ulTimeoutMs)
{
	ModbusPortResult_e xResult; /*!< 写低和随后写高的综合结果。 */

	xResult = xModbusPortWriteCoil(pxPort, COFFEE3_ROBOT_UNIT_ID,
		usCoil, false, ulTimeoutMs);
	if (xResult == MODBUS_PORT_RESULT_OK) {
		vTaskDelay(pdMS_TO_TICKS(COFFEE3_ROBOT_EDGE_LOW_MS));
		xResult = xModbusPortWriteCoil(pxPort,
			COFFEE3_ROBOT_UNIT_ID, usCoil, true, ulTimeoutMs);
	}
	return xResult;
}

/*-----------------------------------------------------------*/
/**
  * @brief 推进动作的准备重试、等待接受、运动和结果清零状态机。
  * @param[in,out] pxPort 已初始化的机器人 Modbus 端口。
  * @param[in,out] pxTransaction 当前活动动作事务。
  * @param[out] pucActionTimedOut 可为空；准备阶段耗尽时置为 1。
  * @retval MODBUS_PORT_RESULT_BUSY 动作仍在准备或等待机器人。
  * @retval MODBUS_PORT_RESULT_OK 结果线圈已清零，动作完成。
  * @retval MODBUS_PORT_RESULT_CANCELED 命令已被上位业务取消。
  * @retval 其他值 参数、协议或 Modbus 操作失败。
  * @note 接受或运动逾期通常只标记并继续等待，不立即判定失败。
  */
static ModbusPortResult_e prvAdvanceAction(ModbusPort_t *pxPort,
	Coffee3RobotTransaction_t *pxTransaction, uint8_t *pucActionTimedOut)
{
	/* 依次推进等待接受、运动、清结果，并在最后回读确认完成。 */
	ModbusPortResult_e xResult; /*!< 当前状态读取或写入结果。 */
	bool bCommand; /*!< 动作命令线圈当前值。 */
	bool bResult; /*!< 动作结果线圈当前值。 */
	bool bResultZero; /*!< 清除结果线圈后的回读值。 */
	TickType_t xNow; /*!< 当前系统节拍，用于接受期限和日志节流。 */

	if ((pxTransaction == NULL) || (pxTransaction->ucActive == 0U)) {
		return MODBUS_PORT_RESULT_INVALID_ARG;
	}
	if (pucActionTimedOut != NULL) {
		*pucActionTimedOut = 0U;
	}
	if (ucCoffee3CommandIsCanceled(&pxTransaction->xCommand) != 0U) {
		return MODBUS_PORT_RESULT_CANCELED;
	}
	/* 第一步：命令尚未写出时，按预定节拍执行有限准备重试。 */
	if (pxTransaction->ucCommandWriteAttempted == 0U) {
		if ((int32_t)(xTaskGetTickCount() -
			pxTransaction->xNextPrepareRetryTick) < 0) {
			return MODBUS_PORT_RESULT_BUSY;
		}
		xResult = prvExecute(pxPort, &pxTransaction->xCommand,
			pxTransaction, pucActionTimedOut);
		if ((xResult == MODBUS_PORT_RESULT_TIMEOUT) &&
			(pxTransaction->ucCommandWriteAttempted == 0U) &&
			(pucActionTimedOut != NULL)) {
			*pucActionTimedOut = 1U;
		}
		return xResult;
	}
	/* 第二步：等待机器人清除命令线圈，以此确认接受动作。 */
	if (pxTransaction->xPhase == COFFEE3_ROBOT_PHASE_WAIT_ACCEPT) {
		xResult = xModbusPortReadCoils(pxPort, COFFEE3_ROBOT_UNIT_ID,
			pxTransaction->usCommandCoil, 1U, &bCommand,
			COFFEE3_ROBOT_IO_TIMEOUT_MS);
		if (xResult != MODBUS_PORT_RESULT_OK) {
			return xResult;
		}
		xResult = xModbusPortReadCoils(pxPort, COFFEE3_ROBOT_UNIT_ID,
			pxTransaction->usResultCoil, 1U, &bResult,
			COFFEE3_ROBOT_IO_TIMEOUT_MS);
		if (xResult != MODBUS_PORT_RESULT_OK) {
			return xResult;
		}
		xNow = xTaskGetTickCount();
		if ((bCommand == false) &&
			!((pxTransaction->ucAmbiguous != 0U) &&
				(pxTransaction->ucAccepted == 0U) &&
				(bResult == false))) {
			if (pxTransaction->ucAccepted == 0U) {
				pxTransaction->ucAccepted = 1U;
				pxTransaction->xAcceptedTick = xNow;
				pxTransaction->xMotionDeadline = xNow +
					pdMS_TO_TICKS(COFFEE3_ROBOT_MOTION_TIMEOUT_MS);
				vCoffee3DeviceSetRobotAccepted(1U);
				vCoffee3DeviceSetRobotPhase(
					COFFEE3_ROBOT_PHASE_MOVING);
				(void)xCoffee3LogWriteFieldOrder(COFFEE3_LOG_LEVEL_INFO,
					COFFEE3_LOG_SOURCE_ROBOT,
					(uint16_t)pxTransaction->xCommand.ulOrderId,
					"ROBOT_ACTION_ACCEPTED", 0,
					"coil", (int32_t)pxTransaction->usCommandCoil);
				(void)xCoffee3LogWriteFieldOrder(COFFEE3_LOG_LEVEL_INFO,
					COFFEE3_LOG_SOURCE_ROBOT,
					(uint16_t)pxTransaction->xCommand.ulOrderId,
					"ROBOT_ACTION_MOVING", 0,
					"action", (int32_t)pxTransaction->xCommand.usAction);
			}
			if (bResult != false) {
				pxTransaction->ucCompletionObserved = 1U;
				(void)xCoffee3LogWriteFieldOrder(COFFEE3_LOG_LEVEL_INFO,
					COFFEE3_LOG_SOURCE_ROBOT,
					(uint16_t)pxTransaction->xCommand.ulOrderId,
					"ROBOT_ACTION_COMPLETE_SIGNAL", 0,
					"coil", (int32_t)pxTransaction->usResultCoil);
				pxTransaction->xPhase =
					COFFEE3_ROBOT_PHASE_CLEAR_RESULT;
			} else {
				pxTransaction->xPhase =
					COFFEE3_ROBOT_PHASE_MOVING;
			}
			return MODBUS_PORT_RESULT_BUSY;
		}
		if ((bCommand != false) && (bResult != false) &&
			(pxTransaction->ucResultWhileCommandHigh == 0U)) {
			pxTransaction->ucResultWhileCommandHigh = 1U;
			(void)xCoffee3LogWriteFieldOrder(COFFEE3_LOG_LEVEL_WARNING,
				COFFEE3_LOG_SOURCE_ROBOT,
				(uint16_t)pxTransaction->xCommand.ulOrderId,
				"ROBOT_ACTION_PROTOCOL_SEQUENCE",
				MODBUS_PORT_RESULT_PROTOCOL, "action",
				(int32_t)pxTransaction->xCommand.usAction);
		}
		if ((int32_t)(xNow - pxTransaction->xAcceptDeadline) >= 0) {
			prvMarkActionOverdue(pxTransaction, "accept");
			return MODBUS_PORT_RESULT_BUSY;
		}
		if ((pxTransaction->ucOverdue == 0U) &&
			((pxTransaction->xLastAcceptLogTick == 0U) ||
			((xNow - pxTransaction->xLastAcceptLogTick) >=
				pdMS_TO_TICKS(COFFEE3_ROBOT_ACCEPT_LOG_INTERVAL_MS)))) {
			pxTransaction->xLastAcceptLogTick = xNow;
			(void)xCoffee3LogWriteFieldOrder(COFFEE3_LOG_LEVEL_INFO,
				COFFEE3_LOG_SOURCE_ROBOT,
				COFFEE3_LOG_ORDER_SYSTEM,
				"ROBOT_ACTION_ACCEPT_WAITING", 0,
				"coil", (int32_t)pxTransaction->usCommandCoil);
		}
		return MODBUS_PORT_RESULT_BUSY;
	}
	/* 第三步：运动期间只等待结果线圈；逾期后仍保持事务活动。 */
	if (pxTransaction->xPhase == COFFEE3_ROBOT_PHASE_MOVING) {
		xResult = xModbusPortReadCoils(pxPort, COFFEE3_ROBOT_UNIT_ID,
			pxTransaction->usResultCoil, 1U, &bResult,
			COFFEE3_ROBOT_IO_TIMEOUT_MS);
		if (xResult != MODBUS_PORT_RESULT_OK) {
			return xResult;
		}
		if (bResult) {
			pxTransaction->ucCompletionObserved = 1U;
			(void)xCoffee3LogWriteFieldOrder(COFFEE3_LOG_LEVEL_INFO,
				COFFEE3_LOG_SOURCE_ROBOT,
				(uint16_t)pxTransaction->xCommand.ulOrderId,
				"ROBOT_ACTION_COMPLETE_SIGNAL", 0,
				"coil", (int32_t)pxTransaction->usResultCoil);
			pxTransaction->xPhase = COFFEE3_ROBOT_PHASE_CLEAR_RESULT;
			vCoffee3DeviceSetRobotPhase(
				COFFEE3_ROBOT_PHASE_CLEAR_RESULT);
			return MODBUS_PORT_RESULT_BUSY;
		}
		if ((int32_t)(xTaskGetTickCount() -
			pxTransaction->xMotionDeadline) >= 0) {
			prvMarkActionOverdue(pxTransaction, "motion");
			return MODBUS_PORT_RESULT_BUSY;
		}
		return MODBUS_PORT_RESULT_BUSY;
	}
	/* 第四步：清除并回读结果线圈，确认后才返回动作完成。 */
	if (pxTransaction->xPhase == COFFEE3_ROBOT_PHASE_CLEAR_RESULT) {
		xResult = xModbusPortWriteCoil(pxPort, COFFEE3_ROBOT_UNIT_ID,
			pxTransaction->usResultCoil, false,
			COFFEE3_ROBOT_IO_TIMEOUT_MS);
		if (xResult != MODBUS_PORT_RESULT_OK) {
			return xResult;
		}
		xResult = xModbusPortReadCoils(pxPort, COFFEE3_ROBOT_UNIT_ID,
			pxTransaction->usResultCoil, 1U, &bResultZero,
			COFFEE3_ROBOT_IO_TIMEOUT_MS);
		if (xResult != MODBUS_PORT_RESULT_OK) {
			return xResult;
		}
		if (bResultZero) {
			return MODBUS_PORT_RESULT_PROTOCOL;
		}
		(void)xCoffee3LogWriteFieldOrder(COFFEE3_LOG_LEVEL_INFO,
			COFFEE3_LOG_SOURCE_ROBOT,
			(uint16_t)pxTransaction->xCommand.ulOrderId,
			"ROBOT_ACTION_RESULT_CLEARED", 0,
			"coil", (int32_t)pxTransaction->usResultCoil);
		(void)xCoffee3LogWriteFieldOrder(COFFEE3_LOG_LEVEL_INFO,
			COFFEE3_LOG_SOURCE_ROBOT,
			(uint16_t)pxTransaction->xCommand.ulOrderId,
			"ROBOT_ACTION_COMPLETE", 0,
			"action", (int32_t)pxTransaction->xCommand.usAction);
		return MODBUS_PORT_RESULT_OK;
	}
	return MODBUS_PORT_RESULT_BUSY;
}

/*-----------------------------------------------------------*/







