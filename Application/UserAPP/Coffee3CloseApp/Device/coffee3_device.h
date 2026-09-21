/**
  * @file      coffee3_device.h
  * @brief     Define Coffee3 device bindings, commands, events, and status.
  * @author    WHong
  * @date      2026-07-30
  */

#ifndef COFFEE3_DEVICE_H
#define COFFEE3_DEVICE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "FreeRTOS.h"
#include "device_library.h"
#include "event_groups.h"
#include "queue.h"

/** @brief Identify every independently monitored Coffee3 device. */
typedef enum {
	COFFEE3_DEVICE_NONE = 0,
	COFFEE3_DEVICE_ROBOT = 1,
	COFFEE3_DEVICE_COFFEE_MACHINE = 2,
	COFFEE3_DEVICE_CUP_MACHINE = 3,
	COFFEE3_DEVICE_SYRUP_MACHINE = 4,
	COFFEE3_DEVICE_LID_MACHINE = 5,
	COFFEE3_DEVICE_ICE_MACHINE = 6,
	COFFEE3_DEVICE_SCALE = 7,
	COFFEE3_DEVICE_POWER_METER = 8,
	COFFEE3_DEVICE_IO_INPUT = 9,
	COFFEE3_DEVICE_IO_OUTPUT = 10,
	COFFEE3_DEVICE_COUNT = 11
} Coffee3DeviceId_e;

/** @brief Identify the producer of a Coffee3 device command. */
typedef enum {
	COFFEE3_COMMAND_SOURCE_WORKFLOW = 0,
	COFFEE3_COMMAND_SOURCE_SERVER = 1,
	COFFEE3_COMMAND_SOURCE_MAINTENANCE = 2
} Coffee3CommandSource_e;

/** @brief Mark an interactive debug command exempt from system ownership gates. */
#define COFFEE3_COMMAND_FLAG_DEBUG 0x04U

/** @brief Expose the Robot owner transaction phase to workflow timing. */
typedef enum {
	COFFEE3_ROBOT_PHASE_IDLE = 0,
	COFFEE3_ROBOT_PHASE_PREPARING = 1,
	COFFEE3_ROBOT_PHASE_WAIT_ACCEPT = 2,
	COFFEE3_ROBOT_PHASE_MOVING = 3,
	COFFEE3_ROBOT_PHASE_CLEAR_RESULT = 4,
	COFFEE3_ROBOT_PHASE_RECOVERING = 5
} Coffee3RobotPhase_e;

/** @brief Identify Coffee3 actions consumed by the private owners. */
typedef enum {
	/* 基础操作 */
	COFFEE3_ACTION_REFRESH = 1,          // 刷新
	COFFEE3_ACTION_CANCEL = 2,           // 取消
	COFFEE3_ACTION_RESET = 3,            // 重置/复位

	/* 机器人（机械臂）控制：100~129 */
	COFFEE3_ACTION_ROBOT_START = 100,        // 机器人启动
	COFFEE3_ACTION_ROBOT_STOP = 101,         // 机器人停止
	COFFEE3_ACTION_ROBOT_ENABLE = 102,       // 机器人使能
	COFFEE3_ACTION_ROBOT_CLEAR_ALARM = 103,  // 清除机器人报警
	COFFEE3_ACTION_ROBOT_PAUSE = 104,        // 机器人暂停
	COFFEE3_ACTION_ROBOT_DISABLE = 105,      // 机器人去使能（禁用）
	COFFEE3_ACTION_ROBOT_ENTER_DRAG = 106,   // 进入拖动模式（手动拖拽示教）
	COFFEE3_ACTION_ROBOT_EXIT_DRAG = 107,    // 退出拖动模式
	COFFEE3_ACTION_ROBOT_AUTO_MODE = 108,    // 机器人自动模式
	COFFEE3_ACTION_ROBOT_MANUAL_MODE = 109,  // 机器人手动模式
	COFFEE3_ACTION_ROBOT_HOME = 110,         // 机器人回原点（归零）
	COFFEE3_ACTION_ROBOT_TAKE_HOT_CUP = 111, // 取热杯
	COFFEE3_ACTION_ROBOT_TAKE_COLD_CUP = 112,// 取冷杯
	COFFEE3_ACTION_ROBOT_TO_COFFEE = 113,    // 移动到咖啡机位置
	COFFEE3_ACTION_ROBOT_TO_ICE = 114,       // 移动到出冰站位置
	COFFEE3_ACTION_ROBOT_TO_LID = 115,       // 移动到杯盖机位置
	COFFEE3_ACTION_ROBOT_TAKE_LID = 116,     // 取杯盖
	COFFEE3_ACTION_ROBOT_COVER_LID = 117,    // 盖杯盖
	COFFEE3_ACTION_ROBOT_PUT_OUTPUT = 118,   // 放到出杯口（取餐口）
	COFFEE3_ACTION_ROBOT_PUT_STORAGE = 119,  // 放到储藏位/存放位
	COFFEE3_ACTION_ROBOT_TO_PRINTER = 120,   // 移动到打印机位置
	COFFEE3_ACTION_ROBOT_TAKE_OUTPUT_1 = 121,// 从取餐口1取杯
	COFFEE3_ACTION_ROBOT_TAKE_OUTPUT_2 = 122,// 从取餐口2取杯
	COFFEE3_ACTION_ROBOT_TAKE_COFFEE = 123,  // 取咖啡（接咖啡）
	COFFEE3_ACTION_ROBOT_TAKE_STORAGE = 124, // 从储藏位取杯
	COFFEE3_ACTION_ROBOT_START_SIGNAL = 125, // 机器人启动信号
	COFFEE3_ACTION_ROBOT_TO_FRUIT_SYRUP = 126,   // 移动到果糖浆机位置
	COFFEE3_ACTION_ROBOT_PREPARE_ORDER = 127,    // 机器人准备订单（预处理）

	/* 咖啡机控制：200~203 */
	COFFEE3_ACTION_COFFEE_MAKE = 200,     // 制作咖啡
	COFFEE3_ACTION_COFFEE_PAUSE = 201,    // 咖啡制作暂停
	COFFEE3_ACTION_COFFEE_RESUME = 202,   // 咖啡制作恢复（继续）
	COFFEE3_ACTION_COFFEE_CLEAN = 203,    // 咖啡机清洗

	/* 出杯/出盖/糖浆：300~322 */
	COFFEE3_ACTION_CUP_DROP_1 = 300,      // 出杯口1落杯
	COFFEE3_ACTION_CUP_DROP_2 = 301,      // 出杯口2落杯
	COFFEE3_ACTION_LID_DROP_1 = 310,      // 杯盖口1落盖
	COFFEE3_ACTION_LID_DROP_2 = 311,      // 杯盖口2落盖
	COFFEE3_ACTION_SYRUP_DISPENSE = 320,  // 糖浆出液（分配糖浆）
	COFFEE3_ACTION_SYRUP_CLEAN = 321,     // 糖浆管路清洗
	COFFEE3_ACTION_SYRUP_SET_REMAINING = 322, // 设置糖浆剩余量

	/* 出冰/称重/IO：330~351 */
	COFFEE3_ACTION_ICE_SET_VALVE = 330,   // 设置出冰阀（开/关冰阀）
	COFFEE3_ACTION_SCALE_TARE = 340,      // 电子秤去皮（扣重）
	COFFEE3_ACTION_SCALE_CLEAR_TARE = 341,// 电子秤清除去皮
	COFFEE3_ACTION_SCALE_ZERO = 342,      // 电子秤置零（校零）
	COFFEE3_ACTION_IO_WRITE = 350,        // IO 写操作（写输出信号）
	COFFEE3_ACTION_IO_WRITE_MASK = 351    // IO 掩码写操作（按位掩码写入）
} Coffee3Action_e;  // 咖啡机3 动作枚举类型


/** @brief Per-device EventGroup bits with identical meaning for all devices. */
#define COFFEE3_DEVICE_EVENT_ONLINE          (1UL << 0)
#define COFFEE3_DEVICE_EVENT_READY           (1UL << 1)
#define COFFEE3_DEVICE_EVENT_BUSY            (1UL << 2)
#define COFFEE3_DEVICE_EVENT_COMMAND_DONE    (1UL << 3)
#define COFFEE3_DEVICE_EVENT_COMMAND_FAILED  (1UL << 4)
#define COFFEE3_DEVICE_EVENT_TIMEOUT         (1UL << 5)
#define COFFEE3_DEVICE_EVENT_COMM_FAULT      (1UL << 6)
#define COFFEE3_DEVICE_EVENT_DEVICE_FAULT    (1UL << 7)
#define COFFEE3_DEVICE_EVENT_CANCELED        (1UL << 8)
#define COFFEE3_DEVICE_EVENT_DATA_UPDATED    (1UL << 9)
#define COFFEE3_DEVICE_EVENT_RECOVERING      (1UL << 10)

/** @brief Normalized result used when an order epoch is canceled. */
#define COFFEE3_COMMAND_RESULT_CANCELED       (-9)
#define COFFEE3_COMMAND_RESULT_SUPERSEDED     (-10)
#define COFFEE3_COMMAND_FLAG_SAFETY_STOP       0x01U
#define COFFEE3_COMMAND_FLAG_MANUAL_RESERVED   0x02U

/** @brief Terminal command bits waited by workflow steps. */
#define COFFEE3_DEVICE_EVENT_TERMINAL         \
	(COFFEE3_DEVICE_EVENT_COMMAND_DONE |       \
	 COFFEE3_DEVICE_EVENT_COMMAND_FAILED |     \
	 COFFEE3_DEVICE_EVENT_TIMEOUT |            \
	 COFFEE3_DEVICE_EVENT_CANCELED)

/** @brief Store one Coffee3 command copied through static owner queues. */
typedef struct {
	uint32_t ulCommandId; /*!< Submission sequence; pair with epoch to identify completion. */
	uint32_t ulOrderId; /*!< Log correlation id; not a queue priority or slave address. */
	uint32_t ulOrderEpoch; /*!< Cancellation generation; zero denotes non-order work. */
	uint32_t ulTimeoutMs; /*!< Owner transaction budget in milliseconds, not queue wait ticks. */
	uint16_t usStepId; /*!< Workflow diagnostic step, preserved through the owner queue. */
	uint16_t usAction; /*!< Product action; owner translates it to a device-native request. */
	uint16_t ausParameter[4]; /*!< Four inline action parameters, copied with the queue item. */
	uint8_t ucDeviceId; /*!< Logical device id resolved through the immutable binding table. */
	uint8_t ucSource; /*!< Producer category used by admission, cancellation and logging. */
	uint8_t ucRetryLimit; /*!< Additional owner attempts; non-idempotent coffee writes override it. */
	uint8_t ucFlags; /*!< Safety-stop, manual-reservation and debug policy bits. */
} Coffee3Command_t;

typedef char Coffee3CommandSizeMustBe32[
	(sizeof(Coffee3Command_t) == 32U) ? 1 : -1];

/** @brief Bind one logical device to one task route and native protocol. */
typedef struct {
	Coffee3DeviceId_e xDeviceId; /*!< Product device key; differs from the Modbus slave unit. */
	uint8_t ucRouteId; /*!< Queue owner: Robot route 0 or UART bus routes 2 through 5. */
	uint8_t ucUnitId; /*!< Protocol slave address on the selected physical route. */
	uint16_t usMinimumIntervalMs; /*!< Minimum gap between owner transactions, in milliseconds. */
	uint8_t ucCategory; /*!< Public device category, independent of installed driver model. */
	uint8_t ucRole; /*!< Device role; cup and lid may share a controller with different roles. */
	uint8_t ucDriverId; /*!< Selected public driver model for this product binding. */
	uint8_t ucProtocolId; /*!< Wire protocol selected for the bound device or bus. */
	const char *pcName; /*!< Borrowed static diagnostic name; storage outlives owner tasks. */
} Coffee3DeviceBinding_t;

/** @brief Store globally observable status for one logical device. */
typedef struct {
	uint32_t ulLastCommandId; /*!< Most recently started command, not necessarily a terminal result. */
	uint32_t ulLastOrderEpoch; /*!< Generation of the most recently started command. */
	uint32_t ulLastSuccessTick; /*!< RTOS tick of last success; not a millisecond counter. */
	uint32_t ulCommandCount; /*!< Cumulative commands observed by this owner/status object. */
	uint32_t ulErrorCount; /*!< Cumulative failures; latest cause is retained separately. */
	int32_t lLastResult; /*!< Latest native owner result; zero alone does not prove readiness. */
	uint16_t usLastAction; /*!< Action associated with the most recently started command. */
	uint8_t ucOnline; /*!< Communication availability; independent of control readiness. */
	uint8_t ucBusy; /*!< Owner has published a started command without its terminal result. */
	uint8_t ucReady; /*!< Module-specific readiness; consult the producer before issuing work. */
	uint8_t ucRecovering; /*!< Current command is retained while its link is being recovered. */
	uint8_t ucRobotPhase; /*!< Robot transaction phase used by workflow timeout decisions. */
	uint8_t ucRobotAccepted; /*!< Acceptance edge for the current robot command. */
	uint8_t ucTerminalValid; /*!< Latest terminal fields contain a published result. */
	uint8_t ucPreviousTerminalValid; /*!< Previous terminal fields contain a published result. */
	uint32_t ulTerminalCommandId; /*!< Sequence key of the latest retained terminal result. */
	uint32_t ulTerminalOrderEpoch; /*!< Generation key of the latest retained terminal result. */
	int32_t lTerminalResult; /*!< Uncollapsed owner result for the latest completed command. */
	uint16_t usTerminalAction; /*!< Completed action; successful CANCEL maps to canceled event. */
	uint8_t ucTerminalTimedOut; /*!< Explicit timeout classification of the latest result. */
	uint32_t ulPreviousTerminalCommandId; /*!< Sequence key of the preceding retained completion. */
	uint32_t ulPreviousTerminalOrderEpoch; /*!< Generation key of the preceding completion. */
	int32_t lPreviousTerminalResult; /*!< Original owner result from the preceding completion. */
	uint16_t usPreviousTerminalAction; /*!< Action associated with the preceding completion. */
	uint8_t ucPreviousTerminalTimedOut; /*!< Timeout classification of the preceding completion. */
} Coffee3DeviceStatus_t;

/** @brief Public status array indexed by Coffee3DeviceId_e. */
extern Coffee3DeviceStatus_t
	g_axCoffee3DeviceStatus[COFFEE3_DEVICE_COUNT];

/**
  * @brief Create the independent static EventGroup for every device.
  * @retval pdPASS All device event groups are available.
  * @retval pdFAIL Initialization failed or ran with invalid resources.
  */
BaseType_t xCoffee3DeviceInitialize(void);

/**
  * @brief Register the command queue owned by one task route.
  * @param[in] ucRouteId Zero for Robot TCP, two through five for RTU buses.
  * @param[in] xQueue Persistent queue handle.
  */
void vCoffee3DeviceRegisterRoute(uint8_t ucRouteId, QueueHandle_t xQueue);

/**
  * @brief Return the immutable binding for one device.
  * @param[in] xDeviceId Logical device identifier.
  * @return Binding pointer, or NULL for an invalid device.
  */
const Coffee3DeviceBinding_t *pxCoffee3DeviceGetBinding(
	Coffee3DeviceId_e xDeviceId);

/**
  * @brief Submit one command to the queue selected by its device binding.
  * @param[in,out] pxCommand Command copied into a bounded route queue.
  * @param[in] xWaitTicks Maximum queue wait.
  * @retval pdPASS The route queue accepted the command.
  * @retval pdFAIL The command, device, route, or queue was invalid/full.
  */
BaseType_t xCoffee3CommandSubmit(Coffee3Command_t *pxCommand,
	TickType_t xWaitTicks);

/**
  * @brief Submit a safety command at the front of its owner queue.
  * @param[in,out] pxCommand Command copied into a bounded route queue.
  * @param[in] xWaitTicks Maximum queue wait.
  * @retval pdPASS The route queue accepted the urgent command.
  * @retval pdFAIL The command, device, route, or queue was invalid/full.
  */
BaseType_t xCoffee3CommandSubmitUrgent(Coffee3Command_t *pxCommand,
	TickType_t xWaitTicks);

/**
  * @brief Mark an order epoch as cooperatively canceled.
  * @param[in] ulOrderEpoch Nonzero workflow epoch to cancel.
  */
void vCoffee3OrderCancelRequest(uint32_t ulOrderEpoch);

/**
  * @brief Test whether a workflow command belongs to the canceled epoch.
  * @param[in] pxCommand Command currently owned by a device task.
  * @retval 1 The command should stop at its next cooperative poll point.
  * @retval 0 The command remains valid.
  */
uint8_t ucCoffee3CommandIsCanceled(const Coffee3Command_t *pxCommand);

/**
  * @brief Publish command start and clear prior terminal state.
  * @param[in] pxCommand Command being executed by its owning task.
  */
void vCoffee3DeviceCommandStarted(const Coffee3Command_t *pxCommand);

/**
  * @brief Publish command completion and normalized result.
  * @param[in] pxCommand Completed command.
  * @param[in] lResult Zero for success, negative for failure.
  * @param[in] ucTimedOut Nonzero classifies the failure as timeout.
  */
void vCoffee3DeviceCommandCompleted(const Coffee3Command_t *pxCommand,
	int32_t lResult, uint8_t ucTimedOut);

/**
  * @brief Set or clear the device online state and communication fault bit.
  * @param[in] xDeviceId Logical device identifier.
  * @param[in] ucOnline Nonzero when communication is available.
  */
void vCoffee3DeviceSetOnline(Coffee3DeviceId_e xDeviceId,
	uint8_t ucOnline);

/**
  * @brief Publish strict device-control readiness independently of link state.
  * @param[in] xDeviceId Logical device identifier.
  * @param[in] ucReady Nonzero only when the device is safe for commands.
  */
void vCoffee3DeviceSetReady(Coffee3DeviceId_e xDeviceId,
	uint8_t ucReady);

/**
  * @brief Publish that the current command is being held for link recovery.
  * @param[in] xDeviceId Logical device identifier.
  * @param[in] ucRecovering Nonzero while the command remains BUSY.
  */
void vCoffee3DeviceSetRecovering(Coffee3DeviceId_e xDeviceId,
	uint8_t ucRecovering);

/** @brief Publish Robot transaction phase for workflow timing and diagnostics. */
void vCoffee3DeviceSetRobotPhase(Coffee3RobotPhase_e xPhase);

/** @brief Publish the Robot command acceptance edge. */
void vCoffee3DeviceSetRobotAccepted(uint8_t ucAccepted);

/**
  * @brief Read the independent EventGroup bits for one device.
  * @param[in] xDeviceId Logical device identifier.
  * @return Current EventGroup bits, or zero for an invalid device.
  */
EventBits_t xCoffee3DeviceGetEvents(Coffee3DeviceId_e xDeviceId);

/**
  * @brief Wait for the terminal event belonging to a specific command.
  * @param[in] xDeviceId Logical device identifier.
  * @param[in] ulOrderEpoch Expected order generation.
  * @param[in] ulCommandId Expected command sequence.
  * @param[in] xWaitTicks Maximum wait.
  * @return Terminal device event bits, or zero on timeout/stale completion.
  */
EventBits_t xCoffee3DeviceWaitCommand(Coffee3DeviceId_e xDeviceId,
	uint32_t ulOrderEpoch, uint32_t ulCommandId, TickType_t xWaitTicks);

/** @brief Read an exact terminal result retained in static command history. */
int32_t lCoffee3DeviceGetTerminalResult(Coffee3DeviceId_e xDeviceId,
	uint32_t ulOrderEpoch, uint32_t ulCommandId, uint8_t *pucValid);

#ifdef __cplusplus
}
#endif

#endif /* COFFEE3_DEVICE_H */
