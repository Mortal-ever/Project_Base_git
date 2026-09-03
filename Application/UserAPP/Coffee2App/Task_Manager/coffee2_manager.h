/**
  * @file      app_task_manager.h
  * @brief     Define the Coffee2 startup task manager interface.
  * @author    WHong
  * @date      2026-07-30
  */

#ifndef COFFEE2_MANAGER_H
#define COFFEE2_MANAGER_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/** @brief Define Coffee2 task-manager startup results. */
typedef enum {
	APP_TASK_MANAGER_RESULT_OK = 0,
	APP_TASK_MANAGER_RESULT_ALREADY_CREATED = 1,
	APP_TASK_MANAGER_RESULT_NO_RESOURCE = -1,
	APP_TASK_MANAGER_RESULT_LOG_INIT = -2,
	APP_TASK_MANAGER_RESULT_SERIAL_INIT = -3,
	APP_TASK_MANAGER_RESULT_MODULE_INIT = -4
} AppTaskManagerResult_e;

/** @brief Identify Coffee2 application tasks in startup masks. */
#define APP_TASK_MASK_LOG                    (1UL << 0)
#define APP_TASK_MASK_SERVER                 (1UL << 1)
#define APP_TASK_MASK_ROBOT                  (1UL << 2)
#define APP_TASK_MASK_BUS2                   (1UL << 3)
#define APP_TASK_MASK_BUS3                   (1UL << 4)
#define APP_TASK_MASK_BUS4                   (1UL << 5)
#define APP_TASK_MASK_BUS5                   (1UL << 6)
#define APP_TASK_MASK_WORKFLOW               (1UL << 7)
#define APP_TASK_MASK_ALL                    0x000000FFUL

/** @brief Store observable Coffee2 startup and network readiness. */
typedef struct {
	AppTaskManagerResult_e xStartResult;
	uint32_t ulResetCause;
	uint32_t ulTaskCreatedMask;
	uint32_t ulTaskFailedMask;
	uint32_t ulFreeHeapBeforeTasks;
	uint32_t ulFreeHeapAfterTasks;
	uint8_t ucInfrastructureCreated;
	uint8_t ucTasksCreated;
	uint8_t ucLogReady; /*!< Log Transport and C2Log task are ready. */
	uint8_t ucDeviceReady;
	uint8_t ucServerReady;
	uint8_t ucRobotReady;
	uint8_t ucRtuReady;
	uint8_t ucWorkflowReady;
	uint8_t ucNetworkStackReady;
	uint8_t ucNetworkReady;
} AppTaskManagerStatus_t;

/**
  * @brief  Create Coffee2 static startup infrastructure.
  * @retval APP_TASK_MANAGER_RESULT_OK Infrastructure was created.
  * @retval APP_TASK_MANAGER_RESULT_ALREADY_CREATED Creation ran before.
  * @retval APP_TASK_MANAGER_RESULT_NO_RESOURCE RTOS object creation failed.
  * @retval APP_TASK_MANAGER_RESULT_LOG_INIT Log transport setup failed.
  * @retval APP_TASK_MANAGER_RESULT_SERIAL_INIT UART override failed.
  * @retval APP_TASK_MANAGER_RESULT_MODULE_INIT A product module failed.
  * @note   Call once before the scheduler starts.
  */
AppTaskManagerResult_e xAppTaskManagerCreateTasks(void);

/**
  * @brief  Initialize and run Coffee2 services from the default task.
  * @note   Call after MX_LWIP_Init(); this function never returns.
  */
void vAppTaskManagerRunDefaultTask(void);

/**
  * @brief  Block until the default task reports LwIP stack initialization.
  */
void vAppTaskManagerWaitNetworkStackReady(void);

/**
  * @brief  Read current link, netif, and IPv4 readiness.
  * @retval 1 The network is ready.
  * @retval 0 At least one network prerequisite is unavailable.
  */
uint8_t ucAppTaskManagerIsNetworkReady(void);

/**
  * @brief  Copy the current Coffee2 task-manager status.
  * @param[out] pxStatus Caller-owned destination; ignored when NULL.
  */
void vAppTaskManagerGetStatus(AppTaskManagerStatus_t *pxStatus);

#ifdef __cplusplus
}
#endif

#endif /* APP_TASK_MANAGER_H */
