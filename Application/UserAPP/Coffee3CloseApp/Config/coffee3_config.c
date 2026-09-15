/* Target-private configuration. No OTA or robot port data belongs here. */
#include "coffee3_config.h"
#include "coffee3_log.h"
#include "Ota/app_ota_flash.h"
#include "FreeRTOS.h"
#include "task.h"

#define COFFEE3_CONFIG_V1_MARK 0x43334331UL

static Coffee3Config_t s_xConfig;
static uint8_t s_ucValid;

uint16_t usCoffee3ConfigStorageMask(void)
{
	return (uint16_t)s_xConfig.ulStorageEnabledMask;
}

ConfigStoreResult_e xCoffee3ConfigSetStorageMask(uint16_t usMask)
{
	uint32_t aulWords[2];
	ConfigStoreResult_e xResult;
	BaseType_t xScheduled;

	xScheduled = (xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED);
	/* Same task-owned exclusion as the existing robot port writer. IRQs
	 * remain enabled; OTA owns Flash across its entire active upload. */
	if (xScheduled != pdFALSE) {
		vTaskSuspendAll();
	}
	if (ucAppOtaFlashIsActive() != 0U) {
		xResult = CONFIG_STORE_BUSY;
	} else if ((s_ucValid != 0U) &&
		(s_xConfig.ulStorageEnabledMask == usMask)) {
		xResult = CONFIG_STORE_OK;
	} else {
		aulWords[0] = COFFEE3_CONFIG_V1_MARK;
		aulWords[1] = usMask;
		xResult = xConfigStoreWrite(aulWords, 2U);
		if (xResult == CONFIG_STORE_OK) {
			s_xConfig.ulValidMark = aulWords[0];
			s_xConfig.ulStorageEnabledMask = aulWords[1];
			s_ucValid = 1U;
		} else {
			/* RAM survives, but the single Flash copy may have been erased. */
			s_ucValid = 0U;
		}
	}
	if (xScheduled != pdFALSE) {
		(void)xTaskResumeAll();
	}
	(void)xCoffee3LogPrintfOrder((xResult == CONFIG_STORE_OK) ?
		COFFEE3_LOG_LEVEL_INFO : COFFEE3_LOG_LEVEL_ERROR,
		COFFEE3_LOG_SOURCE_SERVER, COFFEE3_LOG_ORDER_SYSTEM,
		"Storage configuration save: mask=0x%04X result=%u",
		(unsigned int)usMask, (unsigned int)xResult);
	return xResult;
}

ConfigStoreResult_e xCoffee3ConfigInitialize(void)
{
	uint32_t aulWords[2];
	ConfigStoreResult_e xResult;

	s_ucValid = 0U;
	xResult = xConfigStoreRead(aulWords, 2U);
	if (xResult != CONFIG_STORE_OK) {
		return xResult;
	}
	if ((aulWords[0] == COFFEE3_CONFIG_V1_MARK) &&
		(aulWords[1] <= 0xFFFFUL)) {
		s_xConfig.ulValidMark = aulWords[0];
		s_xConfig.ulStorageEnabledMask = aulWords[1];
		s_ucValid = 1U;
		(void)xCoffee3LogPrintfOrder(COFFEE3_LOG_LEVEL_INFO,
			COFFEE3_LOG_SOURCE_SYSTEM, COFFEE3_LOG_ORDER_SYSTEM,
			"Storage configuration loaded: mask=0x%04X",
			(unsigned int)aulWords[1]);
		return CONFIG_STORE_OK;
	}
	(void)xCoffee3LogPrintfOrder(COFFEE3_LOG_LEVEL_WARNING,
		COFFEE3_LOG_SOURCE_SYSTEM, COFFEE3_LOG_ORDER_SYSTEM,
		"Storage configuration missing/invalid; saving default 0x0003");
	return xCoffee3ConfigSetStorageMask(0x0003U);
}
