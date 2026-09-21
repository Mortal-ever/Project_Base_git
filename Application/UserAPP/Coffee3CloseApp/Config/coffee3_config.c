/* Target-private configuration. No OTA or robot port data belongs here. */
#include "coffee3_config.h"
#include "coffee3_log.h"
#include "Ota/app_ota_flash.h"
#include "FreeRTOS.h"
#include "task.h"
#include <stddef.h>

#define COFFEE3_CONFIG_WORD_COUNT \
	(3U + COFFEE3_CONFIG_FRUIT_CHANNEL_COUNT)

static Coffee3Config_t s_xConfig;
static uint8_t s_ucValid;

/*-----------------------------------------------------------*/
static void prvEncodeConfig(const Coffee3Config_t *pxConfig,
	uint32_t *pulWords)
{
	uint8_t ucIndex;

	pulWords[0] = COFFEE3_CONFIG_MARK;
	pulWords[1] = pxConfig->ulStorageEnabledMask;
	for (ucIndex = 0U; ucIndex < COFFEE3_CONFIG_FRUIT_CHANNEL_COUNT;
		ucIndex++) {
		pulWords[2U + ucIndex] = pxConfig->aulFruitCoefficient[ucIndex];
	}
	pulWords[2U + COFFEE3_CONFIG_FRUIT_CHANNEL_COUNT] =
		pxConfig->ulIceSlopeMsPerGram;
}

/*-----------------------------------------------------------*/
static ConfigStoreResult_e prvSaveConfig(const Coffee3Config_t *pxConfig)
{
	uint32_t aulWords[COFFEE3_CONFIG_WORD_COUNT];
	ConfigStoreResult_e xResult;
	BaseType_t xScheduled;

	prvEncodeConfig(pxConfig, aulWords);
	xScheduled = (xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED);
	if (xScheduled != pdFALSE) {
		vTaskSuspendAll();
	}
	if (ucAppOtaFlashIsActive() != 0U) {
		xResult = CONFIG_STORE_BUSY;
	} else {
		xResult = xConfigStoreWrite(aulWords, COFFEE3_CONFIG_WORD_COUNT);
	}
	if (xScheduled != pdFALSE) {
		(void)xTaskResumeAll();
	}
	return xResult;
}

uint16_t usCoffee3ConfigStorageMask(void)
{
	return (uint16_t)s_xConfig.ulStorageEnabledMask;
}

/*-----------------------------------------------------------*/
uint16_t usCoffee3ConfigFruitCoefficient(uint8_t ucChannel)
{
	if ((ucChannel == 0U) ||
		(ucChannel > COFFEE3_CONFIG_FRUIT_CHANNEL_COUNT)) {
		return COFFEE3_CONFIG_FRUIT_COEFFICIENT_DEFAULT;
	}
	return (uint16_t)s_xConfig.aulFruitCoefficient[ucChannel - 1U];
}

/*-----------------------------------------------------------*/
uint16_t usCoffee3ConfigIceSlopeMsPerGram(void)
{
	if (s_ucValid == 0U) {
		return COFFEE3_CONFIG_ICE_SLOPE_DEFAULT_MS_PER_GRAM;
	}
	return (uint16_t)s_xConfig.ulIceSlopeMsPerGram;
}

ConfigStoreResult_e xCoffee3ConfigSetStorageMask(uint16_t usMask)
{
	Coffee3Config_t xNext;
	ConfigStoreResult_e xResult;

	if ((s_ucValid != 0U) &&
		(s_xConfig.ulStorageEnabledMask == usMask)) {
		xResult = CONFIG_STORE_OK;
	} else {
		xNext = s_xConfig;
		xNext.ulValidMark = COFFEE3_CONFIG_MARK;
		xNext.ulStorageEnabledMask = usMask;
		xResult = prvSaveConfig(&xNext);
		if (xResult == CONFIG_STORE_OK) {
			s_xConfig = xNext;
			s_ucValid = 1U;
		}
	}
	(void)xCoffee3LogPrintfOrder((xResult == CONFIG_STORE_OK) ?
		COFFEE3_LOG_LEVEL_INFO : COFFEE3_LOG_LEVEL_ERROR,
		COFFEE3_LOG_SOURCE_SERVER, COFFEE3_LOG_ORDER_SYSTEM,
		"Storage configuration save: mask=0x%04X result=%u",
		(unsigned int)usMask, (unsigned int)xResult);
	return xResult;
}

/*-----------------------------------------------------------*/
ConfigStoreResult_e xCoffee3ConfigSetFruitCoefficients(
	const uint16_t *pusCoefficients)
{
	Coffee3Config_t xNext;
	ConfigStoreResult_e xResult;
	uint8_t ucChanged;
	uint8_t ucIndex;

	if (pusCoefficients == NULL) {
		return CONFIG_STORE_INVALID;
	}
	xNext = s_xConfig;
	xNext.ulValidMark = COFFEE3_CONFIG_MARK;
	ucChanged = (s_ucValid == 0U) ? 1U : 0U;
	for (ucIndex = 0U; ucIndex < COFFEE3_CONFIG_FRUIT_CHANNEL_COUNT;
		ucIndex++) {
		if ((pusCoefficients[ucIndex] <
			COFFEE3_CONFIG_FRUIT_COEFFICIENT_MIN) ||
			(pusCoefficients[ucIndex] >
			COFFEE3_CONFIG_FRUIT_COEFFICIENT_MAX)) {
			return CONFIG_STORE_INVALID;
		}
		if (xNext.aulFruitCoefficient[ucIndex] !=
			pusCoefficients[ucIndex]) {
			ucChanged = 1U;
		}
		xNext.aulFruitCoefficient[ucIndex] = pusCoefficients[ucIndex];
	}
	if (ucChanged == 0U) {
		return CONFIG_STORE_OK;
	}
	xResult = prvSaveConfig(&xNext);
	if (xResult == CONFIG_STORE_OK) {
		s_xConfig = xNext;
		s_ucValid = 1U;
	}
	return xResult;
}

/*-----------------------------------------------------------*/
ConfigStoreResult_e xCoffee3ConfigSetIceSlopeMsPerGram(uint16_t usSlope)
{
	Coffee3Config_t xNext;
	ConfigStoreResult_e xResult;

	/* A zero written by a legacy host restores the documented default. */
	if (usSlope == 0U) {
		usSlope = COFFEE3_CONFIG_ICE_SLOPE_DEFAULT_MS_PER_GRAM;
	}
	if ((usSlope < COFFEE3_CONFIG_ICE_SLOPE_MIN_MS_PER_GRAM) ||
		(usSlope > COFFEE3_CONFIG_ICE_SLOPE_MAX_MS_PER_GRAM) ||
		(s_ucValid == 0U)) {
		return CONFIG_STORE_INVALID;
	}
	if (s_xConfig.ulIceSlopeMsPerGram == usSlope) {
		return CONFIG_STORE_OK;
	}
	xNext = s_xConfig;
	xNext.ulIceSlopeMsPerGram = usSlope;
	xResult = prvSaveConfig(&xNext);
	if (xResult == CONFIG_STORE_OK) {
		s_xConfig = xNext;
	}
	(void)xCoffee3LogPrintfOrder((xResult == CONFIG_STORE_OK) ?
		COFFEE3_LOG_LEVEL_INFO : COFFEE3_LOG_LEVEL_ERROR,
		COFFEE3_LOG_SOURCE_SERVER, COFFEE3_LOG_ORDER_DEBUG,
		"Ice slope save: ms_per_g=%u result=%u",
		(unsigned int)usSlope, (unsigned int)xResult);
	return xResult;
}

ConfigStoreResult_e xCoffee3ConfigInitialize(void)
{
	uint32_t aulWords[COFFEE3_CONFIG_WORD_COUNT];
	Coffee3Config_t xLoaded;
	ConfigStoreResult_e xResult;
	uint8_t ucNeedsSave;
	uint8_t ucIndex;

	s_ucValid = 0U;
	xResult = xConfigStoreRead(aulWords, COFFEE3_CONFIG_WORD_COUNT);
	if (xResult != CONFIG_STORE_OK) {
		return xResult;
	}
	if ((aulWords[0] == COFFEE3_CONFIG_MARK) &&
		(aulWords[1] <= 0xFFFFUL)) {
		xLoaded.ulValidMark = COFFEE3_CONFIG_MARK;
		xLoaded.ulStorageEnabledMask = aulWords[1];
		ucNeedsSave = 0U;
		for (ucIndex = 0U; ucIndex < COFFEE3_CONFIG_FRUIT_CHANNEL_COUNT;
			ucIndex++) {
			if ((aulWords[2U + ucIndex] <
				COFFEE3_CONFIG_FRUIT_COEFFICIENT_MIN) ||
				(aulWords[2U + ucIndex] >
				COFFEE3_CONFIG_FRUIT_COEFFICIENT_MAX)) {
				xLoaded.aulFruitCoefficient[ucIndex] =
					COFFEE3_CONFIG_FRUIT_COEFFICIENT_DEFAULT;
				ucNeedsSave = 1U;
			} else {
				xLoaded.aulFruitCoefficient[ucIndex] =
					aulWords[2U + ucIndex];
			}
		}
		if ((aulWords[2U + COFFEE3_CONFIG_FRUIT_CHANNEL_COUNT] <
			COFFEE3_CONFIG_ICE_SLOPE_MIN_MS_PER_GRAM) ||
			(aulWords[2U + COFFEE3_CONFIG_FRUIT_CHANNEL_COUNT] >
			COFFEE3_CONFIG_ICE_SLOPE_MAX_MS_PER_GRAM)) {
			xLoaded.ulIceSlopeMsPerGram =
				COFFEE3_CONFIG_ICE_SLOPE_DEFAULT_MS_PER_GRAM;
			ucNeedsSave = 1U;
		} else {
			xLoaded.ulIceSlopeMsPerGram =
				aulWords[2U + COFFEE3_CONFIG_FRUIT_CHANNEL_COUNT];
		}
		s_xConfig = xLoaded;
		s_ucValid = 1U;
		if (ucNeedsSave != 0U) {
			xResult = prvSaveConfig(&xLoaded);
			if (xResult != CONFIG_STORE_OK) {
				return xResult;
			}
		}
		(void)xCoffee3LogPrintfOrder(COFFEE3_LOG_LEVEL_INFO,
			COFFEE3_LOG_SOURCE_SYSTEM, COFFEE3_LOG_ORDER_SYSTEM,
			"Configuration loaded: storage=0x%04X fruit=%u/%u/%u/%u/%u/%u",
			(unsigned int)aulWords[1],
			(unsigned int)s_xConfig.aulFruitCoefficient[0],
			(unsigned int)s_xConfig.aulFruitCoefficient[1],
			(unsigned int)s_xConfig.aulFruitCoefficient[2],
			(unsigned int)s_xConfig.aulFruitCoefficient[3],
			(unsigned int)s_xConfig.aulFruitCoefficient[4],
			(unsigned int)s_xConfig.aulFruitCoefficient[5]);
		(void)xCoffee3LogWriteField(COFFEE3_LOG_LEVEL_INFO,
			COFFEE3_LOG_SOURCE_SYSTEM, "ICE_SLOPE_LOADED", 0,
			"ms_per_g", (int32_t)s_xConfig.ulIceSlopeMsPerGram);
		return CONFIG_STORE_OK;
	}
	(void)xCoffee3LogPrintfOrder(COFFEE3_LOG_LEVEL_WARNING,
		COFFEE3_LOG_SOURCE_SYSTEM, COFFEE3_LOG_ORDER_SYSTEM,
		"Configuration missing/invalid; saving defaults");
	s_xConfig.ulValidMark = COFFEE3_CONFIG_MARK;
	s_xConfig.ulStorageEnabledMask = 0x0003U;
	for (ucIndex = 0U; ucIndex < COFFEE3_CONFIG_FRUIT_CHANNEL_COUNT;
		ucIndex++) {
		s_xConfig.aulFruitCoefficient[ucIndex] =
			COFFEE3_CONFIG_FRUIT_COEFFICIENT_DEFAULT;
	}
	s_xConfig.ulIceSlopeMsPerGram =
		COFFEE3_CONFIG_ICE_SLOPE_DEFAULT_MS_PER_GRAM;
	xResult = prvSaveConfig(&s_xConfig);
	if (xResult == CONFIG_STORE_OK) {
		s_ucValid = 1U;
	}
	return xResult;
}
