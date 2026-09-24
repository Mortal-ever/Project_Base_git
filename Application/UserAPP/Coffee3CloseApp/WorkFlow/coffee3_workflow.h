/**
  * @file      coffee3_workflow.h
  * @brief     定义 Coffee3 订单流程接口及供 Server 读取的状态。
  * @author    WHong
  * @date      2026-09-24
  */

#ifndef COFFEE3_WORKFLOW_H
#define COFFEE3_WORKFLOW_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "FreeRTOS.h"
#include "coffee3_app_config.h"

/** @brief 一份已接收订单保存的寄存器数量。 */
#define COFFEE3_ORDER_REGISTER_COUNT          32U

/** @brief 保存接单时复制的寄存器快照，后续执行不再读取上位机缓冲区。 */
typedef struct {
	uint16_t ausRegister[COFFEE3_ORDER_REGISTER_COUNT]; /*!< 由工作流持有的 32 字寄存器副本。 */
} Coffee3Order_t;

/** @brief 区分工作流空闲、执行、完成、失败和取消阶段。 */
typedef enum {
	COFFEE3_WORKFLOW_IDLE = 0, /*!< 当前工作流空闲。 */
	COFFEE3_WORKFLOW_RUNNING = 1, /*!< 当前工作流步骤正在推进。 */
	COFFEE3_WORKFLOW_COMPLETED = 2, /*!< 最近一次工作流操作完成。 */
	COFFEE3_WORKFLOW_FAILED = 3, /*!< 最近一次工作流操作失败。 */
	COFFEE3_WORKFLOW_CANCELING = 4 /*!< 已请求取消，正在收尾。 */
} Coffee3WorkflowState_e;

/** @brief 对上位机发布的整机状态，与工作流状态分别维护。 */
typedef enum {
	COFFEE3_MACHINE_DEFAULT = 0, /*!< 尚未进入确定的业务状态。 */
	COFFEE3_MACHINE_IDLE = 1, /*!< 整机空闲。 */
	COFFEE3_MACHINE_INITIALIZING = 2, /*!< 正在初始化。 */
	COFFEE3_MACHINE_BUSY = 3, /*!< 整机业务占用。 */
	COFFEE3_MACHINE_ALARM = 4 /*!< 整机告警。 */
} Coffee3MachineState_e;

/** @brief 由工作流任务串行执行的非订单维护类型。 */
typedef enum {
	COFFEE3_MAINTENANCE_NONE = 0, /*!< 无维护请求。 */
	COFFEE3_MAINTENANCE_SYRUP_CLEAN = 1, /*!< 糖浆机清洗。 */
	COFFEE3_MAINTENANCE_COFFEE_CLEAN = 2, /*!< 咖啡机清洗。 */
	COFFEE3_MAINTENANCE_FRUIT_DISPENSE = 3, /*!< 果奶定量出料。 */
	COFFEE3_MAINTENANCE_FRUIT_CLEAN = 4 /*!< 果奶清洗。 */
} Coffee3MaintenanceType_e;

/** @brief 投影到上位机寄存器的维护进度。 */
typedef enum {
	COFFEE3_MAINTENANCE_IDLE = 0, /*!< 当前维护空闲。 */
	COFFEE3_MAINTENANCE_RUNNING = 1, /*!< 维护步骤正在推进。 */
	COFFEE3_MAINTENANCE_COMPLETED = 2, /*!< 最近维护成功完成。 */
	COFFEE3_MAINTENANCE_FAILED = 3, /*!< 最近维护执行失败。 */
	COFFEE3_MAINTENANCE_ALARM = 4 /*!< 维护进入告警状态。 */
} Coffee3MaintenanceState_e;

/**
  * @brief 保存工作流进度及对外可观察的业务状态。
  * @details 命令入队、设备成功完成和物理后置条件分别记录；
  *          Server 读取这些字段时不应把某一阶段当作整单完成。
  */
typedef struct {
	uint32_t ulCompletedOrderCount; /*!< 成功走完订单路径后递增。 */
	uint32_t ulFailedOrderCount; /*!< 订单处理失败后递增。 */
	uint32_t ulOrderEpoch; /*!< 命令取消代次；零表示尚未分配。 */
	uint16_t usCurrentOrderId; /*!< 当前或最近订单号，也可能是系统、调试日志标识。 */
	uint16_t usCurrentStep; /*!< 最近发布的工作流步骤号，不是设备地址。 */
	int32_t lLastError; /*!< 业务错误，与设备命令终态分开保存。 */
	Coffee3WorkflowState_e xState; /*!< 工作流自身的生命周期。 */
	Coffee3MachineState_e xMachineState; /*!< 对外发布的整机状态。 */
	uint8_t ucCancelRequested; /*!< 等待点检查的协作式取消请求。 */
	uint8_t ucOrderAdmissionOpen; /*!< 业务接单许可，不代表网络已就绪。 */
	uint8_t ucHotWaterState; /*!< 热水服务的对外状态。 */
	uint8_t ucCoffeeCleanState; /*!< 咖啡清洗的对外状态。 */
	uint8_t aucFruitState[2]; /*!< 果乳 A/B 两路维护状态。 */
	uint16_t ausOutputState[2]; /*!< 两个出口保留的交付事务状态。 */
	uint16_t ausOutputOrderId[2]; /*!< 最近进入状态 2 时登记的订单号，释放后仍保留。 */
	uint16_t usActiveOutput; /*!< 当前或最近正式订单选择的出口；初始为 0。 */
	uint16_t usAction; /*!< 产品动作，由设备所有者转换为原生命令。 */
	uint8_t ucDeviceId; /*!< 静态绑定表中的逻辑设备编号。 */
	uint8_t ucCommandSent; /*!< 步骤已入队，设备可能尚未动作。 */
	uint8_t ucDeviceDone; /*!< 对应命令已成功完成，传感器仍须另行核对。 */
	uint8_t ucPhysicalVerified; /*!< 工作流已确认要求的物理后置条件。 */
	uint8_t ucPositionUncertain; /*!< 失败前位置动作可能已改变硬件。 */
	uint8_t ucRecoveryRequired; /*!< 恢复锁有效时禁止正常接单。 */
	int32_t lSafetyResult; /*!< 安全停止结果，独立于原订单错误。 */
	uint8_t ucStorageReserved; /*!< 当前或最近订单选中的储位；0 表示未选择。 */
	uint8_t ucContentComplete; /*!< 内容制作里程碑，可能早于最终交付。 */
} Coffee3WorkflowStatus_t;

extern Coffee3WorkflowStatus_t g_xCoffee3WorkflowStatus;

BaseType_t xCoffee3WorkflowAcquireManual(void);
BaseType_t xCoffee3WorkflowAcquireDeferredManual(void);
void vCoffee3WorkflowReleaseManual(void);
uint8_t ucCoffee3WorkflowManualDispatchAllowed(void);
BaseType_t xCoffee3WorkflowAcquireOta(void);
void vCoffee3WorkflowReleaseOta(void);
void vCoffee3WorkflowConfirmPickup(uint16_t usOutput);
BaseType_t xCoffee3WorkflowSubmitDoorDebug(uint8_t ucDirection);
BaseType_t xCoffee3WorkflowSubmitStoragePickup(uint16_t usStorage,
	uint16_t usOutput);

BaseType_t xCoffee3WorkflowInitialize(void);

uint8_t ucCoffee3WorkflowInitializationComplete(void);

BaseType_t xCoffee3WorkflowSubmitOrder(const Coffee3Order_t *pxOrder);

BaseType_t xCoffee3WorkflowSubmitManualIce(uint16_t usTargetGrams);

BaseType_t xCoffee3WorkflowSubmitMaintenance(
	Coffee3MaintenanceType_e xType, uint16_t usParameter0,
	uint16_t usParameter1);

BaseType_t xCoffee3WorkflowSetHotWater(uint8_t ucStart,
	uint16_t usHeatMinutes);

void vCoffee3WorkflowAcknowledgeAlarm(void);

void vCoffee3WorkflowRequestCancel(void);

void vCoffee3WorkflowTask(void *pvArgument);

#ifdef __cplusplus
}
#endif

#endif /* COFFEE3_WORKFLOW_H */
