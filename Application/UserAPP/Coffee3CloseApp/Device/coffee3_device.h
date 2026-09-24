/**
  * @file      coffee3_device.h
  * @brief     定义 Coffee3 设备绑定、命令、事件与公共状态。
  * @author    WHong
  * @date      2026-09-24
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

/** @brief 标识每个独立监测的 Coffee3 逻辑设备。 */
typedef enum {
	COFFEE3_DEVICE_NONE = 0, /*!< 无设备占位值。 */
	COFFEE3_DEVICE_ROBOT = 1, /*!< 机器人。 */
	COFFEE3_DEVICE_COFFEE_MACHINE = 2, /*!< 咖啡机。 */
	COFFEE3_DEVICE_CUP_MACHINE = 3, /*!< 落杯机。 */
	COFFEE3_DEVICE_SYRUP_MACHINE = 4, /*!< 糖浆机。 */
	COFFEE3_DEVICE_LID_MACHINE = 5, /*!< 落盖机。 */
	COFFEE3_DEVICE_ICE_MACHINE = 6, /*!< 制冰机。 */
	COFFEE3_DEVICE_SCALE = 7, /*!< 称重设备。 */
	COFFEE3_DEVICE_POWER_METER = 8, /*!< 电能表。 */
	COFFEE3_DEVICE_IO_INPUT = 9, /*!< 外部输入模块。 */
	COFFEE3_DEVICE_IO_OUTPUT = 10, /*!< 外部输出模块。 */
	COFFEE3_DEVICE_COUNT = 11 /*!< 设备编号数量边界。 */
} Coffee3DeviceId_e;

/** @brief 标识 Coffee3 设备命令的生产者。 */
typedef enum {
	COFFEE3_COMMAND_SOURCE_WORKFLOW = 0, /*!< 自动订单工作流。 */
	COFFEE3_COMMAND_SOURCE_SERVER = 1, /*!< 主机人工调试接口。 */
	COFFEE3_COMMAND_SOURCE_MAINTENANCE = 2 /*!< 维护流程。 */
} Coffee3CommandSource_e;

/** @brief 标记交互调试命令，用于选择人工控制策略。 */
#define COFFEE3_COMMAND_FLAG_DEBUG 0x04U

/** @brief 向工作流暴露机器人拥有者的事务阶段。 */
typedef enum {
	COFFEE3_ROBOT_PHASE_IDLE = 0, /*!< 没有活动事务。 */
	COFFEE3_ROBOT_PHASE_PREPARING = 1, /*!< 正在准备动作寄存器。 */
	COFFEE3_ROBOT_PHASE_WAIT_ACCEPT = 2, /*!< 等待机器人接受命令。 */
	COFFEE3_ROBOT_PHASE_MOVING = 3, /*!< 机器人正在执行动作。 */
	COFFEE3_ROBOT_PHASE_CLEAR_RESULT = 4, /*!< 正在清除结果位。 */
	COFFEE3_ROBOT_PHASE_RECOVERING = 5 /*!< 链路恢复中。 */
} Coffee3RobotPhase_e;

/** @brief 标识各私有设备拥有者消费的 Coffee3 产品动作。 */
typedef enum {
	/* 基础操作。 */
	COFFEE3_ACTION_REFRESH = 1, /*!< 刷新设备状态。 */
	COFFEE3_ACTION_CANCEL = 2, /*!< 取消当前动作。 */
	COFFEE3_ACTION_RESET = 3, /*!< 复位设备。 */

	/* 机器人控制：100 至 129。 */
	COFFEE3_ACTION_ROBOT_START = 100, /*!< 启动机器人。 */
	COFFEE3_ACTION_ROBOT_STOP = 101, /*!< 停止机器人。 */
	COFFEE3_ACTION_ROBOT_ENABLE = 102, /*!< 使能机器人。 */
	COFFEE3_ACTION_ROBOT_CLEAR_ALARM = 103, /*!< 清除机器人报警。 */
	COFFEE3_ACTION_ROBOT_PAUSE = 104, /*!< 暂停机器人。 */
	COFFEE3_ACTION_ROBOT_DISABLE = 105, /*!< 去使能机器人。 */
	COFFEE3_ACTION_ROBOT_ENTER_DRAG = 106, /*!< 进入拖动示教模式。 */
	COFFEE3_ACTION_ROBOT_EXIT_DRAG = 107, /*!< 退出拖动示教模式。 */
	COFFEE3_ACTION_ROBOT_AUTO_MODE = 108, /*!< 切换自动模式。 */
	COFFEE3_ACTION_ROBOT_MANUAL_MODE = 109, /*!< 切换手动模式。 */
	COFFEE3_ACTION_ROBOT_HOME = 110, /*!< 返回原点。 */
	COFFEE3_ACTION_ROBOT_TAKE_HOT_CUP = 111, /*!< 取热杯。 */
	COFFEE3_ACTION_ROBOT_TAKE_COLD_CUP = 112, /*!< 取冷杯。 */
	COFFEE3_ACTION_ROBOT_TO_COFFEE = 113, /*!< 移动到咖啡机。 */
	COFFEE3_ACTION_ROBOT_TO_ICE = 114, /*!< 移动到出冰站。 */
	COFFEE3_ACTION_ROBOT_TO_LID = 115, /*!< 移动到杯盖机。 */
	COFFEE3_ACTION_ROBOT_TAKE_LID = 116, /*!< 取杯盖。 */
	COFFEE3_ACTION_ROBOT_COVER_LID = 117, /*!< 盖杯盖。 */
	COFFEE3_ACTION_ROBOT_PUT_OUTPUT = 118, /*!< 把杯放到取餐口。 */
	COFFEE3_ACTION_ROBOT_PUT_STORAGE = 119, /*!< 把杯放到储位。 */
	COFFEE3_ACTION_ROBOT_TO_PRINTER = 120, /*!< 移动到打印机。 */
	COFFEE3_ACTION_ROBOT_TAKE_OUTPUT_1 = 121, /*!< 从取餐口一取杯。 */
	COFFEE3_ACTION_ROBOT_TAKE_OUTPUT_2 = 122, /*!< 从取餐口二取杯。 */
	COFFEE3_ACTION_ROBOT_TAKE_COFFEE = 123, /*!< 从咖啡机取杯。 */
	COFFEE3_ACTION_ROBOT_TAKE_STORAGE = 124, /*!< 从储位取杯。 */
	COFFEE3_ACTION_ROBOT_START_SIGNAL = 125, /*!< 触发机器人启动信号。 */
	COFFEE3_ACTION_ROBOT_TO_FRUIT_SYRUP = 126, /*!< 移动到果奶工位。 */
	COFFEE3_ACTION_ROBOT_PREPARE_ORDER = 127, /*!< 预处理订单动作。 */

	/* 咖啡机控制：200 至 203。 */
	COFFEE3_ACTION_COFFEE_MAKE = 200, /*!< 制作咖啡。 */
	COFFEE3_ACTION_COFFEE_PAUSE = 201, /*!< 暂停制作。 */
	COFFEE3_ACTION_COFFEE_RESUME = 202, /*!< 继续制作。 */
	COFFEE3_ACTION_COFFEE_CLEAN = 203, /*!< 清洗咖啡机。 */

	/* 出杯、出盖与糖浆控制：300 至 322。 */
	COFFEE3_ACTION_CUP_DROP_1 = 300, /*!< 一号槽落杯。 */
	COFFEE3_ACTION_CUP_DROP_2 = 301, /*!< 二号槽落杯。 */
	COFFEE3_ACTION_LID_DROP_1 = 310, /*!< 一号槽落盖。 */
	COFFEE3_ACTION_LID_DROP_2 = 311, /*!< 二号槽落盖。 */
	COFFEE3_ACTION_SYRUP_DISPENSE = 320, /*!< 定量输出糖浆。 */
	COFFEE3_ACTION_SYRUP_CLEAN = 321, /*!< 清洗糖浆管路。 */
	COFFEE3_ACTION_SYRUP_SET_REMAINING = 322, /*!< 设置糖浆剩余量。 */

	/* 出冰、称重与 IO 控制：330 至 351。 */
	COFFEE3_ACTION_ICE_SET_VALVE = 330, /*!< 开关出冰阀。 */
	COFFEE3_ACTION_SCALE_TARE = 340, /*!< 电子秤去皮。 */
	COFFEE3_ACTION_SCALE_CLEAR_TARE = 341, /*!< 清除电子秤去皮。 */
	COFFEE3_ACTION_SCALE_ZERO = 342, /*!< 电子秤置零。 */
	COFFEE3_ACTION_IO_WRITE = 350, /*!< 写单个 IO 输出。 */
	COFFEE3_ACTION_IO_WRITE_MASK = 351 /*!< 按掩码写 IO 输出。 */
} Coffee3Action_e;


/** @brief 所有设备共用相同语义的独立事件组位。 */
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

/** @brief 订单取消、命令替换与命令策略标志。 */
#define COFFEE3_COMMAND_RESULT_CANCELED       (-9)
#define COFFEE3_COMMAND_RESULT_SUPERSEDED     (-10)
#define COFFEE3_COMMAND_FLAG_SAFETY_STOP       0x01U
#define COFFEE3_COMMAND_FLAG_MANUAL_RESERVED   0x02U

/** @brief 工作流等待设备命令终态时使用的事件位集合。 */
#define COFFEE3_DEVICE_EVENT_TERMINAL         \
	(COFFEE3_DEVICE_EVENT_COMMAND_DONE |       \
	 COFFEE3_DEVICE_EVENT_COMMAND_FAILED |     \
	 COFFEE3_DEVICE_EVENT_TIMEOUT |            \
	 COFFEE3_DEVICE_EVENT_CANCELED)

/** @brief 保存通过静态拥有者队列按值复制的一条 Coffee3 命令。 */
typedef struct {
	uint32_t ulCommandId; /*!< 与归属代次共同标识终态的提交序号。 */
	uint32_t ulOrderId; /*!< 日志关联订单号，不表示优先级或从站地址。 */
	uint32_t ulOrderEpoch; /*!< 命令归属与取消代次；零表示无归属代次。 */
	uint32_t ulTimeoutMs; /*!< 拥有者事务预算，单位毫秒。 */
	uint16_t usStepId; /*!< 随队列保留的工作流诊断步骤号。 */
	uint16_t usAction; /*!< 由设备拥有者翻译的产品动作。 */
	uint16_t ausParameter[4]; /*!< 随队列项复制的四个动作参数。 */
	uint8_t ucDeviceId; /*!< 通过固定绑定表解析的逻辑设备编号。 */
	uint8_t ucSource; /*!< 用于准入、取消和日志的命令来源。 */
	uint8_t ucRetryLimit; /*!< 拥有者可执行的附加重试次数。 */
	uint8_t ucFlags; /*!< 安全停止、人工占用与调试策略位。 */
} Coffee3Command_t;

/** @brief 在编译期约束队列命令结构必须保持 32 字节。 */
typedef char Coffee3CommandSizeMustBe32[
	(sizeof(Coffee3Command_t) == 32U) ? 1 : -1];

/** @brief 把一个逻辑设备绑定到任务路由和原生协议。 */
typedef struct {
	Coffee3DeviceId_e xDeviceId; /*!< 产品逻辑设备键，不等同于 Modbus 从站号。 */
	uint8_t ucRouteId; /*!< 队列拥有者路由：机器人为零，RTU 为二至五。 */
	uint8_t ucUnitId; /*!< 所选物理路由上的协议从站地址。 */
	uint16_t usMinimumIntervalMs; /*!< 拥有者事务最小间隔，单位毫秒。 */
	uint8_t ucCategory; /*!< 与具体驱动型号无关的公共设备类别。 */
	uint8_t ucRole; /*!< 同一控制器中的设备角色。 */
	uint8_t ucDriverId; /*!< 产品绑定选择的公共驱动型号。 */
	uint8_t ucProtocolId; /*!< 绑定设备或总线使用的线协议。 */
	const char *pcName; /*!< 生命周期覆盖拥有者任务的静态诊断名称。 */
} Coffee3DeviceBinding_t;

/** @brief 保存一个逻辑设备的全局可观察运行状态。 */
typedef struct {
	uint32_t ulLastCommandId; /*!< 最近开始的命令序号，不一定已有终态。 */
	uint32_t ulLastOrderEpoch; /*!< 最近开始命令的归属代次。 */
	uint32_t ulLastSuccessTick; /*!< 最近成功时的 RTOS 节拍。 */
	uint32_t ulCommandCount; /*!< 此状态对象累计观察到的命令数。 */
	uint32_t ulErrorCount; /*!< 累计失败次数。 */
	int32_t lLastResult; /*!< 最近原始结果，单独为零不能证明设备就绪。 */
	uint16_t usLastAction; /*!< 最近开始命令的动作编号。 */
	uint8_t ucOnline; /*!< 通信是否可用，与控制就绪状态独立。 */
	uint8_t ucBusy; /*!< 已发布开始但尚未发布终态。 */
	uint8_t ucReady; /*!< 模块特定的业务控制就绪状态。 */
	uint8_t ucRecovering; /*!< 当前命令是否在链路恢复期间保持。 */
	uint8_t ucRobotPhase; /*!< 工作流超时判断使用的机器人事务阶段。 */
	uint8_t ucRobotAccepted; /*!< 当前机器人命令的接受边沿。 */
	uint8_t ucTerminalValid; /*!< 最新终态字段是否包含已发布结果。 */
	uint8_t ucPreviousTerminalValid; /*!< 上一个终态字段是否有效。 */
	uint32_t ulTerminalCommandId; /*!< 最新保留终态的命令序号。 */
	uint32_t ulTerminalOrderEpoch; /*!< 最新保留终态的归属代次。 */
	int32_t lTerminalResult; /*!< 最新完成命令的原始结果。 */
	uint16_t usTerminalAction; /*!< 最新完成命令的动作编号。 */
	uint8_t ucTerminalTimedOut; /*!< 最新结果是否按超时分类。 */
	uint32_t ulPreviousTerminalCommandId; /*!< 上一个保留终态的命令序号。 */
	uint32_t ulPreviousTerminalOrderEpoch; /*!< 上一个保留终态的归属代次。 */
	int32_t lPreviousTerminalResult; /*!< 上一个完成命令的原始结果。 */
	uint16_t usPreviousTerminalAction; /*!< 上一个完成命令的动作编号。 */
	uint8_t ucPreviousTerminalTimedOut; /*!< 上一个结果是否按超时分类。 */
} Coffee3DeviceStatus_t;

/** @brief 按 Coffee3DeviceId_e 索引的公共设备状态数组。 */
extern Coffee3DeviceStatus_t
	g_axCoffee3DeviceStatus[COFFEE3_DEVICE_COUNT];

BaseType_t xCoffee3DeviceInitialize(void);
void vCoffee3DeviceRegisterRoute(uint8_t ucRouteId, QueueHandle_t xQueue);
const Coffee3DeviceBinding_t *pxCoffee3DeviceGetBinding(
	Coffee3DeviceId_e xDeviceId);
BaseType_t xCoffee3CommandSubmit(Coffee3Command_t *pxCommand,
	TickType_t xWaitTicks);
BaseType_t xCoffee3CommandSubmitUrgent(Coffee3Command_t *pxCommand,
	TickType_t xWaitTicks);
void vCoffee3OrderCancelRequest(uint32_t ulOrderEpoch);
uint8_t ucCoffee3CommandIsCanceled(const Coffee3Command_t *pxCommand);
void vCoffee3DeviceCommandStarted(const Coffee3Command_t *pxCommand);
void vCoffee3DeviceCommandCompleted(const Coffee3Command_t *pxCommand,
	int32_t lResult, uint8_t ucTimedOut);
void vCoffee3DeviceSetOnline(Coffee3DeviceId_e xDeviceId,
	uint8_t ucOnline);
void vCoffee3DeviceSetReady(Coffee3DeviceId_e xDeviceId,
	uint8_t ucReady);
void vCoffee3DeviceSetRecovering(Coffee3DeviceId_e xDeviceId,
	uint8_t ucRecovering);
void vCoffee3DeviceSetRobotPhase(Coffee3RobotPhase_e xPhase);
void vCoffee3DeviceSetRobotAccepted(uint8_t ucAccepted);
EventBits_t xCoffee3DeviceGetEvents(Coffee3DeviceId_e xDeviceId);
EventBits_t xCoffee3DeviceWaitCommand(Coffee3DeviceId_e xDeviceId,
	uint32_t ulOrderEpoch, uint32_t ulCommandId, TickType_t xWaitTicks);
int32_t lCoffee3DeviceGetTerminalResult(Coffee3DeviceId_e xDeviceId,
	uint32_t ulOrderEpoch, uint32_t ulCommandId, uint8_t *pucValid);

#ifdef __cplusplus
}
#endif

#endif /* COFFEE3_DEVICE_H */
