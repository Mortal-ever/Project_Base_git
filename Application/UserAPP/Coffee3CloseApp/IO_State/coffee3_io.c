/**
  * @file      coffee3_io.c
  * @brief     Implement the unified direct and external IO image.
  * @author    WHong
  * @date      2026-07-30
  */

#include "coffee3_io.h"

#include <stddef.h>
#include <string.h>

#include "FreeRTOS.h"
#include "coffee3_app_config.h"
#include "coffee3_io_names.h"
#include "coffee3_log.h"
#include "main.h"
#include "task.h"

/** @brief Describe one generated STM32 GPIO point. */
typedef struct {
	GPIO_TypeDef *pxPort;
	uint16_t usPin;
} Coffee3GpioPoint_t;

/** @brief Global coherent IO image. */
COFFEE3_CCM_DATA
Coffee3IoState_t g_xCoffee3Io;

/** @brief Generated direct-input descriptors in logical X order. */
static const Coffee3GpioPoint_t s_axInputPoints[
	COFFEE3_LOCAL_IO_COUNT] = {
	{ PE0_DI_1_GPIO_Port, PE0_DI_1_Pin },
	{ PE1_DI_2_GPIO_Port, PE1_DI_2_Pin },
	{ PE2_DI_3_GPIO_Port, PE2_DI_3_Pin },
	{ PE3_DI_4_GPIO_Port, PE3_DI_4_Pin },
	{ PE4_DI_5_GPIO_Port, PE4_DI_5_Pin },
	{ PE5_DI_6_GPIO_Port, PE5_DI_6_Pin },
	{ PE6_DI_7_GPIO_Port, PE6_DI_7_Pin },
	{ PE7_DI_8_GPIO_Port, PE7_DI_8_Pin }
};

/** @brief Generated direct-output descriptors in logical Y order. */
static const Coffee3GpioPoint_t s_axOutputPoints[
	COFFEE3_LOCAL_IO_COUNT] = {
	{ PE8_DO_1_GPIO_Port, PE8_DO_1_Pin },
	{ PE9_DO_2_GPIO_Port, PE9_DO_2_Pin },
	{ PE10_DO_3_GPIO_Port, PE10_DO_3_Pin },
	{ PE11_DO_4_GPIO_Port, PE11_DO_4_Pin },
	{ PE12_DO_5_GPIO_Port, PE12_DO_5_Pin },
	{ PE13_DO_6_GPIO_Port, PE13_DO_6_Pin },
	{ PE14_DO_7_GPIO_Port, PE14_DO_7_Pin },
	{ PE15_DO_8_GPIO_Port, PE15_DO_8_Pin }
};

static uint8_t s_aucLastLocalDi[COFFEE3_LOCAL_IO_COUNT];
static uint8_t s_aucLastLocalDo[COFFEE3_LOCAL_IO_COUNT];
static uint8_t s_aucLastMb1Di[COFFEE3_MODBUS_IO_COUNT];
static uint8_t s_aucLastMb2Do[COFFEE3_MODBUS_IO_COUNT];
static uint8_t s_ucLocalBaselineReady;
static uint8_t s_ucMb1BaselineReady;
static uint8_t s_ucMb2BaselineReady;

static const char * const s_apcLocalDiNames[COFFEE3_LOCAL_IO_COUNT] = {
	COFFEE3_IO_NAME_LOCAL_DI_1, COFFEE3_IO_NAME_LOCAL_DI_2,
	COFFEE3_IO_NAME_LOCAL_DI_3, COFFEE3_IO_NAME_LOCAL_DI_4,
	COFFEE3_IO_NAME_LOCAL_DI_5, COFFEE3_IO_NAME_LOCAL_DI_6,
	COFFEE3_IO_NAME_LOCAL_DI_7, COFFEE3_IO_NAME_LOCAL_DI_8
};
static const char * const s_apcLocalDoNames[COFFEE3_LOCAL_IO_COUNT] = {
	COFFEE3_IO_NAME_LOCAL_DO_1, COFFEE3_IO_NAME_LOCAL_DO_2,
	COFFEE3_IO_NAME_LOCAL_DO_3, COFFEE3_IO_NAME_LOCAL_DO_4,
	COFFEE3_IO_NAME_LOCAL_DO_5, COFFEE3_IO_NAME_LOCAL_DO_6,
	COFFEE3_IO_NAME_LOCAL_DO_7, COFFEE3_IO_NAME_LOCAL_DO_8
};
static const char * const s_apcMb1DiNames[COFFEE3_MODBUS_IO_COUNT] = {
	COFFEE3_IO_NAME_MB1_DI_1, COFFEE3_IO_NAME_MB1_DI_2,
	COFFEE3_IO_NAME_MB1_DI_3, COFFEE3_IO_NAME_MB1_DI_4,
	COFFEE3_IO_NAME_MB1_DI_5, COFFEE3_IO_NAME_MB1_DI_6,
	COFFEE3_IO_NAME_MB1_DI_7, COFFEE3_IO_NAME_MB1_DI_8,
	COFFEE3_IO_NAME_MB1_DI_9, COFFEE3_IO_NAME_MB1_DI_10,
	COFFEE3_IO_NAME_MB1_DI_11, COFFEE3_IO_NAME_MB1_DI_12,
	COFFEE3_IO_NAME_MB1_DI_13, COFFEE3_IO_NAME_MB1_DI_14,
	COFFEE3_IO_NAME_MB1_DI_15, COFFEE3_IO_NAME_MB1_DI_16
};
static const char * const s_apcMb2DoNames[COFFEE3_MODBUS_IO_COUNT] = {
	COFFEE3_IO_NAME_MB2_DO_1, COFFEE3_IO_NAME_MB2_DO_2,
	COFFEE3_IO_NAME_MB2_DO_3, COFFEE3_IO_NAME_MB2_DO_4,
	COFFEE3_IO_NAME_MB2_DO_5, COFFEE3_IO_NAME_MB2_DO_6,
	COFFEE3_IO_NAME_MB2_DO_7, COFFEE3_IO_NAME_MB2_DO_8,
	COFFEE3_IO_NAME_MB2_DO_9, COFFEE3_IO_NAME_MB2_DO_10,
	COFFEE3_IO_NAME_MB2_DO_11, COFFEE3_IO_NAME_MB2_DO_12,
	COFFEE3_IO_NAME_MB2_DO_13, COFFEE3_IO_NAME_MB2_DO_14,
	COFFEE3_IO_NAME_MB2_DO_15, COFFEE3_IO_NAME_MB2_DO_16
};

static void prvLogIoChanges(Coffee3LogSource_e xSource,
	const char * const *ppcNames, const uint8_t *pucOld,
	const uint8_t *pucNew, uint8_t ucCount);

/*-----------------------------------------------------------*/
void vCoffee3IoInitialize(void)
{
	taskENTER_CRITICAL();
	memset(&g_xCoffee3Io, 0, sizeof(g_xCoffee3Io));
	s_ucLocalBaselineReady = 0U;
	s_ucMb1BaselineReady = 0U;
	s_ucMb2BaselineReady = 0U;
	taskEXIT_CRITICAL();
	vCoffee3IoRefreshLocal();
}

/*-----------------------------------------------------------*/
void vCoffee3IoRefreshLocal(void)
{
	uint8_t aucInputs[COFFEE3_LOCAL_IO_COUNT];
	uint8_t aucOutputs[COFFEE3_LOCAL_IO_COUNT];
	uint8_t aucOldInputs[COFFEE3_LOCAL_IO_COUNT];
	uint8_t aucOldOutputs[COFFEE3_LOCAL_IO_COUNT];
	uint8_t ucLogChanges;
	uint8_t ucIndex;

	for (ucIndex = 0U; ucIndex < COFFEE3_LOCAL_IO_COUNT; ucIndex++) {
		aucInputs[ucIndex] =
			(HAL_GPIO_ReadPin(s_axInputPoints[ucIndex].pxPort,
				s_axInputPoints[ucIndex].usPin) == GPIO_PIN_SET) ?
			0U : 1U;
		aucOutputs[ucIndex] =
			((s_axOutputPoints[ucIndex].pxPort->ODR &
				s_axOutputPoints[ucIndex].usPin) != 0U) ? 1U : 0U;
	}
	taskENTER_CRITICAL();
	memcpy(aucOldInputs, g_xCoffee3Io.xInput.aucXPin,
		sizeof(aucOldInputs));
	memcpy(aucOldOutputs, g_xCoffee3Io.xOutput.aucYPin,
		sizeof(aucOldOutputs));
	memcpy(g_xCoffee3Io.xInput.aucXPin, aucInputs, sizeof(aucInputs));
	memcpy(g_xCoffee3Io.xOutput.aucYPin, aucOutputs,
		sizeof(aucOutputs));
	g_xCoffee3Io.ulLocalUpdateTick = (uint32_t)xTaskGetTickCount();
	g_xCoffee3Io.ulVersion++;
	ucLogChanges = s_ucLocalBaselineReady;
	memcpy(s_aucLastLocalDi, aucInputs, sizeof(aucInputs));
	memcpy(s_aucLastLocalDo, aucOutputs, sizeof(aucOutputs));
	s_ucLocalBaselineReady = 1U;
	taskEXIT_CRITICAL();
	if (ucLogChanges != 0U) {
		prvLogIoChanges(COFFEE3_LOG_SOURCE_IO, s_apcLocalDiNames,
			aucOldInputs, aucInputs, COFFEE3_LOCAL_IO_COUNT);
		prvLogIoChanges(COFFEE3_LOG_SOURCE_IO, s_apcLocalDoNames,
			aucOldOutputs, aucOutputs, COFFEE3_LOCAL_IO_COUNT);
	}
}

/*-----------------------------------------------------------*/
uint8_t ucCoffee3IoSetLocalOutput(uint8_t ucIndex, uint8_t ucValue)
{
	uint8_t ucOldValue;
	uint8_t ucLogChange;

	if (ucIndex >= COFFEE3_LOCAL_IO_COUNT) {
		return 0U;
	}
	ucValue = (ucValue != 0U) ? 1U : 0U;
	taskENTER_CRITICAL();
	if ((ucIndex < 2U) && (ucValue != 0U) &&
		(HAL_GPIO_ReadPin(s_axOutputPoints[1U - ucIndex].pxPort,
		 s_axOutputPoints[1U - ucIndex].usPin) == GPIO_PIN_SET)) {
		taskEXIT_CRITICAL();
		return 0U;
	}
	HAL_GPIO_WritePin(s_axOutputPoints[ucIndex].pxPort,
		s_axOutputPoints[ucIndex].usPin,
		(ucValue != 0U) ? GPIO_PIN_SET : GPIO_PIN_RESET);
	ucValue = (ucValue != 0U) ? 1U : 0U;
	ucOldValue = g_xCoffee3Io.xOutput.aucYPin[ucIndex];
	ucLogChange = ((s_ucLocalBaselineReady != 0U) &&
		(ucOldValue != ucValue)) ? 1U : 0U;
	if ((s_ucLocalBaselineReady != 0U) &&
		(s_aucLastLocalDo[ucIndex] != ucValue)) {
		s_aucLastLocalDo[ucIndex] = ucValue;
	}
	g_xCoffee3Io.xOutput.aucYPin[ucIndex] = ucValue;
	g_xCoffee3Io.ulLocalUpdateTick = (uint32_t)xTaskGetTickCount();
	g_xCoffee3Io.ulVersion++;
	taskEXIT_CRITICAL();
	if (ucLogChange != 0U) {
		(void)xCoffee3LogPrintfOrder(COFFEE3_LOG_LEVEL_INFO,
			COFFEE3_LOG_SOURCE_IO, COFFEE3_LOG_ORDER_SYSTEM,
			"IO_STATE_CHANGED NAME=%s %u->%u", s_apcLocalDoNames[ucIndex],
			(unsigned int)ucOldValue, (unsigned int)ucValue);
	}
	return 1U;
}

/*-----------------------------------------------------------*/
uint8_t ucCoffee3IoApplyLocalDebugMask(uint16_t usMask)
{
	uint8_t ucIndex;

	for (ucIndex = 0U; ucIndex < COFFEE3_LOCAL_IO_COUNT; ucIndex++) {
		HAL_GPIO_WritePin(s_axOutputPoints[ucIndex].pxPort,
			s_axOutputPoints[ucIndex].usPin,
			((usMask & (uint16_t)(1U << ucIndex)) != 0U) ?
			GPIO_PIN_SET : GPIO_PIN_RESET);
	}
	vCoffee3IoRefreshLocal();
	return 1U;
}

/*-----------------------------------------------------------*/
void vCoffee3IoCommitModbusInput(const uint8_t *pucInputs)
{
	uint8_t aucOldInputs[COFFEE3_MODBUS_IO_COUNT];
	uint8_t ucLogChanges;

	if (pucInputs == NULL) {
		return;
	}
	taskENTER_CRITICAL();
	memcpy(aucOldInputs, g_xCoffee3Io.xInput.aucMB1XPin,
		sizeof(aucOldInputs));
	memcpy(g_xCoffee3Io.xInput.aucMB1XPin, pucInputs,
		COFFEE3_MODBUS_IO_COUNT);
	g_xCoffee3Io.aulModbusUpdateTick[0] =
		(uint32_t)xTaskGetTickCount();
	g_xCoffee3Io.aucModbusValid[0] = 1U;
	g_xCoffee3Io.ulVersion++;
	ucLogChanges = s_ucMb1BaselineReady;
	memcpy(s_aucLastMb1Di, pucInputs, sizeof(s_aucLastMb1Di));
	s_ucMb1BaselineReady = 1U;
	taskEXIT_CRITICAL();
	if (ucLogChanges != 0U) {
		prvLogIoChanges(COFFEE3_LOG_SOURCE_IO_INPUT, s_apcMb1DiNames,
			aucOldInputs, pucInputs, COFFEE3_MODBUS_IO_COUNT);
	}
}

/*-----------------------------------------------------------*/
void vCoffee3IoCommitModbusOutputImage(const uint8_t *pucOutputs)
{
	uint8_t aucOldOutputs[COFFEE3_MODBUS_IO_COUNT];
	uint8_t ucLogChanges;

	if (pucOutputs == NULL) {
		return;
	}
	taskENTER_CRITICAL();
	memcpy(aucOldOutputs, g_xCoffee3Io.xOutput.aucMB2YPin,
		sizeof(aucOldOutputs));
	memcpy(g_xCoffee3Io.xOutput.aucMB2YPin, pucOutputs,
		COFFEE3_MODBUS_IO_COUNT);
	g_xCoffee3Io.aulModbusUpdateTick[1] =
		(uint32_t)xTaskGetTickCount();
	g_xCoffee3Io.aucModbusValid[1] = 1U;
	g_xCoffee3Io.ulVersion++;
	ucLogChanges = s_ucMb2BaselineReady;
	memcpy(s_aucLastMb2Do, pucOutputs, sizeof(s_aucLastMb2Do));
	s_ucMb2BaselineReady = 1U;
	taskEXIT_CRITICAL();
	if (ucLogChanges != 0U) {
		prvLogIoChanges(COFFEE3_LOG_SOURCE_IO_OUTPUT, s_apcMb2DoNames,
			aucOldOutputs, pucOutputs, COFFEE3_MODBUS_IO_COUNT);
	}
}

/*-----------------------------------------------------------*/
static void prvLogIoChanges(Coffee3LogSource_e xSource,
	const char * const *ppcNames, const uint8_t *pucOld,
	const uint8_t *pucNew, uint8_t ucCount)
{
	uint8_t ucIndex;

	for (ucIndex = 0U; ucIndex < ucCount; ucIndex++) {
		if (pucOld[ucIndex] != pucNew[ucIndex]) {
			(void)xCoffee3LogPrintfOrder(COFFEE3_LOG_LEVEL_INFO, xSource,
				COFFEE3_LOG_ORDER_SYSTEM,
				"IO_STATE_CHANGED NAME=%s %u->%u", ppcNames[ucIndex],
				(unsigned int)pucOld[ucIndex],
				(unsigned int)pucNew[ucIndex]);
		}
	}
}

/*-----------------------------------------------------------*/
void vCoffee3IoGetSnapshot(Coffee3IoState_t *pxSnapshot)
{
	if (pxSnapshot == NULL) {
		return;
	}
	taskENTER_CRITICAL();
	*pxSnapshot = g_xCoffee3Io;
	taskEXIT_CRITICAL();
}
