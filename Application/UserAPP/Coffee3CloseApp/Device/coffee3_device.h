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
	COFFEE3_ACTION_REFRESH = 1, COFFEE3_ACTION_CANCEL = 2,
	COFFEE3_ACTION_RESET = 3, COFFEE3_ACTION_ROBOT_START = 100,
	COFFEE3_ACTION_ROBOT_STOP = 101, COFFEE3_ACTION_ROBOT_ENABLE = 102,
	COFFEE3_ACTION_ROBOT_CLEAR_ALARM = 103, COFFEE3_ACTION_ROBOT_PAUSE = 104,
	COFFEE3_ACTION_ROBOT_DISABLE = 105, COFFEE3_ACTION_ROBOT_ENTER_DRAG = 106,
	COFFEE3_ACTION_ROBOT_EXIT_DRAG = 107, COFFEE3_ACTION_ROBOT_AUTO_MODE = 108,
	COFFEE3_ACTION_ROBOT_MANUAL_MODE = 109, COFFEE3_ACTION_ROBOT_HOME = 110,
	COFFEE3_ACTION_ROBOT_TAKE_HOT_CUP = 111,
	COFFEE3_ACTION_ROBOT_TAKE_COLD_CUP = 112,
	COFFEE3_ACTION_ROBOT_TO_COFFEE = 113, COFFEE3_ACTION_ROBOT_TO_ICE = 114,
	COFFEE3_ACTION_ROBOT_TO_LID = 115, COFFEE3_ACTION_ROBOT_TAKE_LID = 116,
	COFFEE3_ACTION_ROBOT_COVER_LID = 117, COFFEE3_ACTION_ROBOT_PUT_OUTPUT = 118,
	COFFEE3_ACTION_ROBOT_PUT_STORAGE = 119,
	COFFEE3_ACTION_ROBOT_TO_PRINTER = 120,
	COFFEE3_ACTION_ROBOT_TAKE_OUTPUT_1 = 121,
	COFFEE3_ACTION_ROBOT_TAKE_OUTPUT_2 = 122,
	COFFEE3_ACTION_ROBOT_TAKE_COFFEE = 123,
	COFFEE3_ACTION_ROBOT_TAKE_STORAGE = 124,
	COFFEE3_ACTION_ROBOT_START_SIGNAL = 125,
	COFFEE3_ACTION_ROBOT_TO_FRUIT_SYRUP = 126,
	COFFEE3_ACTION_COFFEE_MAKE = 200, COFFEE3_ACTION_COFFEE_PAUSE = 201,
	COFFEE3_ACTION_COFFEE_RESUME = 202, COFFEE3_ACTION_COFFEE_CLEAN = 203,
	COFFEE3_ACTION_CUP_DROP_1 = 300, COFFEE3_ACTION_CUP_DROP_2 = 301,
	COFFEE3_ACTION_LID_DROP_1 = 310, COFFEE3_ACTION_LID_DROP_2 = 311,
	COFFEE3_ACTION_SYRUP_DISPENSE = 320,
	COFFEE3_ACTION_SYRUP_CLEAN = 321,
	COFFEE3_ACTION_SYRUP_SET_REMAINING = 322,
	COFFEE3_ACTION_ICE_SET_VALVE = 330, COFFEE3_ACTION_SCALE_TARE = 340,
	COFFEE3_ACTION_SCALE_CLEAR_TARE = 341, COFFEE3_ACTION_SCALE_ZERO = 342,
	COFFEE3_ACTION_IO_WRITE = 350,
	COFFEE3_ACTION_IO_WRITE_MASK = 351
} Coffee3Action_e;

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
	uint32_t ulCommandId;
	uint32_t ulOrderId;
	uint32_t ulOrderEpoch;
	uint32_t ulTimeoutMs;
	uint16_t usStepId;
	uint16_t usAction;
	uint16_t ausParameter[4];
	uint8_t ucDeviceId;
	uint8_t ucSource;
	uint8_t ucRetryLimit;
	uint8_t ucFlags;
} Coffee3Command_t;

typedef char Coffee3CommandSizeMustBe32[
	(sizeof(Coffee3Command_t) == 32U) ? 1 : -1];

/** @brief Bind one logical device to one task route and native protocol. */
typedef struct {
	Coffee3DeviceId_e xDeviceId;
	uint8_t ucRouteId;
	uint8_t ucUnitId;
	uint16_t usMinimumIntervalMs;
	uint8_t ucCategory;
	uint8_t ucRole;
	uint8_t ucDriverId;
	uint8_t ucProtocolId;
	const char *pcName;
} Coffee3DeviceBinding_t;

/** @brief Store globally observable status for one logical device. */
typedef struct {
	uint32_t ulLastCommandId;
	uint32_t ulLastOrderEpoch;
	uint32_t ulLastSuccessTick;
	uint32_t ulCommandCount;
	uint32_t ulErrorCount;
	int32_t lLastResult;
	uint16_t usLastAction;
	uint8_t ucOnline;
	uint8_t ucBusy;
	uint8_t ucReady;
	uint8_t ucRecovering;
	uint8_t ucRobotPhase;
	uint8_t ucRobotAccepted;
	uint8_t ucTerminalValid;
	uint8_t ucPreviousTerminalValid;
	uint32_t ulTerminalCommandId;
	uint32_t ulTerminalOrderEpoch;
	int32_t lTerminalResult;
	uint16_t usTerminalAction;
	uint8_t ucTerminalTimedOut;
	uint32_t ulPreviousTerminalCommandId;
	uint32_t ulPreviousTerminalOrderEpoch;
	int32_t lPreviousTerminalResult;
	uint16_t usPreviousTerminalAction;
	uint8_t ucPreviousTerminalTimedOut;
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
