/**
  * @file      coffee3_manager.h
  * @brief     定义 Coffee3 启动任务管理器接口。
  * @author    WHong
  * @date      2026-09-24
  */

#ifndef COFFEE3_MANAGER_H
#define COFFEE3_MANAGER_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/** @brief 定义 Coffee3 任务管理器启动结果。 */
typedef enum {
	APP_TASK_MANAGER_RESULT_OK = 0, /*!< 启动成功。 */
	APP_TASK_MANAGER_RESULT_ALREADY_CREATED = 1, /*!< 已经完成过创建。 */
	APP_TASK_MANAGER_RESULT_NO_RESOURCE = -1, /*!< 事件组或必需任务资源不足。 */
	APP_TASK_MANAGER_RESULT_LOG_INIT = -2, /*!< 兼容保留的日志初始化失败结果。 */
	APP_TASK_MANAGER_RESULT_SERIAL_INIT = -3, /*!< 业务串口默认配置失败。 */
	APP_TASK_MANAGER_RESULT_MODULE_INIT = -4 /*!< 产品模块初始化失败。 */
} AppTaskManagerResult_e;

/** @brief 标识启动状态掩码中的 Coffee3 应用任务。 */
#define APP_TASK_MASK_LOG                    (1UL << 0)
#define APP_TASK_MASK_SERVER                 (1UL << 1)
#define APP_TASK_MASK_ROBOT                  (1UL << 2)
#define APP_TASK_MASK_BUS2                   (1UL << 3)
#define APP_TASK_MASK_BUS3                   (1UL << 4)
#define APP_TASK_MASK_BUS4                   (1UL << 5)
#define APP_TASK_MASK_BUS5                   (1UL << 6)
#define APP_TASK_MASK_WORKFLOW               (1UL << 7)
#define APP_TASK_MASK_ALL                    0x000000FFUL

/** @brief 保存可查询的 Coffee3 启动状态与网络就绪状态。 */
typedef struct {
	AppTaskManagerResult_e xStartResult; /*!< 软件启动结果，不代表物理设备初始化结果。 */
	uint32_t ulResetCause; /*!< 清除 RCC 标志前捕获的复位原因位。 */
	uint32_t ulTaskCreatedMask; /*!< 启动期间成功创建的任务位。 */
	uint32_t ulTaskFailedMask; /*!< 因资源不足而创建失败的任务位。 */
	uint32_t ulFreeHeapBeforeTasks; /*!< 创建任务前的 FreeRTOS 空闲堆字节数。 */
	uint32_t ulFreeHeapAfterTasks; /*!< 创建任务后的 FreeRTOS 空闲堆字节数。 */
	uint8_t ucInfrastructureCreated; /*!< 防止重复构造已完成的模块。 */
	uint8_t ucTasksCreated; /*!< 核心任务创建序列是否成功完成。 */
	uint8_t ucLogReady; /*!< 日志传输与日志任务是否都已就绪。 */
	uint8_t ucDeviceReady; /*!< 设备软件事件与路由已初始化，不证明硬件就绪。 */
	uint8_t ucServerReady; /*!< 服务端软件已初始化，不要求已有客户端。 */
	uint8_t ucRobotReady; /*!< 机器人软件已初始化，传输仍可能离线。 */
	uint8_t ucRtuReady; /*!< RTU 队列已初始化，串口由任务稍后打开。 */
	uint8_t ucWorkflowReady; /*!< 工作流软件已初始化，残杯检查稍后执行。 */
	uint8_t ucNetworkStackReady; /*!< 默认任务已从 LwIP 初始化返回。 */
	uint8_t ucNetworkReady; /*!< 周期更新的链路、接口与 IPv4 综合就绪状态。 */
} AppTaskManagerStatus_t;

AppTaskManagerResult_e xAppTaskManagerCreateTasks(void);
void vAppTaskManagerRunDefaultTask(void);
void vAppTaskManagerWaitNetworkStackReady(void);
uint8_t ucAppTaskManagerIsNetworkReady(void);
void vAppTaskManagerGetStatus(AppTaskManagerStatus_t *pxStatus);

#ifdef __cplusplus
}
#endif

#endif /* APP_TASK_MANAGER_H */
