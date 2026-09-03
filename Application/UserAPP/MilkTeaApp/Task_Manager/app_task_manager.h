/**
  * @file      app_task_manager.h
  * @brief     Define product task creation and network coordination APIs.
  * @author    WHong
  * @date      2026-07-28
  */

#ifndef APP_TASK_MANAGER_H
#define APP_TASK_MANAGER_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/** @brief Define task-manager initialization results. */
typedef enum {
	APP_TASK_MANAGER_RESULT_OK = 0, /*!< Tasks were created. */
	APP_TASK_MANAGER_RESULT_ALREADY_CREATED = 1, /*!< Creation already ran. */
	APP_TASK_MANAGER_RESULT_NO_RESOURCE = -1, /*!< RTOS object failed. */
	APP_TASK_MANAGER_RESULT_CLIENT_INIT = -2, /*!< TCP app init failed. */
	APP_TASK_MANAGER_RESULT_LOG_INIT = -3 /*!< Log transport failed. */
} AppTaskManagerResult_e;

/** @brief Store the task manager's observable startup state. */
typedef struct {
	AppTaskManagerResult_e xStartResult; /*!< Latest startup result. */
	uint8_t ucTasksCreated; /*!< All product tasks exist. */
	uint8_t ucNetworkStackReady; /*!< CubeMX LwIP init returned. */
	uint8_t ucNetworkReady; /*!< Link, netif, and IPv4 are ready. */
	uint8_t ucClientReady; /*!< TCP application state is ready. */
} AppTaskManagerStatus_t;

/**
  * @brief Create all product-owned FreeRTOS tasks and shared objects.
  * @retval APP_TASK_MANAGER_RESULT_OK All required tasks were created.
  * @retval APP_TASK_MANAGER_RESULT_ALREADY_CREATED Initialization already ran.
  * @retval APP_TASK_MANAGER_RESULT_NO_RESOURCE An RTOS object was unavailable.
  * @retval APP_TASK_MANAGER_RESULT_CLIENT_INIT TCP application setup failed.
  * @retval APP_TASK_MANAGER_RESULT_LOG_INIT Log transport setup failed.
  * @note Call once before the scheduler starts.
  */
AppTaskManagerResult_e xAppTaskManagerCreateTasks(void);

/**
  * @brief Run network readiness monitoring from the CubeMX default task.
  * @note Call after MX_LWIP_Init(); this function never returns.
  */
void vAppTaskManagerRunDefaultTask(void);

/**
  * @brief Block until the default task reports LwIP stack initialization.
  * @note Physical link and IPv4 readiness are not required.
  */
void vAppTaskManagerWaitNetworkStackReady(void);

/**
  * @brief Read the current netif, link, and IPv4 readiness.
  * @retval 1 All network prerequisites are ready.
  * @retval 0 At least one prerequisite is unavailable.
  */
uint8_t ucAppTaskManagerIsNetworkReady(void);

/**
  * @brief Copy the current task-manager status.
  * @param[out] pxStatus Destination status object; ignored when NULL.
  */
void vAppTaskManagerGetStatus(AppTaskManagerStatus_t *pxStatus);

#ifdef __cplusplus
}
#endif

#endif /* APP_TASK_MANAGER_H */
