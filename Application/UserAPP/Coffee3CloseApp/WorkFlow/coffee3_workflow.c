/**
  * @file      coffee3_workflow.c
  * @brief     实现 Coffee3 订单状态机、维护流程与故障收敛路径。
  * @author    WHong
  * @date      2026-09-24
  */

#include "coffee3_workflow.h"
#include "coffee3_config.h"

#include <stddef.h>
#include <string.h>

#include "coffee3_device.h"
#include "coffee3_device_image.h"
#include "coffee3_io.h"
#include "coffee3_io_names.h"
#include "coffee3_log.h"
#include "coffee3_server.h"
#include "coffee3_robot_tcp.h"
#include "queue.h"
#include "task.h"

/** @brief 工作流层统一使用的内部故障码。 */
#define COFFEE3_WORKFLOW_ERROR_QUEUE          (-1001)
#define COFFEE3_WORKFLOW_ERROR_TIMEOUT        (-1002)
#define COFFEE3_WORKFLOW_ERROR_DEVICE         (-1003)
#define COFFEE3_WORKFLOW_ERROR_CANCELED       (-1004)
#define COFFEE3_WORKFLOW_ERROR_ICE_WEIGHT     (-1006)
#define COFFEE3_WORKFLOW_ERROR_ICE_RANGE      (-1011)
#define COFFEE3_WORKFLOW_ERROR_ICE_NO_CUP     (-1012)
#define COFFEE3_WORKFLOW_ERROR_ICE_BASELINE   (-1013)
#define COFFEE3_WORKFLOW_ERROR_SAFE_STOP      (-1007)
#define COFFEE3_WORKFLOW_ERROR_UNSUPPORTED    (-1008)
#define COFFEE3_WORKFLOW_ERROR_INIT_OCCUPIED  (-1009)
#define COFFEE3_WORKFLOW_ERROR_IO             (-1010)

/** @brief 每个安全停止命令等待终态确认的最长时间，单位为毫秒。 */
#define COFFEE3_WORKFLOW_SAFE_STOP_MS          5000U
#define COFFEE3_WORKFLOW_ROBOT_MOTION_MS       60000U
#define COFFEE3_RESIDUAL_SETTLE_MS              800U
#define COFFEE3_RESIDUAL_SAMPLE_GAP_MS          200U
#define COFFEE3_CONDITION_CUP_1                1U
#define COFFEE3_CONDITION_CUP_2                2U
#define COFFEE3_CONDITION_LID_1                3U
#define COFFEE3_CONDITION_LID_2                4U
#define COFFEE3_CONDITION_OUTPUT_1             5U
#define COFFEE3_CONDITION_OUTPUT_2             6U
#define COFFEE3_CONDITION_STORAGE_1            7U
#define COFFEE3_CONDITION_STORAGE_2            8U

/** @brief 热水服务从补水、加热到关闭确认的内部阶段。 */
typedef enum {
	COFFEE3_HOT_WATER_IDLE = 0, /*!< 热水流程空闲。 */
	COFFEE3_HOT_WATER_PREPARE_OFF = 1, /*!< 先确认加热输出关闭。 */
	COFFEE3_HOT_WATER_FILLING = 2, /*!< 补水阀打开，等待液位或超时。 */
	COFFEE3_HOT_WATER_WAIT_HEATER_ON = 3, /*!< 等待加热开启命令终态。 */
	COFFEE3_HOT_WATER_HEATING = 4, /*!< 加热输出开启，按设定时间计时。 */
	COFFEE3_HOT_WATER_WAIT_OFF_DONE = 5, /*!< 正常结束或取消后等待关闭终态。 */
	COFFEE3_HOT_WATER_WAIT_OFF_ALARM = 6 /*!< 故障后等待关闭终态。 */
} Coffee3HotWaterPhase_e;

/** @brief 热水服务的跨周期命令、计时与告警状态。 */
typedef struct {
	Coffee3Command_t xCommand; /*!< 待确认的外部加热输出命令。 */
	TickType_t xStartTick; /*!< 当前补水或加热阶段的起始节拍。 */
	uint16_t usHeatMinutes; /*!< 本次加热时长，单位为分钟。 */
	uint8_t ucPhase; /*!< 当前阶段，取值见 Coffee3HotWaterPhase_e。 */
	uint8_t ucIoPending; /*!< 外部输出命令是否等待终态。 */
	uint8_t ucCancelRequested; /*!< 停止请求，非零时关闭补水及加热。 */
	uint8_t ucAlarmReason; /*!< 告警原因码；零表示未设置。 */
} Coffee3HotWaterContext_t;

/** @brief 等待工作流任务领取的一份非订单维护请求。 */
typedef struct {
	Coffee3MaintenanceType_e xType; /*!< 维护类型。 */
	uint16_t usParameter0; /*!< 咖啡参数或果奶通道编号。 */
	uint16_t usParameter1; /*!< 果奶定量出料量；其他类型当前未使用。 */
	uint8_t ucPending; /*!< 非零表示请求尚未被工作流任务领取。 */
} Coffee3MaintenanceRequest_t;

/** @brief 工作流自有订单队列及其静态存储。 */
COFFEE3_CCM_DATA
static StaticQueue_t s_xOrderQueueStorage; /*!< 静态订单队列控制块。 */
COFFEE3_CCM_DATA
static uint8_t s_aucOrderQueueStorage[
	COFFEE3_WORKFLOW_QUEUE_LENGTH * sizeof(Coffee3Order_t)]; /*!< 队列数据区。 */
COFFEE3_CCM_DATA
static QueueHandle_t s_xOrderQueue; /*!< 订单队列句柄，空值表示尚未创建。 */
COFFEE3_CCM_DATA
static uint16_t s_usManualIceWeight; /*!< 待执行手动出冰的目标克重。 */
COFFEE3_CCM_DATA
static uint8_t s_ucManualIcePending; /*!< 非零表示手动出冰请求待领取。 */
COFFEE3_CCM_DATA
static Coffee3Order_t s_xPendingOrder; /*!< 与挂起标志配对的待回插订单快照。 */
COFFEE3_CCM_DATA
static uint8_t s_ucPendingOrder; /*!< 非零表示存在待回插订单；当前无置位路径。 */
COFFEE3_CCM_DATA
static uint32_t s_ulNextOrderEpoch; /*!< 本地订单代次计数器，溢出时回绕。 */
COFFEE3_CCM_DATA
static uint8_t s_ucManualOverride; /*!< 本订单机械臂命令是否被人工命令替代。 */
static uint16_t s_usManualReservations; /*!< 人工执行权预留计数，零表示无人持有。 */
static uint8_t s_ucOtaReserved; /*!< OTA 独占预留，同时占用一次人工预留。 */
static uint8_t s_ucMaintenanceActive; /*!< 维护、手动出冰或独立取餐执行中。 */
static uint8_t s_ucPickupPending; /*!< 主机取杯确认已锁存，等待流程消费。 */
static uint8_t s_ucStoragePickupPending; /*!< 待领取的储位取餐编号：0 无、1 或 2 为储位。 */
/* 暂存位取杯可与不占用机械臂的工作并行，但机械臂所有权始终互斥。
 * 活动阶段覆盖 TAKE_STORAGE 到 PUT_OUTPUT，确认阶段覆盖随后异步等待
 * X3、源暂存位传感器及出杯门联动的阶段。 */
static uint8_t s_ucStoragePickupActive; /*!< 储位取餐机械臂阶段是否占用中。 */
static uint8_t s_ucStoragePickupConfirmStorage; /*!< 等待物理确认的来源储位编号。 */
static uint16_t s_usStoragePickupConfirmOrderId; /*!< 异步取餐关联的订单号。 */
static uint16_t s_usDetachedPickupLogOrder; /*!< 独立取餐期间的日志订单号。 */
static TickType_t s_xStoragePickupConfirmStart; /*!< 物理确认开始的系统节拍。 */
static uint8_t s_ucStoragePickupConfirmOverdueLogged; /*!< 确认逾期日志是否已记。 */
static uint16_t s_ausStoredOrderId[2]; /*!< 两个储位分别关联的订单号。 */
static uint8_t s_ucWaterAlarm; /*!< 外部纯水输入异常告警锁存。 */
static uint8_t s_ucCoffeeFillActive; /*!< 咖啡机补水泵是否正在运行。 */
static uint8_t s_ucCoffeeFillAlarm; /*!< 咖啡机补水超时告警锁存。 */
static TickType_t s_xCoffeeFillStart; /*!< 本次咖啡补水开始的系统节拍。 */
static uint8_t s_ucOutletPhase; /*!< 出杯口阶段：0 空闲、1 待有杯、2 待清空、3 待确认。 */
static uint8_t s_ucDoorDirection; /*!< 门方向：0 停止、1 上升关门、2 下降开门。 */
static uint8_t s_ucDoorFault; /*!< 门限位冲突或运动超时故障锁存。 */
static TickType_t s_xDoorStart; /*!< 本次门运动开始的系统节拍。 */
static uint8_t s_ucDoorDebugPending; /*!< 门调试请求是否等待领取。 */
static uint8_t s_ucDoorDebugDirection; /*!< 待执行的门调试方向。 */
static uint8_t s_ucDoorDebugActive; /*!< 当前门运动是否来自调试请求。 */
static uint8_t s_ucDoorDebugActiveDirection; /*!< 正在执行的门调试方向。 */
static TickType_t s_xEmptyStart; /*!< 出杯口连续无杯计时的起始节拍。 */
static uint8_t s_ucEmptyTiming; /*!< 出杯口连续无杯计时是否进行中。 */
static uint16_t s_usActiveDevices; /*!< 已启动设备位掩码，用于选择安全停止对象。 */
COFFEE3_CCM_DATA
static Coffee3HotWaterContext_t s_xHotWater; /*!< 热水跨周期状态机上下文。 */
COFFEE3_CCM_DATA
static Coffee3MaintenanceRequest_t s_xMaintenance; /*!< 单槽维护请求。 */
COFFEE3_CCM_DATA
static uint8_t s_ucInitializationAcknowledged; /*!< 初始化确认调用标志，目前无读取方。 */
COFFEE3_CCM_DATA
static uint8_t s_ucInitializationComplete; /*!< 当前基础生产条件是否就绪。 */
static uint8_t s_ucResidualState; /*!< 残杯阶段：0 未开始、1 执行中、2 成功、3 失败。 */
static uint8_t s_ucDoorInitStarted; /*!< 初始化关门动作是否已发起。 */
static uint8_t s_ucDoorExpectedLimit; /*!< 最近门动作目标限位：1 上、2 下。 */
static Coffee3Command_t s_xInitHome; /*!< 预留的初始化回原命令；当前仅检查命令号。 */
static uint16_t s_usLastReadyMask; /*!< 上轮设备就绪位掩码，用于识别变化。 */
static uint8_t s_ucLastBaseReady; /*!< 上轮基础生产条件是否就绪。 */
static int32_t s_lIceScaleEmptyBaselineGram; /*!< 冰秤空秤重量基线，单位为克。 */
static TickType_t s_xIceScaleBaselineRetryTick; /*!< 冰秤基线失败后的重试节拍。 */
static uint8_t s_ucIceScaleBaselineValid; /*!< 冰秤空秤基线是否有效。 */

COFFEE3_CCM_DATA
Coffee3WorkflowStatus_t g_xCoffee3WorkflowStatus; /*!< 对外发布的工作流业务状态。 */

static int32_t prvRunStep(uint16_t usStep, Coffee3DeviceId_e xDeviceId,
	Coffee3Action_e xAction, uint16_t usParameter0,
	uint16_t usParameter1, uint32_t ulTimeoutMs);
static int32_t prvRunOrder(const Coffee3Order_t *pxOrder);
static int32_t prvDispenseIce(uint16_t usTargetGram,
	int32_t lCupBaselineGram);
static int32_t prvConfirmIceCup(int32_t lEmptyBaselineGram,
	uint16_t usStepBase, int32_t *plCupBaselineGram);
static uint32_t prvCalculateIcePulseMs(uint16_t usTargetGram,
	uint16_t usSlopeMsPerGram, uint8_t ucAttempt);
static int32_t prvReadStableScale(uint16_t usStepBase,
	int32_t *plWeightGram);
static int32_t prvAbortDevices(void);
static int32_t prvRunInitialization(void);
static int32_t prvProbeResidualCup(uint16_t usStepBase,
	Coffee3Action_e xPickupAction, uint8_t ucOccupiedPoint);
static int32_t prvRunMaintenance(const Coffee3MaintenanceRequest_t *pxRequest);
static int32_t prvRunFruit(uint8_t ucChannel, uint16_t usAmountMl,
	uint8_t ucClean, uint16_t usStepBase);
static int32_t prvRunIoOutput(uint16_t usStep, uint8_t ucPoint,
	uint8_t ucValue);
static int32_t prvSetProductOutputsOff(void);
static void prvServiceHotWater(void);
static BaseType_t prvSubmitHotWaterIo(uint8_t ucValue);
static int32_t prvPollHotWaterIo(uint8_t *pucDone);
static void prvSetHotWaterPublicState(uint8_t ucState,
	const char *pcEvent, int32_t lResult);
static uint8_t prvOrderValid(const Coffee3Order_t *pxOrder,
	int32_t *plError);
static void prvDelayWithServices(uint32_t ulDelayMs);
static int32_t prvWaitBusinessCondition(uint16_t usStep,
	uint8_t ucCondition);
static int32_t prvRefreshDeviceQuiet(uint16_t usStep,
	Coffee3DeviceId_e xDeviceId);
static uint8_t prvBusinessConditionActive(uint8_t ucCondition);
static const char *prvWorkflowDeviceName(Coffee3DeviceId_e xDeviceId);
static const char *prvWorkflowActionName(Coffee3Action_e xAction);
static int32_t prvWaitDeviceReportedComplete(uint16_t usStep,
	Coffee3DeviceId_e xDeviceId, uint8_t ucStatusIndex,
	uint16_t usSuccessValue, uint16_t usFailedValue);
static void prvServiceIoRefresh(void);
static uint16_t s_usLastInitializationFailure = 0xFFFFU; /*!< 最近失败步骤，0xFFFF 表示尚未记录。 */
static void prvPublish(Coffee3WorkflowState_e xState,
	uint16_t usStep, int32_t lError);
static uint8_t prvQueuePendingOrder(void);
static uint8_t prvRobotPositionAction(Coffee3Action_e xAction);
static void prvServicePickup(void);
static void prvPublishOutput(uint16_t usOutput, uint16_t usState);
static void prvPublishOutputForOrder(uint16_t usOutput, uint16_t usState,
	uint16_t usOrderId);
static int32_t prvCheckOutputEmpty(uint16_t usOutput);
static int32_t prvWaitOutputAvailableForOrder(void);
static int32_t prvSelectStorage(void);
static int32_t prvRunStoragePickup(uint16_t usStorage);
static int32_t prvTryRunInterleavedStoragePickup(void);
static uint16_t prvStorageStatusMask(const Coffee3IoState_t *pxIo);
static void prvRejectStoragePickup(uint16_t usStorage,
	const char *pcReason, uint16_t usStatusMask);
static int32_t prvWaitM50Clean(void);
static uint8_t prvIoValid(const Coffee3IoState_t *pxIo);
static void prvStartDoor(uint8_t ucDirection);
static void prvServiceInitialization(void);
static uint16_t prvReadyDevices(void);
static uint8_t prvOrderDevicesReady(const Coffee3Order_t *pxOrder);
static uint8_t prvMaintenanceReady(Coffee3MaintenanceType_e xType);
static uint8_t prvMaintenanceIdle(void);
static void prvServiceIceScaleBaseline(uint16_t usReadyMask);
static int32_t prvEstablishIceScaleBaseline(void);
static uint8_t prvIceTerminalFailure(int32_t lResult);

/*-----------------------------------------------------------*/
/**
  * @brief 为人工命令预留执行权，成功后须调用释放接口。
  * @retval pdPASS 当前业务允许人工操作且预留计数已增加。
  * @retval pdFAIL 存在订单、维护、OTA、取餐或恢复等冲突。
  */
BaseType_t xCoffee3WorkflowAcquireManual(void)
{
	BaseType_t xResult; /*!< 本次人工操作预留是否成功。 */

	xResult = pdFAIL;
	taskENTER_CRITICAL();
	if ((s_ucOtaReserved == 0U) && (s_ucCoffeeFillActive == 0U) &&
		(s_ucDoorDirection == 0U) &&
		(g_xCoffee3WorkflowStatus.ucRecoveryRequired == 0U) &&
		(g_xCoffee3WorkflowStatus.xMachineState !=
		COFFEE3_MACHINE_INITIALIZING) &&
		(g_xCoffee3WorkflowStatus.xState != COFFEE3_WORKFLOW_RUNNING) &&
		(g_xCoffee3WorkflowStatus.xState != COFFEE3_WORKFLOW_CANCELING) &&
		(s_ucOutletPhase == 0U) && (s_ucStoragePickupPending == 0U) &&
		(s_ucStoragePickupActive == 0U) &&
		(s_ucStoragePickupConfirmStorage == 0U) &&
		(s_ucMaintenanceActive == 0U) && (s_xMaintenance.ucPending == 0U) &&
		(s_ucManualIcePending == 0U) &&
		(s_xHotWater.ucPhase == COFFEE3_HOT_WATER_IDLE) &&
		(s_xHotWater.ucIoPending == 0U) &&
		((g_xCoffee3WorkflowStatus.ucOrderAdmissionOpen != 0U) ||
		 ((s_ucInitializationComplete == 0U) &&
		  (g_xCoffee3WorkflowStatus.xMachineState == COFFEE3_MACHINE_ALARM)))) {
		s_usManualReservations++;
		xResult = pdPASS;
	}
	taskEXIT_CRITICAL();
	return xResult;
}

/*-----------------------------------------------------------*/
/**
  * @brief 为可延后执行的机器人调试动作预留人工操作权。
  * @retval pdPASS 已建立预留；实际发往机器人前还需检查派发条件。
  * @retval pdFAIL 初始化、取消、OTA 或恢复状态不允许预留。
  */
BaseType_t xCoffee3WorkflowAcquireDeferredManual(void)
{
	BaseType_t xResult; /*!< 本次延后派发预留是否成功。 */

	xResult = pdFAIL;
	taskENTER_CRITICAL();
	if ((s_ucInitializationComplete != 0U) &&
		(s_ucOtaReserved == 0U) &&
		(g_xCoffee3WorkflowStatus.ucRecoveryRequired == 0U) &&
		(g_xCoffee3WorkflowStatus.xMachineState != COFFEE3_MACHINE_INITIALIZING) &&
		(g_xCoffee3WorkflowStatus.xState != COFFEE3_WORKFLOW_CANCELING)) {
		s_usManualReservations++;
		xResult = pdPASS;
	}
	taskEXIT_CRITICAL();
	return xResult;
}

/*-----------------------------------------------------------*/
/** @brief 在任务上下文释放一次已取得的人工操作预留。 */
void vCoffee3WorkflowReleaseManual(void)
{
	taskENTER_CRITICAL();
	if (s_usManualReservations != 0U) {
		s_usManualReservations--;
	}
	taskEXIT_CRITICAL();
}

/**
  * @brief 在初始化完成、业务空闲且本地和外置输出均关闭时预留 OTA。
  * @retval pdPASS 已持有 OTA 预留，或本次成功取得预留。
  * @retval pdFAIL 输出状态或业务准入条件不满足。
  * @note   预留在升级复位或启动失败释放前持续持有。
  */
BaseType_t xCoffee3WorkflowAcquireOta(void)
{
	Coffee3IoState_t xIo; /*!< 当前读取的输入与输出状态快照。 */
	uint8_t ucIndex; /*!< 当前遍历项的下标。 */
	BaseType_t xResult; /*!< 本次请求或预留操作的成功状态。 */

	xResult = pdFAIL;
	if (s_ucOtaReserved != 0U) {
		return pdPASS;
	}
	vCoffee3IoRefreshLocal();
	vCoffee3IoGetSnapshot(&xIo);
	for (ucIndex = 0U; ucIndex < COFFEE3_LOCAL_IO_COUNT; ucIndex++) {
		if (xIo.xOutput.aucYPin[ucIndex] != 0U) {
			return pdFAIL;
		}
	}
	if ((xIo.aucModbusValid[1] == 0U) ||
		(g_axCoffee3DeviceStatus[COFFEE3_DEVICE_IO_OUTPUT].ucOnline == 0U)) {
		return pdFAIL;
	}
	for (ucIndex = 0U; ucIndex < COFFEE3_MODBUS_IO_COUNT; ucIndex++) {
		if (xIo.xOutput.aucMB2YPin[ucIndex] != 0U) {
			return pdFAIL;
		}
	}
	taskENTER_CRITICAL();
	if (s_ucOtaReserved != 0U) {
		xResult = pdPASS;
	} else if ((s_ucInitializationComplete != 0U) &&
		(s_usManualReservations == 0U) &&
		(xCoffee3WorkflowAcquireManual() == pdPASS)) {
		s_ucOtaReserved = 1U;
		xResult = pdPASS;
	}
	taskEXIT_CRITICAL();
	return xResult;
}

/** @brief OTA 启动失败时释放预留；正常升级期间由复位结束占用。 */
void vCoffee3WorkflowReleaseOta(void)
{
	taskENTER_CRITICAL();
	if (s_ucOtaReserved != 0U) {
		s_ucOtaReserved = 0U;
		vCoffee3WorkflowReleaseManual();
	}
	taskEXIT_CRITICAL();
}

/*-----------------------------------------------------------*/
/**
  * @brief 仅在出口 1 已进入待取餐阶段时锁存客户取走确认。
  * @param[in] usOutput 出口编号；当前仅接受 1。
  */
void vCoffee3WorkflowConfirmPickup(uint16_t usOutput)
{
	taskENTER_CRITICAL();
	if ((usOutput == 1U) && (s_ucOutletPhase == 3U)) {
		s_ucPickupPending = 1U;
	}
	taskEXIT_CRITICAL();
}

/*-----------------------------------------------------------*/
/**
  * @brief 检查已预留的机器人调试动作能否开始派发。
  * @retval 非零 当前人工预留及业务状态允许派发。
  * @retval 0 尚有互斥业务或恢复、取消、OTA 等限制。
  */
uint8_t ucCoffee3WorkflowManualDispatchAllowed(void)
{
	uint8_t ucAllowed; /*!< 当前业务前置条件是否允许继续。 */

	ucAllowed = 0U;
	taskENTER_CRITICAL();
	if ((s_usManualReservations != 0U) &&
		(s_ucInitializationComplete != 0U) &&
		(g_xCoffee3WorkflowStatus.ucRecoveryRequired == 0U) &&
		(g_xCoffee3WorkflowStatus.xState != COFFEE3_WORKFLOW_CANCELING) &&
		(s_ucOtaReserved == 0U) &&
		(s_ucStoragePickupActive == 0U) &&
		((g_xCoffee3WorkflowStatus.xState == COFFEE3_WORKFLOW_RUNNING) ||
		 ((s_ucMaintenanceActive == 0U) &&
		  (s_ucStoragePickupPending == 0U) &&
		  (s_xMaintenance.ucPending == 0U) &&
		  (s_ucManualIcePending == 0U) &&
		  (s_ucCoffeeFillActive == 0U) &&
		  (s_ucOutletPhase == 0U) &&
		  (s_xHotWater.ucPhase == COFFEE3_HOT_WATER_IDLE)))) {
		ucAllowed = 1U;
	}
	taskEXIT_CRITICAL();
	return ucAllowed;
}

/*-----------------------------------------------------------*/
/**
  * @brief 提交门调试方向，由工作流任务执行。
  * @param[in] ucDirection 0 停止，1 向上关门，2 向下开门。
  * @retval pdPASS 请求已接受。
  * @retval pdFAIL 方向无效或业务状态不允许。
  */
BaseType_t xCoffee3WorkflowSubmitDoorDebug(uint8_t ucDirection)
{
	BaseType_t xResult; /*!< 本次请求或预留操作的成功状态。 */

	if (ucDirection > 2U) {
		return pdFAIL;
	}
	xResult = pdFAIL;
	taskENTER_CRITICAL();
	if ((s_ucInitializationComplete != 0U) &&
		(s_ucDoorDebugPending == 0U)) {
		s_ucDoorDebugDirection = ucDirection;
		s_ucDoorDebugPending = 1U;
		xResult = pdPASS;
	}
	taskEXIT_CRITICAL();
	return xResult;
}

/*-----------------------------------------------------------*/

/**
  * @brief  汇总各设备所有者发布的在线、就绪与 IO 新鲜度状态。
  * @retval 位掩码 位号对应 Coffee3DeviceId_e，置位表示设备可参与流程。
  */
static uint16_t prvReadyDevices(void)
{
	Coffee3IoState_t xIo; /*!< 当前读取的输入与输出状态快照。 */
	uint16_t usMask; /*!< 已就绪设备的位掩码。 */
	uint8_t ucId; /*!< 当前检查的逻辑设备编号。 */
	vCoffee3IoGetSnapshot(&xIo);
	usMask = 0U;
	for (ucId = 1U; ucId < COFFEE3_DEVICE_COUNT; ucId++) {
		if ((g_axCoffee3DeviceStatus[ucId].ucOnline != 0U) &&
			(g_axCoffee3DeviceStatus[ucId].ucReady != 0U)) {
			usMask |= (uint16_t)(1U << ucId);
		}
	}
	/* IO 就绪位由 RTU 所有者的健康确认发布，机械臂的控制就绪位
	 * 不由 RTU 所有者代为发布。 */
	if ((g_axCoffee3DeviceStatus[COFFEE3_DEVICE_IO_INPUT].ucOnline == 0U) ||
		(prvIoValid(&xIo) == 0U)) {
		usMask &= (uint16_t)~(1U << COFFEE3_DEVICE_IO_INPUT);
	} else {
		usMask |= (uint16_t)(1U << COFFEE3_DEVICE_IO_INPUT);
	}
	if ((g_axCoffee3DeviceStatus[COFFEE3_DEVICE_IO_OUTPUT].ucOnline == 0U) ||
		(xIo.aucModbusValid[1] == 0U)) {
		usMask &= (uint16_t)~(1U << COFFEE3_DEVICE_IO_OUTPUT);
	} else {
		usMask |= (uint16_t)(1U << COFFEE3_DEVICE_IO_OUTPUT);
	}
	return usMask;
}

/**
  * @brief  检查维护请求所需的互斥条件。
  * @retval 1 残杯探测及回原命令未占用，维护、取餐、人工和 OTA 未占用，
  *           订单队列为空且工作流不在运行或取消中。
  * @retval 0 上述任一条件不满足。
  */
static uint8_t prvMaintenanceIdle(void)
{
	return ((s_ucResidualState != 1U) &&
		(s_xInitHome.ulCommandId == 0U) &&
		(s_ucMaintenanceActive == 0U) && (s_xMaintenance.ucPending == 0U) &&
		(s_ucManualIcePending == 0U) && (s_ucStoragePickupPending == 0U) &&
		(s_ucStoragePickupActive == 0U) &&
		(s_ucStoragePickupConfirmStorage == 0U) &&
		(s_usManualReservations == 0U) && (s_ucOtaReserved == 0U) &&
		(uxQueueMessagesWaiting(s_xOrderQueue) == 0U) &&
		(g_xCoffee3WorkflowStatus.xState != COFFEE3_WORKFLOW_RUNNING) &&
		(g_xCoffee3WorkflowStatus.xState != COFFEE3_WORKFLOW_CANCELING)) ? 1U : 0U;
}

/*-----------------------------------------------------------*/
/**
  * @brief  清除皮重、执行置零并采集启动空秤基线。
  * @retval 0 空秤重量位于允许偏差内，基线已保存。
  * @retval 负数 秤命令、采样或基线范围检查失败。
  */
static int32_t prvEstablishIceScaleBaseline(void)
{
	int32_t lResult; /*!< 当前子步骤的执行结果或错误码。 */
	int32_t lWeightGram; /*!< 当前秤读取的重量，单位为克。 */

	lWeightGram = 0;
	(void)xCoffee3LogPrintfOrder(COFFEE3_LOG_LEVEL_INFO,
		COFFEE3_LOG_SOURCE_WORKFLOW, COFFEE3_LOG_ORDER_SYSTEM,
		"Scale baseline start: reason=startup empty zero");
	lResult = prvRunStep(0xFC10U, COFFEE3_DEVICE_SCALE,
		COFFEE3_ACTION_SCALE_CLEAR_TARE, 0U, 0U, 3000U);
	if (lResult == 0) {
		lResult = prvRunStep(0xFC11U, COFFEE3_DEVICE_SCALE,
			COFFEE3_ACTION_SCALE_ZERO, 0U, 0U, 3000U);
	}
	if (lResult != 0) {
		(void)xCoffee3LogPrintfOrder(COFFEE3_LOG_LEVEL_WARNING,
			COFFEE3_LOG_SOURCE_WORKFLOW, COFFEE3_LOG_ORDER_SYSTEM,
			"Scale baseline failed: reason=zero command result=%ld",
			(long)lResult);
		return lResult;
	}
	vTaskDelay(pdMS_TO_TICKS(COFFEE3_ICE_BASELINE_SETTLE_MS));
	lResult = prvReadStableScale(0xFC12U, &lWeightGram);
	if (lResult != 0) {
		(void)xCoffee3LogPrintfOrder(COFFEE3_LOG_LEVEL_WARNING,
			COFFEE3_LOG_SOURCE_WORKFLOW, COFFEE3_LOG_ORDER_SYSTEM,
			"Scale baseline failed: reason=read result=%ld", (long)lResult);
		return lResult;
	}
	if ((lWeightGram < -COFFEE3_ICE_BASELINE_TOLERANCE_GRAM) ||
		(lWeightGram > COFFEE3_ICE_BASELINE_TOLERANCE_GRAM)) {
		(void)xCoffee3LogPrintfOrder(COFFEE3_LOG_LEVEL_WARNING,
			COFFEE3_LOG_SOURCE_WORKFLOW, COFFEE3_LOG_ORDER_SYSTEM,
			"Scale baseline failed: reason=zero unstable weight=%ld g",
			(long)lWeightGram);
		return COFFEE3_WORKFLOW_ERROR_ICE_BASELINE;
	}
	taskENTER_CRITICAL();
	s_lIceScaleEmptyBaselineGram = lWeightGram;
	s_ucIceScaleBaselineValid = 1U;
	taskEXIT_CRITICAL();
	(void)xCoffee3LogPrintfOrder(COFFEE3_LOG_LEVEL_INFO,
		COFFEE3_LOG_SOURCE_WORKFLOW, COFFEE3_LOG_ORDER_SYSTEM,
		"Scale baseline ready: empty=%ld g reason=startup zero",
		(long)lWeightGram);
	return 0;
}

/*-----------------------------------------------------------*/
/**
  * @brief  在秤首次就绪或重新上线后建立空秤基线。
  * @param[in] usReadyMask 当前设备就绪位掩码。
  * @note   失败后按配置间隔重试，不阻塞其他初始化服务。
  */
static void prvServiceIceScaleBaseline(uint16_t usReadyMask)
{
	TickType_t xNow; /*!< 当前系统节拍，用于判断等待时长。 */
	int32_t lResult; /*!< 当前子步骤的执行结果或错误码。 */
	uint16_t usScaleMask; /*!< 电子秤设备的就绪位掩码。 */

	usScaleMask = (uint16_t)(1U << COFFEE3_DEVICE_SCALE);
	if ((usReadyMask & usScaleMask) == 0U) {
		if (s_ucIceScaleBaselineValid != 0U) {
			(void)xCoffee3LogPrintfOrder(COFFEE3_LOG_LEVEL_WARNING,
				COFFEE3_LOG_SOURCE_WORKFLOW, COFFEE3_LOG_ORDER_SYSTEM,
				"Scale baseline lost: reason=scale offline");
		}
		taskENTER_CRITICAL();
		s_ucIceScaleBaselineValid = 0U;
		taskEXIT_CRITICAL();
		s_xIceScaleBaselineRetryTick = 0U;
		return;
	}
	if ((s_ucIceScaleBaselineValid != 0U) ||
		(s_ucResidualState != 2U) || (prvMaintenanceIdle() == 0U)) {
		return;
	}
	if (g_xCoffee3WorkflowStatus.ucRecoveryRequired != 0U) {
		return;
	}
	xNow = xTaskGetTickCount();
	if ((s_xIceScaleBaselineRetryTick != 0U) &&
		((int32_t)(xNow - s_xIceScaleBaselineRetryTick) < 0)) {
		return;
	}
	lResult = prvEstablishIceScaleBaseline();
	if (lResult != 0) {
		s_xIceScaleBaselineRetryTick = xTaskGetTickCount() +
			pdMS_TO_TICKS(COFFEE3_ICE_BASELINE_RETRY_MS);
	} else {
		s_xIceScaleBaselineRetryTick = 0U;
	}
}

/*-----------------------------------------------------------*/
/**
  * @brief  判断出冰错误是否属于禁止自动恢复运动的终态故障。
  * @param[in] lResult 出冰或判杯结果。
  * @retval 1 杯体或冰重状态需人工确认。
  * @retval 0 可进入常规安全停止路径。
  */
static uint8_t prvIceTerminalFailure(int32_t lResult)
{
	return ((lResult == COFFEE3_WORKFLOW_ERROR_ICE_RANGE) ||
		(lResult == COFFEE3_WORKFLOW_ERROR_ICE_NO_CUP) ||
		(lResult == COFFEE3_WORKFLOW_ERROR_ICE_WEIGHT)) ? 1U : 0U;
}

/**
  * @brief  检查指定维护类型依赖的设备是否已就绪。
  * @param[in] xType 维护类型。
  * @retval 1 所需设备均已就绪。
  * @retval 0 存在未就绪设备或类型不受支持。
  */
static uint8_t prvMaintenanceReady(Coffee3MaintenanceType_e xType)
{
	uint16_t usRequired; /*!< 本流程所需设备的位掩码。 */
	switch (xType) {
	case COFFEE3_MAINTENANCE_SYRUP_CLEAN:
		usRequired = (1U << COFFEE3_DEVICE_SYRUP_MACHINE);
		break;
	case COFFEE3_MAINTENANCE_COFFEE_CLEAN:
		usRequired = (1U << COFFEE3_DEVICE_COFFEE_MACHINE);
		break;
	default:
		usRequired = (1U << COFFEE3_DEVICE_IO_INPUT) |
			(1U << COFFEE3_DEVICE_IO_OUTPUT);
		break;
	}
	return ((prvReadyDevices() & usRequired) == usRequired) ? 1U : 0U;
}

/**
  * @brief  按订单配方检查本次实际使用的设备是否全部就绪。
  * @param[in] pxOrder 待检查的订单寄存器镜像。
  * @retval 1 本订单依赖的设备均可用。
  * @retval 0 至少一个必要设备尚未就绪。
  */
static uint8_t prvOrderDevicesReady(const Coffee3Order_t *pxOrder)
{
	uint16_t usRequired; /*!< 本流程所需设备的位掩码。 */
	/* 机械臂的 RUNNING 状态由所有者在订单启动时建立，准入阶段不预置。 */
	usRequired = (1U << COFFEE3_DEVICE_IO_INPUT) | (1U << COFFEE3_DEVICE_CUP_MACHINE) |
		(1U << COFFEE3_DEVICE_LID_MACHINE);
	if (pxOrder->ausRegister[COFFEE3_REG_COFFEE_TYPE] != 0xFFFFU) {
		usRequired |= (1U << COFFEE3_DEVICE_COFFEE_MACHINE);
	}
	if (pxOrder->ausRegister[COFFEE3_REG_ICE_AMOUNT] != 0U) {
		usRequired |= (1U << COFFEE3_DEVICE_ICE_MACHINE) |
			(1U << COFFEE3_DEVICE_SCALE);
		if (s_ucIceScaleBaselineValid == 0U) {
			return 0U;
		}
	}
	if ((pxOrder->ausRegister[COFFEE3_REG_FRUIT_MILK_A] != 0U) ||
		(pxOrder->ausRegister[COFFEE3_REG_FRUIT_MILK_B] != 0U)) {
		usRequired |= (1U << COFFEE3_DEVICE_IO_OUTPUT);
	}
	if ((pxOrder->ausRegister[COFFEE3_REG_SYRUP_1] != 0U) ||
		(pxOrder->ausRegister[COFFEE3_REG_SYRUP_2] != 0U) ||
		(pxOrder->ausRegister[COFFEE3_REG_SYRUP_3] != 0U) ||
		(pxOrder->ausRegister[COFFEE3_REG_SYRUP_4] != 0U)) {
		usRequired |= (1U << COFFEE3_DEVICE_SYRUP_MACHINE);
	}
	return ((s_ucInitializationComplete != 0U) &&
		(g_xCoffee3RobotTcpStatus.ucConnected != 0U) &&
		((prvReadyDevices() & usRequired) == usRequired)) ? 1U : 0U;
}

/* 该服务复用现有工作流任务，不额外创建任务或队列。 */
/**
  * @brief  推进设备就绪、残杯检查、空秤基线及出杯门初始化。
  * @note   由工作流任务周期调用，完成前保持订单准入关闭。
  */
static void prvServiceInitialization(void)
{
	Coffee3IoState_t xIo; /*!< 当前读取的输入与输出状态快照。 */
	uint16_t usReady; /*!< 当前已就绪设备的位掩码。 */
	uint16_t usResidualRequired; /*!< 残杯处理所需设备的位掩码。 */
	uint8_t ucBaseReady; /*!< 基础设备是否满足初始化前置条件。 */
	int32_t lResult; /*!< 当前子步骤的执行结果或错误码。 */

	usReady = prvReadyDevices();
	if (usReady != s_usLastReadyMask) {
		uint8_t ucId; /*!< 当前检查的逻辑设备编号。 */
		for (ucId = 1U; ucId < (uint8_t)COFFEE3_DEVICE_COUNT; ucId++) {
			if (((usReady & (uint16_t)(1U << ucId)) != 0U) &&
				((s_usLastReadyMask & (uint16_t)(1U << ucId)) == 0U)) {
				(void)xCoffee3LogPrintfOrder(COFFEE3_LOG_LEVEL_INFO,
					COFFEE3_LOG_SOURCE_WORKFLOW,
					COFFEE3_LOG_ORDER_SYSTEM,
					"Device ready: name=%s reason=health poll recv",
					prvWorkflowDeviceName((Coffee3DeviceId_e)ucId));
			}
			if (((usReady & (uint16_t)(1U << ucId)) == 0U) &&
				((s_usLastReadyMask & (uint16_t)(1U << ucId)) != 0U)) {
				(void)xCoffee3LogPrintfOrder(COFFEE3_LOG_LEVEL_WARNING,
					COFFEE3_LOG_SOURCE_WORKFLOW,
					COFFEE3_LOG_ORDER_SYSTEM,
					"Device not ready: name=%s reason=link or data lost",
					prvWorkflowDeviceName((Coffee3DeviceId_e)ucId));
			}
		}
		(void)xCoffee3LogPrintfOrder(COFFEE3_LOG_LEVEL_INFO,
			COFFEE3_LOG_SOURCE_WORKFLOW, COFFEE3_LOG_ORDER_SYSTEM,
			"Readiness changed: ready=0x%04X lost=0x%04X",
			(unsigned int)usReady,
			(unsigned int)(s_usLastReadyMask & ~usReady));
		s_usLastReadyMask = usReady;
	}
	usResidualRequired = (1U << COFFEE3_DEVICE_ROBOT) |
		(1U << COFFEE3_DEVICE_IO_INPUT);
	if ((s_ucResidualState == 0U) &&
		((usReady & usResidualRequired) == usResidualRequired) &&
		(prvMaintenanceIdle() != 0U) &&
		(g_axCoffee3DeviceStatus[COFFEE3_DEVICE_ROBOT].ucBusy == 0U)) {
		s_ucResidualState = 1U;
		lResult = prvRunInitialization();
		s_ucResidualState = (lResult == 0) ? 2U : 3U;
		g_xCoffee3WorkflowStatus.lLastError = lResult;
		(void)xCoffee3LogPrintfOrder((lResult == 0) ?
			COFFEE3_LOG_LEVEL_INFO : COFFEE3_LOG_LEVEL_ERROR,
			COFFEE3_LOG_SOURCE_WORKFLOW, COFFEE3_LOG_ORDER_SYSTEM,
			(lResult == 0) ? "Residual check passed; debug ready; HOME runs before next order" :
			"Residual check failed (%ld); inspect cups and reset",
			(long)lResult);
		if (lResult != 0) {
			s_usActiveDevices = (1U << COFFEE3_DEVICE_ROBOT);
			g_xCoffee3WorkflowStatus.lSafetyResult = prvAbortDevices();
			g_xCoffee3WorkflowStatus.ucRecoveryRequired = 1U;
			g_xCoffee3WorkflowStatus.ucOrderAdmissionOpen = 0U;
		}
	}
	prvServiceIceScaleBaseline(usReady);

	vCoffee3IoGetSnapshot(&xIo);
	if ((s_ucResidualState == 2U) && (s_ucDoorInitStarted == 0U)) {
		s_ucDoorInitStarted = 1U;
		prvStartDoor(1U);
	}
	ucBaseReady = ((s_ucResidualState == 2U) &&
		 (s_ucDoorFault == 0U) &&
		(s_ucDoorDirection == 0U) &&
		(xIo.xInput.aucXPin[0] != 0U) &&
		(xIo.xInput.aucXPin[1] == 0U) &&
		/* 手动 STOP 不撤销已完成的残杯初始化；订单所有者会在生产动作前
		 * 重新准备机械臂本体的就绪状态。 */
		(g_xCoffee3RobotTcpStatus.ucConnected != 0U) &&
		((usReady & (1U << COFFEE3_DEVICE_IO_INPUT)) != 0U)) ? 1U : 0U;
	s_ucInitializationComplete = ucBaseReady;
	if (ucBaseReady != s_ucLastBaseReady) {
		s_ucLastBaseReady = ucBaseReady;
		(void)xCoffee3LogPrintfOrder(COFFEE3_LOG_LEVEL_INFO,
			COFFEE3_LOG_SOURCE_WORKFLOW, COFFEE3_LOG_ORDER_SYSTEM,
			"Production ready=%u; recipe devices checked per order",
			(unsigned int)ucBaseReady);
	}
	if (prvMaintenanceIdle() != 0U) {
		g_xCoffee3WorkflowStatus.ucOrderAdmissionOpen =
			(ucBaseReady != 0U) &&
			(g_xCoffee3WorkflowStatus.ucRecoveryRequired == 0U);
		g_xCoffee3WorkflowStatus.xMachineState = (s_ucResidualState == 3U) ?
			COFFEE3_MACHINE_ALARM : (ucBaseReady != 0U) ?
			COFFEE3_MACHINE_IDLE : COFFEE3_MACHINE_INITIALIZING;
	}
}

/**
  * @brief  检查外部输入模块快照是否在线且有效。
  * @param[in] pxIo 待检查的 IO 快照。
  * @retval 1 Modbus 输入镜像有效且设备在线。
  * @retval 0 输入镜像无效或设备离线。
  */
static uint8_t prvIoValid(const Coffee3IoState_t *pxIo)
{
	return ((pxIo->aucModbusValid[0] != 0U) &&
		(g_axCoffee3DeviceStatus[COFFEE3_DEVICE_IO_INPUT].ucOnline != 0U)) ?
		1U : 0U;
}

/**
  * @brief  先关闭两个门输出，再登记新的门运动方向和计时起点。
  * @param[in] ucDirection 0 停止，1 关门上升，2 开门下降。
  */
static void prvStartDoor(uint8_t ucDirection)
{
	(void)ucCoffee3IoSetLocalOutput(COFFEE3_LOCAL_DO_DOOR_UP, 0U);
	(void)ucCoffee3IoSetLocalOutput(COFFEE3_LOCAL_DO_DOOR_DOWN, 0U);
	s_ucDoorDirection = ucDirection;
	if (ucDirection != 0U) {
		s_ucDoorExpectedLimit = ucDirection;
	}
	s_xDoorStart = xTaskGetTickCount();
}

/* 仅工作流可驱动门的两个方向，任何时刻都不能同时给两个方向上电。 */
/**
  * @brief  服务出杯门调试、暂存取杯确认及顾客取杯状态机。
  * @note   本函数同时处理门限位冲突、运动超时和出杯口释放确认。
  */
static void prvServicePickup(void)
{
	/* 出杯口机械流程独立于生产流程：杯体传感器边沿、持续空闲 30 秒和
	 * 主机取杯确认是三个相互独立的条件。 */
	Coffee3IoState_t xIo; /*!< 当前读取的输入与输出状态快照。 */
	TickType_t xNow; /*!< 当前系统节拍，用于判断等待时长。 */
	uint8_t ucCup; /*!< 出餐口当前是否检测到杯体。 */
	uint8_t ucDoorDebugRequest; /*!< 是否存在待处理的出餐门调试请求。 */
	uint8_t ucDoorDebugDirection; /*!< 本次调试请求指定的门运动方向。 */
	uint8_t ucDoorConflict; /*!< 门上、下限位是否同时有效。 */
	uint8_t ucDoorTimeout; /*!< 门运动是否超过限位等待时间。 */
	uint8_t ucConfirmStorage; /*!< 待异步确认已清空的来源储位编号。 */
	uint8_t ucSourceCup; /*!< 来源储位传感器当前是否检测到杯体。 */
	uint16_t usConfirmOrderId; /*!< 本次异步取餐确认关联的订单号。 */
	const char *pcDoorDirection; /*!< 当前门动作在日志中的方向描述。 */
	const char *pcPreviousDoorDirection; /*!< 前一门动作在日志中的方向描述。 */
	const char *pcDoorFailure; /*!< 门动作异常时写入日志的原因描述。 */

	/* 步骤 1：刷新本地门限位、读取外部杯体快照，并原子领取调试请求。 */
	vCoffee3IoRefreshLocal();
	vCoffee3IoGetSnapshot(&xIo);
	xNow = xTaskGetTickCount();
	ucDoorDebugRequest = 0U;
	ucDoorDebugDirection = 0U;
	taskENTER_CRITICAL();
	if (s_ucDoorDebugPending != 0U) {
		ucDoorDebugRequest = 1U;
		ucDoorDebugDirection = s_ucDoorDebugDirection;
		s_ucDoorDebugPending = 0U;
	}
	taskEXIT_CRITICAL();
	/* 步骤 2：调试动作可中断当前门运动，但启动前仍检查限位状态。 */
	if (ucDoorDebugRequest != 0U) {
		pcDoorDirection = (ucDoorDebugDirection == 1U) ? "CLOSE" :
			(ucDoorDebugDirection == 2U) ? "OPEN" : "STOP";
		if (s_ucDoorDirection != 0U) {
			pcPreviousDoorDirection = (s_ucDoorDirection == 1U) ?
				"CLOSE" : "OPEN";
			(void)xCoffee3LogPrintfOrder(COFFEE3_LOG_LEVEL_WARNING,
				COFFEE3_LOG_SOURCE_WORKFLOW, COFFEE3_LOG_ORDER_DEBUG,
				"Door %s interrupted by %s; DO1 and DO2 off",
				pcPreviousDoorDirection, pcDoorDirection);
			s_ucDoorDebugActive = 0U;
			s_ucDoorDebugActiveDirection = 0U;
		}
		if (ucDoorDebugDirection == 0U) {
			(void)xCoffee3LogPrintfOrder(COFFEE3_LOG_LEVEL_INFO,
				COFFEE3_LOG_SOURCE_WORKFLOW, COFFEE3_LOG_ORDER_DEBUG,
				"Door STOP execution started: DO1 and DO2 off");
		} else {
			(void)xCoffee3LogPrintfOrder(COFFEE3_LOG_LEVEL_INFO,
				COFFEE3_LOG_SOURCE_WORKFLOW, COFFEE3_LOG_ORDER_DEBUG,
				"Door %s execution requested: DO%u, target DI%u", pcDoorDirection,
				(unsigned int)ucDoorDebugDirection,
				(unsigned int)ucDoorDebugDirection);
		}
		if (ucDoorDebugDirection == 0U) {
			prvStartDoor(0U);
			(void)xCoffee3LogPrintfOrder(COFFEE3_LOG_LEVEL_INFO,
				COFFEE3_LOG_SOURCE_WORKFLOW, COFFEE3_LOG_ORDER_DEBUG,
				"Door STOP complete: DO1 and DO2 off");
		} else if ((xIo.xInput.aucXPin[0] != 0U) &&
			(xIo.xInput.aucXPin[1] != 0U)) {
			prvStartDoor(ucDoorDebugDirection);
			s_ucDoorDebugActive = 1U;
			s_ucDoorDebugActiveDirection = ucDoorDebugDirection;
		} else if (xIo.xInput.aucXPin[ucDoorDebugDirection - 1U] != 0U) {
			prvStartDoor(0U);
			(void)xCoffee3LogPrintfOrder(COFFEE3_LOG_LEVEL_INFO,
				COFFEE3_LOG_SOURCE_WORKFLOW, COFFEE3_LOG_ORDER_DEBUG,
				"Door %s already at DI%u; DO1 and DO2 off",
				pcDoorDirection, (unsigned int)ucDoorDebugDirection);
		} else {
			prvStartDoor(ucDoorDebugDirection);
			s_ucDoorDebugActive = 1U;
			s_ucDoorDebugActiveDirection = ucDoorDebugDirection;
		}
		xNow = xTaskGetTickCount();
	}
	/* 步骤 3：停止状态下只有目标限位单独有效，才解除门故障锁存。 */
	if ((s_ucDoorFault != 0U) && (s_ucDoorDirection == 0U) &&
		(xIo.xInput.aucXPin[s_ucDoorExpectedLimit - 1U] != 0U) &&
		(xIo.xInput.aucXPin[2U - s_ucDoorExpectedLimit] == 0U)) {
		s_ucDoorFault = 0U;
		(void)xCoffee3LogPrintfOrder(COFFEE3_LOG_LEVEL_INFO,
			COFFEE3_LOG_SOURCE_WORKFLOW, COFFEE3_LOG_ORDER_SYSTEM,
			"Outlet door recovered: DI%u confirmed", s_ucDoorExpectedLimit);
	}
	if ((s_ucInitializationComplete != 0U) && (prvIoValid(&xIo) != 0U) &&
		(xIo.xInput.aucMB1XPin[3] == 0U) && (s_ucWaterAlarm == 0U)) {
		s_ucWaterAlarm = 1U;
		(void)xCoffee3LogPrintfOrder(COFFEE3_LOG_LEVEL_WARNING,
			COFFEE3_LOG_SOURCE_WORKFLOW, g_xCoffee3WorkflowStatus.usCurrentOrderId,
			"Water X4 lost; finish order; block next order until recovery");
	}
	/* 步骤 4：运动期间监视双限位冲突、超时和目标限位；任一故障
	 * 都先关闭两个方向输出，并关闭新订单准入。 */
	if (s_ucDoorDirection != 0U) {
		ucDoorConflict = ((xIo.xInput.aucXPin[0] != 0U) &&
			(xIo.xInput.aucXPin[1] != 0U)) ? 1U : 0U;
		ucDoorTimeout = ((xNow - s_xDoorStart) >=
			pdMS_TO_TICKS(COFFEE3_DOOR_MOTION_TIMEOUT_MS)) ? 1U : 0U;
		if ((s_ucDoorFault != 0U) ||
			(ucDoorConflict != 0U) || (ucDoorTimeout != 0U)) {
			pcDoorFailure = (s_ucDoorFault != 0U) ?
				"Outlet door fault latched; outputs off; waiting for valid target limit" :
				((xIo.xInput.aucXPin[0] != 0U) &&
				 (xIo.xInput.aucXPin[1] != 0U)) ?
				"Door limit conflict: DI1 and DI2 active; outputs off" :
				(s_ucDoorDirection == 1U) ?
				"Outlet door CLOSE/UP timeout: DI1 not confirmed; DO1 off; waiting for DI1" :
				"Outlet door OPEN/DOWN timeout: DI2 not confirmed; DO2 off; waiting for DI2";
			prvStartDoor(0U);
			s_ucDoorFault = 1U;
			g_xCoffee3WorkflowStatus.ucOrderAdmissionOpen = 0U;
			if (s_ucDoorDebugActive != 0U) {
				pcDoorDirection =
					(s_ucDoorDebugActiveDirection == 1U) ? "CLOSE" : "OPEN";
				if (ucDoorConflict != 0U) {
					(void)xCoffee3LogPrintfOrder(COFFEE3_LOG_LEVEL_ERROR,
						COFFEE3_LOG_SOURCE_WORKFLOW, COFFEE3_LOG_ORDER_DEBUG,
						"Door %s conflict: DI1 and DI2 active; DO1 and DO2 off",
						pcDoorDirection);
				} else if (ucDoorTimeout != 0U) {
					(void)xCoffee3LogPrintfOrder(COFFEE3_LOG_LEVEL_WARNING,
						COFFEE3_LOG_SOURCE_WORKFLOW, COFFEE3_LOG_ORDER_DEBUG,
						"Door %s timeout: DI%u not confirmed; DO1 and DO2 off",
						pcDoorDirection,
						(unsigned int)s_ucDoorDebugActiveDirection);
				} else {
					(void)xCoffee3LogPrintfOrder(COFFEE3_LOG_LEVEL_WARNING,
						COFFEE3_LOG_SOURCE_WORKFLOW, COFFEE3_LOG_ORDER_DEBUG,
						"Door %s interrupted: fault latched; DO1 and DO2 off",
						pcDoorDirection);
				}
				s_ucDoorDebugActive = 0U;
				s_ucDoorDebugActiveDirection = 0U;
			} else {
				(void)xCoffee3LogPrintfOrder(COFFEE3_LOG_LEVEL_ERROR,
					COFFEE3_LOG_SOURCE_WORKFLOW,
					g_xCoffee3WorkflowStatus.ausOutputOrderId[0],
					"%s", pcDoorFailure);
			}
		} else if (xIo.xInput.aucXPin[s_ucDoorDirection - 1U] != 0U) {
			if (s_ucDoorDebugActive != 0U) {
				pcDoorDirection =
					(s_ucDoorDebugActiveDirection == 1U) ? "CLOSE" : "OPEN";
				(void)xCoffee3LogPrintfOrder(COFFEE3_LOG_LEVEL_INFO,
					COFFEE3_LOG_SOURCE_WORKFLOW, COFFEE3_LOG_ORDER_DEBUG,
					"Door %s complete: DI%u confirmed; DO1 and DO2 off",
					pcDoorDirection,
					(unsigned int)s_ucDoorDebugActiveDirection);
				s_ucDoorDebugActive = 0U;
				s_ucDoorDebugActiveDirection = 0U;
			}
			prvStartDoor(0U);
		} else {
			(void)ucCoffee3IoSetLocalOutput((uint8_t)(s_ucDoorDirection - 1U), 1U);
		}
	}
	/* 步骤 5：机械臂放杯后异步等待 X3 有杯且源暂存位无杯，
	 * 物理确认完成后再清除源订单并开启出杯门。 */
	ucConfirmStorage = s_ucStoragePickupConfirmStorage;
	if ((ucConfirmStorage >= 1U) && (ucConfirmStorage <= 2U) &&
		(prvIoValid(&xIo) != 0U)) {
		ucCup = xIo.xInput.aucMB1XPin[COFFEE3_EXTERNAL_DI_OUTLET_CUP];
		ucSourceCup = xIo.xInput.aucMB1XPin[ucConfirmStorage - 1U];
		if ((ucCup != 0U) && (ucSourceCup == 0U) &&
			(s_ucDoorFault == 0U) && (s_ucDoorDirection == 0U)) {
			usConfirmOrderId = s_usStoragePickupConfirmOrderId;
			taskENTER_CRITICAL();
			s_ucStoragePickupConfirmStorage = 0U;
			s_usStoragePickupConfirmOrderId = 0U;
			s_ucStoragePickupConfirmOverdueLogged = 0U;
			s_ausStoredOrderId[ucConfirmStorage - 1U] = 0U;
			s_ucOutletPhase = 1U;
			s_ucPickupPending = 0U;
			s_ucEmptyTiming = 0U;
			taskEXIT_CRITICAL();
			prvPublishOutputForOrder(1U, 5U, usConfirmOrderId);
			prvStartDoor(2U);
			(void)xCoffee3LogPrintfOrder(COFFEE3_LOG_LEVEL_INFO,
				COFFEE3_LOG_SOURCE_WORKFLOW,
				(usConfirmOrderId != 0U) ? usConfirmOrderId :
					COFFEE3_LOG_ORDER_SYSTEM,
				"Pickup placement confirmed: device=IoInput storage=%u X3=1 source=0; open outlet",
				(unsigned int)ucConfirmStorage);
		} else if ((s_ucStoragePickupConfirmOverdueLogged == 0U) &&
			((xNow - s_xStoragePickupConfirmStart) >=
			 pdMS_TO_TICKS(COFFEE3_WORKFLOW_DEFAULT_TIMEOUT_MS))) {
			s_ucStoragePickupConfirmOverdueLogged = 1U;
			(void)xCoffee3LogPrintfOrder(COFFEE3_LOG_LEVEL_WARNING,
				COFFEE3_LOG_SOURCE_WORKFLOW,
				(s_usStoragePickupConfirmOrderId != 0U) ?
					s_usStoragePickupConfirmOrderId : COFFEE3_LOG_ORDER_SYSTEM,
				"Pickup confirmation overdue: device=IoInput storage=%u X3=%u source=%u; waiting",
				(unsigned int)ucConfirmStorage, (unsigned int)ucCup,
				(unsigned int)ucSourceCup);
		}
	}
	/* 步骤 6：顾客取走杯体并持续空闲 30 秒后关门；门到位且收到
	 * 主机确认时，才清空出杯口状态并释放资源。 */
	if ((s_ucOutletPhase == 0U) || (s_ucDoorFault != 0U)) {
		return;
	}
	if (prvIoValid(&xIo) == 0U) {
		s_ucEmptyTiming = 0U;
		return;
	}
	ucCup = xIo.xInput.aucMB1XPin[COFFEE3_EXTERNAL_DI_OUTLET_CUP];
	if ((s_ucOutletPhase == 1U) && (ucCup != 0U)) {
		s_ucOutletPhase = 2U;
	}
	if (s_ucOutletPhase == 2U) {
		if ((ucCup != 0U) || (s_ucDoorDirection != 0U)) {
			s_ucEmptyTiming = 0U;
		} else if (s_ucEmptyTiming == 0U) {
			s_ucEmptyTiming = 1U;
			s_xEmptyStart = xNow;
		} else if ((xNow - s_xEmptyStart) >= pdMS_TO_TICKS(COFFEE3_OUTLET_EMPTY_HOLD_MS)) {
			s_ucOutletPhase = 3U;
			prvPublishOutput(1U, 0x15U);
			prvStartDoor(1U);
			(void)xCoffee3LogPrintfOrder(COFFEE3_LOG_LEVEL_INFO,
				COFFEE3_LOG_SOURCE_WORKFLOW,
				g_xCoffee3WorkflowStatus.ausOutputOrderId[0],
				"Outlet X3 empty for 30 s; customer take complete, raise door, wait ACK");
		}
	}
	if ((s_ucOutletPhase == 3U) && (s_ucPickupPending != 0U) &&
		(s_ucDoorDirection == 0U) && (xIo.xInput.aucXPin[0] != 0U) &&
		(ucCup == 0U)) {
		taskENTER_CRITICAL();
		s_ucPickupPending = 0U;
		s_ucOutletPhase = 0U;
		taskEXIT_CRITICAL();
		prvPublishOutput(1U, 0U);
		(void)xCoffee3LogPrintfOrder(COFFEE3_LOG_LEVEL_INFO,
			COFFEE3_LOG_SOURCE_WORKFLOW,
			g_xCoffee3WorkflowStatus.ausOutputOrderId[0],
			"Customer take ACK consumed; door closed, outlet released");
	}
}

/*-----------------------------------------------------------*/
/**
  * @brief  为当前订单发布指定出杯口状态。
  * @param[in] usOutput 出杯口编号。
  * @param[in] usState 写入 Server 的出杯口状态。
  */
static void prvPublishOutput(uint16_t usOutput, uint16_t usState)
{
	prvPublishOutputForOrder(usOutput, usState,
		g_xCoffee3WorkflowStatus.usCurrentOrderId);
}

/*-----------------------------------------------------------*/
/**
  * @brief  为给定订单发布出杯口状态并保存其关联订单号。
  * @param[in] usOutput 出杯口编号。
  * @param[in] usState 写入 Server 的出杯口状态。
  * @param[in] usOrderId 与该出杯口关联的订单号。
  */
static void prvPublishOutputForOrder(uint16_t usOutput, uint16_t usState,
	uint16_t usOrderId)
{
	taskENTER_CRITICAL();
	g_xCoffee3WorkflowStatus.ausOutputState[usOutput - 1U] = usState;
	if (usState == 2U) {
		g_xCoffee3WorkflowStatus.ausOutputOrderId[usOutput - 1U] =
			usOrderId;
	}
	taskEXIT_CRITICAL();
	vCoffee3ServerPublishOutput(usOutput, usState);
}

/*-----------------------------------------------------------*/
/**
  * @brief  确认目标出杯口可用且没有检测到杯体。
  * @param[in] usOutput 出杯口编号，当前仅支持 1。
  * @retval 0 出杯口空闲且 IO 反馈有效。
  * @retval 负数 参数、IO 或出杯口占用错误。
  */
static int32_t prvCheckOutputEmpty(uint16_t usOutput)
{
	Coffee3IoState_t xIo; /*!< 当前读取的输入与输出状态快照。 */
	int32_t lResult; /*!< 当前子步骤的执行结果或错误码。 */

	prvServicePickup();
	if ((usOutput != 1U) || (s_ucOutletPhase != 0U) ||
		(s_ucStoragePickupConfirmStorage != 0U) ||
		(s_ucDoorFault != 0U) || (s_ucDoorDirection != 0U)) {
		return COFFEE3_WORKFLOW_ERROR_INIT_OCCUPIED;
	}
	lResult = prvRefreshDeviceQuiet(29U, COFFEE3_DEVICE_IO_INPUT);
	vCoffee3IoGetSnapshot(&xIo);
	if ((lResult != 0) || (prvIoValid(&xIo) == 0U)) {
		return COFFEE3_WORKFLOW_ERROR_IO;
	}
	return ((xIo.xInput.aucMB1XPin[COFFEE3_EXTERNAL_DI_OUTLET_CUP] != 0U) ||
		(xIo.xInput.aucXPin[COFFEE3_LOCAL_DI_DOOR_UPPER] == 0U)) ?
		COFFEE3_WORKFLOW_ERROR_INIT_OCCUPIED : 0;
}

/*-----------------------------------------------------------*/
/**
  * @brief  等待出杯口从插入取杯或顾客取杯流程中完全释放。
  * @retval 0 出杯口已空闲且可放置新成品。
  * @retval COFFEE3_WORKFLOW_ERROR_CANCELED 等待期间收到取消。
  * @note   IO 无效、出口仍被占用或等待逾期时继续等待；逾期只告警一次。
  */
static int32_t prvWaitOutputAvailableForOrder(void)
{
	Coffee3IoState_t xIo; /*!< 当前读取的输入与输出状态快照。 */
	TickType_t xStart; /*!< 本段等待开始的系统节拍。 */
	uint8_t ucOverdueLogged; /*!< 逾期日志是否已记录，避免重复告警。 */

	xStart = xTaskGetTickCount();
	ucOverdueLogged = 0U;
	for (;;) {
		if (g_xCoffee3WorkflowStatus.ucCancelRequested != 0U) {
			return COFFEE3_WORKFLOW_ERROR_CANCELED;
		}
		prvServicePickup();
		vCoffee3IoGetSnapshot(&xIo);
		if ((prvIoValid(&xIo) != 0U) &&
			(s_ucOutletPhase == 0U) &&
			(s_ucStoragePickupActive == 0U) &&
			(s_ucStoragePickupConfirmStorage == 0U) &&
			(s_ucDoorFault == 0U) && (s_ucDoorDirection == 0U) &&
			(xIo.xInput.aucMB1XPin[COFFEE3_EXTERNAL_DI_OUTLET_CUP] == 0U) &&
			(xIo.xInput.aucXPin[COFFEE3_LOCAL_DI_DOOR_UPPER] != 0U)) {
			return 0;
		}
		if ((ucOverdueLogged == 0U) &&
			((xTaskGetTickCount() - xStart) >=
			 pdMS_TO_TICKS(COFFEE3_WORKFLOW_DEFAULT_TIMEOUT_MS))) {
			ucOverdueLogged = 1U;
			(void)xCoffee3LogPrintfOrder(COFFEE3_LOG_LEVEL_WARNING,
				COFFEE3_LOG_SOURCE_WORKFLOW,
				g_xCoffee3WorkflowStatus.usCurrentOrderId,
				"Outlet wait overdue: device=IoInput X3=%u phase=%u; waiting for pickup release",
				(unsigned int)xIo.xInput.aucMB1XPin[
					COFFEE3_EXTERNAL_DI_OUTLET_CUP],
				(unsigned int)s_ucOutletPhase);
		}
		prvDelayWithServices(100U);
	}
}

/**
  * @brief  从已安装且无杯的暂存位中选择一个可用位置。
  * @retval 1或2 已选中的暂存位编号。
  * @retval 负数 IO 无效、暂存位占用或无可用位置。
  * @note   本函数只返回编号；调用者负责保存预约结果。
  */
static int32_t prvSelectStorage(void)
{
	Coffee3IoState_t xIo; /*!< 当前读取的输入与输出状态快照。 */
	int32_t lResult; /*!< 当前子步骤的执行结果或错误码。 */
	uint8_t ucIndex; /*!< 当前遍历项的下标。 */
	uint16_t usEnabled; /*!< 配置中已启用的暂存位位掩码。 */

	lResult = prvRefreshDeviceQuiet(0xFD1FU, COFFEE3_DEVICE_IO_INPUT);
	vCoffee3IoGetSnapshot(&xIo);
	if ((lResult != 0) || (prvIoValid(&xIo) == 0U)) {
		return COFFEE3_WORKFLOW_ERROR_IO;
	}
	usEnabled = usCoffee3ConfigStorageMask() & COFFEE3_STORAGE_INSTALLED_MASK;
	for (ucIndex = 0U; ucIndex < 2U; ucIndex++) {
		if (((usEnabled & (1U << ucIndex)) != 0U) &&
			(xIo.xInput.aucMB1XPin[ucIndex] == 0U)) {
			return (int32_t)(ucIndex + 1U);
		}
	}
	return COFFEE3_WORKFLOW_ERROR_INIT_OCCUPIED;
}

/*-----------------------------------------------------------*/
/**
  * @brief 清空工作流状态并创建固定容量的静态订单队列。
  * @retval pdPASS 队列创建成功。
  * @retval pdFAIL 队列创建失败。
  */
BaseType_t xCoffee3WorkflowInitialize(void)
{
	memset(&g_xCoffee3WorkflowStatus, 0,
		sizeof(g_xCoffee3WorkflowStatus));
	s_usManualIceWeight = 0U;
	s_ucManualIcePending = 0U;
	memset(&s_xPendingOrder, 0, sizeof(s_xPendingOrder));
	s_ucPendingOrder = 0U;
	s_ulNextOrderEpoch = 0U;
	s_ucManualOverride = 0U;
	s_usManualReservations = 0U;
	s_ucMaintenanceActive = 0U;
	s_ucPickupPending = 0U;
	s_ucStoragePickupPending = 0U;
	s_ucStoragePickupActive = 0U;
	s_ucStoragePickupConfirmStorage = 0U;
	s_usStoragePickupConfirmOrderId = 0U;
	s_usDetachedPickupLogOrder = 0U;
	s_xStoragePickupConfirmStart = 0U;
	s_ucStoragePickupConfirmOverdueLogged = 0U;
	memset(s_ausStoredOrderId, 0, sizeof(s_ausStoredOrderId));
	s_ucWaterAlarm = 0U;
	s_ucOutletPhase = 0U;
	s_ucDoorDirection = 0U;
	s_ucDoorFault = 0U;
	s_ucDoorDebugPending = 0U;
	s_ucDoorDebugDirection = 0U;
	s_ucDoorDebugActive = 0U;
	s_ucDoorDebugActiveDirection = 0U;
	s_ucEmptyTiming = 0U;
	s_usActiveDevices = 0U;
	memset(&s_xHotWater, 0, sizeof(s_xHotWater));
	memset(&s_xMaintenance, 0, sizeof(s_xMaintenance));
	s_ucInitializationAcknowledged = 0U;
	s_ucInitializationComplete = 0U;
	s_ucResidualState = 0U;
	s_ucDoorInitStarted = 0U;
	s_ucDoorExpectedLimit = 1U;
	memset(&s_xInitHome, 0, sizeof(s_xInitHome));
	s_usLastReadyMask = 0U;
	s_ucLastBaseReady = 0U;
	s_lIceScaleEmptyBaselineGram = 0;
	s_xIceScaleBaselineRetryTick = 0U;
	s_ucIceScaleBaselineValid = 0U;
	g_xCoffee3WorkflowStatus.xMachineState =
		COFFEE3_MACHINE_INITIALIZING;
	g_xCoffee3WorkflowStatus.ucOrderAdmissionOpen = 0U;
	s_xOrderQueue = xQueueCreateStatic(COFFEE3_WORKFLOW_QUEUE_LENGTH,
		sizeof(Coffee3Order_t), s_aucOrderQueueStorage,
		&s_xOrderQueueStorage);
	return (s_xOrderQueue != NULL) ? pdPASS : pdFAIL;
}

/*-----------------------------------------------------------*/
/**
  * @brief 查询当前基础生产条件及初始化残杯检查是否就绪。
  * @retval 非零 初始化已完成。
  * @retval 0 初始化尚未完成。
  */
uint8_t ucCoffee3WorkflowInitializationComplete(void)
{
	return s_ucInitializationComplete;
}

/*-----------------------------------------------------------*/
/**
  * @brief 提交一项由工作流任务串行执行的非订单维护操作。
  * @param[in] xType 维护类型，不接受 NONE 或超出枚举范围的值。
  * @param[in] usParameter0 第一业务参数，含义随维护类型变化。
  * @param[in] usParameter1 第二业务参数，含义随维护类型变化。
  * @retval pdPASS 已保存维护请求并关闭新订单接单门槛。
  * @retval pdFAIL 类型无效、业务忙或目标设备未就绪。
  */
BaseType_t xCoffee3WorkflowSubmitMaintenance(
	Coffee3MaintenanceType_e xType, uint16_t usParameter0,
	uint16_t usParameter1)
{
	BaseType_t xResult; /*!< 本次请求或预留操作的成功状态。 */

	if ((xType <= COFFEE3_MAINTENANCE_NONE) ||
		(xType > COFFEE3_MAINTENANCE_FRUIT_CLEAN)) {
		return pdFAIL;
	}
	xResult = pdFAIL;
	taskENTER_CRITICAL();
	if ((prvMaintenanceIdle() != 0U) &&
		(prvMaintenanceReady(xType) != 0U)) {
		s_xMaintenance.xType = xType;
		s_xMaintenance.usParameter0 = usParameter0;
		s_xMaintenance.usParameter1 = usParameter1;
		s_xMaintenance.ucPending = 1U;
		g_xCoffee3WorkflowStatus.ucOrderAdmissionOpen = 0U;
		xResult = pdPASS;
	}
	taskEXIT_CRITICAL();
	return xResult;
}

/*-----------------------------------------------------------*/
/**
  * @brief 启动热水服务，或对当前热水服务提出取消请求。
  * @param[in] ucStart 零表示请求停止，非零表示开始。
  * @param[in] usHeatMinutes 加热时长；零使用默认值，超过上限会被拒绝。
  * @retval pdPASS 停止请求已记录，或启动请求已进入热水状态机。
  * @retval pdFAIL 启动参数或业务准入条件不满足。
  */
BaseType_t xCoffee3WorkflowSetHotWater(uint8_t ucStart,
	uint16_t usHeatMinutes)
{
	if (ucStart == 0U) {
		taskENTER_CRITICAL();
		s_xHotWater.ucCancelRequested = 1U;
		taskEXIT_CRITICAL();
		return pdPASS;
	}
	if (usHeatMinutes == 0U) {
		usHeatMinutes = COFFEE3_HOT_WATER_DEFAULT_HEAT_MIN;
	}
	if (usHeatMinutes > COFFEE3_HOT_WATER_MAX_HEAT_MIN) {
		return pdFAIL;
	}
	taskENTER_CRITICAL();
	if ((prvMaintenanceReady(COFFEE3_MAINTENANCE_FRUIT_CLEAN) == 0U) ||
		(s_usManualReservations != 0U) ||
		(s_xHotWater.ucPhase != COFFEE3_HOT_WATER_IDLE) ||
		(s_xHotWater.ucIoPending != 0U)) {
		taskEXIT_CRITICAL();
		return pdFAIL;
	}
	memset(&s_xHotWater, 0, sizeof(s_xHotWater));
	s_xHotWater.usHeatMinutes = usHeatMinutes;
	s_xHotWater.ucPhase = COFFEE3_HOT_WATER_PREPARE_OFF;
	taskEXIT_CRITICAL();
	(void)xCoffee3LogWriteFieldOrder(COFFEE3_LOG_LEVEL_INFO,
		COFFEE3_LOG_SOURCE_WORKFLOW, COFFEE3_LOG_ORDER_DEBUG,
		"HOT_WATER_START", 0, "minutes", (int32_t)usHeatMinutes);
	return pdPASS;
}

/*-----------------------------------------------------------*/
/**
  * @brief  确认初始化，按当前液位确认供水和补水告警，并尝试恢复接单。
  * @note   热水服务空闲时重置其公开状态；不清除咖啡清洗或果奶维护结果。
  */
void vCoffee3WorkflowAcknowledgeAlarm(void)
{
	Coffee3IoState_t xIo; /*!< 当前读取的输入与输出状态快照。 */
	vCoffee3IoGetSnapshot(&xIo);
	taskENTER_CRITICAL();
	if ((s_ucInitializationComplete != 0U) &&
		(prvIoValid(&xIo) != 0U) && (xIo.xInput.aucMB1XPin[3] != 0U)) {
		s_ucWaterAlarm = 0U;
		if (xIo.xInput.aucXPin[COFFEE3_LOCAL_DI_COFFEE_WATER_LOW] != 0U) {
			s_ucCoffeeFillAlarm = 0U;
		}
	}

	s_ucInitializationAcknowledged = 1U;
	if ((s_ucInitializationComplete == 0U) || (s_ucWaterAlarm != 0U)) {
		taskEXIT_CRITICAL();
		return;
	}
	if (s_xHotWater.ucPhase == COFFEE3_HOT_WATER_IDLE) {
		g_xCoffee3WorkflowStatus.ucHotWaterState =
			COFFEE3_MAINTENANCE_IDLE;
	}
	if ((g_xCoffee3WorkflowStatus.ucRecoveryRequired == 0U) &&
		(g_xCoffee3WorkflowStatus.xState != COFFEE3_WORKFLOW_RUNNING)) {
		g_xCoffee3WorkflowStatus.xMachineState = COFFEE3_MACHINE_IDLE;
		g_xCoffee3WorkflowStatus.ucOrderAdmissionOpen = 1U;
	}
	taskEXIT_CRITICAL();
}

/*-----------------------------------------------------------*/
/**
  * @brief 复查接单门槛并将订单快照复制到工作流队列。
  * @param[in] pxOrder 待提交订单；成功时队列复制其 32 字内容。
  * @retval pdPASS 订单已入队，尚未开始设备动作。
  * @retval pdFAIL 参数、设备就绪、业务准入或队列容量不满足。
  */
BaseType_t xCoffee3WorkflowSubmitOrder(const Coffee3Order_t *pxOrder)
{
	BaseType_t xResult; /*!< 本次请求或预留操作的成功状态。 */
	if ((pxOrder == NULL) || (s_xOrderQueue == NULL)) {
		return pdFAIL;
	}
	taskENTER_CRITICAL();
	if ((prvOrderDevicesReady(pxOrder) == 0U) ||
		(g_xCoffee3WorkflowStatus.ucOrderAdmissionOpen == 0U) ||
		(s_ucWaterAlarm != 0U) || (s_ucCoffeeFillAlarm != 0U) ||
		(s_ucStoragePickupPending != 0U) || (s_usManualReservations != 0U) ||
		(g_xCoffee3WorkflowStatus.ucRecoveryRequired != 0U)) {
		taskEXIT_CRITICAL();
		return pdFAIL;
	}
	g_xCoffee3WorkflowStatus.ucOrderAdmissionOpen = 0U;
	xResult = xQueueSend(s_xOrderQueue, pxOrder, 0U);
	if (xResult != pdPASS) {
		g_xCoffee3WorkflowStatus.ucOrderAdmissionOpen = 1U;
	}
	taskEXIT_CRITICAL();
	return xResult;
}

/**
  * @brief 预留一次储位到出餐口的独立取餐请求。
  * @param[in] usStorage 储位编号，取值 1 或 2，且必须已启用。
  * @param[in] usOutput 出餐口编号，当前仅接受 1。
  * @retval pdPASS 请求已保存，后续由工作流任务复查并执行。
  * @retval pdFAIL 参数无效或当前存在冲突业务。
  */
BaseType_t xCoffee3WorkflowSubmitStoragePickup(uint16_t usStorage,
	uint16_t usOutput)
{
	if ((usStorage < 1U) || (usStorage > 2U) || (usOutput != 1U) ||
		((usCoffee3ConfigStorageMask() &
		 (uint16_t)(1U << (usStorage - 1U))) == 0U)) {
		return pdFAIL;
	}
	taskENTER_CRITICAL();
	if ((s_ucInitializationComplete == 0U) ||
		((g_xCoffee3WorkflowStatus.xState != COFFEE3_WORKFLOW_RUNNING) &&
		 (g_xCoffee3WorkflowStatus.ucOrderAdmissionOpen == 0U)) ||
		(g_xCoffee3WorkflowStatus.xState == COFFEE3_WORKFLOW_CANCELING) ||
		(g_xCoffee3WorkflowStatus.ucRecoveryRequired != 0U) ||
		(s_ucMaintenanceActive != 0U) || (s_xMaintenance.ucPending != 0U) ||
		(s_ucManualIcePending != 0U) || (s_ucOtaReserved != 0U) ||
		(s_ucOutletPhase != 0U) || (s_ucDoorDirection != 0U) ||
		(s_ucDoorFault != 0U) || (s_usManualReservations != 0U) ||
		(s_ucStoragePickupPending != 0U) ||
		(s_ucStoragePickupActive != 0U) ||
		(s_ucStoragePickupConfirmStorage != 0U)) {
		taskEXIT_CRITICAL();
		return pdFAIL;
	}
	s_ucStoragePickupPending = (uint8_t)usStorage;
	if (g_xCoffee3WorkflowStatus.xState != COFFEE3_WORKFLOW_RUNNING) {
		g_xCoffee3WorkflowStatus.ucOrderAdmissionOpen = 0U;
	}
	taskEXIT_CRITICAL();
	return pdPASS;
}

/*-----------------------------------------------------------*/
/**
  * @brief 提交一次需确认杯重并使用电子秤反馈的手动出冰请求。
  * @param[in] usTargetGrams 寄存器 0x0071 提供的目标冰量，单位克。
  * @retval pdPASS 请求已保存，后续由工作流任务执行。
  * @retval pdFAIL 目标为零、业务忙或冰机、电子秤与空秤基线未就绪。
  */
BaseType_t xCoffee3WorkflowSubmitManualIce(uint16_t usTargetGrams)
{
	BaseType_t xResult; /*!< 本次请求或预留操作的成功状态。 */
	const char *pcRejectReason; /*!< 本次请求被拒绝的日志原因。 */
	uint16_t usReadyMask; /*!< 当前设备就绪状态位掩码。 */

	xResult = pdFAIL;
	pcRejectReason = NULL;
	usReadyMask = 0U;
	if (usTargetGrams == 0U) {
		pcRejectReason = "target_zero";
	} else if (s_xOrderQueue == NULL) {
		pcRejectReason = "workflow_not_ready";
	} else if (uxQueueMessagesWaiting(s_xOrderQueue) != 0U) {
		pcRejectReason = "order_pending";
	} else {
		taskENTER_CRITICAL();
		usReadyMask = prvReadyDevices();
		if (s_ucResidualState == 1U) {
			pcRejectReason = "residual_check_running";
		} else if (prvMaintenanceIdle() == 0U) {
			pcRejectReason = "workflow_busy";
		} else if ((usReadyMask &
			(1U << COFFEE3_DEVICE_ICE_MACHINE)) == 0U) {
			pcRejectReason = "ice_not_ready";
		} else if ((usReadyMask &
			(1U << COFFEE3_DEVICE_SCALE)) == 0U) {
			pcRejectReason = "scale_not_ready";
		} else if (s_ucIceScaleBaselineValid == 0U) {
			pcRejectReason = "scale_baseline_pending";
		} else {
			s_usManualIceWeight = usTargetGrams;
			s_ucManualIcePending = 1U;
			g_xCoffee3WorkflowStatus.ucOrderAdmissionOpen = 0U;
			xResult = pdPASS;
		}
		taskEXIT_CRITICAL();
	}
	if (xResult != pdPASS) {
		(void)xCoffee3LogPrintfOrder(COFFEE3_LOG_LEVEL_WARNING,
			COFFEE3_LOG_SOURCE_WORKFLOW, COFFEE3_LOG_ORDER_DEBUG,
			"Manual ice rejected: %s, ready=0x%04X, target=%u g",
			pcRejectReason, (unsigned int)usReadyMask,
			(unsigned int)usTargetGrams);
	}
	return xResult;
}

/*-----------------------------------------------------------*/
/** @brief 标记当前工作流取消请求，并把订单代次通知设备路由层。 */
void vCoffee3WorkflowRequestCancel(void)
{
	uint32_t ulOrderEpoch; /*!< 取消与命令归属使用的订单代次。 */

	taskENTER_CRITICAL();
	g_xCoffee3WorkflowStatus.ucCancelRequested = 1U;
	ulOrderEpoch = g_xCoffee3WorkflowStatus.ulOrderEpoch;
	taskEXIT_CRITICAL();
	vCoffee3OrderCancelRequest(ulOrderEpoch);
	(void)xCoffee3LogWriteFieldOrder(COFFEE3_LOG_LEVEL_WARNING,
		COFFEE3_LOG_SOURCE_WORKFLOW,
		g_xCoffee3WorkflowStatus.usCurrentOrderId,
		"WORKFLOW_CANCEL_REQUEST", 0,
		"order", (int32_t)g_xCoffee3WorkflowStatus.usCurrentOrderId);
}

/*-----------------------------------------------------------*/
/**
  * @brief 在独立任务中推进初始化、订单、维护、取餐和异常收尾。
  * @param[in] pvArgument 未使用的任务入口参数。
  * @note   此函数不返回；长等待期间按各等待点服务其他业务。
  */
void vCoffee3WorkflowTask(void *pvArgument)
{
	Coffee3Order_t xOrder; /*!< 从队列取出的订单寄存器快照。 */
	int32_t lResult; /*!< 当前子步骤的执行结果或错误码。 */
	int32_t lSafetyResult; /*!< 异常收尾时安全停止动作的结果。 */
	uint16_t usStoragePickup; /*!< 本次独立取餐选择的储位编号。 */
	uint16_t usManualIceWeight; /*!< 待执行手动出冰的目标克重。 */
	uint8_t ucManualBarrier; /*!< 人工预留是否阻止本轮从订单队列取单。 */
	uint8_t ucManualIceDispenseStarted; /*!< 本次手动出冰是否已开始驱动制冰机。 */
	const char *pcIceFailureLog; /*!< 出冰失败时对应的日志事件名。 */
	Coffee3MaintenanceRequest_t xMaintenance; /*!< 工作流接管的一份维护请求快照。 */

	(void)pvArgument;
	(void)xCoffee3LogWrite(COFFEE3_LOG_LEVEL_INFO,
		COFFEE3_LOG_SOURCE_WORKFLOW, "TASK_RUNNING:C3Workflow", 0);
	vTaskDelay(pdMS_TO_TICKS(500U));
	prvPublish(COFFEE3_WORKFLOW_IDLE, 0U, 0);
	for (;;) {
		/* 步骤 1：每轮先推进热水、初始化和取杯相关的常驻服务。 */
		prvServiceHotWater();
		prvServiceInitialization();
		/* 步骤 2：出杯口和门状态允许时，原子领取独立暂存取杯请求；
		 * 该流程暂用维护所有权，完成或失败后再统一释放。 */
		taskENTER_CRITICAL();
		usStoragePickup = 0U;
		if ((s_ucStoragePickupPending != 0U) &&
			(s_ucOutletPhase == 0U) &&
			(s_ucStoragePickupConfirmStorage == 0U) &&
			(s_ucDoorDirection == 0U) && (s_ucDoorFault == 0U)) {
			usStoragePickup = s_ucStoragePickupPending;
			s_ucStoragePickupPending = 0U;
		}
		if (usStoragePickup != 0U) {
			s_ucStoragePickupActive = 1U;
			s_ucMaintenanceActive = 1U;
		}
		taskEXIT_CRITICAL();
		if (usStoragePickup != 0U) {
			s_usActiveDevices = 0U;
			g_xCoffee3WorkflowStatus.ucPositionUncertain = 0U;
			g_xCoffee3WorkflowStatus.ucCancelRequested = 0U;
			g_xCoffee3WorkflowStatus.ulOrderEpoch = ++s_ulNextOrderEpoch;
			lResult = prvRunStoragePickup(usStoragePickup);
			if (lResult != 0) {
				g_xCoffee3WorkflowStatus.lSafetyResult =
				(s_usActiveDevices != 0U) ? prvAbortDevices() : 0;
				g_xCoffee3WorkflowStatus.ucRecoveryRequired =
					(g_xCoffee3WorkflowStatus.ucPositionUncertain != 0U) ||
					(g_xCoffee3WorkflowStatus.lSafetyResult != 0);
			}
			s_ucStoragePickupActive = 0U;
			s_ucMaintenanceActive = 0U;
			g_xCoffee3WorkflowStatus.ucOrderAdmissionOpen =
				(g_xCoffee3WorkflowStatus.ucRecoveryRequired == 0U) ? 1U : 0U;
			g_xCoffee3WorkflowStatus.xState = (lResult == 0) ?
				COFFEE3_WORKFLOW_IDLE : COFFEE3_WORKFLOW_FAILED;
			vCoffee3ServerFinishRequest(1U);
			continue;
		}
		/* 步骤 3：上方工作释放后，延迟的机械臂手动预约优先占用
		 * 下一个边界，之后才允许订单出队。 */
		taskENTER_CRITICAL();
		ucManualBarrier = ((s_usManualReservations != 0U) &&
			(s_ucManualIcePending == 0U) &&
			(s_xMaintenance.ucPending == 0U)) ? 1U : 0U;
		taskEXIT_CRITICAL();
		if (ucManualBarrier != 0U) {
			prvServiceIoRefresh();
			vTaskDelay(pdMS_TO_TICKS(COFFEE3_WORKFLOW_IO_REFRESH_MS));
			continue;
		}
		if (prvQueuePendingOrder() != 0U) {
			continue;
		}
		/* 步骤 4：没有订单时才领取手动出冰或维护请求，确保这些动作
		 * 与正式订单共享同一个执行所有者。 */
		if (xQueueReceive(s_xOrderQueue, &xOrder,
			pdMS_TO_TICKS(COFFEE3_WORKFLOW_IO_REFRESH_MS)) !=
			pdPASS) {
			memset(&xMaintenance, 0, sizeof(xMaintenance));
			taskENTER_CRITICAL();
			if (s_ucManualIcePending != 0U) {
				usManualIceWeight = s_usManualIceWeight;
				s_ucManualIcePending = 0U;
				s_ucMaintenanceActive = 1U;
			} else {
				usManualIceWeight = 0U;
			}
			if ((usManualIceWeight == 0U) &&
				(s_xMaintenance.ucPending != 0U)) {
				xMaintenance = s_xMaintenance;
				s_xMaintenance.ucPending = 0U;
				s_ucMaintenanceActive = 1U;
			}
			taskEXIT_CRITICAL();
			/* 步骤 5：手动出冰先确认杯体，再执行出冰；故障时根据阀门是否
			 * 已动作决定直接收敛还是执行完整安全停止。 */
			if (usManualIceWeight != 0U) {
				int32_t lCupBaselineGram; /*!< 杯体放在秤上时的稳定总重量，单位为克。 */

				s_usActiveDevices = 0U;
				ucManualIceDispenseStarted = 0U;
				lCupBaselineGram = 0;
				taskENTER_CRITICAL();
				s_ulNextOrderEpoch++;
				if (s_ulNextOrderEpoch == 0U) {
					s_ulNextOrderEpoch = 1U;
				}
				g_xCoffee3WorkflowStatus.ulOrderEpoch =
					s_ulNextOrderEpoch;
				g_xCoffee3WorkflowStatus.usCurrentOrderId =
					COFFEE3_LOG_ORDER_DEBUG;
				g_xCoffee3WorkflowStatus.ucCancelRequested = 0U;
				taskEXIT_CRITICAL();
				(void)xCoffee3LogWriteFieldOrder(
					COFFEE3_LOG_LEVEL_INFO,
					COFFEE3_LOG_SOURCE_WORKFLOW,
					COFFEE3_LOG_ORDER_DEBUG,
					"MANUAL_ICE_START", 0, "target_g",
					(int32_t)usManualIceWeight);
				prvPublish(COFFEE3_WORKFLOW_RUNNING, 900U, 0);
				lResult = prvConfirmIceCup(
					s_lIceScaleEmptyBaselineGram, 901U,
					&lCupBaselineGram);
				if (lResult == 0) {
					ucManualIceDispenseStarted = 1U;
					lResult = prvDispenseIce(usManualIceWeight,
						lCupBaselineGram);
				}
				if (lResult == 0) {
					taskENTER_CRITICAL();
					g_xCoffee3WorkflowStatus.ucOrderAdmissionOpen = 1U;
					taskEXIT_CRITICAL();
					prvPublish(COFFEE3_WORKFLOW_COMPLETED,
						903U, 0);
					(void)xCoffee3LogWriteFieldOrder(
						COFFEE3_LOG_LEVEL_INFO,
						COFFEE3_LOG_SOURCE_WORKFLOW,
						COFFEE3_LOG_ORDER_DEBUG,
						"MANUAL_ICE_DONE", 0, "target_g",
						(int32_t)usManualIceWeight);
				} else {
					/* 出冰前判杯失败时没有活动执行机构需要停止；重量范围失败返回前
					 * 也已经关闭出冰阀。 */
					if ((prvIceTerminalFailure(lResult) != 0U) ||
						(ucManualIceDispenseStarted == 0U)) {
						s_usActiveDevices = 0U;
						lSafetyResult = 0;
						g_xCoffee3WorkflowStatus.lSafetyResult = 0;
						taskENTER_CRITICAL();
						g_xCoffee3WorkflowStatus.ucOrderAdmissionOpen = 1U;
						taskEXIT_CRITICAL();
					} else {
						lSafetyResult = prvAbortDevices();
						g_xCoffee3WorkflowStatus.lSafetyResult = lSafetyResult;
						if (lSafetyResult == 0) {
							taskENTER_CRITICAL();
							g_xCoffee3WorkflowStatus.ucOrderAdmissionOpen = 1U;
							taskEXIT_CRITICAL();
						} else {
							g_xCoffee3WorkflowStatus.ucRecoveryRequired = 1U;
						}
					}
					prvPublish(COFFEE3_WORKFLOW_FAILED,
						g_xCoffee3WorkflowStatus.usCurrentStep,
						lResult);
					(void)xCoffee3LogWriteFieldOrder(
						COFFEE3_LOG_LEVEL_ERROR,
						COFFEE3_LOG_SOURCE_WORKFLOW,
						COFFEE3_LOG_ORDER_DEBUG,
						"MANUAL_ICE_FAILED", lResult, "step",
						(int32_t)
							g_xCoffee3WorkflowStatus.usCurrentStep);
				}
			}
			if (usManualIceWeight != 0U) {
				s_ucMaintenanceActive = 0U;
			}
			/* 步骤 6：维护请求使用独立代次执行，失败后关闭活动设备并
			 * 保持告警状态，等待操作者再次处理。 */
			if (xMaintenance.xType != COFFEE3_MAINTENANCE_NONE) {
				s_usActiveDevices = 0U;
				taskENTER_CRITICAL();
				s_ulNextOrderEpoch++;
				if (s_ulNextOrderEpoch == 0U) {
					s_ulNextOrderEpoch = 1U;
				}
				g_xCoffee3WorkflowStatus.ulOrderEpoch = s_ulNextOrderEpoch;
				g_xCoffee3WorkflowStatus.usCurrentOrderId = COFFEE3_LOG_ORDER_DEBUG;
				g_xCoffee3WorkflowStatus.ucCancelRequested = 0U;
				taskEXIT_CRITICAL();
				g_xCoffee3WorkflowStatus.xMachineState =
					COFFEE3_MACHINE_BUSY;
				lResult = prvRunMaintenance(&xMaintenance);
				if (lResult != 0) {
					g_xCoffee3WorkflowStatus.lSafetyResult = prvAbortDevices();
					/* 维护失败保持为本地故障，保留再次提交维护请求的能力。 */
				}
				taskENTER_CRITICAL();
				s_ucMaintenanceActive = 0U;
				g_xCoffee3WorkflowStatus.ucOrderAdmissionOpen =
					(lResult == 0) ? 1U : 0U;
				taskEXIT_CRITICAL();
				g_xCoffee3WorkflowStatus.xMachineState =
					(lResult == 0) ? COFFEE3_MACHINE_IDLE :
						COFFEE3_MACHINE_ALARM;
				g_xCoffee3WorkflowStatus.xState = (lResult == 0) ?
					COFFEE3_WORKFLOW_IDLE : COFFEE3_WORKFLOW_FAILED;
				(void)xCoffee3LogWriteFieldOrder(
					(lResult == 0) ? COFFEE3_LOG_LEVEL_INFO :
						COFFEE3_LOG_LEVEL_ERROR,
					COFFEE3_LOG_SOURCE_WORKFLOW,
					COFFEE3_LOG_ORDER_DEBUG,
					(lResult == 0) ? "MAINTENANCE_COMPLETE" :
						"MAINTENANCE_FAILED",
					lResult, "type", (int32_t)xMaintenance.xType);
			}
			prvServiceIoRefresh();
			continue;
		}
		taskENTER_CRITICAL();
		/* 步骤 7：在同一预约边界内接管新订单，分配不可复用的代次并
		 * 清空上一单的过程状态；运行期间接受的调试请求仅在机械臂
		 * 所有权空闲时插入执行。 */
		if (s_usManualReservations != 0U) {
			taskEXIT_CRITICAL();
			if (xQueueSendToFront(s_xOrderQueue, &xOrder, 0U) != pdPASS) {
				(void)xCoffee3LogWrite(COFFEE3_LOG_LEVEL_ERROR,
					COFFEE3_LOG_SOURCE_WORKFLOW,
					"ORDER_REQUEUE_FAILED_FOR_MANUAL", -1);
			}
			continue;
		}
		s_ulNextOrderEpoch++;
		if (s_ulNextOrderEpoch == 0U) {
			s_ulNextOrderEpoch = 1U;
		}
		g_xCoffee3WorkflowStatus.ulOrderEpoch = s_ulNextOrderEpoch;
		g_xCoffee3WorkflowStatus.usCurrentOrderId =
			xOrder.ausRegister[COFFEE3_REG_ORDER_NUMBER];
		g_xCoffee3WorkflowStatus.ucCancelRequested = 0U;
		s_ucManualOverride = 0U;
		s_usActiveDevices = 0U;
		g_xCoffee3WorkflowStatus.ucPositionUncertain = 0U;
		g_xCoffee3WorkflowStatus.ucCommandSent = 0U;
		g_xCoffee3WorkflowStatus.ucDeviceDone = 0U;
		g_xCoffee3WorkflowStatus.ucPhysicalVerified = 0U;
		g_xCoffee3WorkflowStatus.lSafetyResult = 0;
		g_xCoffee3WorkflowStatus.usActiveOutput = 1U;
		g_xCoffee3WorkflowStatus.ucContentComplete = 0U;
		g_xCoffee3WorkflowStatus.xState = COFFEE3_WORKFLOW_RUNNING;
		taskEXIT_CRITICAL();
		vCoffee3ServerPublishOrder(&xOrder);
		prvPublish(COFFEE3_WORKFLOW_RUNNING, 1U, 0);
		(void)xCoffee3LogWriteFieldOrder(COFFEE3_LOG_LEVEL_INFO,
			COFFEE3_LOG_SOURCE_WORKFLOW,
			g_xCoffee3WorkflowStatus.usCurrentOrderId,
			"ORDER_START", 0,
			"order", (int32_t)g_xCoffee3WorkflowStatus.usCurrentOrderId);
		/* 步骤 8：执行订单后立即结束主机请求；成功时开放下一单，
		 * 失败时按故障类型选择人工恢复锁定或安全停止。 */
		lResult = prvRunOrder(&xOrder);
		vCoffee3ServerFinishRequest(0U);
		if (lResult == 0) {
			g_xCoffee3WorkflowStatus.ucPositionUncertain = 0U;
			if (prvQueuePendingOrder() == 0U) {
				taskENTER_CRITICAL();
				if ((s_ucStoragePickupPending == 0U) &&
					(s_ucStoragePickupActive == 0U)) {
					g_xCoffee3WorkflowStatus.ucOrderAdmissionOpen = 1U;
				}
				taskEXIT_CRITICAL();
			}
			taskENTER_CRITICAL();
			g_xCoffee3WorkflowStatus.ulCompletedOrderCount++;
			taskEXIT_CRITICAL();
			prvPublish(COFFEE3_WORKFLOW_COMPLETED,
				g_xCoffee3WorkflowStatus.usCurrentStep, 0);
			(void)xCoffee3LogWriteFieldOrder(COFFEE3_LOG_LEVEL_INFO,
				COFFEE3_LOG_SOURCE_WORKFLOW,
				g_xCoffee3WorkflowStatus.usCurrentOrderId,
				"ORDER_COMPLETED", 0,
				"order",
				(int32_t)g_xCoffee3WorkflowStatus.usCurrentOrderId);
		} else {
			taskENTER_CRITICAL();
			g_xCoffee3WorkflowStatus.ulFailedOrderCount++;
			taskEXIT_CRITICAL();
			if (prvIceTerminalFailure(lResult) != 0U) {
				/* 已确认的冰重或判杯终态故障会让机械臂停留在制冰位，
				 * 不再发出恢复移动命令。 */
				s_usActiveDevices = 0U;
				lSafetyResult = 0;
				g_xCoffee3WorkflowStatus.lSafetyResult = 0;
				g_xCoffee3WorkflowStatus.ucRecoveryRequired = 1U;
				g_xCoffee3WorkflowStatus.ucOrderAdmissionOpen = 0U;
				s_ucPendingOrder = 0U;
				if (lResult == COFFEE3_WORKFLOW_ERROR_ICE_RANGE) {
					pcIceFailureLog =
						"Order failed: reason=ice weight unsafe; cup stays at ice station";
				} else if (lResult == COFFEE3_WORKFLOW_ERROR_ICE_NO_CUP) {
					pcIceFailureLog =
						"Order failed: reason=no cup at ice station; motion skipped";
				} else {
					pcIceFailureLog =
						"Order failed: reason=cup weight invalid; motion skipped";
				}
				(void)xCoffee3LogPrintfOrder(COFFEE3_LOG_LEVEL_ERROR,
					COFFEE3_LOG_SOURCE_WORKFLOW,
					g_xCoffee3WorkflowStatus.usCurrentOrderId,
					pcIceFailureLog);
			} else {
				lSafetyResult = (s_usActiveDevices != 0U) ? prvAbortDevices() : 0;
				g_xCoffee3WorkflowStatus.lSafetyResult = lSafetyResult;
				if ((lSafetyResult == 0) &&
					(g_xCoffee3WorkflowStatus.ucPositionUncertain == 0U)) {
					if (prvQueuePendingOrder() == 0U) {
						taskENTER_CRITICAL();
						if ((s_ucStoragePickupPending == 0U) &&
							(s_ucStoragePickupActive == 0U)) {
							g_xCoffee3WorkflowStatus.ucOrderAdmissionOpen = 1U;
						}
						taskEXIT_CRITICAL();
					}
				} else {
					g_xCoffee3WorkflowStatus.ucRecoveryRequired = 1U;
					g_xCoffee3WorkflowStatus.ucOrderAdmissionOpen = 0U;
					s_ucPendingOrder = 0U;
					(void)xCoffee3LogPrintfOrder(COFFEE3_LOG_LEVEL_ERROR,
						COFFEE3_LOG_SOURCE_WORKFLOW,
						g_xCoffee3WorkflowStatus.usCurrentOrderId,
						"Recovery locked: stop=%ld position_unknown=%u; inspect/reset",
						(long)lSafetyResult,
						(unsigned int)g_xCoffee3WorkflowStatus.ucPositionUncertain);
				}
			}
			prvPublish(COFFEE3_WORKFLOW_FAILED,
				g_xCoffee3WorkflowStatus.usCurrentStep, lResult);
			(void)xCoffee3LogWriteFieldOrder(
				(s_ucManualOverride != 0U) ? COFFEE3_LOG_LEVEL_WARNING :
					COFFEE3_LOG_LEVEL_ERROR,
				COFFEE3_LOG_SOURCE_WORKFLOW,
				g_xCoffee3WorkflowStatus.usCurrentOrderId,
				(s_ucManualOverride != 0U) ? "ORDER_CANCELED" : "ORDER_FAILED",
				lResult, "step",
				(int32_t)g_xCoffee3WorkflowStatus.usCurrentStep);
		}
	}
}

/*-----------------------------------------------------------*/
/**
  * @brief  判断动作是否会改变机械臂的物理位置。
  * @param[in] xAction 待检查的机械臂动作。
  * @retval 1 动作会改变位置，执行期间位置应视为不确定。
  * @retval 0 动作不属于位置移动。
  */
static uint8_t prvRobotPositionAction(Coffee3Action_e xAction)
{
	return (((xAction >= COFFEE3_ACTION_ROBOT_HOME) &&
		(xAction <= COFFEE3_ACTION_ROBOT_TAKE_STORAGE)) ||
		(xAction == COFFEE3_ACTION_ROBOT_TO_FRUIT_SYRUP)) ? 1U : 0U;
}

/*-----------------------------------------------------------*/
/**
  * @brief  将取消期间暂存的订单重新送入队首。
  * @retval 1 已存在待处理订单，本轮不应开放新订单准入。
  * @retval 0 没有暂存订单。
  */
static uint8_t prvQueuePendingOrder(void)
{
	Coffee3Order_t xOrder; /*!< 取消期间暂存、准备回插队首的订单快照。 */

	taskENTER_CRITICAL();
	if ((s_ucPendingOrder == 0U) || (s_usManualReservations != 0U)) {
		taskEXIT_CRITICAL();
		return (s_ucPendingOrder != 0U) ? 1U : 0U;
	}
	xOrder = s_xPendingOrder;
	s_ucPendingOrder = 0U;
	taskEXIT_CRITICAL();
	if (xQueueSendToFront(s_xOrderQueue, &xOrder, 0U) != pdPASS) {
		taskENTER_CRITICAL();
		s_xPendingOrder = xOrder;
		s_ucPendingOrder = 1U;
		taskEXIT_CRITICAL();
		return 1U;
	}
	(void)xCoffee3LogWriteFieldOrder(COFFEE3_LOG_LEVEL_INFO,
		COFFEE3_LOG_SOURCE_WORKFLOW,
		g_xCoffee3WorkflowStatus.usCurrentOrderId,
		"ORDER_CANCELED", 0,
		"order", (int32_t)
			g_xCoffee3WorkflowStatus.usCurrentOrderId);
	(void)xCoffee3LogWriteFieldOrder(COFFEE3_LOG_LEVEL_INFO,
		COFFEE3_LOG_SOURCE_WORKFLOW,
		xOrder.ausRegister[COFFEE3_REG_ORDER_NUMBER],
		"ORDER_ACCEPTED", 0,
		"order", (int32_t)xOrder.ausRegister[
			COFFEE3_REG_ORDER_NUMBER]);
	return 1U;
}

/*-----------------------------------------------------------*/
/**
  * @brief  提交一个设备命令并等待该设备独立事件组的终态。
  * @param[in] usStep 当前工作流步骤编号。
  * @param[in] xDeviceId 命令目标逻辑设备。
  * @param[in] xAction 目标设备动作枚举值。
  * @param[in] usParameter0 动作参数 0。
  * @param[in] usParameter1 动作参数 1。
  * @param[in] ulTimeoutMs 当前步骤总超时时间，单位为毫秒。
  * @retval 0 设备报告命令完成。
  * @retval 负数 工作流取消、队列、设备或超时错误。
  */
static int32_t prvRunStep(uint16_t usStep, Coffee3DeviceId_e xDeviceId,
	Coffee3Action_e xAction, uint16_t usParameter0,
	uint16_t usParameter1, uint32_t ulTimeoutMs)
{
	/* 本函数负责一次完整的提交、等待和结果读取；设备事务成功后，
	 * 调用者仍可能需要继续验证物理传感器后置条件。 */
	Coffee3Command_t xCommand; /*!< 本次向设备提交并追踪终态的命令。 */
	EventBits_t xEvents; /*!< 设备事件组返回的完成或失败标志。 */
	TickType_t xStartTick; /*!< 本次动作或等待开始的系统节拍。 */
	TickType_t xTimeoutTicks; /*!< 本步骤超时预算对应的节拍数。 */
	uint8_t ucOrderStep; /*!< 是否采用低编号业务步骤的处理规则。 */
	uint16_t usLogOrder; /*!< 当前日志使用的订单号或调试标识。 */
	uint8_t ucInitializationStep; /*!< 步骤号是否属于初始化流程。 */
	uint8_t ucResultValid; /*!< 设备命令终态是否已取得。 */
	uint8_t ucRobotMotion; /*!< 当前机器人动作是否可能改变位置。 */
	uint8_t ucTimeoutLogged; /*!< 本次等待的逾期日志是否已经写入。 */
	int32_t lCommandResult; /*!< 当前设备命令的终态结果。 */
	int32_t lInterleaveResult; /*!< 插入执行的暂存位取餐结果。 */
	const Coffee3DeviceBinding_t *pxBinding; /*!< 目标逻辑设备对应的静态绑定信息。 */

	/* 步骤 1：按步骤号区分订单、初始化和维护上下文，确定日志订单号、
	 * 机械臂位置风险以及进入本步骤前是否已收到取消。 */
	/* FDxx 为初始化诊断步骤，低编号步骤属于订单执行过程。 */
	ucOrderStep = (usStep < 0xF000U) ? 1U : 0U;
	pxBinding = pxCoffee3DeviceGetBinding(xDeviceId);
	ucInitializationStep = ((usStep >= 0xFD00U) &&
		(usStep < 0xFE00U)) ? 1U : 0U;
	ucRobotMotion = ((xDeviceId == COFFEE3_DEVICE_ROBOT) &&
		(prvRobotPositionAction(xAction) != 0U)) ? 1U : 0U;
	ucTimeoutLogged = 0U;
	usLogOrder = (ucOrderStep != 0U) ?
		g_xCoffee3WorkflowStatus.usCurrentOrderId :
		((s_usDetachedPickupLogOrder != 0U) ?
		 s_usDetachedPickupLogOrder : COFFEE3_LOG_ORDER_DEBUG);
	if (((ucOrderStep != 0U) || (s_ucMaintenanceActive != 0U)) &&
		(g_xCoffee3WorkflowStatus.ucCancelRequested != 0U)) {
		return COFFEE3_WORKFLOW_ERROR_CANCELED;
	}
	if (ucInitializationStep == 0U) {
		(void)xCoffee3LogPrintfOrder(COFFEE3_LOG_LEVEL_INFO,
			COFFEE3_LOG_SOURCE_WORKFLOW, usLogOrder,
			"Step start: %s %s step=%u",
			prvWorkflowDeviceName(xDeviceId), prvWorkflowActionName(xAction),
			(unsigned int)usStep);
	}
	if (ucOrderStep != 0U) {
		g_xCoffee3WorkflowStatus.ucDeviceId = (uint8_t)xDeviceId;
		g_xCoffee3WorkflowStatus.usAction = (uint16_t)xAction;
		g_xCoffee3WorkflowStatus.ucCommandSent = 0U;
		g_xCoffee3WorkflowStatus.ucDeviceDone = 0U;
		g_xCoffee3WorkflowStatus.ucPhysicalVerified = 0U;
		prvPublish(COFFEE3_WORKFLOW_RUNNING, usStep, 0);
	}
	/* 步骤 2：构造带订单代次和独立超时的设备命令；提交失败时立即
	 * 返回队列错误，不进入等待阶段。 */
	memset(&xCommand, 0, sizeof(xCommand));
	xCommand.ulOrderId = usLogOrder;
	xCommand.ulOrderEpoch = ((ucOrderStep != 0U) ||
		(s_ucMaintenanceActive != 0U)) ?
		g_xCoffee3WorkflowStatus.ulOrderEpoch : 0U;
	xCommand.usStepId = usStep;
	xCommand.usAction = (uint16_t)xAction;
	xCommand.ausParameter[0] = usParameter0;
	xCommand.ausParameter[1] = usParameter1;
	if (xDeviceId == COFFEE3_DEVICE_ROBOT) {
		xCommand.ulTimeoutMs = ulTimeoutMs;
	} else if (xDeviceId == COFFEE3_DEVICE_COFFEE_MACHINE) {
		xCommand.ulTimeoutMs = (xAction == COFFEE3_ACTION_COFFEE_MAKE) ?
			COFFEE3_COFFEE_ACTION_TIMEOUT_MS : COFFEE3_WORKFLOW_DEVICE_IO_TIMEOUT_MS;
	} else {
		xCommand.ulTimeoutMs = COFFEE3_RTU_IO_TIMEOUT_MS;
	}
	xCommand.ucDeviceId = (uint8_t)xDeviceId;
	xCommand.ucSource = ((ucOrderStep != 0U) ||
		(s_ucMaintenanceActive != 0U)) ?
		(uint8_t)COFFEE3_COMMAND_SOURCE_WORKFLOW :
		(uint8_t)COFFEE3_COMMAND_SOURCE_MAINTENANCE;
	xCommand.ucRetryLimit = 1U;
	if (xCoffee3CommandSubmit(&xCommand, pdMS_TO_TICKS(100U)) !=
		pdPASS) {
		(void)xCoffee3LogWriteFieldOrder(COFFEE3_LOG_LEVEL_ERROR,
			COFFEE3_LOG_SOURCE_WORKFLOW,
			usLogOrder,
			"WORKFLOW_STEP_QUEUE_FAILED",
			COFFEE3_WORKFLOW_ERROR_QUEUE, "step", (int32_t)usStep);
		return COFFEE3_WORKFLOW_ERROR_QUEUE;
	}
	/* 步骤 3：提交成功后登记活动设备；机械臂位置动作在收到终态前
	 * 始终标记为位置不确定。 */
	xStartTick = xTaskGetTickCount();
	if (ucOrderStep != 0U) {
		g_xCoffee3WorkflowStatus.ucCommandSent = 1U;
		if ((xDeviceId == COFFEE3_DEVICE_ROBOT) &&
			(prvRobotPositionAction(xAction) != 0U)) {
			g_xCoffee3WorkflowStatus.ucPositionUncertain = 1U;
		}
	}
	if (((ucOrderStep != 0U) || (s_ucMaintenanceActive != 0U)) &&
		(xAction != COFFEE3_ACTION_REFRESH)) {
		s_usActiveDevices |= (uint16_t)(1U << (uint8_t)xDeviceId);
	}
	xTimeoutTicks = pdMS_TO_TICKS((xDeviceId == COFFEE3_DEVICE_ROBOT) ?
		ulTimeoutMs : (ulTimeoutMs +
			(4U * COFFEE3_WORKFLOW_DEVICE_IO_TIMEOUT_MS)));
	if (ucRobotMotion != 0U) {
		/* 为机械臂所有者接收前滞留的命令设置独立诊断期限；该期限到期
		 * 仅报告异常，不会直接终止机械臂的实际运动。 */
		xTimeoutTicks = pdMS_TO_TICKS(COFFEE3_ROBOT_RECOVERY_TIMEOUT_MS +
			COFFEE3_ROBOT_ACCEPT_TIMEOUT_MS + COFFEE3_ROBOT_MOTION_TIMEOUT_MS +
			(COFFEE3_ROBOT_PREPARE_RETRY_LIMIT + 1U) *
			COFFEE3_ROBOT_PREPARE_RETRY_MS + 2U * COFFEE3_ROBOT_IO_TIMEOUT_MS);
	}
	/* 步骤 4：按命令编号和订单代次等待唯一终态，避免把其他命令的
	 * 完成事件误认为当前步骤结果。 */
	for (;;) {
		xEvents = xCoffee3DeviceWaitCommand(xDeviceId,
			xCommand.ulOrderEpoch, xCommand.ulCommandId,
			pdMS_TO_TICKS(100U));
		/* 步骤 5：设备取消事件优先处理；若机械臂命令被手动动作替代，
		 * 同时记录手动接管，后续不再自动取消该机械臂动作。 */
		if ((xEvents & COFFEE3_DEVICE_EVENT_CANCELED) != 0U) {
			uint8_t ucTerminalValid; /*!< 目标设备命令终态是否有效。 */
			int32_t lTerminalResult; /*!< 目标设备命令返回的终态结果。 */
			lTerminalResult = lCoffee3DeviceGetTerminalResult(xDeviceId,
				xCommand.ulOrderEpoch, xCommand.ulCommandId,
				&ucTerminalValid);
			if ((ucOrderStep != 0U) &&
				(xDeviceId == COFFEE3_DEVICE_ROBOT) &&
				(ucTerminalValid != 0U) &&
				(lTerminalResult == COFFEE3_COMMAND_RESULT_SUPERSEDED)) {
				s_ucManualOverride = 1U;
				(void)xCoffee3LogWriteFieldOrder(COFFEE3_LOG_LEVEL_WARNING,
					COFFEE3_LOG_SOURCE_WORKFLOW,
					usLogOrder,
					"WORKFLOW_MANUAL_OVERRIDE",
					COFFEE3_COMMAND_RESULT_CANCELED,
					"step", (int32_t)usStep);
			}
			(void)xCoffee3LogWriteFieldOrder(COFFEE3_LOG_LEVEL_WARNING,
				COFFEE3_LOG_SOURCE_WORKFLOW,
				usLogOrder,
				"WORKFLOW_STEP_CANCELED",
				COFFEE3_WORKFLOW_ERROR_CANCELED, "step",
				(int32_t)usStep);
			return COFFEE3_WORKFLOW_ERROR_CANCELED;
		}
		if ((xEvents & COFFEE3_DEVICE_EVENT_COMMAND_DONE) != 0U) {
			if (ucOrderStep != 0U) {
				g_xCoffee3WorkflowStatus.ucDeviceDone = 1U;
			}
			if (ucInitializationStep == 0U) {
				(void)xCoffee3LogPrintfOrder(COFFEE3_LOG_LEVEL_INFO,
				COFFEE3_LOG_SOURCE_WORKFLOW, usLogOrder,
				"Step done: %s %s step=%u result=0",
				prvWorkflowDeviceName(xDeviceId), prvWorkflowActionName(xAction),
				(unsigned int)usStep);
			}
			return 0;
		}
		if ((xEvents & COFFEE3_DEVICE_EVENT_TERMINAL) != 0U) {
			lCommandResult = lCoffee3DeviceGetTerminalResult(xDeviceId,
				xCommand.ulOrderEpoch, xCommand.ulCommandId, &ucResultValid);
			if (ucResultValid == 0U) {
				lCommandResult = COFFEE3_WORKFLOW_ERROR_DEVICE;
			}
			if (ucInitializationStep != 0U) {
				if (s_usLastInitializationFailure != usStep) {
					const char *pcDeviceName; /*!< 日志中使用的目标设备名称。 */
					switch (usStep) {
					case 0xFD10U:
						pcDeviceName = "IO_INPUT_MODULE_16CH";
						break;
					case 0xFD11U:
						pcDeviceName = "CUP_MACHINE";
						break;
					case 0xFD12U:
						pcDeviceName = "LID_MACHINE";
						break;
					default:
						pcDeviceName = "INITIALIZATION_DEVICE";
						break;
					}
					(void)xCoffee3LogPrintfOrder(COFFEE3_LOG_LEVEL_ERROR,
						COFFEE3_LOG_SOURCE_WORKFLOW,
						COFFEE3_LOG_ORDER_SYSTEM,
						"%s_INIT_FAILED_RESULT=%ld",
						pcDeviceName,
						(long)lCommandResult);
					s_usLastInitializationFailure = usStep;
				}
			} else {
				(void)xCoffee3LogPrintfOrder(COFFEE3_LOG_LEVEL_ERROR,
					COFFEE3_LOG_SOURCE_WORKFLOW,
					usLogOrder,
					"Step %u %s failed: action=%u result=%ld",
					(unsigned int)usStep,
					(pxBinding != NULL) ? pxBinding->pcName : "Unknown",
					(unsigned int)xAction, (long)lCommandResult);
			}
			return COFFEE3_WORKFLOW_ERROR_DEVICE;
		}
		if (((ucOrderStep != 0U) || (s_ucMaintenanceActive != 0U)) &&
			(g_xCoffee3WorkflowStatus.ucCancelRequested != 0U)) {
			(void)xCoffee3LogWriteFieldOrder(COFFEE3_LOG_LEVEL_WARNING,
				COFFEE3_LOG_SOURCE_WORKFLOW,
				usLogOrder,
				"WORKFLOW_STEP_CANCELED",
				COFFEE3_WORKFLOW_ERROR_CANCELED, "step",
				(int32_t)usStep);
			return COFFEE3_WORKFLOW_ERROR_CANCELED;
		}
		/* 步骤 6：咖啡制作等待期间允许插入一次暂存取杯。Coffee1 允许
		 * 咖啡机持杯且机械臂空闲时执行暂存取杯；复用当前
		 * 等待循环作为调度点，分离的取杯步骤仍由机械臂所有者串行执行。 */
		if ((xDeviceId == COFFEE3_DEVICE_COFFEE_MACHINE) &&
			(xAction == COFFEE3_ACTION_COFFEE_MAKE)) {
			lInterleaveResult = prvTryRunInterleavedStoragePickup();
			if (lInterleaveResult != 0) {
				return lInterleaveResult;
			}
		}
		/* 步骤 7：普通设备逾期立即返回超时；机械臂位置动作逾期仅记录
		 * 一次诊断并继续等待真实终态，避免误判运动已经停止。 */
		if ((xTaskGetTickCount() - xStartTick) >= xTimeoutTicks) {
			if (ucRobotMotion != 0U) {
				if (ucTimeoutLogged == 0U) {
					ucTimeoutLogged = 1U;
					(void)xCoffee3LogPrintfOrder(
						COFFEE3_LOG_LEVEL_WARNING,
						COFFEE3_LOG_SOURCE_WORKFLOW,
						usLogOrder,
						"Step overdue: device=Robot action=%s step=%u; waiting",
						prvWorkflowActionName(xAction),
						(unsigned int)usStep);
				}
				prvServiceHotWater();
				prvServiceIoRefresh();
				continue;
			}
			(void)xCoffee3LogWriteFieldOrder(COFFEE3_LOG_LEVEL_ERROR,
				COFFEE3_LOG_SOURCE_WORKFLOW,
				usLogOrder,
				"WORKFLOW_STEP_TIMEOUT",
				COFFEE3_WORKFLOW_ERROR_TIMEOUT, "step",
				(int32_t)usStep);
			return COFFEE3_WORKFLOW_ERROR_TIMEOUT;
		}
		prvServiceHotWater();
		prvServiceIoRefresh();
	}
}

/*-----------------------------------------------------------*/
/**
  * @brief  按当前配置执行一份完整咖啡订单。
  * @param[in] pxOrder 已复制到工作流上下文的订单寄存器镜像。
  * @retval 0 所有必要步骤完成。
  * @retval 负数 设备、打印、称重、取消或工作流超时错误。
  */
static int32_t prvRunOrder(const Coffee3Order_t *pxOrder)
{
	/* 机械臂运动前先预约暂存位或出杯口，再按订单顺序驱动设备，
	 * 并逐项验证对应的物理后置条件。 */
	int32_t lResult; /*!< 当前子步骤的执行结果或错误码。 */
	int32_t lValidationError; /*!< 订单字段校验失败时的业务错误码。 */
	int32_t lIceEmptyBaselineGram; /*!< 空秤稳定总重量，单位为克。 */
	int32_t lIceCupBaselineGram; /*!< 带杯后的稳定总重量，单位为克。 */
	int32_t lIceEmptyDeltaGram; /*!< 复核空秤时相对基线的重量差，单位为克。 */
	uint16_t usIceAmount; /*!< 配方要求的冰重，单位为克。 */
	uint16_t usSyrupAmount; /*!< 配方要求的糖浆用量。 */
	uint16_t usColdOrder; /*!< 配方冰量非零时选择冷饮制作路径。 */
	uint16_t usLidLane; /*!< 本订单选择的落盖通道。 */
	uint16_t usOutput; /*!< 本订单目标出餐口编号。 */
	uint8_t ucOffline; /*!< 订单是否走直接出餐的离线路径。 */
	Coffee3IoState_t xIo; /*!< 当前读取的输入与输出状态快照。 */
	uint8_t ucNeedCoffeeStation; /*!< 配方是否需要经过咖啡工位。 */
	uint8_t ucNeedFlavorStation; /*!< 配方是否需要经过加料工位。 */

	lIceEmptyBaselineGram = 0;
	lIceCupBaselineGram = 0;
	lIceEmptyDeltaGram = 0;

	/* 步骤 1：校验订单及配方依赖，并在任何机械动作前确认水、废料、
	 * IO 和目标暂存位或出杯口均可用。 */
	if (prvOrderValid(pxOrder, &lValidationError) == 0U) {
		(void)xCoffee3LogWriteFieldOrder(COFFEE3_LOG_LEVEL_ERROR,
			COFFEE3_LOG_SOURCE_WORKFLOW,
			(pxOrder != NULL) ? pxOrder->ausRegister[
				COFFEE3_REG_ORDER_NUMBER] : COFFEE3_LOG_ORDER_SYSTEM,
			"ORDER_VALIDATION_FAILED", lValidationError,
			"reason", lValidationError);
		return lValidationError;
	}
	if (prvOrderDevicesReady(pxOrder) == 0U) {
		return COFFEE3_WORKFLOW_ERROR_DEVICE;
	}
	usIceAmount = pxOrder->ausRegister[COFFEE3_REG_ICE_AMOUNT];
	usColdOrder = (usIceAmount != 0U) ? 1U : 0U;
	usLidLane = (usColdOrder != 0U) ? 2U : 1U;
	ucOffline = ((pxOrder->ausRegister[COFFEE3_REG_ORDER_NUMBER] & 0xF000U) == 0xD000U);
	usOutput = 1U;
	g_xCoffee3WorkflowStatus.ucContentComplete = 0U;
	g_xCoffee3WorkflowStatus.ucStorageReserved = 0U;
	lResult = prvRefreshDeviceQuiet(9U, COFFEE3_DEVICE_IO_INPUT);
	vCoffee3IoGetSnapshot(&xIo);
	if ((lResult != 0) || (prvIoValid(&xIo) == 0U) ||
		(xIo.xInput.aucMB1XPin[3] == 0U) ||
		(xIo.xInput.aucMB1XPin[4] == 0U) ||
		(xIo.xInput.aucMB1XPin[5] == 0U) ||
		(xIo.xInput.aucMB1XPin[6] != 0U)) {
		(void)xCoffee3LogPrintfOrder(COFFEE3_LOG_LEVEL_WARNING,
			COFFEE3_LOG_SOURCE_WORKFLOW, g_xCoffee3WorkflowStatus.usCurrentOrderId,
			"Order rejected before motion: IO, water, bins, or waste not ready");
		return COFFEE3_WORKFLOW_ERROR_IO;
	}
	lResult = (ucOffline != 0U) ? prvCheckOutputEmpty(1U) : prvSelectStorage();
	if ((ucOffline == 0U) && (lResult > 0)) {
		g_xCoffee3WorkflowStatus.ucStorageReserved = (uint8_t)lResult;
		lResult = 0;
	}
	if (lResult != 0) {
		(void)xCoffee3LogPrintfOrder(COFFEE3_LOG_LEVEL_WARNING,
			COFFEE3_LOG_SOURCE_WORKFLOW, g_xCoffee3WorkflowStatus.usCurrentOrderId,
			"Order rejected: %s unavailable; no production action sent",
			(ucOffline != 0U) ? "outlet" : "storage");
		return lResult;
	}
	/* 步骤 2：根据配方裁剪设备路径，准备机械臂，并刷新本单实际使用的
	 * 咖啡、杯、盖、糖浆和制冰设备。 */
	ucNeedCoffeeStation =
		(pxOrder->ausRegister[COFFEE3_REG_COFFEE_TYPE] != 0xFFFFU) ?
			1U : 0U;
	ucNeedFlavorStation =
		((pxOrder->ausRegister[COFFEE3_REG_FRUIT_MILK_A] != 0U) ||
		(pxOrder->ausRegister[COFFEE3_REG_FRUIT_MILK_B] != 0U) ||
		(pxOrder->ausRegister[COFFEE3_REG_SYRUP_1] != 0U) ||
		(pxOrder->ausRegister[COFFEE3_REG_SYRUP_2] != 0U) ||
		(pxOrder->ausRegister[COFFEE3_REG_SYRUP_3] != 0U) ||
		(pxOrder->ausRegister[COFFEE3_REG_SYRUP_4] != 0U)) ? 1U : 0U;
	lResult = prvRunStep(10U, COFFEE3_DEVICE_ROBOT,
		COFFEE3_ACTION_ROBOT_PREPARE_ORDER, 0U, 0U, 30000U);
	if ((lResult == 0) && (ucNeedCoffeeStation != 0U)) {
		lResult = prvRunStep(20U, COFFEE3_DEVICE_COFFEE_MACHINE,
			COFFEE3_ACTION_REFRESH, 0U, 0U, 3000U);
	}
	if (lResult == 0) {
		lResult = prvRunStep(22U, COFFEE3_DEVICE_CUP_MACHINE,
			COFFEE3_ACTION_REFRESH, 0U, 0U, 3000U);
	}
	if ((lResult == 0) &&
		(pxOrder->ausRegister[COFFEE3_REG_LID_ENABLE] != 0U)) {
		lResult = prvRunStep(24U, COFFEE3_DEVICE_LID_MACHINE,
			COFFEE3_ACTION_REFRESH, 0U, 0U, 3000U);
	}
	if ((lResult == 0) &&
		((pxOrder->ausRegister[COFFEE3_REG_SYRUP_1] != 0U) ||
		(pxOrder->ausRegister[COFFEE3_REG_SYRUP_2] != 0U) ||
		(pxOrder->ausRegister[COFFEE3_REG_SYRUP_3] != 0U) ||
		(pxOrder->ausRegister[COFFEE3_REG_SYRUP_4] != 0U))) {
		lResult = prvRunStep(26U, COFFEE3_DEVICE_SYRUP_MACHINE,
			COFFEE3_ACTION_REFRESH, 0U, 0U, 3000U);
	}
	if ((lResult == 0) && (usIceAmount != 0U)) {
		lResult = prvRunStep(28U, COFFEE3_DEVICE_ICE_MACHINE,
			COFFEE3_ACTION_REFRESH, 0U, 0U, 3000U);
	}
	if ((lResult == 0) && (ucOffline != 0U)) {
		lResult = prvCheckOutputEmpty(usOutput);
	}
	/* 步骤 3：机械臂到杯机取冷热杯，杯机落杯后必须等待对应杯位
	 * 物理传感器确认，成功后才继续移动。 */
	if (lResult == 0) {
		lResult = prvRunStep(40U, COFFEE3_DEVICE_ROBOT,
			(usColdOrder != 0U) ?
				COFFEE3_ACTION_ROBOT_TAKE_COLD_CUP :
				COFFEE3_ACTION_ROBOT_TAKE_HOT_CUP,
			0U, 0U, COFFEE3_WORKFLOW_ROBOT_MOTION_MS);
	}
	if (lResult == 0) {
		lResult = prvRunStep(50U, COFFEE3_DEVICE_CUP_MACHINE,
			(usColdOrder != 0U) ?
				COFFEE3_ACTION_CUP_DROP_2 :
				COFFEE3_ACTION_CUP_DROP_1,
			0U, 0U, 3000U);
	}
	if (lResult == 0) {
		lResult = prvWaitBusinessCondition(55U,
			(usColdOrder != 0U) ? COFFEE3_CONDITION_CUP_2 :
				COFFEE3_CONDITION_CUP_1);
	}
	/* 步骤 4：冷饮先验证空秤仍接近启动基线，再将杯送到制冰位；
	 * 判杯成功后按重量出冰，防止秤上残留载荷被当作杯体。 */
	if ((lResult == 0) && (usIceAmount != 0U)) {
		lResult = prvReadStableScale(56U, &lIceEmptyBaselineGram);
		if (lResult == 0) {
			lIceEmptyDeltaGram = lIceEmptyBaselineGram -
				s_lIceScaleEmptyBaselineGram;
			if (lIceEmptyDeltaGram < 0) {
				lIceEmptyDeltaGram = -lIceEmptyDeltaGram;
			}
			if (lIceEmptyDeltaGram > COFFEE3_ICE_CUP_DETECT_GRAM) {
				(void)xCoffee3LogPrintfOrder(COFFEE3_LOG_LEVEL_ERROR,
					COFFEE3_LOG_SOURCE_WORKFLOW,
					g_xCoffee3WorkflowStatus.usCurrentOrderId,
					"Empty scale failed: current=%ld g startup=%ld g reason=load present",
					(long)lIceEmptyBaselineGram,
					(long)s_lIceScaleEmptyBaselineGram);
				lResult = COFFEE3_WORKFLOW_ERROR_ICE_BASELINE;
			} else {
				(void)xCoffee3LogPrintfOrder(COFFEE3_LOG_LEVEL_INFO,
					COFFEE3_LOG_SOURCE_WORKFLOW,
					g_xCoffee3WorkflowStatus.usCurrentOrderId,
					"Empty scale ready: current=%ld g startup=%ld g",
					(long)lIceEmptyBaselineGram,
					(long)s_lIceScaleEmptyBaselineGram);
			}
		}
	}
	if ((lResult == 0) && (usIceAmount != 0U)) {
		lResult = prvRunStep(60U, COFFEE3_DEVICE_ROBOT,
			COFFEE3_ACTION_ROBOT_TO_ICE, 0U, 0U,
			COFFEE3_WORKFLOW_ROBOT_MOTION_MS);
		if (lResult == 0) {
			lResult = prvConfirmIceCup(lIceEmptyBaselineGram, 61U,
				&lIceCupBaselineGram);
		}
		if (lResult == 0) {
			lResult = prvDispenseIce(usIceAmount,
				lIceCupBaselineGram);
		}
	}
	/* 步骤 5：配方需要果奶或糖浆时先移动到调味位，随后按通道顺序
	 * 出料；糖浆命令提交成功后还要等待设备状态字终态。 */
	if ((lResult == 0) && (ucNeedFlavorStation != 0U)) {
		lResult = prvRunStep(65U, COFFEE3_DEVICE_ROBOT,
			COFFEE3_ACTION_ROBOT_TO_FRUIT_SYRUP, 0U, 0U,
			COFFEE3_WORKFLOW_ROBOT_MOTION_MS);
	}
	if ((lResult == 0) &&
		(pxOrder->ausRegister[COFFEE3_REG_FRUIT_MILK_A] != 0U)) {
		lResult = prvRunFruit(1U,
			pxOrder->ausRegister[COFFEE3_REG_FRUIT_MILK_A], 0U, 66U);
	}
	if ((lResult == 0) &&
		(pxOrder->ausRegister[COFFEE3_REG_FRUIT_MILK_B] != 0U)) {
		lResult = prvRunFruit(2U,
			pxOrder->ausRegister[COFFEE3_REG_FRUIT_MILK_B], 0U, 72U);
	}
	usSyrupAmount = pxOrder->ausRegister[COFFEE3_REG_SYRUP_1];
	if ((lResult == 0) && (usSyrupAmount != 0U)) {
		lResult = prvRunStep(110U, COFFEE3_DEVICE_SYRUP_MACHINE,
			COFFEE3_ACTION_SYRUP_DISPENSE, 1U,
			(uint16_t)(usSyrupAmount *
				COFFEE3_SYRUP_TIME_PER_VOLUME_UNIT), 3000U);
		if (lResult == 0) {
			lResult = prvWaitDeviceReportedComplete(111U,
				COFFEE3_DEVICE_SYRUP_MACHINE, 1U,
				SYRUP_MACHINE_STATUS_SUCCESS,
				SYRUP_MACHINE_STATUS_FAILED);
		}
	}
	usSyrupAmount = pxOrder->ausRegister[COFFEE3_REG_SYRUP_2];
	if ((lResult == 0) && (usSyrupAmount != 0U)) {
		lResult = prvRunStep(120U, COFFEE3_DEVICE_SYRUP_MACHINE,
			COFFEE3_ACTION_SYRUP_DISPENSE, 2U,
			(uint16_t)(usSyrupAmount *
				COFFEE3_SYRUP_TIME_PER_VOLUME_UNIT), 3000U);
		if (lResult == 0) {
			lResult = prvWaitDeviceReportedComplete(121U,
				COFFEE3_DEVICE_SYRUP_MACHINE, 2U,
				SYRUP_MACHINE_STATUS_SUCCESS,
				SYRUP_MACHINE_STATUS_FAILED);
		}
	}
	usSyrupAmount = pxOrder->ausRegister[COFFEE3_REG_SYRUP_3];
	if ((lResult == 0) && (usSyrupAmount != 0U)) {
		lResult = prvRunStep(123U, COFFEE3_DEVICE_SYRUP_MACHINE,
			COFFEE3_ACTION_SYRUP_DISPENSE, 3U,
			(uint16_t)(usSyrupAmount *
				COFFEE3_SYRUP_TIME_PER_VOLUME_UNIT), 3000U);
		if (lResult == 0) {
			lResult = prvWaitDeviceReportedComplete(124U,
				COFFEE3_DEVICE_SYRUP_MACHINE, 3U,
				SYRUP_MACHINE_STATUS_SUCCESS,
				SYRUP_MACHINE_STATUS_FAILED);
		}
	}
	usSyrupAmount = pxOrder->ausRegister[COFFEE3_REG_SYRUP_4];
	if ((lResult == 0) && (usSyrupAmount != 0U)) {
		lResult = prvRunStep(126U, COFFEE3_DEVICE_SYRUP_MACHINE,
			COFFEE3_ACTION_SYRUP_DISPENSE, 4U,
			(uint16_t)(usSyrupAmount *
				COFFEE3_SYRUP_TIME_PER_VOLUME_UNIT), 3000U);
		if (lResult == 0) {
			lResult = prvWaitDeviceReportedComplete(127U,
				COFFEE3_DEVICE_SYRUP_MACHINE, 4U,
				SYRUP_MACHINE_STATUS_SUCCESS,
				SYRUP_MACHINE_STATUS_FAILED);
		}
	}
	/* 步骤 6：需要咖啡时将杯送入咖啡位、启动制作并在完成后取回。 */
	if ((lResult == 0) && (ucNeedCoffeeStation != 0U)) {
		lResult = prvRunStep(70U, COFFEE3_DEVICE_ROBOT,
			COFFEE3_ACTION_ROBOT_TO_COFFEE, 0U, 0U,
			COFFEE3_WORKFLOW_ROBOT_MOTION_MS);
		if (lResult == 0) {
			lResult = prvRunStep(75U, COFFEE3_DEVICE_ROBOT,
				COFFEE3_ACTION_ROBOT_TO_COFFEE, 1U, 0U,
				COFFEE3_WORKFLOW_ROBOT_MOTION_MS);
		}
		if ((lResult == 0) &&
			(pxOrder->ausRegister[COFFEE3_REG_COFFEE_TYPE] != 0xFFFFU)) {
			lResult = prvRunStep(80U,
				COFFEE3_DEVICE_COFFEE_MACHINE,
				COFFEE3_ACTION_COFFEE_MAKE,
				pxOrder->ausRegister[COFFEE3_REG_COFFEE_TYPE],
				0U, COFFEE3_COFFEE_ACTION_TIMEOUT_MS);
		}
	}
	if ((lResult == 0) &&
		(pxOrder->ausRegister[COFFEE3_REG_COFFEE_TYPE] != 0xFFFFU)) {
		lResult = prvRunStep(90U, COFFEE3_DEVICE_ROBOT,
			COFFEE3_ACTION_ROBOT_TAKE_COFFEE, 0U, 0U,
			COFFEE3_WORKFLOW_ROBOT_MOTION_MS);
	}
	/* 步骤 7：需要杯盖时依次完成到位、落盖、传感器确认、取盖和压盖。 */
	if ((lResult == 0) &&
		(pxOrder->ausRegister[COFFEE3_REG_LID_ENABLE] != 0U)) {
		lResult = prvRunStep(140U, COFFEE3_DEVICE_ROBOT,
			COFFEE3_ACTION_ROBOT_TO_LID, usLidLane, 0U,
			COFFEE3_WORKFLOW_ROBOT_MOTION_MS);
		if (lResult == 0) {
			lResult = prvRunStep(150U,
				COFFEE3_DEVICE_LID_MACHINE,
				(usLidLane == 1U) ?
					COFFEE3_ACTION_LID_DROP_1 :
					COFFEE3_ACTION_LID_DROP_2,
				0U, 0U, 3000U);
		}
		if (lResult == 0) {
			lResult = prvWaitBusinessCondition(155U,
				(usLidLane == 1U) ? COFFEE3_CONDITION_LID_1 :
					COFFEE3_CONDITION_LID_2);
		}
		if (lResult == 0) {
			lResult = prvRunStep(160U, COFFEE3_DEVICE_ROBOT,
				COFFEE3_ACTION_ROBOT_TAKE_LID,
				0U, 0U, COFFEE3_WORKFLOW_ROBOT_MOTION_MS);
		}
		if (lResult == 0) {
			lResult = prvRunStep(170U, COFFEE3_DEVICE_ROBOT,
				COFFEE3_ACTION_ROBOT_COVER_LID,
				0U, 0U, COFFEE3_WORKFLOW_ROBOT_MOTION_MS);
		}
	}
	if (lResult != 0) {
		return lResult;
	}
	/* 步骤 8：在线订单放入已预约暂存位并记录订单关联；离线订单先
	 * 发布内容完成，再等待出杯口释放、放杯并开门。本函数随后返回，
	 * 顾客取杯确认由 prvServicePickup 异步处理。 */
	if (ucOffline == 0U) {
		uint8_t ucStorage; /*!< 当前选择或预约的暂存位编号。 */
		ucStorage = g_xCoffee3WorkflowStatus.ucStorageReserved;
		lResult = prvRefreshDeviceQuiet(179U, COFFEE3_DEVICE_IO_INPUT);
		vCoffee3IoGetSnapshot(&xIo);
		if ((lResult != 0) || (prvIoValid(&xIo) == 0U) ||
			(xIo.xInput.aucMB1XPin[ucStorage - 1U] != 0U)) {
			return COFFEE3_WORKFLOW_ERROR_INIT_OCCUPIED;
		}
		vCoffee3ServerSelectStorage(ucStorage);
		lResult = prvRunStep(180U, COFFEE3_DEVICE_ROBOT,
			COFFEE3_ACTION_ROBOT_PUT_STORAGE, ucStorage, 0U,
			COFFEE3_WORKFLOW_ROBOT_MOTION_MS);
		if (lResult == 0) {
			lResult = prvWaitBusinessCondition(185U,
				(ucStorage == 1U) ? COFFEE3_CONDITION_STORAGE_1 :
					COFFEE3_CONDITION_STORAGE_2);
		}
		if (lResult == 0) {
			s_ausStoredOrderId[ucStorage - 1U] = g_xCoffee3WorkflowStatus.usCurrentOrderId;
		}
	} else {
		g_xCoffee3WorkflowStatus.ucContentComplete = 1U;
		vCoffee3ServerPublishWorkflow(g_xCoffee3WorkflowStatus.usCurrentOrderId,
			COFFEE3_PRODUCTION_COMPLETED, 179U, 0);
		(void)xCoffee3LogPrintfOrder(COFFEE3_LOG_LEVEL_INFO,
			COFFEE3_LOG_SOURCE_WORKFLOW, g_xCoffee3WorkflowStatus.usCurrentOrderId,
			"Content done; report production=2 before placement; no host ACK");
		/* 插入执行的在线取杯可能仍占用唯一出杯口；当前离线订单需继续等待，
		 * 直到顾客取杯确认释放出杯口。 */
		lResult = prvWaitOutputAvailableForOrder();
		if (lResult == 0) {
			prvPublishOutput(1U, 2U);
			lResult = prvRunStep(180U, COFFEE3_DEVICE_ROBOT,
				COFFEE3_ACTION_ROBOT_PUT_OUTPUT, 1U, 0U,
				COFFEE3_WORKFLOW_ROBOT_MOTION_MS);
		}
		if (lResult == 0) {
			lResult = prvWaitBusinessCondition(185U, COFFEE3_CONDITION_OUTPUT_1);
		}
		if (lResult == 0) {
			prvPublishOutput(1U, 5U);
			s_ucOutletPhase = 1U;
			s_ucPickupPending = 0U;
			s_ucEmptyTiming = 0U;
			prvStartDoor(2U);
		} else {
			prvPublishOutput(1U, 3U);
		}
	}
	return lResult;
}

/*-----------------------------------------------------------*/
/**
  * @brief  以已知空秤基线确认制冰位是否存在杯体。
  * @param[in] lEmptyBaselineGram 空秤稳定总重量，单位为克。
  * @param[in] usStepBase 稳定称重使用的首个步骤号。
  * @param[out] plCupBaselineGram 杯体在秤上的稳定总重量，单位为克。
  * @retval 0 检测到重量超过 Coffee1 判杯阈值的杯体。
  * @retval COFFEE3_WORKFLOW_ERROR_ICE_NO_CUP 连续三次检查均未发现杯体。
  * @retval 负数 称重通信或杯重合法性检查失败。
  */
static int32_t prvConfirmIceCup(int32_t lEmptyBaselineGram,
	uint16_t usStepBase, int32_t *plCupBaselineGram)
{
	int32_t lResult; /*!< 当前子步骤的执行结果或错误码。 */
	int32_t lCupWeightGram; /*!< 减去空秤基线后的杯体重量，单位为克。 */
	int32_t lTotalWeightGram; /*!< 秤当前稳定总重量，单位为克。 */
	uint8_t ucCommRetry; /*!< 称重通信失败的累计重试次数。 */
	uint8_t ucNoCupAttempt; /*!< 未检出杯体的累计检查次数。 */

	if (plCupBaselineGram == NULL) {
		return COFFEE3_WORKFLOW_ERROR_ICE_WEIGHT;
	}
	ucCommRetry = 0U;
	ucNoCupAttempt = 0U;
	lCupWeightGram = 0;
	lTotalWeightGram = 0;
	vTaskDelay(pdMS_TO_TICKS(COFFEE3_ICE_CUP_FIRST_SETTLE_MS));
	/* 依次读取稳定重量；通信失败和未检测到杯体分别使用独立重试次数。 */
	for (;;) {
		lResult = prvReadStableScale(usStepBase, &lTotalWeightGram);
		if (lResult != 0) {
			ucCommRetry++;
			if (ucCommRetry >= COFFEE3_ICE_CUP_COMM_RETRIES) {
				(void)xCoffee3LogPrintfOrder(COFFEE3_LOG_LEVEL_ERROR,
					COFFEE3_LOG_SOURCE_WORKFLOW,
					g_xCoffee3WorkflowStatus.usCurrentOrderId,
					"Cup check failed: reason=scale read try=%u/%u result=%ld",
					(unsigned int)ucCommRetry,
					(unsigned int)COFFEE3_ICE_CUP_COMM_RETRIES,
					(long)lResult);
				return lResult;
			}
			(void)xCoffee3LogPrintfOrder(COFFEE3_LOG_LEVEL_WARNING,
				COFFEE3_LOG_SOURCE_WORKFLOW,
				g_xCoffee3WorkflowStatus.usCurrentOrderId,
				"Cup check retry: reason=scale read try=%u/%u result=%ld",
				(unsigned int)ucCommRetry,
				(unsigned int)COFFEE3_ICE_CUP_COMM_RETRIES,
				(long)lResult);
			vTaskDelay(pdMS_TO_TICKS(COFFEE3_ICE_CUP_RETRY_MS));
			continue;
		}
		ucCommRetry = 0U;
		lCupWeightGram = lTotalWeightGram - lEmptyBaselineGram;
		*plCupBaselineGram = lTotalWeightGram;
		if (lCupWeightGram > COFFEE3_ICE_CUP_MAX_GRAM) {
			(void)xCoffee3LogPrintfOrder(COFFEE3_LOG_LEVEL_ERROR,
				COFFEE3_LOG_SOURCE_WORKFLOW,
				g_xCoffee3WorkflowStatus.usCurrentOrderId,
				"Cup check failed: reason=weight high cup=%ld g max=%ld g",
				(long)lCupWeightGram,
				(long)COFFEE3_ICE_CUP_MAX_GRAM);
			return COFFEE3_WORKFLOW_ERROR_ICE_WEIGHT;
		}
		if (lCupWeightGram > COFFEE3_ICE_CUP_DETECT_GRAM) {
			(void)xCoffee3LogPrintfOrder(COFFEE3_LOG_LEVEL_INFO,
				COFFEE3_LOG_SOURCE_WORKFLOW,
				g_xCoffee3WorkflowStatus.usCurrentOrderId,
				"Cup confirmed: cup=%ld g total=%ld g empty=%ld g",
				(long)lCupWeightGram, (long)lTotalWeightGram,
				(long)lEmptyBaselineGram);
			return 0;
		}
		ucNoCupAttempt++;
		if (ucNoCupAttempt >= COFFEE3_ICE_CUP_DETECT_ATTEMPTS) {
			(void)xCoffee3LogPrintfOrder(COFFEE3_LOG_LEVEL_ERROR,
				COFFEE3_LOG_SOURCE_WORKFLOW,
				g_xCoffee3WorkflowStatus.usCurrentOrderId,
				"Cup check failed: reason=no cup weight=%ld g try=%u/%u",
				(long)lCupWeightGram, (unsigned int)ucNoCupAttempt,
				(unsigned int)COFFEE3_ICE_CUP_DETECT_ATTEMPTS);
			return COFFEE3_WORKFLOW_ERROR_ICE_NO_CUP;
		}
		(void)xCoffee3LogPrintfOrder(COFFEE3_LOG_LEVEL_WARNING,
			COFFEE3_LOG_SOURCE_WORKFLOW,
			g_xCoffee3WorkflowStatus.usCurrentOrderId,
			"Cup check retry: reason=no cup weight=%ld g try=%u/%u",
			(long)lCupWeightGram, (unsigned int)ucNoCupAttempt,
			(unsigned int)COFFEE3_ICE_CUP_DETECT_ATTEMPTS);
		vTaskDelay(pdMS_TO_TICKS(COFFEE3_ICE_CUP_RETRY_MS));
	}
}

/*-----------------------------------------------------------*/
/**
  * @brief  依据有效输入快照生成两个暂存位的物理占用掩码。
  * @param[in] pxIo 当前 IO 快照。
  * @retval 位掩码 位 0、位 1 分别表示前、后暂存位检测到杯体。
  */
static uint16_t prvStorageStatusMask(const Coffee3IoState_t *pxIo)
{
	uint16_t usPhysicalMask; /*!< 传感器确认有杯的暂存位位掩码。 */

	if ((pxIo == NULL) || (prvIoValid(pxIo) == 0U)) {
		return 0U;
	}
	usPhysicalMask = (uint16_t)(
		((pxIo->xInput.aucMB1XPin[0] != 0U) ? 0x0001U : 0U) |
		((pxIo->xInput.aucMB1XPin[1] != 0U) ? 0x0002U : 0U));
	return (uint16_t)(usPhysicalMask & usCoffee3ConfigStorageMask() &
		COFFEE3_STORAGE_INSTALLED_MASK);
}

/*-----------------------------------------------------------*/
/**
  * @brief  记录暂存取杯拒绝原因并发布最新暂存状态。
  * @param[in] usStorage 请求的暂存位编号。
  * @param[in] pcReason 可直接写入日志的拒绝原因。
  * @param[in] usStatusMask 当前暂存位物理状态掩码。
  */
static void prvRejectStoragePickup(uint16_t usStorage,
	const char *pcReason, uint16_t usStatusMask)
{
	prvPublishOutputForOrder(1U, 3U, COFFEE3_LOG_ORDER_SYSTEM);
	vCoffee3ServerFinishRequest(1U);
	(void)xCoffee3LogPrintfOrder(COFFEE3_LOG_LEVEL_WARNING,
		COFFEE3_LOG_SOURCE_WORKFLOW, COFFEE3_LOG_ORDER_SYSTEM,
		"Pickup failed: device=Robot storage=%u outlet=1 reason=%s status_0x1027=0x%04X",
		(unsigned int)usStorage,
		(pcReason != NULL) ? pcReason : "unknown",
		(unsigned int)usStatusMask);
}

/*-----------------------------------------------------------*/
/**
  * @brief  以杯体稳定重量为基线执行按重量出冰和补偿。
  * @param[in] usTargetGram 目标冰重，单位为克。
  * @param[in] lCupBaselineGram 开阀前杯体与秤的稳定总重量，单位为克。
  * @retval 0 最终冰重位于允许的安全范围内。
  * @retval 负数 称重、通信、取消或重量范围检查失败。
  */
static int32_t prvDispenseIce(uint16_t usTargetGram,
	int32_t lCupBaselineGram)
{
	int32_t lResult; /*!< 当前子步骤的执行结果或错误码。 */
	int32_t lCloseResult; /*!< 出冰阀关闭命令的结果。 */
	int32_t lWeightGram; /*!< 当前秤读取的重量，单位为克。 */
	int32_t lTotalWeightGram; /*!< 秤当前稳定总重量，单位为克。 */
	int32_t lDeficitGram; /*!< 距离目标冰重的欠重，单位为克。 */
	int32_t lSafeMinGram; /*!< 冰重允许区间的下限，单位为克。 */
	int32_t lSafeMaxGram; /*!< 冰重允许区间的上限，单位为克。 */
	int32_t lDeviationGram; /*!< 实际冰重与目标的绝对偏差，单位为克。 */
	uint32_t ulPulseMs; /*!< 本轮制冰阀开启时间，单位为毫秒。 */
	uint32_t ulWaitedMs; /*!< 本轮阀门开启期间已运行的时间，单位为毫秒。 */
	uint8_t ucAttempt; /*!< 当前出冰轮次，零表示首次出冰。 */
	uint16_t usSlopeMsPerGram; /*!< 标定的每克出冰脉冲时间，单位为毫秒每克。 */

	if (usTargetGram == 0U) {
		return COFFEE3_WORKFLOW_ERROR_ICE_WEIGHT;
	}
	/* 步骤 1：先计算安全上下限。安全下限向上取整、
	 * 上限向下取整，保证整数克判定范围始终落在
	 * 目标重量的 70% 至 130% 之内。 */
	lSafeMinGram = (int32_t)(((uint32_t)usTargetGram *
		COFFEE3_ICE_SAFE_MIN_PERCENT + 99U) / 100U);
	lSafeMaxGram = (int32_t)(((uint32_t)usTargetGram *
		COFFEE3_ICE_SAFE_MAX_PERCENT) / 100U);
	(void)xCoffee3LogPrintfOrder(COFFEE3_LOG_LEVEL_INFO,
		COFFEE3_LOG_SOURCE_WORKFLOW,
		g_xCoffee3WorkflowStatus.usCurrentOrderId,
		"Ice start: target=%u g cup_base=%ld g",
		(unsigned int)usTargetGram, (long)lCupBaselineGram);
	/* 步骤 2：整个出冰及补偿周期固定使用同一份斜率标定值。 */
	usSlopeMsPerGram = usCoffee3ConfigIceSlopeMsPerGram();
	(void)xCoffee3LogWriteField(COFFEE3_LOG_LEVEL_INFO,
		COFFEE3_LOG_SOURCE_WORKFLOW, "ICE_SLOPE_SELECTED", 0,
		"ms_per_g", (int32_t)usSlopeMsPerGram);
	lWeightGram = 0;
	lTotalWeightGram = lCupBaselineGram;
	lDeficitGram = (int32_t)usTargetGram;
	/* 步骤 3：首次按目标重量计算脉冲，后续仅按剩余欠重计算补偿脉冲。 */
	for (ucAttempt = 0U;
		ucAttempt <= COFFEE3_ICE_MAX_CORRECTIONS; ucAttempt++) {
		ulPulseMs = prvCalculateIcePulseMs(
			(uint16_t)lDeficitGram, usSlopeMsPerGram,
			ucAttempt);
		(void)xCoffee3LogPrintfOrder(COFFEE3_LOG_LEVEL_INFO,
			COFFEE3_LOG_SOURCE_WORKFLOW,
			g_xCoffee3WorkflowStatus.usCurrentOrderId,
			"Ice valve open: try=%u/%u pulse=%lu ms deficit=%ld g",
			(unsigned int)(ucAttempt + 1U),
			(unsigned int)(COFFEE3_ICE_MAX_CORRECTIONS + 1U),
			(unsigned long)ulPulseMs, (long)lDeficitGram);
		lResult = prvRunStep((uint16_t)(101U + ucAttempt * 4U),
			COFFEE3_DEVICE_ICE_MACHINE,
			COFFEE3_ACTION_ICE_SET_VALVE, 1U, 0U, 3000U);
		ulWaitedMs = 0U;
		while ((lResult == 0) && (ulWaitedMs < ulPulseMs)) {
			if (g_xCoffee3WorkflowStatus.ucCancelRequested != 0U) {
				lResult = COFFEE3_WORKFLOW_ERROR_CANCELED;
				break;
			}
			vTaskDelay(pdMS_TO_TICKS(COFFEE3_ICE_PULSE_STEP_MS));
			ulWaitedMs += COFFEE3_ICE_PULSE_STEP_MS;
		}
		/* 步骤 4：等待脉冲期间可响应取消，但无论等待结果如何都必须
		 * 尝试关闭出冰阀，并优先保留原始失败。 */
		lCloseResult = prvRunStep(
			(uint16_t)(102U + ucAttempt * 4U),
			COFFEE3_DEVICE_ICE_MACHINE,
			COFFEE3_ACTION_ICE_SET_VALVE, 0U, 0U, 3000U);
		if (lResult == 0) {
			lResult = lCloseResult;
		}
		if (lResult != 0) {
			(void)xCoffee3LogPrintfOrder(COFFEE3_LOG_LEVEL_ERROR,
				COFFEE3_LOG_SOURCE_WORKFLOW,
				g_xCoffee3WorkflowStatus.usCurrentOrderId,
				"Ice dispense failed: step=valve close result=%ld",
				(long)lResult);
			return lResult;
		}
		(void)xCoffee3LogPrintfOrder(COFFEE3_LOG_LEVEL_INFO,
			COFFEE3_LOG_SOURCE_WORKFLOW,
			g_xCoffee3WorkflowStatus.usCurrentOrderId,
			"Ice valve closed: try=%u reason=pulse complete result=0",
			(unsigned int)(ucAttempt + 1U));
		(void)xCoffee3LogPrintfOrder(COFFEE3_LOG_LEVEL_INFO,
			COFFEE3_LOG_SOURCE_WORKFLOW,
			g_xCoffee3WorkflowStatus.usCurrentOrderId,
			"Ice weight wait: try=%u settle_ms=%u reason=ice settle",
			(unsigned int)(ucAttempt + 1U),
			(unsigned int)COFFEE3_ICE_SETTLE_MS);
		/* 步骤 5：关阀后等待冰块与秤稳定，再用总重量减去杯体基线。 */
		vTaskDelay(pdMS_TO_TICKS(COFFEE3_ICE_SETTLE_MS));
		lResult = prvReadStableScale(
			(uint16_t)(300U + (uint16_t)ucAttempt * 3U),
			&lTotalWeightGram);
		if (lResult != 0) {
			return lResult;
		}
		lWeightGram = lTotalWeightGram - lCupBaselineGram;
		(void)xCoffee3LogPrintfOrder(COFFEE3_LOG_LEVEL_INFO,
			COFFEE3_LOG_SOURCE_WORKFLOW,
			g_xCoffee3WorkflowStatus.usCurrentOrderId,
			"Ice weight: try=%u/%u target=%u g actual=%ld g total=%ld g last=%d",
			(unsigned int)(ucAttempt + 1U),
			(unsigned int)(COFFEE3_ICE_MAX_CORRECTIONS + 1U),
			(unsigned int)usTargetGram, (long)lWeightGram,
			(long)lTotalWeightGram,
			(int)g_xCoffee3ScaleImage.sRawValue);
		lDeficitGram = (int32_t)usTargetGram - lWeightGram;
		lDeviationGram = -lDeficitGram;
		if (lDeviationGram < 0) {
			lDeviationGram = -lDeviationGram;
		}
		if (lDeviationGram > COFFEE3_ICE_TOLERANCE_GRAM) {
			(void)xCoffee3LogPrintfOrder(COFFEE3_LOG_LEVEL_WARNING,
				COFFEE3_LOG_SOURCE_WORKFLOW,
				g_xCoffee3WorkflowStatus.usCurrentOrderId,
				"Ice weight warning: target=%u g actual=%ld g deviation=%ld g",
				(unsigned int)usTargetGram, (long)lWeightGram,
				(long)(-lDeficitGram));
		}
		/* 步骤 6：已落入杯中的冰无法回收，因此超上限时立即形成最终结果，
		 * 并仅按配置的安全范围决定是否失败。 */
		if (lWeightGram > lSafeMaxGram) {
			(void)xCoffee3LogPrintfOrder(COFFEE3_LOG_LEVEL_ERROR,
				COFFEE3_LOG_SOURCE_WORKFLOW,
				g_xCoffee3WorkflowStatus.usCurrentOrderId,
				"Ice failed: reason=above 130%% target=%u g actual=%ld g limit=%ld g",
				(unsigned int)usTargetGram, (long)lWeightGram,
				(long)lSafeMaxGram);
			return COFFEE3_WORKFLOW_ERROR_ICE_RANGE;
		}
		/* 冰块无法回收；实测冰重达到目标值时结束，避免继续补偿导致超重。 */
		if (lDeficitGram <= 0) {
			(void)xCoffee3LogPrintfOrder(
				(lDeviationGram > COFFEE3_ICE_TOLERANCE_GRAM) ?
				COFFEE3_LOG_LEVEL_WARNING : COFFEE3_LOG_LEVEL_INFO,
				COFFEE3_LOG_SOURCE_WORKFLOW,
				g_xCoffee3WorkflowStatus.usCurrentOrderId,
				"Ice done: reason=target reached target=%u g actual=%ld g",
				(unsigned int)usTargetGram, (long)lWeightGram);
			return 0;
		}
		/* 步骤 7：补偿次数用尽后，低于 70% 判为危险欠重；仍处于
		 * 安全区间则告警后接受，避免无界追加脉冲。 */
		if (ucAttempt == COFFEE3_ICE_MAX_CORRECTIONS) {
			if (lWeightGram < lSafeMinGram) {
				(void)xCoffee3LogPrintfOrder(COFFEE3_LOG_LEVEL_ERROR,
					COFFEE3_LOG_SOURCE_WORKFLOW,
					g_xCoffee3WorkflowStatus.usCurrentOrderId,
					"Ice failed: reason=below 70%% target=%u g actual=%ld g limit=%ld g",
					(unsigned int)usTargetGram, (long)lWeightGram,
					(long)lSafeMinGram);
				return COFFEE3_WORKFLOW_ERROR_ICE_RANGE;
			}
			(void)xCoffee3LogPrintfOrder(COFFEE3_LOG_LEVEL_WARNING,
				COFFEE3_LOG_SOURCE_WORKFLOW,
				g_xCoffee3WorkflowStatus.usCurrentOrderId,
				"Ice done: reason=low but safe target=%u g actual=%ld g limit=%ld g",
				(unsigned int)usTargetGram, (long)lWeightGram,
				(long)lSafeMinGram);
			return 0;
		}
		(void)xCoffee3LogPrintfOrder(COFFEE3_LOG_LEVEL_INFO,
			COFFEE3_LOG_SOURCE_WORKFLOW,
			g_xCoffee3WorkflowStatus.usCurrentOrderId,
			"Ice correction: target=%u g actual=%ld g deficit=%ld g",
			(unsigned int)usTargetGram, (long)lWeightGram,
			(long)lDeficitGram);
	}
	(void)xCoffee3LogPrintfOrder(COFFEE3_LOG_LEVEL_ERROR,
		COFFEE3_LOG_SOURCE_WORKFLOW,
		g_xCoffee3WorkflowStatus.usCurrentOrderId,
		"Ice failed: reason=correction limit target=%u g actual=%ld g corrections=%u",
		(unsigned int)usTargetGram, (long)lWeightGram,
		(unsigned int)COFFEE3_ICE_MAX_CORRECTIONS);
	return COFFEE3_WORKFLOW_ERROR_ICE_WEIGHT;
}

/*-----------------------------------------------------------*/
/**
  * @brief  按 Coffee1 初始时间限制计算本轮出冰阀脉冲。
  * @param[in] usTargetGram 目标冰重或剩余欠重，单位为克。
  * @param[in] usSlopeMsPerGram 标定斜率，单位为毫秒每克。
  * @param[in] ucAttempt 0 表示首次出冰，非 0 表示补偿轮次。
  * @retval 经过配置上下限约束的阀门开启时间，单位为毫秒。
  */
static uint32_t prvCalculateIcePulseMs(uint16_t usTargetGram,
	uint16_t usSlopeMsPerGram, uint8_t ucAttempt)
{
	int32_t lPulseMs; /*!< 按目标重量计算的原始脉冲时长，单位为毫秒。 */
	int32_t lMaximumMs; /*!< 当前轮次允许的最大脉冲时长，单位为毫秒。 */

	lPulseMs = (int32_t)usTargetGram * (int32_t)usSlopeMsPerGram;
	lPulseMs += COFFEE3_ICE_CORRECTION_OFFSET_MS;
	lMaximumMs = (int32_t)COFFEE3_ICE_MAX_PULSE_MS;
	if (ucAttempt == 0U) {
		lMaximumMs = (usTargetGram < COFFEE3_ICE_INITIAL_SPLIT_GRAM) ?
			(int32_t)COFFEE3_ICE_INITIAL_SMALL_MS :
			(int32_t)COFFEE3_ICE_INITIAL_LARGE_MS;
	}
	if (lPulseMs < (int32_t)COFFEE3_ICE_MIN_PULSE_MS) {
		lPulseMs = (int32_t)COFFEE3_ICE_MIN_PULSE_MS;
	}
	lPulseMs = ((lPulseMs + (int32_t)COFFEE3_ICE_PULSE_STEP_MS - 1L) /
		(int32_t)COFFEE3_ICE_PULSE_STEP_MS) *
		(int32_t)COFFEE3_ICE_PULSE_STEP_MS;
	if (lPulseMs > lMaximumMs) {
		lPulseMs = lMaximumMs;
	}
	return (uint32_t)lPulseMs;
}

/*-----------------------------------------------------------*/
/**
  * @brief  连续读取三次秤值并返回中位数。
  * @param[in] usStepBase 三次采样使用的首个步骤号。
  * @param[out] plWeightGram 稳定总重量的中位数，单位为克。
  * @retval 0 已取得三次有效采样。
  * @retval 负数 称重通信或数值换算失败。
  */
static int32_t prvReadStableScale(uint16_t usStepBase,
	int32_t *plWeightGram)
{
	int32_t alSample[3]; /*!< 连续三次取得的秤重量样本，单位为克。 */
	int32_t lSwap; /*!< 三样本排序时暂存的一个重量值。 */
	int32_t lResult; /*!< 当前子步骤的执行结果或错误码。 */
	uint8_t ucIndex; /*!< 当前遍历项的下标。 */

	if (plWeightGram == NULL) {
		return COFFEE3_WORKFLOW_ERROR_ICE_WEIGHT;
	}
	for (ucIndex = 0U; ucIndex < 3U; ucIndex++) {
		lResult = prvRunStep((uint16_t)(usStepBase + ucIndex),
			COFFEE3_DEVICE_SCALE, COFFEE3_ACTION_REFRESH,
			0U, 0U, 3000U);
		if (lResult != 0) {
			return lResult;
		}
		alSample[ucIndex] = g_xCoffee3ScaleImage.lWeightGram;
		vTaskDelay(pdMS_TO_TICKS(100U));
	}
	if (alSample[0] > alSample[1]) {
		lSwap = alSample[0];
		alSample[0] = alSample[1];
		alSample[1] = lSwap;
	}
	if (alSample[1] > alSample[2]) {
		lSwap = alSample[1];
		alSample[1] = alSample[2];
		alSample[2] = lSwap;
	}
	if (alSample[0] > alSample[1]) {
		lSwap = alSample[0];
		alSample[0] = alSample[1];
		alSample[1] = lSwap;
	}
	*plWeightGram = alSample[1];
	return 0;
}

/*-----------------------------------------------------------*/
/**
  * @brief  写入一个产品 IO 输出并等待输出模块报告终态。
  * @param[in] usStep 工作流步骤号。
  * @param[in] ucPoint 输出点编号。
  * @param[in] ucValue 0 关闭，非 0 开启。
  * @retval 0 输出模块确认写入完成。
  * @retval 负数 提交、设备终态或取消失败。
  */
static int32_t prvRunIoOutput(uint16_t usStep, uint8_t ucPoint,
	uint8_t ucValue)
{
	Coffee3Command_t xCommand; /*!< 本次向设备提交并追踪终态的命令。 */
	EventBits_t xEvents; /*!< 设备事件组返回的完成或失败标志。 */
	TickType_t xStartTick; /*!< 本次动作或等待开始的系统节拍。 */
	uint8_t ucTerminalValid; /*!< 目标设备命令终态是否有效。 */
	int32_t lTerminalResult; /*!< 目标设备命令返回的终态结果。 */

	memset(&xCommand, 0, sizeof(xCommand));
	xCommand.ulOrderId =
		(g_xCoffee3WorkflowStatus.xState == COFFEE3_WORKFLOW_RUNNING) ?
		g_xCoffee3WorkflowStatus.usCurrentOrderId :
		COFFEE3_LOG_ORDER_DEBUG;
	xCommand.ulOrderEpoch = g_xCoffee3WorkflowStatus.ulOrderEpoch;
	xCommand.ulTimeoutMs = COFFEE3_RTU_IO_TIMEOUT_MS;
	xCommand.usStepId = usStep;
	xCommand.usAction = (uint16_t)COFFEE3_ACTION_IO_WRITE;
	xCommand.ausParameter[0] = ucPoint;
	xCommand.ausParameter[1] = (ucValue != 0U) ? 1U : 0U;
	xCommand.ucDeviceId = (uint8_t)COFFEE3_DEVICE_IO_OUTPUT;
	xCommand.ucSource =
		(g_xCoffee3WorkflowStatus.xState == COFFEE3_WORKFLOW_RUNNING) ?
		(uint8_t)COFFEE3_COMMAND_SOURCE_WORKFLOW :
		(uint8_t)COFFEE3_COMMAND_SOURCE_MAINTENANCE;
	xCommand.ucRetryLimit = 1U;
	if (xCoffee3CommandSubmit(&xCommand, pdMS_TO_TICKS(100U)) != pdPASS) {
		return COFFEE3_WORKFLOW_ERROR_QUEUE;
	}
	xStartTick = xTaskGetTickCount();
	for (;;) {
		xEvents = xCoffee3DeviceWaitCommand(COFFEE3_DEVICE_IO_OUTPUT,
			xCommand.ulOrderEpoch, xCommand.ulCommandId,
			pdMS_TO_TICKS(100U));
		if ((xEvents & COFFEE3_DEVICE_EVENT_COMMAND_DONE) != 0U) {
			return 0;
		}
		if ((xEvents & COFFEE3_DEVICE_EVENT_TERMINAL) != 0U) {
			lTerminalResult = lCoffee3DeviceGetTerminalResult(
				COFFEE3_DEVICE_IO_OUTPUT, xCommand.ulOrderEpoch,
				xCommand.ulCommandId, &ucTerminalValid);
			return (ucTerminalValid != 0U) ? lTerminalResult :
				COFFEE3_WORKFLOW_ERROR_IO;
		}
		if ((xTaskGetTickCount() - xStartTick) >=
			pdMS_TO_TICKS(COFFEE3_WORKFLOW_IO_ACTION_TIMEOUT_MS)) {
			return COFFEE3_WORKFLOW_ERROR_TIMEOUT;
		}
		prvServiceHotWater();
	}
}

/*-----------------------------------------------------------*/
/**
  * @brief  尽力关闭果奶、冷水和热水相关产品输出。
  * @retval 0 所有输出关闭命令均成功。
  * @retval 负数 至少一个输出关闭失败。
  */
static int32_t prvSetProductOutputsOff(void)
{
	static const uint8_t aucPoints[] = {
		COFFEE3_EXTERNAL_DO_WATER_HEATER_RELAY,
		COFFEE3_EXTERNAL_DO_MILK_VALVE,
		COFFEE3_EXTERNAL_DO_FRUIT_MILK_A_PUMP,
		COFFEE3_EXTERNAL_DO_FRUIT_MILK_B_PUMP,
		COFFEE3_EXTERNAL_DO_FRUIT_MILK_A_VALVE,
		COFFEE3_EXTERNAL_DO_FRUIT_MILK_B_VALVE
	}; /*!< 需要关闭的产品输出点编号表。 */
	int32_t lResult; /*!< 当前子步骤的执行结果或错误码。 */
	uint8_t ucIndex; /*!< 当前遍历项的下标。 */

	(void)ucCoffee3IoSetLocalOutput(
		COFFEE3_LOCAL_DO_HOT_WATER_SUPPLY_VALVE, 0U);
	lResult = 0;
	for (ucIndex = 0U;
		ucIndex < (sizeof(aucPoints) / sizeof(aucPoints[0])); ucIndex++) {
		if (prvRunIoOutput((uint16_t)(0xFE00U + ucIndex),
			aucPoints[ucIndex], 0U) != 0) {
			lResult = COFFEE3_WORKFLOW_ERROR_SAFE_STOP;
		}
	}
	return lResult;
}

/*-----------------------------------------------------------*/
/**
  * @brief  依次处理三个残杯来源，使用已验证空储位承接机械臂动作。
  * @retval 0 三段按需取杯、放入储位及目标储位采样均完成；
  *           不保证所有物理位置已无杯。
  * @retval 负数 储位选择、设备刷新或机械臂动作失败。
  */
static int32_t prvRunInitialization(void)
{
	int32_t lResult; /*!< 当前子步骤的执行结果或错误码。 */
	uint8_t ucSource; /*!< 当前处理的残杯来源序号。 */
	static const Coffee3Action_e axSources[3] = {
		(Coffee3Action_e)0, COFFEE3_ACTION_ROBOT_TAKE_COFFEE,
		COFFEE3_ACTION_ROBOT_TAKE_LID
	}; /*!< 三个来源对应的机器人取杯动作；首项跳过取杯。 */

	/* 仅在机械臂和输入模块的新鲜反馈均可用后调用；首次运动前缺少
	 * 前置条件表示继续等待，不在本函数中判为故障。 */
	for (ucSource = 0U; ucSource < 3U; ucSource++) {
		lResult = prvProbeResidualCup((uint16_t)(0xFD20U + 4U * ucSource),
			axSources[ucSource], (uint8_t)(ucSource + 1U));
		if (lResult != 0) {
			return lResult;
		}
	}
	return 0;
}

/*-----------------------------------------------------------*/
/**
  * @brief  选择空储位，按需执行来源取杯，再执行放入储位动作。
  * @param[in] usStepBase 本段动作使用的首个步骤号。
  * @param[in] xPickupAction 机械臂取杯动作；零表示跳过取杯。
  * @param[in] ucOccupiedPoint 仅用于日志标识来源，不作为源传感器索引。
  * @retval 0 动作及目标储位两次一致采样完成；采样仍可显示有杯。
  * @retval 负数 无可用储位、目标储位 IO 刷新或机械臂动作失败。
  */
static int32_t prvProbeResidualCup(uint16_t usStepBase,
	Coffee3Action_e xPickupAction, uint8_t ucOccupiedPoint)
{
	Coffee3IoState_t xIo; /*!< 当前读取的输入与输出状态快照。 */
	int32_t lResult; /*!< 当前子步骤的执行结果或错误码。 */
	int32_t lStorage; /*!< 本次选中的空储位编号或选择错误码。 */
	uint8_t ucFirstCup; /*!< 目标储位第一次采样的有杯状态。 */
	uint8_t ucCupPresent; /*!< 目标储位第二次采样的有杯状态。 */
	uint8_t ucMismatchLogged; /*!< 目标储位两次采样不一致的日志是否已记录。 */
	const char *pcIoName; /*!< 当前物理条件对应的输入点日志名称。 */

	lStorage = prvSelectStorage();
	if (lStorage < 1) {
		(void)xCoffee3LogPrintfOrder(COFFEE3_LOG_LEVEL_ERROR,
			COFFEE3_LOG_SOURCE_WORKFLOW, COFFEE3_LOG_ORDER_SYSTEM,
			"Residual source=%u has no verified empty storage; init failed (%ld)",
			(unsigned int)ucOccupiedPoint, (long)lStorage);
		return lStorage;
	}
	vCoffee3ServerSelectStorage((uint16_t)lStorage);
	lResult = 0;
	if ((uint16_t)xPickupAction != 0U) {
		lResult = prvRunStep(usStepBase, COFFEE3_DEVICE_ROBOT,
			xPickupAction, 0U, 0U, COFFEE3_WORKFLOW_ROBOT_MOTION_MS);
	}
	if (lResult == 0) {
		lResult = prvRunStep((uint16_t)(usStepBase + 1U), COFFEE3_DEVICE_ROBOT,
			COFFEE3_ACTION_ROBOT_PUT_STORAGE, (uint16_t)lStorage, 0U,
			COFFEE3_WORKFLOW_ROBOT_MOTION_MS);
	}
	if (lResult != 0) {
		return lResult;
	}
	prvDelayWithServices(COFFEE3_RESIDUAL_SETTLE_MS);
	pcIoName = (lStorage == 1) ? COFFEE3_IO_NAME_MB1_DI_1 :
		COFFEE3_IO_NAME_MB1_DI_2;
	ucMismatchLogged = 0U;
	for (;;) {
		lResult = prvRefreshDeviceQuiet((uint16_t)(usStepBase + 2U),
			COFFEE3_DEVICE_IO_INPUT);
		vCoffee3IoGetSnapshot(&xIo);
		if ((lResult != 0) || (prvIoValid(&xIo) == 0U)) {
			return (lResult != 0) ? lResult : COFFEE3_WORKFLOW_ERROR_IO;
		}
		ucFirstCup = xIo.xInput.aucMB1XPin[lStorage - 1];
		prvDelayWithServices(COFFEE3_RESIDUAL_SAMPLE_GAP_MS);
		lResult = prvRefreshDeviceQuiet((uint16_t)(usStepBase + 2U),
			COFFEE3_DEVICE_IO_INPUT);
		vCoffee3IoGetSnapshot(&xIo);
		if ((lResult != 0) || (prvIoValid(&xIo) == 0U)) {
			return (lResult != 0) ? lResult : COFFEE3_WORKFLOW_ERROR_IO;
		}
		ucCupPresent = xIo.xInput.aucMB1XPin[lStorage - 1];
		if (ucFirstCup == ucCupPresent) {
			break;
		}
		if (ucMismatchLogged == 0U) {
			(void)xCoffee3LogPrintfOrder(COFFEE3_LOG_LEVEL_WARNING,
				COFFEE3_LOG_SOURCE_WORKFLOW, COFFEE3_LOG_ORDER_SYSTEM,
				"Residual %s unstable: %u/%u; retry",
				pcIoName, (unsigned int)ucFirstCup,
				(unsigned int)ucCupPresent);
			ucMismatchLogged = 1U;
		}
	}
	(void)xCoffee3LogPrintfOrder(
		(ucCupPresent != 0U) ?
			COFFEE3_LOG_LEVEL_WARNING : COFFEE3_LOG_LEVEL_INFO,
		COFFEE3_LOG_SOURCE_WORKFLOW, COFFEE3_LOG_ORDER_SYSTEM,
		"Residual source=%u storage=%ld io=%s cup=%u",
		(unsigned int)ucOccupiedPoint, (long)lStorage,
		pcIoName, (unsigned int)ucCupPresent);
	return 0;
}

/*-----------------------------------------------------------*/
/**
  * @brief  按 ucClean 选择定时出料或清洗脉冲，结束时关闭输出。
  * @param[in] ucChannel 果奶通道编号。
  * @param[in] usAmountMl 目标容量，单位为毫升。
  * @param[in] ucClean 非 0 时执行清洗。
  * @param[in] usStepBase 本段动作使用的首个步骤号。
  * @retval 0 选定的出料或清洗流程完成。
  * @retval 负数 输出写入或取消失败。
  */
static int32_t prvRunFruit(uint8_t ucChannel, uint16_t usAmountMl,
	uint8_t ucClean, uint16_t usStepBase)
{
	Coffee3IoState_t xIo; /*!< 当前读取的输入与输出状态快照。 */
	uint8_t ucInputPoint; /*!< 当前果奶通道的低液位输入点编号。 */
	uint8_t ucValvePoint; /*!< 当前果奶通道的阀门输出点编号。 */
	uint8_t ucPumpPoint; /*!< 当前果奶通道的泵输出点编号。 */
	uint8_t ucValveRunValue; /*!< 本次果奶出料或清洗时写入阀门的值。 */
	uint32_t ulRunMs; /*!< 本次果奶出料或清洗持续时间，单位为毫秒。 */
	uint16_t usLogOrder; /*!< 当前日志使用的订单号或调试标识。 */
	int32_t lResult; /*!< 当前子步骤的执行结果或错误码。 */

	if ((ucChannel < 1U) || (ucChannel > 2U) ||
		((ucClean == 0U) && (usAmountMl == 0U))) {
		return COFFEE3_WORKFLOW_ERROR_UNSUPPORTED;
	}
	ucInputPoint = (ucChannel == 1U) ?
		COFFEE3_EXTERNAL_DI_FRUIT_MILK_A_LOW :
		COFFEE3_EXTERNAL_DI_FRUIT_MILK_B_LOW;
	ucValvePoint = (ucChannel == 1U) ?
		COFFEE3_EXTERNAL_DO_FRUIT_MILK_A_VALVE :
		COFFEE3_EXTERNAL_DO_FRUIT_MILK_B_VALVE;
	ucPumpPoint = (ucChannel == 1U) ?
		COFFEE3_EXTERNAL_DO_FRUIT_MILK_A_PUMP :
		COFFEE3_EXTERNAL_DO_FRUIT_MILK_B_PUMP;
	ucValveRunValue = ((ucClean != 0U) && (ucChannel == 1U)) ? 1U : 0U;
	usLogOrder = (g_xCoffee3WorkflowStatus.xState ==
		COFFEE3_WORKFLOW_RUNNING) ?
		g_xCoffee3WorkflowStatus.usCurrentOrderId : COFFEE3_LOG_ORDER_DEBUG;
	g_xCoffee3WorkflowStatus.aucFruitState[ucChannel - 1U] =
		COFFEE3_MAINTENANCE_RUNNING;
	lResult = prvRunStep(usStepBase,
		COFFEE3_DEVICE_IO_INPUT, COFFEE3_ACTION_REFRESH,
		0U, 0U, 3000U);
	if (lResult == 0) {
		vCoffee3IoGetSnapshot(&xIo);
		if (xIo.xInput.aucMB1XPin[ucInputPoint] != 0U) {
			lResult = COFFEE3_WORKFLOW_ERROR_DEVICE;
			(void)xCoffee3LogWriteFieldOrder(COFFEE3_LOG_LEVEL_WARNING,
				COFFEE3_LOG_SOURCE_WORKFLOW, usLogOrder,
				"FRUIT_LOW_LEVEL", lResult, "channel",
				(int32_t)ucChannel);
		}
	}
	if (lResult == 0) {
		lResult = prvRunIoOutput((uint16_t)(usStepBase + 1U),
			ucValvePoint, ucValveRunValue);
	}
	if (lResult == 0) {
		lResult = prvRunIoOutput((uint16_t)(usStepBase + 2U),
			ucPumpPoint, 1U);
	}
	if (lResult == 0) {
		ulRunMs = (ucClean != 0U) ? COFFEE3_FRUIT_MILK_CLEAN_MS :
			((uint32_t)usAmountMl *
			usCoffee3ConfigFruitCoefficient(ucChannel));
		prvDelayWithServices(ulRunMs);
		if (g_xCoffee3WorkflowStatus.ucCancelRequested != 0U) {
			lResult = COFFEE3_WORKFLOW_ERROR_CANCELED;
		}
	}
	if (prvRunIoOutput((uint16_t)(usStepBase + 3U),
		ucPumpPoint, 0U) != 0) {
		lResult = COFFEE3_WORKFLOW_ERROR_SAFE_STOP;
	}
	if (prvRunIoOutput((uint16_t)(usStepBase + 4U),
		ucValvePoint, 0U) != 0) {
		lResult = COFFEE3_WORKFLOW_ERROR_SAFE_STOP;
	}
	g_xCoffee3WorkflowStatus.aucFruitState[ucChannel - 1U] =
		(lResult == 0) ? COFFEE3_MAINTENANCE_COMPLETED :
		COFFEE3_MAINTENANCE_FAILED;
	return lResult;
}

/*-----------------------------------------------------------*/
/**
  * @brief  按维护请求分派咖啡清洗、糖浆清洗、果奶出料或清洗流程。
  * @param[in] pxRequest 已由工作流接管的维护请求。
  * @retval 0 对应维护动作完成。
  * @retval 负数 设备、输出或请求类型错误。
  */
static int32_t prvRunMaintenance(const Coffee3MaintenanceRequest_t *pxRequest)
{
	int32_t lResult; /*!< 当前子步骤的执行结果或错误码。 */

	if (pxRequest == NULL) {
		return COFFEE3_WORKFLOW_ERROR_UNSUPPORTED;
	}
	switch (pxRequest->xType) {
	case COFFEE3_MAINTENANCE_SYRUP_CLEAN:
		lResult = prvRunStep(0xFB10U, COFFEE3_DEVICE_SYRUP_MACHINE,
			COFFEE3_ACTION_SYRUP_CLEAN, 0U, 0U, 3000U);
		if (lResult == 0) {
			lResult = prvWaitDeviceReportedComplete(0xFB11U,
				COFFEE3_DEVICE_SYRUP_MACHINE, 5U,
				SYRUP_MACHINE_STATUS_SUCCESS,
				SYRUP_MACHINE_STATUS_FAILED);
		}
		return lResult;
	case COFFEE3_MAINTENANCE_COFFEE_CLEAN:
		g_xCoffee3WorkflowStatus.ucCoffeeCleanState =
			COFFEE3_MAINTENANCE_RUNNING;
		lResult = prvRunStep(0xFB20U,
			COFFEE3_DEVICE_COFFEE_MACHINE,
			COFFEE3_ACTION_COFFEE_CLEAN,
			pxRequest->usParameter0, 0U, 3000U);
		if (lResult == 0) {
			lResult = prvWaitM50Clean();
		}
		g_xCoffee3WorkflowStatus.ucCoffeeCleanState =
			(lResult == 0) ? COFFEE3_MAINTENANCE_COMPLETED :
			COFFEE3_MAINTENANCE_FAILED;
		return lResult;
	case COFFEE3_MAINTENANCE_FRUIT_DISPENSE:
		return prvRunFruit((uint8_t)pxRequest->usParameter0,
			pxRequest->usParameter1, 0U,
			(uint16_t)(0xFC10U + (pxRequest->usParameter0 * 8U)));
	case COFFEE3_MAINTENANCE_FRUIT_CLEAN:
		return prvRunFruit((uint8_t)pxRequest->usParameter0, 0U, 1U,
			(uint16_t)(0xFC30U + (pxRequest->usParameter0 * 8U)));
	default:
		return COFFEE3_WORKFLOW_ERROR_UNSUPPORTED;
	}
}

/*-----------------------------------------------------------*/
/**
  * @brief  校验订单指针、目标冰重及订单编号。
  * @param[in] pxOrder 待校验订单。
  * @param[out] plError 校验失败时写入工作流错误码。
  * @retval 1 订单满足基础格式要求。
  * @retval 0 订单无效。
  */
static uint8_t prvOrderValid(const Coffee3Order_t *pxOrder,
	int32_t *plError)
{
	uint16_t usCoffeeType; /*!< 订单寄存器中的咖啡种类编号。 */
	uint8_t ucOffline; /*!< 订单是否走直接出餐的离线路径。 */
	if (plError != NULL) {
		*plError = COFFEE3_WORKFLOW_ERROR_UNSUPPORTED;
	}
	if (pxOrder == NULL) {
		return 0U;
	}
	usCoffeeType = pxOrder->ausRegister[COFFEE3_REG_COFFEE_TYPE];
	if ((usCoffeeType != 0xFFFFU) && (usCoffeeType > COFFEE3_COFFEE_RECIPE_MAX)) {
		return 0U;
	}
	ucOffline = ((pxOrder->ausRegister[COFFEE3_REG_ORDER_NUMBER] & 0xF000U) == 0xD000U);
	if ((pxOrder->ausRegister[COFFEE3_REG_ICE_AMOUNT] > 6553U) ||
		((ucOffline != 0U) &&
		(pxOrder->ausRegister[COFFEE3_REG_OFFLINE_OUTPUT] != 1U))) {
		return 0U;
	}
	if (plError != NULL) {
		*plError = 0;
	}
	return 1U;
}

/*-----------------------------------------------------------*/
/**
  * @brief  静默刷新设备镜像，不改变当前订单步骤发布状态。
  * @param[in] usStep 诊断步骤号。
  * @param[in] xDeviceId 待刷新的设备。
  * @retval 0 刷新成功。
  * @retval 负数 命令提交或设备刷新失败。
  */
static int32_t prvRefreshDeviceQuiet(uint16_t usStep,
	Coffee3DeviceId_e xDeviceId)
{
	Coffee3Command_t xCommand; /*!< 本次向设备提交并追踪终态的命令。 */
	EventBits_t xEvents; /*!< 设备事件组返回的完成或失败标志。 */
	TickType_t xStartTick; /*!< 本次动作或等待开始的系统节拍。 */
	uint8_t ucTerminalValid; /*!< 目标设备命令终态是否有效。 */
	uint8_t ucOrderStep; /*!< 是否采用低编号业务步骤的处理规则。 */
	int32_t lTerminalResult; /*!< 目标设备命令返回的终态结果。 */

	if ((xDeviceId <= COFFEE3_DEVICE_NONE) ||
		(xDeviceId >= COFFEE3_DEVICE_COUNT)) {
		return COFFEE3_WORKFLOW_ERROR_UNSUPPORTED;
	}
	ucOrderStep = (usStep < 0xF000U) ? 1U : 0U;
	memset(&xCommand, 0, sizeof(xCommand));
	xCommand.ulOrderId = (ucOrderStep != 0U) ?
		g_xCoffee3WorkflowStatus.usCurrentOrderId : COFFEE3_LOG_ORDER_DEBUG;
	xCommand.ulOrderEpoch = ((ucOrderStep != 0U) ||
		(s_ucMaintenanceActive != 0U)) ?
		g_xCoffee3WorkflowStatus.ulOrderEpoch : 0U;
	xCommand.usStepId = usStep;
	xCommand.usAction = (uint16_t)COFFEE3_ACTION_REFRESH;
	xCommand.ulTimeoutMs =
		(xDeviceId == COFFEE3_DEVICE_COFFEE_MACHINE) ?
			COFFEE3_WORKFLOW_DEVICE_IO_TIMEOUT_MS :
			COFFEE3_RTU_IO_TIMEOUT_MS;
	xCommand.ucDeviceId = (uint8_t)xDeviceId;
	xCommand.ucSource = ((ucOrderStep != 0U) ||
		(s_ucMaintenanceActive != 0U)) ?
		(uint8_t)COFFEE3_COMMAND_SOURCE_WORKFLOW :
		(uint8_t)COFFEE3_COMMAND_SOURCE_MAINTENANCE;
	xCommand.ucRetryLimit = 1U;
	if (xCoffee3CommandSubmit(&xCommand, pdMS_TO_TICKS(100U)) != pdPASS) {
		return COFFEE3_WORKFLOW_ERROR_QUEUE;
	}
	xStartTick = xTaskGetTickCount();
	for (;;) {
		xEvents = xCoffee3DeviceWaitCommand(xDeviceId,
			xCommand.ulOrderEpoch, xCommand.ulCommandId,
			pdMS_TO_TICKS(100U));
		if ((xEvents & COFFEE3_DEVICE_EVENT_COMMAND_DONE) != 0U) {
			return 0;
		}
		if ((xEvents & COFFEE3_DEVICE_EVENT_TERMINAL) != 0U) {
			lTerminalResult = lCoffee3DeviceGetTerminalResult(xDeviceId,
				xCommand.ulOrderEpoch, xCommand.ulCommandId,
				&ucTerminalValid);
			return (ucTerminalValid != 0U) ? lTerminalResult :
				COFFEE3_WORKFLOW_ERROR_DEVICE;
		}
		if (((ucOrderStep != 0U) || (s_ucMaintenanceActive != 0U)) &&
			(g_xCoffee3WorkflowStatus.ucCancelRequested != 0U)) {
			return COFFEE3_WORKFLOW_ERROR_CANCELED;
		}
		if ((xTaskGetTickCount() - xStartTick) >= pdMS_TO_TICKS(5000U)) {
			return COFFEE3_WORKFLOW_ERROR_TIMEOUT;
		}
		prvServiceHotWater();
	}
}

/*-----------------------------------------------------------*/
/**
  * @brief  从最新 IO 镜像读取一个工作流物理条件。
  * @param[in] ucCondition 杯、盖、出杯口或暂存位条件编号。
  * @retval 1 条件对应输入当前有效。
  * @retval 0 IO 无效、条件未满足或编号不受支持。
  */
static uint8_t prvBusinessConditionActive(uint8_t ucCondition)
{
	Coffee3IoState_t xIo; /*!< 当前读取的输入与输出状态快照。 */

	switch (ucCondition) {
	case COFFEE3_CONDITION_CUP_1:
		return (g_xCoffee3CupLidImage.aucCupCoils[4U] != 0U) ? 1U : 0U;
	case COFFEE3_CONDITION_CUP_2:
		return (g_xCoffee3CupLidImage.aucCupCoils[9U] != 0U) ? 1U : 0U;
	case COFFEE3_CONDITION_LID_1:
		return (g_xCoffee3CupLidImage.aucLidCoils[4U] != 0U) ? 1U : 0U;
	case COFFEE3_CONDITION_LID_2:
		return (g_xCoffee3CupLidImage.aucLidCoils[9U] != 0U) ? 1U : 0U;
	case COFFEE3_CONDITION_OUTPUT_1:
	case COFFEE3_CONDITION_STORAGE_1:
	case COFFEE3_CONDITION_STORAGE_2:
		vCoffee3IoGetSnapshot(&xIo);
		if (prvIoValid(&xIo) == 0U) {
			return 0U;
		}
		return xIo.xInput.aucMB1XPin[
			(ucCondition == COFFEE3_CONDITION_OUTPUT_1) ? 2U :
			((ucCondition == COFFEE3_CONDITION_STORAGE_1) ? 0U : 1U)];
	default:
		return 0U;
	}
}

/*-----------------------------------------------------------*/
/**
  * @brief  周期刷新输入模块，等待指定物理条件成立。
  * @param[in] usStep 对外发布的工作流步骤号。
  * @param[in] ucCondition 待等待的物理条件编号。
  * @retval 0 条件已成立。
  * @retval COFFEE3_WORKFLOW_ERROR_TIMEOUT 杯或盖条件超出等待预算。
  * @retval 负数 取消、设备刷新失败或条件无效。
  * @note   出口及储位的物理确认逾期只告警一次，仍继续等待。
  */
static int32_t prvWaitBusinessCondition(uint16_t usStep,
	uint8_t ucCondition)
{
	Coffee3DeviceId_e xDeviceId; /*!< 待刷新或确认业务条件的逻辑设备。 */
	int32_t lResult; /*!< 当前子步骤的执行结果或错误码。 */
	uint8_t ucWaitLogged; /*!< 等待过程提示日志是否已记录。 */
	uint8_t ucOverdueLogged; /*!< 逾期日志是否已记录，避免重复告警。 */
	uint8_t ucPhysicalPlacement; /*!< 当前条件是否属于出餐口或储位物理确认。 */
	uint8_t ucDelayIndex; /*!< 分段等待中的当前延时序号。 */
	TickType_t xStart; /*!< 本段等待开始的系统节拍。 */
	const char *pcIoName; /*!< 当前物理条件对应的输入点日志名称。 */

	ucWaitLogged = 0U;
	ucOverdueLogged = 0U;
	ucPhysicalPlacement = 0U;
	pcIoName = NULL;
	xStart = xTaskGetTickCount();
	switch (ucCondition) {
	case COFFEE3_CONDITION_CUP_1:
	case COFFEE3_CONDITION_CUP_2:
		xDeviceId = COFFEE3_DEVICE_CUP_MACHINE;
		break;
	case COFFEE3_CONDITION_LID_1:
	case COFFEE3_CONDITION_LID_2:
		xDeviceId = COFFEE3_DEVICE_LID_MACHINE;
		break;
	case COFFEE3_CONDITION_OUTPUT_1:
		xDeviceId = COFFEE3_DEVICE_IO_INPUT;
		ucPhysicalPlacement = 1U;
		pcIoName = "X3_OUTLET_CUP";
		break;
	case COFFEE3_CONDITION_STORAGE_1:
		xDeviceId = COFFEE3_DEVICE_IO_INPUT;
		ucPhysicalPlacement = 1U;
		pcIoName = "X1_FINISHED_FRONT_CUP";
		break;
	case COFFEE3_CONDITION_STORAGE_2:
		xDeviceId = COFFEE3_DEVICE_IO_INPUT;
		ucPhysicalPlacement = 1U;
		pcIoName = "X2_FINISHED_REAR_CUP";
		break;
	default:
		return COFFEE3_WORKFLOW_ERROR_UNSUPPORTED;
	}
	prvPublish(COFFEE3_WORKFLOW_RUNNING, usStep, 0);
	for (;;) {
		if (g_xCoffee3WorkflowStatus.ucCancelRequested != 0U) {
			return COFFEE3_WORKFLOW_ERROR_CANCELED;
		}
		if ((xTaskGetTickCount() - xStart) >=
			pdMS_TO_TICKS(COFFEE3_WORKFLOW_DEFAULT_TIMEOUT_MS)) {
			if (ucPhysicalPlacement == 0U) {
				return COFFEE3_WORKFLOW_ERROR_TIMEOUT;
			}
			if (ucOverdueLogged == 0U) {
				ucOverdueLogged = 1U;
				(void)xCoffee3LogPrintfOrder(COFFEE3_LOG_LEVEL_WARNING,
					COFFEE3_LOG_SOURCE_WORKFLOW,
					g_xCoffee3WorkflowStatus.usCurrentOrderId,
					"Physical confirmation overdue: device=IoInput io=%s; waiting",
					pcIoName);
			}
		}
		lResult = prvRefreshDeviceQuiet(usStep, xDeviceId);
		if (lResult != 0) {
			(void)xCoffee3LogWriteFieldOrder(COFFEE3_LOG_LEVEL_ERROR,
				COFFEE3_LOG_SOURCE_WORKFLOW,
				g_xCoffee3WorkflowStatus.usCurrentOrderId,
				"BUSINESS_CONDITION_REFRESH_FAILED", lResult,
				"condition", (int32_t)ucCondition);
			return lResult;
		}
		if (prvBusinessConditionActive(ucCondition) != 0U) {
			g_xCoffee3WorkflowStatus.ucPhysicalVerified = 1U;
			(void)xCoffee3LogWriteFieldOrder(COFFEE3_LOG_LEVEL_INFO,
				COFFEE3_LOG_SOURCE_WORKFLOW,
				g_xCoffee3WorkflowStatus.usCurrentOrderId,
				"BUSINESS_CONDITION_READY", 0,
				"condition", (int32_t)ucCondition);
			return 0;
		}
		if (ucWaitLogged == 0U) {
			ucWaitLogged = 1U;
			(void)xCoffee3LogWriteFieldOrder(COFFEE3_LOG_LEVEL_WARNING,
				COFFEE3_LOG_SOURCE_WORKFLOW,
				g_xCoffee3WorkflowStatus.usCurrentOrderId,
				"BUSINESS_CONDITION_WAIT", 0,
				"condition", (int32_t)ucCondition);
		}
		for (ucDelayIndex = 0U; ucDelayIndex < 5U; ucDelayIndex++) {
			if (g_xCoffee3WorkflowStatus.ucCancelRequested != 0U) {
				return COFFEE3_WORKFLOW_ERROR_CANCELED;
			}
			prvServiceHotWater();
			vTaskDelay(pdMS_TO_TICKS(100U));
		}
	}
}

/*-----------------------------------------------------------*/
/**
  * @brief  刷新设备镜像并等待指定状态字进入成功或失败终态。
  * @param[in] usStep 对外发布的工作流步骤号。
  * @param[in] xDeviceId 待轮询设备。
  * @param[in] ucStatusIndex 状态字索引。
  * @param[in] usSuccessValue 成功终态值。
  * @param[in] usFailedValue 失败终态值。
  * @retval 0 设备报告成功终态。
  * @retval 负数 设备失败、取消、索引错误或超时。
  */
static int32_t prvWaitDeviceReportedComplete(uint16_t usStep,
	Coffee3DeviceId_e xDeviceId, uint8_t ucStatusIndex,
	uint16_t usSuccessValue, uint16_t usFailedValue)
{
	int32_t lResult; /*!< 当前子步骤的执行结果或错误码。 */
	uint16_t usState; /*!< 当前设备状态寄存器的读取值。 */
	uint16_t usLogOrder; /*!< 当前日志使用的订单号或调试标识。 */
	uint8_t ucWaitLogged; /*!< 等待过程提示日志是否已记录。 */
	uint8_t ucOrderStep; /*!< 是否采用低编号业务步骤的处理规则。 */
	uint8_t ucDelayIndex; /*!< 分段等待中的当前延时序号。 */
	TickType_t xStart; /*!< 本段等待开始的系统节拍。 */

	if (((xDeviceId == COFFEE3_DEVICE_COFFEE_MACHINE) &&
		(ucStatusIndex >= 24U)) ||
		((xDeviceId == COFFEE3_DEVICE_SYRUP_MACHINE) &&
		(ucStatusIndex >= SYRUP_MACHINE_REGISTER_COUNT)) ||
		((xDeviceId != COFFEE3_DEVICE_COFFEE_MACHINE) &&
		(xDeviceId != COFFEE3_DEVICE_SYRUP_MACHINE))) {
		return COFFEE3_WORKFLOW_ERROR_UNSUPPORTED;
	}
	ucOrderStep = (usStep < 0xF000U) ? 1U : 0U;
	usLogOrder = (ucOrderStep != 0U) ?
		g_xCoffee3WorkflowStatus.usCurrentOrderId : COFFEE3_LOG_ORDER_DEBUG;
	ucWaitLogged = 0U;
	xStart = xTaskGetTickCount();
	if (ucOrderStep != 0U) {
		prvPublish(COFFEE3_WORKFLOW_RUNNING, usStep, 0);
	}
	for (;;) {
		if ((xTaskGetTickCount() - xStart) >=
			pdMS_TO_TICKS(COFFEE3_COFFEE_ACTION_TIMEOUT_MS)) {
			return COFFEE3_WORKFLOW_ERROR_TIMEOUT;
		}
		if (((ucOrderStep != 0U) || (s_ucMaintenanceActive != 0U)) &&
			(g_xCoffee3WorkflowStatus.ucCancelRequested != 0U)) {
			return COFFEE3_WORKFLOW_ERROR_CANCELED;
		}
		lResult = prvRefreshDeviceQuiet(usStep, xDeviceId);
		if (lResult != 0) {
			(void)xCoffee3LogWriteFieldOrder(COFFEE3_LOG_LEVEL_ERROR,
				COFFEE3_LOG_SOURCE_WORKFLOW, usLogOrder,
				"DEVICE_ACTION_REFRESH_FAILED", lResult,
				"device", (int32_t)xDeviceId);
			return lResult;
		}
		usState = (xDeviceId == COFFEE3_DEVICE_COFFEE_MACHINE) ?
			g_xCoffee3CoffeeMachineImage.ausStatus[ucStatusIndex] :
			g_xCoffee3SyrupImage.ausRegisters[ucStatusIndex];
		if (usState == usSuccessValue) {
			s_usActiveDevices &= (uint16_t)~(1U << (uint8_t)xDeviceId);
			(void)xCoffee3LogWriteFieldOrder(COFFEE3_LOG_LEVEL_INFO,
				COFFEE3_LOG_SOURCE_WORKFLOW, usLogOrder,
				"DEVICE_ACTION_COMPLETE", 0,
				"device", (int32_t)xDeviceId);
			return 0;
		}
		if (usState == usFailedValue) {
			(void)xCoffee3LogWriteFieldOrder(COFFEE3_LOG_LEVEL_ERROR,
				COFFEE3_LOG_SOURCE_WORKFLOW, usLogOrder,
				"DEVICE_ACTION_FAILED", COFFEE3_WORKFLOW_ERROR_DEVICE,
				"device", (int32_t)xDeviceId);
			return COFFEE3_WORKFLOW_ERROR_DEVICE;
		}
		if (ucWaitLogged == 0U) {
			ucWaitLogged = 1U;
			(void)xCoffee3LogWriteFieldOrder(COFFEE3_LOG_LEVEL_INFO,
				COFFEE3_LOG_SOURCE_WORKFLOW, usLogOrder,
				"DEVICE_ACTION_WAIT", (int32_t)usState,
				"device", (int32_t)xDeviceId);
		}
		for (ucDelayIndex = 0U; ucDelayIndex < 5U; ucDelayIndex++) {
			if (((ucOrderStep != 0U) || (s_ucMaintenanceActive != 0U)) &&
				(g_xCoffee3WorkflowStatus.ucCancelRequested != 0U)) {
				return COFFEE3_WORKFLOW_ERROR_CANCELED;
			}
			prvServiceHotWater();
			vTaskDelay(pdMS_TO_TICKS(100U));
		}
	}
}

/*-----------------------------------------------------------*/
/**
  * @brief  分片延时，并在等待期间持续服务热水、IO 和取杯流程。
  * @param[in] ulDelayMs 总延时时间，单位为毫秒。
  */
static void prvDelayWithServices(uint32_t ulDelayMs)
{
	TickType_t xStartTick; /*!< 本次动作或等待开始的系统节拍。 */

	xStartTick = xTaskGetTickCount();
	while ((xTaskGetTickCount() - xStartTick) < pdMS_TO_TICKS(ulDelayMs)) {
		prvServiceHotWater();
		prvServiceIoRefresh();
		vTaskDelay(pdMS_TO_TICKS(100U));
	}
}

/*-----------------------------------------------------------*/
/**
  * @brief  异步提交热水加热继电器写入命令，并记录待确认命令。
  * @param[in] ucValue 零表示关闭继电器，非零表示开启。
  * @retval pdPASS 命令已入队，终态仍需调用 prvPollHotWaterIo 查询。
  * @retval pdFAIL 已有待确认命令或本次提交失败。
  */
static BaseType_t prvSubmitHotWaterIo(uint8_t ucValue)
{
	if (s_xHotWater.ucIoPending != 0U) {
		return pdFAIL;
	}
	memset(&s_xHotWater.xCommand, 0, sizeof(s_xHotWater.xCommand));
	s_xHotWater.xCommand.ulOrderId = COFFEE3_LOG_ORDER_DEBUG;
	s_xHotWater.xCommand.usAction = (uint16_t)COFFEE3_ACTION_IO_WRITE;
	s_xHotWater.xCommand.ausParameter[0] =
		COFFEE3_EXTERNAL_DO_WATER_HEATER_RELAY;
	s_xHotWater.xCommand.ausParameter[1] = (ucValue != 0U) ? 1U : 0U;
	s_xHotWater.xCommand.ulTimeoutMs = COFFEE3_RTU_IO_TIMEOUT_MS;
	s_xHotWater.xCommand.ucDeviceId =
		(uint8_t)COFFEE3_DEVICE_IO_OUTPUT;
	s_xHotWater.xCommand.ucSource =
		(uint8_t)COFFEE3_COMMAND_SOURCE_MAINTENANCE;
	s_xHotWater.xCommand.ucRetryLimit = 1U;
	if (xCoffee3CommandSubmit(&s_xHotWater.xCommand, 0U) != pdPASS) {
		return pdFAIL;
	}
	s_xHotWater.ucIoPending = 1U;
	return pdPASS;
}

/*-----------------------------------------------------------*/
/**
  * @brief  查询热水继电器写入命令的终态，并在终态到达时清除待确认标志。
  * @param[out] pucDone 非空输出；一表示取得终态，零表示仍待确认或无待确认命令。
  * @retval 0 尚无终态，或命令成功结束。
  * @retval 负数 参数无效，或命令以错误终态结束。
  */
static int32_t prvPollHotWaterIo(uint8_t *pucDone)
{
	uint8_t ucValid; /*!< 设备命令终态是否有效。 */
	int32_t lResult; /*!< 当前子步骤的执行结果或错误码。 */

	if (pucDone == NULL) {
		return COFFEE3_WORKFLOW_ERROR_IO;
	}
	*pucDone = 0U;
	if (s_xHotWater.ucIoPending == 0U) {
		return 0;
	}
	lResult = lCoffee3DeviceGetTerminalResult(
		COFFEE3_DEVICE_IO_OUTPUT,
		s_xHotWater.xCommand.ulOrderEpoch,
		s_xHotWater.xCommand.ulCommandId, &ucValid);
	if (ucValid == 0U) {
		return 0;
	}
	s_xHotWater.ucIoPending = 0U;
	*pucDone = 1U;
	return lResult;
}

/*-----------------------------------------------------------*/
/**
  * @brief  仅在热水公开状态改变时更新状态，并按需记录事件。
  * @param[in] ucState 要发布的热水维护状态；告警态也会设置整机告警。
  * @param[in] pcEvent 非空时记录的事件名。
  * @param[in] lResult 随事件写入的结果值。
  */
static void prvSetHotWaterPublicState(uint8_t ucState,
	const char *pcEvent, int32_t lResult)
{
	if (g_xCoffee3WorkflowStatus.ucHotWaterState == ucState) {
		return;
	}
	g_xCoffee3WorkflowStatus.ucHotWaterState = ucState;
	if (ucState == COFFEE3_MAINTENANCE_ALARM) {
		g_xCoffee3WorkflowStatus.xMachineState = COFFEE3_MACHINE_ALARM;
	}
	if (pcEvent != NULL) {
		(void)xCoffee3LogWriteFieldOrder(
			(ucState == COFFEE3_MAINTENANCE_ALARM) ?
				COFFEE3_LOG_LEVEL_ERROR : COFFEE3_LOG_LEVEL_INFO,
			COFFEE3_LOG_SOURCE_WORKFLOW, COFFEE3_LOG_ORDER_DEBUG,
			pcEvent, lResult, "state", (int32_t)ucState);
	}
}

/*-----------------------------------------------------------*/
/**
  * @brief  按液位和供水条件推进咖啡机水箱的本地补水泵。
  * @details 运行中遇到条件失效、高液位或二十秒超时即关泵；仅超时置告警。
  *          未运行时还需满足人工、OTA、恢复锁等准入条件才会启泵。
  */
static void prvServiceCoffeeFill(void)
{
	Coffee3IoState_t xIo; /*!< 当前读取的输入与输出状态快照。 */
	TickType_t xNow; /*!< 当前系统节拍，用于判断等待时长。 */
	uint8_t ucAllowed; /*!< 当前业务前置条件是否允许继续。 */
	uint8_t ucTimedOut; /*!< 本次咖啡机补水是否达到二十秒时限。 */

	vCoffee3IoGetSnapshot(&xIo);
	xNow = xTaskGetTickCount();
	ucAllowed = ((prvIoValid(&xIo) != 0U) &&
		(xIo.xInput.aucMB1XPin[3] != 0U) &&
		(xIo.xInput.aucMB1XPin[5] != 0U) &&
		(xIo.xInput.aucMB1XPin[6] == 0U)) ? 1U : 0U;
	if (s_ucCoffeeFillActive != 0U) {
		ucTimedOut = ((xNow - s_xCoffeeFillStart) >=
			pdMS_TO_TICKS(20000U)) ? 1U : 0U;
		if ((ucAllowed == 0U) || (ucTimedOut != 0U) ||
			(xIo.xInput.aucXPin[COFFEE3_LOCAL_DI_COFFEE_WATER_HIGH] != 0U)) {
			(void)ucCoffee3IoSetLocalOutput(COFFEE3_LOCAL_DO_COFFEE_WATER_PUMP, 0U);
			s_ucCoffeeFillActive = 0U;
			if (ucTimedOut != 0U) {
				s_ucCoffeeFillAlarm = 1U;
			}
			(void)xCoffee3LogPrintfOrder((ucTimedOut != 0U) ?
				COFFEE3_LOG_LEVEL_WARNING : COFFEE3_LOG_LEVEL_INFO,
				COFFEE3_LOG_SOURCE_WORKFLOW, COFFEE3_LOG_ORDER_SYSTEM,
				"M50 tank fill stopped: high=%u supply_ok=%u timeout=%u; DO4 off",
				(unsigned int)xIo.xInput.aucXPin[2],
				(unsigned int)ucAllowed, (unsigned int)ucTimedOut);
		}
		return;
	}
	taskENTER_CRITICAL();
	if ((ucAllowed != 0U) &&
		(s_ucCoffeeFillAlarm == 0U) && (s_ucOtaReserved == 0U) &&
		((s_usManualReservations == 0U) ||
		 (g_xCoffee3WorkflowStatus.xState == COFFEE3_WORKFLOW_RUNNING) ||
		 (s_ucMaintenanceActive != 0U)) &&
		(g_xCoffee3WorkflowStatus.ucRecoveryRequired == 0U) &&
		(xIo.xInput.aucXPin[COFFEE3_LOCAL_DI_COFFEE_WATER_LOW] == 0U) &&
		(xIo.xInput.aucXPin[COFFEE3_LOCAL_DI_COFFEE_WATER_HIGH] == 0U)) {
		s_ucCoffeeFillActive = 1U;
		s_xCoffeeFillStart = xNow;
	}
	taskEXIT_CRITICAL();
	if (s_ucCoffeeFillActive != 0U) {
		(void)ucCoffee3IoSetLocalOutput(COFFEE3_LOCAL_DO_COFFEE_WATER_PUMP, 1U);
	}
}

/*-----------------------------------------------------------*/
/**
  * @brief  推进热水补水、加热及关闭确认状态机。
  * @note   纯水不足、液位异常或取消都会先关闭本地补水阀，
  *         再异步关闭外部加热输出。
  */
static void prvServiceHotWater(void)
{
	Coffee3IoState_t xIo; /*!< 当前读取的输入与输出状态快照。 */
	TickType_t xNow; /*!< 当前系统节拍，用于判断等待时长。 */
	uint32_t ulHeatMs; /*!< 配置的热水加热时间，单位为毫秒。 */
	uint8_t ucDone; /*!< 热水继电器命令是否已有终态。 */
	int32_t lResult; /*!< 当前子步骤的执行结果或错误码。 */

	/* 步骤 1：先刷新 IO 并服务咖啡补水；空闲且无取消请求时立即返回。 */
	prvServiceIoRefresh();
	prvServiceCoffeeFill();
	if ((s_xHotWater.ucPhase == COFFEE3_HOT_WATER_IDLE) &&
		(s_xHotWater.ucCancelRequested == 0U)) {
		return;
	}
	vCoffee3IoRefreshLocal();
	vCoffee3IoGetSnapshot(&xIo);
	xNow = xTaskGetTickCount();
	/* 步骤 2：运行阶段持续检查输入模块和纯水桶；任一无效都转为取消，
	 * 先关本地阀，再等待外部加热输出关闭。 */
	if ((s_xHotWater.ucPhase < COFFEE3_HOT_WATER_WAIT_OFF_DONE) &&
		((prvIoValid(&xIo) == 0U) ||
		(xIo.xInput.aucMB1XPin[COFFEE3_EXTERNAL_DI_PURE_WATER_LOW] == 0U))) {
		s_xHotWater.ucCancelRequested = 1U;
	}
	if (s_xHotWater.ucCancelRequested != 0U) {
		(void)ucCoffee3IoSetLocalOutput(
			COFFEE3_LOCAL_DO_HOT_WATER_SUPPLY_VALVE, 0U);
		if (s_xHotWater.ucIoPending != 0U) {
			(void)prvPollHotWaterIo(&ucDone);
			return;
		}
		if (s_xHotWater.ucIoPending == 0U) {
			if (prvSubmitHotWaterIo(0U) == pdPASS) {
				s_xHotWater.ucPhase =
					COFFEE3_HOT_WATER_WAIT_OFF_DONE;
				s_xHotWater.ucCancelRequested = 0U;
			}
		}
		return;
	}
	/* 步骤 3：PREPARE_OFF 先确认加热输出关闭，再检查高液位并打开
	 * 本地补水阀，避免带加热补水。 */
	switch ((Coffee3HotWaterPhase_e)s_xHotWater.ucPhase) {
	case COFFEE3_HOT_WATER_PREPARE_OFF:
		if (s_xHotWater.ucIoPending == 0U) {
			(void)prvSubmitHotWaterIo(0U);
			return;
		}
		lResult = prvPollHotWaterIo(&ucDone);
		if (ucDone == 0U) {
			return;
		}
		if (lResult != 0) {
			s_xHotWater.ucAlarmReason = 3U;
			s_xHotWater.ucPhase = COFFEE3_HOT_WATER_WAIT_OFF_ALARM;
			return;
		}
		if (xIo.xInput.aucXPin[COFFEE3_LOCAL_DI_HOT_WATER_HIGH] != 0U) {
			s_xHotWater.ucAlarmReason = 1U;
			s_xHotWater.ucPhase = COFFEE3_HOT_WATER_WAIT_OFF_ALARM;
			return;
		}
		(void)ucCoffee3IoSetLocalOutput(
			COFFEE3_LOCAL_DO_HOT_WATER_SUPPLY_VALVE, 1U);
		s_xHotWater.xStartTick = xNow;
		s_xHotWater.ucPhase = COFFEE3_HOT_WATER_FILLING;
		prvSetHotWaterPublicState(COFFEE3_MAINTENANCE_RUNNING,
			"HOT_WATER_FILLING", 0);
		break;
	/* 步骤 4：补水以低液位到达为正常终点；高液位先到或补水超时
	 * 都关闭补水阀并进入告警关闭路径。 */
	case COFFEE3_HOT_WATER_FILLING:
		if (xIo.xInput.aucXPin[COFFEE3_LOCAL_DI_HOT_WATER_HIGH] != 0U) {
			(void)ucCoffee3IoSetLocalOutput(
				COFFEE3_LOCAL_DO_HOT_WATER_SUPPLY_VALVE, 0U);
			s_xHotWater.ucAlarmReason = 1U;
			if (prvSubmitHotWaterIo(0U) == pdPASS) {
				s_xHotWater.ucPhase =
					COFFEE3_HOT_WATER_WAIT_OFF_ALARM;
			}
		} else if (xIo.xInput.aucXPin[
			COFFEE3_LOCAL_DI_HOT_WATER_LOW] != 0U) {
			(void)ucCoffee3IoSetLocalOutput(
				COFFEE3_LOCAL_DO_HOT_WATER_SUPPLY_VALVE, 0U);
			(void)xCoffee3LogWriteFieldOrder(COFFEE3_LOG_LEVEL_INFO,
				COFFEE3_LOG_SOURCE_WORKFLOW, COFFEE3_LOG_ORDER_DEBUG,
				"HOT_WATER_FILL_COMPLETE", 0, "low_level", 1);
			if (prvSubmitHotWaterIo(1U) == pdPASS) {
				s_xHotWater.ucPhase =
					COFFEE3_HOT_WATER_WAIT_HEATER_ON;
			}
		} else if ((xNow - s_xHotWater.xStartTick) >=
			pdMS_TO_TICKS(COFFEE3_HOT_WATER_FILL_TIMEOUT_MS)) {
			(void)ucCoffee3IoSetLocalOutput(
				COFFEE3_LOCAL_DO_HOT_WATER_SUPPLY_VALVE, 0U);
			s_xHotWater.ucAlarmReason = 2U;
			if (prvSubmitHotWaterIo(0U) == pdPASS) {
				s_xHotWater.ucPhase =
					COFFEE3_HOT_WATER_WAIT_OFF_ALARM;
			}
		}
		break;
	case COFFEE3_HOT_WATER_WAIT_HEATER_ON:
		lResult = prvPollHotWaterIo(&ucDone);
		if (ucDone == 0U) {
			break;
		}
		if (lResult != 0) {
			s_xHotWater.ucAlarmReason = 3U;
			s_xHotWater.ucPhase = COFFEE3_HOT_WATER_WAIT_OFF_ALARM;
			break;
		}
		s_xHotWater.xStartTick = xNow;
		s_xHotWater.ucPhase = COFFEE3_HOT_WATER_HEATING;
		(void)xCoffee3LogWriteFieldOrder(COFFEE3_LOG_LEVEL_INFO,
			COFFEE3_LOG_SOURCE_WORKFLOW, COFFEE3_LOG_ORDER_DEBUG,
			"HOT_WATER_HEATING", 0, "minutes",
			(int32_t)s_xHotWater.usHeatMinutes);
		break;
	/* 步骤 5：加热按分钟计时，高液位异常会提前停止；正常和告警路径
	 * 最终都必须取得外部输出关闭终态后才回到空闲。 */
	case COFFEE3_HOT_WATER_HEATING:
		ulHeatMs = (uint32_t)s_xHotWater.usHeatMinutes * 60000U;
		if (xIo.xInput.aucXPin[COFFEE3_LOCAL_DI_HOT_WATER_HIGH] != 0U) {
			s_xHotWater.ucAlarmReason = 1U;
			if (prvSubmitHotWaterIo(0U) == pdPASS) {
				s_xHotWater.ucPhase =
					COFFEE3_HOT_WATER_WAIT_OFF_ALARM;
			}
		} else if ((xNow - s_xHotWater.xStartTick) >=
			pdMS_TO_TICKS(ulHeatMs)) {
			if (prvSubmitHotWaterIo(0U) == pdPASS) {
				s_xHotWater.ucPhase =
					COFFEE3_HOT_WATER_WAIT_OFF_DONE;
			}
		}
		break;
	case COFFEE3_HOT_WATER_WAIT_OFF_DONE:
		lResult = prvPollHotWaterIo(&ucDone);
		if (ucDone != 0U) {
			s_xHotWater.ucPhase = COFFEE3_HOT_WATER_IDLE;
			prvSetHotWaterPublicState(
				(lResult == 0) ? COFFEE3_MAINTENANCE_COMPLETED :
					COFFEE3_MAINTENANCE_ALARM,
				(lResult == 0) ? "HOT_WATER_COMPLETE" :
					"HOT_WATER_STOP_FAILED", lResult);
		}
		break;
	case COFFEE3_HOT_WATER_WAIT_OFF_ALARM:
		if (s_xHotWater.ucIoPending == 0U) {
			(void)prvSubmitHotWaterIo(0U);
			break;
		}
		lResult = prvPollHotWaterIo(&ucDone);
		if (ucDone != 0U) {
			s_xHotWater.ucPhase = COFFEE3_HOT_WATER_IDLE;
			prvSetHotWaterPublicState(COFFEE3_MAINTENANCE_ALARM,
				"HOT_WATER_ALARM", (lResult != 0) ? lResult :
					-(int32_t)s_xHotWater.ucAlarmReason);
		}
		break;
	default:
		s_xHotWater.ucPhase = COFFEE3_HOT_WATER_IDLE;
		break;
	}
}

/*-----------------------------------------------------------*/
/**
  * @brief  发生失败后停止热水输出并收敛已活动执行机构。
  * @retval 0 所有已提交的安全停止均取得成功终态，且产品输出已关闭。
  * @retval COFFEE3_WORKFLOW_ERROR_SAFE_STOP 提交、终态确认或输出关闭失败。
  * @note   函数会等待制冰机、咖啡机和机械臂的安全停止终态；
  *         糖浆机协议没有可验证的停止命令，活动时按停止失败处理。
  */
static int32_t prvAbortDevices(void)
{
	Coffee3Command_t axCommand[3]; /*!< 待提交的三类安全停止命令。 */
	EventBits_t xEvents; /*!< 设备事件组返回的完成或失败标志。 */
	BaseType_t axSubmitted[3]; /*!< 各安全停止命令是否成功提交。 */
	uint8_t ucIndex; /*!< 当前遍历项的下标。 */
	int32_t lResult; /*!< 当前子步骤的执行结果或错误码。 */

	/* 步骤 1：发布取消状态、关闭本地热水阀，并广播当前订单代次取消。 */
	prvPublish(COFFEE3_WORKFLOW_CANCELING,
		g_xCoffee3WorkflowStatus.usCurrentStep,
		g_xCoffee3WorkflowStatus.lLastError);
	s_xHotWater.ucCancelRequested = 1U;
	(void)ucCoffee3IoSetLocalOutput(
		COFFEE3_LOCAL_DO_HOT_WATER_SUPPLY_VALVE, 0U);
	vCoffee3OrderCancelRequest(g_xCoffee3WorkflowStatus.ulOrderEpoch);
	/* 步骤 2：构造制冰阀关闭、咖啡取消和机械臂取消三条安全命令，
	 * 每条命令使用独立编号和有界确认时间。 */
	memset(axCommand, 0, sizeof(axCommand));
	for (ucIndex = 0U; ucIndex < 3U; ucIndex++) {
		axCommand[ucIndex].ucSource =
			(uint8_t)COFFEE3_COMMAND_SOURCE_WORKFLOW;
		axCommand[ucIndex].ucFlags = COFFEE3_COMMAND_FLAG_SAFETY_STOP;
		axCommand[ucIndex].ulOrderId =
			g_xCoffee3WorkflowStatus.usCurrentOrderId;
		axCommand[ucIndex].ulOrderEpoch =
			g_xCoffee3WorkflowStatus.ulOrderEpoch;
		axCommand[ucIndex].ulTimeoutMs =
			COFFEE3_WORKFLOW_SAFE_STOP_MS;
		axCommand[ucIndex].usStepId = 0xFFF0U + ucIndex;
	}
	axCommand[0].ucDeviceId = (uint8_t)COFFEE3_DEVICE_ICE_MACHINE;
	axCommand[0].usAction = (uint16_t)COFFEE3_ACTION_ICE_SET_VALVE;
	axCommand[0].ausParameter[0] = 0U;
	axCommand[1].ucDeviceId =
		(uint8_t)COFFEE3_DEVICE_COFFEE_MACHINE;
	axCommand[1].usAction = (uint16_t)COFFEE3_ACTION_CANCEL;
	axCommand[2].ucDeviceId = (uint8_t)COFFEE3_DEVICE_ROBOT;
	axCommand[2].usAction = (uint16_t)COFFEE3_ACTION_CANCEL;

	/* 步骤 3：只向本单实际活动的设备投递紧急命令；手动接管机械臂
	 * 时不覆盖操作者命令。 */
	lResult = 0;
	/* 步骤 4：逐一投递紧急命令，队列失败立即记为安全停止失败。 */
	for (ucIndex = 0U; ucIndex < 3U; ucIndex++) {
		if ((s_usActiveDevices & (1U << axCommand[ucIndex].ucDeviceId)) == 0U) {
			axSubmitted[ucIndex] = pdFAIL;
			continue;
		}
		if ((ucIndex == 2U) && (s_ucManualOverride != 0U)) {
			axSubmitted[ucIndex] = pdPASS;
			continue;
		}
		axSubmitted[ucIndex] = xCoffee3CommandSubmitUrgent(
			&axCommand[ucIndex], pdMS_TO_TICKS(100U));
		if (axSubmitted[ucIndex] != pdPASS) {
			lResult = COFFEE3_WORKFLOW_ERROR_SAFE_STOP;
			(void)xCoffee3LogWriteFieldOrder(COFFEE3_LOG_LEVEL_ERROR,
				COFFEE3_LOG_SOURCE_WORKFLOW,
				g_xCoffee3WorkflowStatus.usCurrentOrderId,
				"SAFE_STOP_QUEUE_FAILED", lResult, "device",
				(int32_t)axCommand[ucIndex].ucDeviceId);
		}
	}
	/* 步骤 5：逐一等待已提交命令的终态，并校验终态结果确实成功。 */
	for (ucIndex = 0U; ucIndex < 3U; ucIndex++) {
		if (axSubmitted[ucIndex] != pdPASS) {
			continue;
		}
		if ((ucIndex == 2U) && (s_ucManualOverride != 0U)) {
			continue;
		}
		xEvents = xCoffee3DeviceWaitCommand(
			(Coffee3DeviceId_e)axCommand[ucIndex].ucDeviceId,
			axCommand[ucIndex].ulOrderEpoch,
			axCommand[ucIndex].ulCommandId,
			pdMS_TO_TICKS(COFFEE3_WORKFLOW_SAFE_STOP_MS));
		{
			uint8_t ucValid; /*!< 设备命令终态是否有效。 */
			int32_t lStopResult; /*!< 单条安全停止命令的终态结果。 */
			lStopResult = lCoffee3DeviceGetTerminalResult(
				(Coffee3DeviceId_e)axCommand[ucIndex].ucDeviceId,
				axCommand[ucIndex].ulOrderEpoch,
				axCommand[ucIndex].ulCommandId, &ucValid);
			if (((xEvents & COFFEE3_DEVICE_EVENT_TERMINAL) == 0U) ||
				(ucValid == 0U) || (lStopResult != 0)) {
				lResult = COFFEE3_WORKFLOW_ERROR_SAFE_STOP;
				(void)xCoffee3LogWriteFieldOrder(COFFEE3_LOG_LEVEL_ERROR,
					COFFEE3_LOG_SOURCE_WORKFLOW,
					g_xCoffee3WorkflowStatus.usCurrentOrderId,
					"SAFE_STOP_ACK_FAILED", lResult, "device",
					(int32_t)axCommand[ucIndex].ucDeviceId);
			}
		}
	}
	/* 步骤 6：最后关闭所有产品 IO 输出；糖浆机活动时因协议没有
	 * 可验证的停止命令，必须保持安全停止失败并要求人工检查。 */
	if (prvSetProductOutputsOff() != 0) {
		lResult = COFFEE3_WORKFLOW_ERROR_SAFE_STOP;
	}
	if ((s_usActiveDevices & (1U << COFFEE3_DEVICE_SYRUP_MACHINE)) != 0U) {
		/* 当前糖浆机协议没有经过验证的 STOP 命令，无法确认其安全停止。 */
		lResult = COFFEE3_WORKFLOW_ERROR_SAFE_STOP;
		(void)xCoffee3LogPrintfOrder(COFFEE3_LOG_LEVEL_ERROR,
			COFFEE3_LOG_SOURCE_WORKFLOW,
			g_xCoffee3WorkflowStatus.usCurrentOrderId,
			"Syrup action unresolved: STOP unsupported; inspect before reset");
	}
	(void)xCoffee3LogWriteOrder((lResult == 0) ? COFFEE3_LOG_LEVEL_INFO :
		COFFEE3_LOG_LEVEL_ERROR,
		COFFEE3_LOG_SOURCE_WORKFLOW,
		g_xCoffee3WorkflowStatus.usCurrentOrderId,
		(lResult == 0) ? "SAFE_STOP_CONFIRMED" :
			"SAFE_STOP_INCOMPLETE", lResult);
	return lResult;
}

/*-----------------------------------------------------------*/
/**
  * @brief  周期刷新本地 GPIO 镜像并服务取杯边界。
  * @note   外部 RTU IO 镜像由 Bus5 自主轮询，本函数不再投递刷新帧。
  */
static void prvServiceIoRefresh(void)
{
	/* 各 RTU 所有者自行刷新设备；工作流这里只采样本地 GPIO，
	 * 不再向各 Bus 队列持续投递周期读请求。 */
	prvServicePickup();
	vCoffee3IoRefreshLocal();
}

/*-----------------------------------------------------------*/
/**
  * @brief  将逻辑设备编号转换为工作流日志使用的设备名称。
  * @param[in] xDeviceId 命令目标逻辑设备。
  * @retval 字符串常量 已知设备名称；未知编号返回未知设备占位文本。
  */
static const char *prvWorkflowDeviceName(Coffee3DeviceId_e xDeviceId)
{
	switch (xDeviceId) {
	case COFFEE3_DEVICE_ROBOT: return "Robot";
	case COFFEE3_DEVICE_COFFEE_MACHINE: return "CoffeeMachine";
	case COFFEE3_DEVICE_CUP_MACHINE: return "CupMachine";
	case COFFEE3_DEVICE_SYRUP_MACHINE: return "SyrupMachine";
	case COFFEE3_DEVICE_LID_MACHINE: return "LidMachine";
	case COFFEE3_DEVICE_ICE_MACHINE: return "IceMachine";
	case COFFEE3_DEVICE_SCALE: return "Weigh Scale";
	case COFFEE3_DEVICE_POWER_METER: return "EnergyMeter";
	case COFFEE3_DEVICE_IO_INPUT: return "IoInput";
	case COFFEE3_DEVICE_IO_OUTPUT: return "IoOutput";
	default: return "UnknownDevice";
	}
}

/*-----------------------------------------------------------*/
/**
  * @brief  将设备动作转换为工作流日志使用的动作名称。
  * @param[in] xAction 设备动作枚举值。
  * @retval 字符串常量 已知动作名称；未知动作返回未知动作占位文本。
  */
static const char *prvWorkflowActionName(Coffee3Action_e xAction)
{
 switch (xAction) {
 case COFFEE3_ACTION_REFRESH: return "status refresh";
 case COFFEE3_ACTION_SCALE_TARE: return "tare scale";
 case COFFEE3_ACTION_SCALE_CLEAR_TARE: return "clear scale tare";
 case COFFEE3_ACTION_SCALE_ZERO: return "zero scale";
 case COFFEE3_ACTION_ICE_SET_VALVE: return "set ice valve";
 case COFFEE3_ACTION_COFFEE_MAKE: return "make coffee";
 case COFFEE3_ACTION_COFFEE_CLEAN: return "clean coffee";
 case COFFEE3_ACTION_SYRUP_DISPENSE: return "dispense syrup";
 case COFFEE3_ACTION_SYRUP_CLEAN: return "clean syrup";
 case COFFEE3_ACTION_CUP_DROP_1: return "drop cup 1";
 case COFFEE3_ACTION_CUP_DROP_2: return "drop cup 2";
 case COFFEE3_ACTION_LID_DROP_1: return "drop lid 1";
 case COFFEE3_ACTION_LID_DROP_2: return "drop lid 2";
 case COFFEE3_ACTION_ROBOT_HOME: return "robot home";
 case COFFEE3_ACTION_ROBOT_TO_COFFEE: return "move to coffee";
 case COFFEE3_ACTION_ROBOT_TO_ICE: return "move to ice";
 case COFFEE3_ACTION_ROBOT_TO_LID: return "move to lid";
 case COFFEE3_ACTION_ROBOT_TAKE_HOT_CUP: return "take hot cup";
 case COFFEE3_ACTION_ROBOT_TAKE_COLD_CUP: return "take cold cup";
 case COFFEE3_ACTION_ROBOT_TAKE_COFFEE: return "take coffee";
 case COFFEE3_ACTION_ROBOT_TAKE_LID: return "take lid";
 case COFFEE3_ACTION_ROBOT_COVER_LID: return "cover lid";
 case COFFEE3_ACTION_ROBOT_PUT_OUTPUT: return "put output";
 case COFFEE3_ACTION_ROBOT_PUT_STORAGE: return "put storage";
 case COFFEE3_ACTION_ROBOT_TAKE_STORAGE: return "take storage";
 case COFFEE3_ACTION_ROBOT_START: return "robot start";
 case COFFEE3_ACTION_ROBOT_STOP: return "robot stop";
 case COFFEE3_ACTION_ROBOT_ENABLE: return "robot enable";
 case COFFEE3_ACTION_ROBOT_CLEAR_ALARM: return "clear robot alarm";
 case COFFEE3_ACTION_IO_WRITE: return "write IO";
 case COFFEE3_ACTION_IO_WRITE_MASK: return "write IO mask";
 case COFFEE3_ACTION_CANCEL: return "cancel";
 default: return "unknown action";
 }
}

/*-----------------------------------------------------------*/
/**
  * @brief  将工作流状态发布到全局状态和 Server 状态寄存器。
  * @param[in] xState 新的工作流状态。
  * @param[in] usStep 当前步骤编号。
  * @param[in] lError 当前错误码；无错误时为 0。
  */
static void prvPublish(Coffee3WorkflowState_e xState,
	uint16_t usStep, int32_t lError)
{
	uint16_t usProductionStatus; /*!< 发布给上位机的制作状态值。 */

	taskENTER_CRITICAL();
	g_xCoffee3WorkflowStatus.xState = xState;
	g_xCoffee3WorkflowStatus.usCurrentStep = usStep;
	g_xCoffee3WorkflowStatus.lLastError = lError;
	if ((g_xCoffee3WorkflowStatus.ucHotWaterState ==
		COFFEE3_MAINTENANCE_ALARM) ||
		(((s_ucWaterAlarm != 0U) || (s_ucCoffeeFillAlarm != 0U)) &&
		 (xState != COFFEE3_WORKFLOW_RUNNING))) {
		g_xCoffee3WorkflowStatus.xMachineState = COFFEE3_MACHINE_ALARM;
	} else if ((xState == COFFEE3_WORKFLOW_RUNNING) ||
		(xState == COFFEE3_WORKFLOW_CANCELING)) {
		g_xCoffee3WorkflowStatus.xMachineState = COFFEE3_MACHINE_BUSY;
	} else if (xState == COFFEE3_WORKFLOW_FAILED) {
		g_xCoffee3WorkflowStatus.xMachineState = COFFEE3_MACHINE_ALARM;
	} else if (g_xCoffee3WorkflowStatus.xMachineState !=
		COFFEE3_MACHINE_INITIALIZING) {
		g_xCoffee3WorkflowStatus.xMachineState = COFFEE3_MACHINE_IDLE;
	}
	taskEXIT_CRITICAL();
	switch (xState) {
	case COFFEE3_WORKFLOW_RUNNING:
	case COFFEE3_WORKFLOW_CANCELING:
		usProductionStatus = COFFEE3_PRODUCTION_RUNNING;
		break;
	case COFFEE3_WORKFLOW_COMPLETED:
		usProductionStatus = COFFEE3_PRODUCTION_COMPLETED;
		break;
	case COFFEE3_WORKFLOW_FAILED:
		usProductionStatus = COFFEE3_PRODUCTION_FAILED;
		break;
	default:
		usProductionStatus = COFFEE3_PRODUCTION_IDLE;
		break;
	}
	if (s_ucMaintenanceActive != 0U) {
		return;
	}
	if ((g_xCoffee3WorkflowStatus.ucContentComplete != 0U) &&
		(usProductionStatus == COFFEE3_PRODUCTION_RUNNING)) {
		usProductionStatus = COFFEE3_PRODUCTION_COMPLETED;
	}
	vCoffee3ServerPublishWorkflow(
		g_xCoffee3WorkflowStatus.usCurrentOrderId,
		usProductionStatus, usStep, lError);
}


/*-----------------------------------------------------------*/
/**
  * @brief  等待咖啡机清洗状态先进入工作态，再回到空闲态。
  * @retval 0 观察到工作态后返回空闲态。
  * @retval COFFEE3_WORKFLOW_ERROR_TIMEOUT 在清洗预算内未观察到完整状态变化。
  * @retval 负数 收到取消或刷新咖啡机状态失败。
  */
static int32_t prvWaitM50Clean(void)
{
	TickType_t xStart; /*!< 本段等待开始的系统节拍。 */
	int32_t lResult; /*!< 当前子步骤的执行结果或错误码。 */
	uint8_t ucWorking; /*!< 清洗状态是否曾进入工作态。 */

	xStart = xTaskGetTickCount();
	ucWorking = 0U;
	while ((xTaskGetTickCount() - xStart) <
		pdMS_TO_TICKS(COFFEE3_COFFEE_ACTION_TIMEOUT_MS)) {
		if (g_xCoffee3WorkflowStatus.ucCancelRequested != 0U) {
			return COFFEE3_WORKFLOW_ERROR_CANCELED;
		}
		lResult = prvRefreshDeviceQuiet(0xFB21U, COFFEE3_DEVICE_COFFEE_MACHINE);
		if (lResult != 0) {
			return lResult;
		}
		if (g_xCoffee3CoffeeMachineImage.ausStatus[0] !=
			g_xCoffeeMachineM50Config.usIdleValue) {
			ucWorking = 1U;
		} else if (ucWorking != 0U) {
			s_usActiveDevices &= (uint16_t)~(1U << COFFEE3_DEVICE_COFFEE_MACHINE);
			return 0;
		}
		prvDelayWithServices(500U);
	}
	return COFFEE3_WORKFLOW_ERROR_TIMEOUT;
}

/*-----------------------------------------------------------*/
/**
  * @brief  在咖啡制作等待窗口尝试插入一次暂存位取杯。
  * @retval 0 无需执行、条件不满足，或机械臂阶段完成并转入异步物理确认。
  * @retval 负数 机械臂取杯或放杯动作失败。
  */
static int32_t prvTryRunInterleavedStoragePickup(void)
{
	Coffee3IoState_t xIo; /*!< 当前读取的输入与输出状态快照。 */
	uint16_t usStorage; /*!< 本次取餐使用的来源储位编号。 */
	uint16_t usStorageBit; /*!< 来源储位在状态掩码中的位。 */
	uint16_t usStatusMask; /*!< 当前储位物理状态掩码。 */
	uint16_t usStoredOrderId; /*!< 储位杯体关联的订单号。 */
	uint8_t ucPhysicalCup; /*!< 来源储位当前是否实际检测到杯体。 */
	int32_t lResult; /*!< 当前子步骤的执行结果或错误码。 */

	/* 步骤 1：检查挂起请求和机械臂所有权。已接受的调试动作优先占用
	 * 下一个机械臂空闲边界；取杯请求继续挂起，
	 * 两类请求都不会在此处转换为队列。 */
	taskENTER_CRITICAL();
	usStorage = s_ucStoragePickupPending;
	if ((usStorage == 0U) || (s_ucStoragePickupActive != 0U) ||
		(s_ucStoragePickupConfirmStorage != 0U) ||
		(s_usManualReservations != 0U)) {
		usStorage = 0U;
	}
	taskEXIT_CRITICAL();
	if (usStorage == 0U) {
		return 0;
	}
	if ((usStorage < 1U) || (usStorage > 2U)) {
		taskENTER_CRITICAL();
		s_ucStoragePickupPending = 0U;
		taskEXIT_CRITICAL();
		prvRejectStoragePickup(usStorage, "storage unsupported", 0U);
		return 0;
	}
	/* 步骤 2：检查主机门控和出杯口，并采样暂存位物理状态。0x0007
	 * 是主机请求门控位，并不表示已接收订单完成；主机清除请求锁存前
	 * 继续延后暂存取杯。 */
	if (usCoffee3ServerGetCommandRegister(
		COFFEE3_REG_ORDER_PRESENT) != 0U) {
		return 0;
	}
	if (prvCheckOutputEmpty(1U) != 0) {
		return 0;
	}
	vCoffee3IoGetSnapshot(&xIo);
	usStorageBit = (uint16_t)(1U << (usStorage - 1U));
	usStatusMask = prvStorageStatusMask(&xIo);
	ucPhysicalCup = ((prvIoValid(&xIo) != 0U) &&
		(xIo.xInput.aucMB1XPin[usStorage - 1U] != 0U)) ? 1U : 0U;

	/* 步骤 3：只读检查完成后再原子接管请求；在该边界前到达的调试预约
	 * 优先。一旦接管，机械臂所有权即保持独占，再按采样结果校验配置、
	 * 物理杯体和状态位。 */
	taskENTER_CRITICAL();
	if ((s_ucStoragePickupPending != (uint8_t)usStorage) ||
		(s_usManualReservations != 0U) ||
		(s_ucStoragePickupActive != 0U)) {
		taskEXIT_CRITICAL();
		return 0;
	}
	s_ucStoragePickupPending = 0U;
	s_ucStoragePickupActive = 1U;
	taskEXIT_CRITICAL();

	if ((usCoffee3ConfigStorageMask() & usStorageBit &
		COFFEE3_STORAGE_INSTALLED_MASK) == 0U) {
		prvRejectStoragePickup(usStorage, "storage disabled", usStatusMask);
		s_ucStoragePickupActive = 0U;
		return 0;
	}
	if (ucPhysicalCup == 0U) {
		prvRejectStoragePickup(usStorage, "cup not detected", usStatusMask);
		s_ucStoragePickupActive = 0U;
		return 0;
	}
	if ((usStatusMask & usStorageBit) == 0U) {
		prvRejectStoragePickup(usStorage, "0x1027 storage bit not set",
			usStatusMask);
		s_ucStoragePickupActive = 0U;
		return 0;
	}

	/* 步骤 4：关联暂存订单并依次执行取杯、放入出杯口；运动期间位置
	 * 保持不确定，且机械臂计入活动设备。 */
	usStoredOrderId = s_ausStoredOrderId[usStorage - 1U];
	s_usDetachedPickupLogOrder = (usStoredOrderId != 0U) ?
		usStoredOrderId : COFFEE3_LOG_ORDER_SYSTEM;
	g_xCoffee3WorkflowStatus.ucPositionUncertain = 1U;
	s_usActiveDevices |= (uint16_t)(1U << COFFEE3_DEVICE_ROBOT);
	vCoffee3ServerSelectStorage(usStorage);
	prvPublishOutputForOrder(1U, 2U, s_usDetachedPickupLogOrder);
	(void)xCoffee3LogPrintfOrder(COFFEE3_LOG_LEVEL_INFO,
		COFFEE3_LOG_SOURCE_WORKFLOW, s_usDetachedPickupLogOrder,
		"Pickup interleave start: device=Robot storage=%u outlet=1 status_0x1027=0x%04X",
		(unsigned int)usStorage, (unsigned int)usStatusMask);
	lResult = prvRunStep(0xFC80U, COFFEE3_DEVICE_ROBOT,
		COFFEE3_ACTION_ROBOT_TAKE_STORAGE, usStorage, 0U,
		COFFEE3_WORKFLOW_ROBOT_MOTION_MS);
	/* 步骤 5：机械臂成功后先释放其所有权，再把 X3 有杯和源位无杯的
	 * 物理确认交给周期取杯服务；失败则发布出杯口故障并结束请求。 */
	if (lResult == 0) {
		lResult = prvRunStep(0xFC81U, COFFEE3_DEVICE_ROBOT,
			COFFEE3_ACTION_ROBOT_PUT_OUTPUT, 1U, 0U,
			COFFEE3_WORKFLOW_ROBOT_MOTION_MS);
	}
	if (lResult == 0) {
		g_xCoffee3WorkflowStatus.ucPositionUncertain = 0U;
		s_usActiveDevices &= (uint16_t)~(1U << COFFEE3_DEVICE_ROBOT);
		taskENTER_CRITICAL();
		s_ucStoragePickupConfirmStorage = (uint8_t)usStorage;
		s_usStoragePickupConfirmOrderId = usStoredOrderId;
		s_xStoragePickupConfirmStart = xTaskGetTickCount();
		s_ucStoragePickupConfirmOverdueLogged = 0U;
		s_ucStoragePickupActive = 0U;
		taskEXIT_CRITICAL();
		vCoffee3ServerFinishRequest(1U);
		(void)xCoffee3LogPrintfOrder(COFFEE3_LOG_LEVEL_INFO,
			COFFEE3_LOG_SOURCE_WORKFLOW, s_usDetachedPickupLogOrder,
			"Pickup Robot released: storage=%u outlet=1; wait X3=1 and source=0",
			(unsigned int)usStorage);
		s_usDetachedPickupLogOrder = 0U;
		return 0;
	}

	prvPublishOutputForOrder(1U, 3U, s_usDetachedPickupLogOrder);
	vCoffee3ServerFinishRequest(1U);
	s_ucStoragePickupActive = 0U;
	s_usDetachedPickupLogOrder = 0U;
	return lResult;
}

/*-----------------------------------------------------------*/
/**
  * @brief  独占执行暂存位取杯、放入出杯口及源位清空确认。
  * @param[in] usStorage 暂存位编号，范围为 1 至 2。
  * @retval 0 取杯完成，或请求在运动前因条件不满足被拒绝。
  * @retval 负数 机械臂、IO、取消或物理确认失败。
  */
static int32_t prvRunStoragePickup(uint16_t usStorage)
{
	Coffee3IoState_t xIo; /*!< 当前读取的输入与输出状态快照。 */
	int32_t lResult; /*!< 当前子步骤的执行结果或错误码。 */
	uint16_t usStoredOrderId; /*!< 储位杯体关联的订单号。 */
	uint16_t usStorageBit; /*!< 来源储位在状态掩码中的位。 */
	uint16_t usStatusMask; /*!< 当前储位物理状态掩码。 */
	uint8_t ucStorageClearOverdueLogged; /*!< 储位清空逾期日志是否已经记录。 */
	TickType_t xStorageClearStart; /*!< 开始等待来源储位清空的系统节拍。 */
	const char *pcStorageIoName; /*!< 来源储位传感器的日志名称。 */

	/* 步骤 1：校验暂存位编号和主机订单门控，运动前拒绝不合法请求。 */
	if ((usStorage < 1U) || (usStorage > 2U)) {
		prvRejectStoragePickup(usStorage, "storage unsupported", 0U);
		return 0;
	}
	usStorageBit = (uint16_t)(1U << (usStorage - 1U));
	/* 机械臂运动前再次检查主机订单门控，避免与新订单请求交叉。 */
	if (usCoffee3ServerGetCommandRegister(
		COFFEE3_REG_ORDER_PRESENT) != 0U) {
		prvRejectStoragePickup(usStorage, "0x0007 order request not cleared", 0U);
		return 0;
	}
	if ((usCoffee3ConfigStorageMask() & usStorageBit &
		COFFEE3_STORAGE_INSTALLED_MASK) == 0U) {
		prvRejectStoragePickup(usStorage, "storage disabled", 0U);
		return 0;
	}
	/* 步骤 2：确认唯一出杯口空闲，输入模块有效，且源暂存位配置启用、
	 * 物理有杯并在状态掩码中置位。 */
	lResult = prvCheckOutputEmpty(1U);
	if (lResult != 0) {
		prvRejectStoragePickup(usStorage, "outlet unavailable", 0U);
		return 0;
	}
	vCoffee3IoGetSnapshot(&xIo);
	usStatusMask = prvStorageStatusMask(&xIo);
	if (prvIoValid(&xIo) == 0U) {
		prvRejectStoragePickup(usStorage, "IoInput stale", usStatusMask);
		return 0;
	}
	if (xIo.xInput.aucMB1XPin[usStorage - 1U] == 0U) {
		prvRejectStoragePickup(usStorage, "cup not detected", usStatusMask);
		return 0;
	}
	if ((usStatusMask & usStorageBit) == 0U) {
		prvRejectStoragePickup(usStorage, "0x1027 storage bit not set",
			usStatusMask);
		return 0;
	}
	/* 步骤 3：主机取杯请求与物理杯体传感器共同构成权威条件；RAM 中的订单号
	 * 仅用于关联日志，重启后允许丢失。 */
	usStoredOrderId = s_ausStoredOrderId[usStorage - 1U];
	g_xCoffee3WorkflowStatus.usCurrentOrderId = (usStoredOrderId != 0U) ?
		usStoredOrderId : COFFEE3_LOG_ORDER_SYSTEM;
	(void)xCoffee3LogPrintfOrder(COFFEE3_LOG_LEVEL_INFO,
		COFFEE3_LOG_SOURCE_WORKFLOW,
		g_xCoffee3WorkflowStatus.usCurrentOrderId,
		"Pickup start: device=Robot storage=%u outlet=1 tracked_order=%u status_0x1027=0x%04X",
		(unsigned int)usStorage, (unsigned int)usStoredOrderId,
		(unsigned int)usStatusMask);
	vCoffee3ServerSelectStorage(usStorage);
	prvPublishOutput(1U, 2U);
	/* 步骤 4：机械臂依次从暂存位取杯并放入出杯口，随后确认 X3 有杯。 */
	lResult = prvRunStep(800U, COFFEE3_DEVICE_ROBOT,
		COFFEE3_ACTION_ROBOT_TAKE_STORAGE, usStorage, 0U,
		COFFEE3_WORKFLOW_ROBOT_MOTION_MS);
	if (lResult == 0) {
		lResult = prvRunStep(810U, COFFEE3_DEVICE_ROBOT,
			COFFEE3_ACTION_ROBOT_PUT_OUTPUT, 1U, 0U,
			COFFEE3_WORKFLOW_ROBOT_MOTION_MS);
	}
	if (lResult == 0) {
		lResult = prvWaitBusinessCondition(815U, COFFEE3_CONDITION_OUTPUT_1);
	}
	/* 步骤 5：持续等待源暂存位传感器清零；超时只记录一次告警并继续
	 * 等待，取消或 IO 失效才终止流程。 */
	ucStorageClearOverdueLogged = 0U;
	xStorageClearStart = xTaskGetTickCount();
	pcStorageIoName = (usStorage == 1U) ?
		"X1_FINISHED_FRONT_CUP" : "X2_FINISHED_REAR_CUP";
	while (lResult == 0) {
		if (g_xCoffee3WorkflowStatus.ucCancelRequested != 0U) {
			lResult = COFFEE3_WORKFLOW_ERROR_CANCELED;
			break;
		}
		lResult = prvRefreshDeviceQuiet(816U, COFFEE3_DEVICE_IO_INPUT);
		vCoffee3IoGetSnapshot(&xIo);
		if ((lResult != 0) || (prvIoValid(&xIo) == 0U)) {
			lResult = (lResult != 0) ? lResult : COFFEE3_WORKFLOW_ERROR_IO;
			break;
		}
		if (xIo.xInput.aucMB1XPin[usStorage - 1U] == 0U) {
			break;
		}
		if ((ucStorageClearOverdueLogged == 0U) &&
			((xTaskGetTickCount() - xStorageClearStart) >=
			 pdMS_TO_TICKS(COFFEE3_WORKFLOW_DEFAULT_TIMEOUT_MS))) {
			ucStorageClearOverdueLogged = 1U;
			(void)xCoffee3LogPrintfOrder(COFFEE3_LOG_LEVEL_WARNING,
				COFFEE3_LOG_SOURCE_WORKFLOW,
				g_xCoffee3WorkflowStatus.usCurrentOrderId,
				"Physical confirmation overdue: device=IoInput io=%s expected=0; waiting",
				pcStorageIoName);
		}
		prvDelayWithServices(500U);
	}
	if (lResult != 0) {
		prvPublishOutput(1U, 3U);
		return lResult;
	}
	/* 步骤 6：源位清空后解除订单关联，发布出杯完成并打开出杯门。 */
	s_ausStoredOrderId[usStorage - 1U] = 0U;
	prvPublishOutput(1U, 5U);
	s_ucOutletPhase = 1U;
	s_ucPickupPending = 0U;
	s_ucEmptyTiming = 0U;
	prvStartDoor(2U);
	g_xCoffee3WorkflowStatus.ucPositionUncertain = 0U;
	return lResult;
}
