/**
  * @file      coffee2_server.h
  * @brief     Define the Coffee2 multi-client Modbus TCP server.
  * @author    WHong
  * @date      2026-07-30
  */

#ifndef COFFEE2_SERVER_H
#define COFFEE2_SERVER_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "FreeRTOS.h"
#include "coffee2_app_config.h"

/** @brief Host writable protocol block 0x0000 through 0x00AF. */
#define COFFEE2_SERVER_COMMAND_COUNT          0x00B0U
/** @brief Host readable protocol block 0x1000 through 0x10D4. */
#define COFFEE2_SERVER_STATUS_COUNT           0x0100U
/** @brief Private monitoring block 0x1100 through 0x117F. */
#define COFFEE2_SERVER_DEBUG_COUNT            0x0080U

/** @brief Important host command register addresses. */
#define COFFEE2_REG_ORDER_NUMBER              0x0000U
#define COFFEE2_REG_COFFEE_TYPE               0x0001U
#define COFFEE2_REG_LID_ENABLE                0x0002U
#define COFFEE2_REG_SYRUP_1                   0x0003U
#define COFFEE2_REG_SYRUP_2                   0x0004U
#define COFFEE2_REG_ICE_AMOUNT                0x0005U
#define COFFEE2_REG_RESERVED_0006             0x0006U
#define COFFEE2_REG_ORDER_PRESENT             0x0007U
#define COFFEE2_REG_ORDER_VERIFIED            0x0008U
#define COFFEE2_REG_RESERVED_0009             0x0009U
#define COFFEE2_REG_ONLINE_OUTPUT             0x000AU
#define COFFEE2_REG_PICKUP_CONFIRM            0x000BU
#define COFFEE2_REG_RESERVED_000C             0x000CU
#define COFFEE2_REG_FRUIT_MILK_A              0x000DU
#define COFFEE2_REG_FRUIT_MILK_B              0x000EU
#define COFFEE2_REG_SYRUP_3                   0x0013U
#define COFFEE2_REG_SYRUP_4                   0x0014U
#define COFFEE2_REG_CLEAR_ALARM               0x0021U
#define COFFEE2_REG_CANCEL_ORDER              0x0022U
#define COFFEE2_REG_RESERVED_002A             0x002AU
#define COFFEE2_REG_HOT_WATER_START            0x0080U
#define COFFEE2_REG_HOT_WATER_MINUTES          0x0081U
#define COFFEE2_REG_SYRUP_CLEAN                0x0082U
#define COFFEE2_REG_COFFEE_PIPE_CLEAN          0x0083U
#define COFFEE2_REG_MANUAL_FRUIT_TYPE          0x00A1U
#define COFFEE2_REG_MANUAL_FRUIT_AMOUNT        0x00A2U
#define COFFEE2_REG_FRUIT_A_CLEAN              0x00A3U
#define COFFEE2_REG_FRUIT_B_CLEAN              0x00A4U
/** @brief Important host readable register addresses. */
#define COFFEE2_REG_STATUS_BASE               0x1000U
#define COFFEE2_REG_PRODUCTION_STATUS         0x1008U
#define COFFEE2_REG_WORKFLOW_STEP             0x1018U
#define COFFEE2_REG_WORKFLOW_ERROR            0x1029U
#define COFFEE2_REG_MACHINE_STATUS             0x1020U
#define COFFEE2_REG_HOT_WATER_STATUS           0x1082U
#define COFFEE2_REG_COFFEE_PIPE_STATUS         0x1083U
#define COFFEE2_REG_FRUIT_STATUS               0x10A0U
#define COFFEE2_REG_FRUIT_A_LOW                0x10A1U
#define COFFEE2_REG_FRUIT_B_LOW                0x10A2U
#define COFFEE2_REG_FRUIT_A_STATUS             0x10A8U
#define COFFEE2_REG_FRUIT_B_STATUS             0x10A9U
/** @brief Read-only 32-channel IO page defined by the host protocol. */
#define COFFEE2_REG_LOCAL_INPUT_LOW           0x10F0U
#define COFFEE2_REG_LOCAL_INPUT_HIGH          0x10F1U
#define COFFEE2_REG_EXTERNAL_INPUT_1_LOW      0x10F2U
#define COFFEE2_REG_EXTERNAL_INPUT_1_HIGH     0x10F3U
#define COFFEE2_REG_EXTERNAL_INPUT_2_LOW      0x10F4U
#define COFFEE2_REG_EXTERNAL_INPUT_2_HIGH     0x10F5U
#define COFFEE2_REG_EXTERNAL_INPUT_3_LOW      0x10F6U
#define COFFEE2_REG_EXTERNAL_INPUT_3_HIGH     0x10F7U
#define COFFEE2_REG_LOCAL_OUTPUT_LOW          0x10F8U
#define COFFEE2_REG_LOCAL_OUTPUT_HIGH         0x10F9U
#define COFFEE2_REG_EXTERNAL_OUTPUT_1_LOW     0x10FAU
#define COFFEE2_REG_EXTERNAL_OUTPUT_1_HIGH    0x10FBU
#define COFFEE2_REG_EXTERNAL_OUTPUT_2_LOW     0x10FCU
#define COFFEE2_REG_EXTERNAL_OUTPUT_2_HIGH    0x10FDU
#define COFFEE2_REG_EXTERNAL_OUTPUT_3_LOW     0x10FEU
#define COFFEE2_REG_EXTERNAL_OUTPUT_3_HIGH    0x10FFU
/** @brief Output debug registers from the host protocol. */
#define COFFEE2_REG_LOCAL_IO_DEBUG             0x0208U
#define COFFEE2_REG_EXTERNAL_IO_DEBUG          0x0209U

/** @brief Production states specified by the supplied host protocol. */
#define COFFEE2_PRODUCTION_IDLE               0U
#define COFFEE2_PRODUCTION_RUNNING            1U
#define COFFEE2_PRODUCTION_COMPLETED          2U
#define COFFEE2_PRODUCTION_FAILED             3U

/** @brief Store one accepted TCP client slot's public status. */
typedef struct {
	uint32_t ulRemoteIpv4;
	uint32_t ulRequestCount;
	uint32_t ulErrorCount;
	uint32_t ulDisconnectCount;
	uint32_t ulLastActivityTick;
	int32_t lLastResult;
	uint16_t usRemotePort;
	uint8_t ucConnected;
} Coffee2ServerClientStatus_t;

/** @brief Store listener and configured-slot runtime counters. */
typedef struct {
	Coffee2ServerClientStatus_t axClient[COFFEE2_SERVER_MAX_CLIENTS];
	uint32_t ulAcceptedCount;
	uint32_t ulRejectedCount;
	uint32_t ulListenerErrorCount;
	uint32_t ulOnlineTransitionCount;
	uint16_t usListenPort;
	uint8_t ucActiveClients;
	uint8_t ucListening;
	uint8_t ucOnline;
} Coffee2ServerStatus_t;

extern Coffee2ServerStatus_t g_xCoffee2ServerStatus;

/**
  * @brief Initialize register images and nanoMODBUS callbacks.
  * @retval pdPASS Initialization succeeded.
  * @retval pdFAIL Configuration is invalid.
  */
BaseType_t xCoffee2ServerInitialize(void);

/**
  * @brief Run the listener and exactly two reusable client slots.
  * @param[in] pvArgument Unused.
  */
void vCoffee2ServerTask(void *pvArgument);

/**
  * @brief Publish workflow progress into the host-readable register block.
  * @param[in] usOrderId Active or last order identifier.
  * @param[in] usProductionStatus Protocol state 0 through 3.
  * @param[in] usStep Current workflow step.
  * @param[in] lError Latest workflow error.
  */
void vCoffee2ServerPublishWorkflow(uint16_t usOrderId,
	uint16_t usProductionStatus, uint16_t usStep, int32_t lError);

/**
  * @brief Publish one Coffee2 output-port state.
  * @param[in] usOutput One-based output port number, 1 or 2.
  * @param[in] usState Protocol output state: 2, 3, 5 or 0x10.
  */
void vCoffee2ServerPublishOutput(uint16_t usOutput, uint16_t usState);

/**
  * @brief Read one host command register safely from workflow.
  * @param[in] usAddress Command-space address.
  * @return Register value, or zero when outside the command block.
  */
uint16_t usCoffee2ServerGetCommandRegister(uint16_t usAddress);

#ifdef __cplusplus
}
#endif

#endif /* COFFEE2_SERVER_H */
