/**
  * @file      coffee3_config.c
  * @brief     实现 Coffee3 产品配置的校验、持久化与运行时访问。
  * @author    WHong
  * @date      2026-09-24
  */
#include "coffee3_config.h"
#include "coffee3_log.h"
#include "Ota/app_ota_flash.h"
#include "FreeRTOS.h"
#include "task.h"
#include <stddef.h>

#define COFFEE3_CONFIG_WORD_COUNT \
	(3U + COFFEE3_CONFIG_FRUIT_CHANNEL_COUNT)

static Coffee3Config_t s_xConfig; /*!< 当前生效的产品配置运行时镜像。 */
static uint8_t s_ucValid; /*!< 非零表示运行时镜像已通过读取或保存确认。 */

/*-----------------------------------------------------------*/
/**
  * @brief  把运行时配置编码为配置存储使用的连续字数组。
  * @param[in] pxConfig 待编码的有效配置。
  * @param[out] pulWords 容量至少为 COFFEE3_CONFIG_WORD_COUNT 的目标数组。
  */
static void prvEncodeConfig(const Coffee3Config_t *pxConfig,
	uint32_t *pulWords)
{
	uint8_t ucIndex; /*!< 当前编码的果奶通道索引。 */

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
/**
  * @brief  在防止 OTA 并发写入的条件下保存完整配置。
  * @param[in] pxConfig 待持久化的配置。
  * @retval CONFIG_STORE_OK 配置写入成功。
  * @retval CONFIG_STORE_BUSY OTA 正在写 Flash。
  * @retval 其他值 配置存储层写入失败。
  * @note 调度器运行时会暂停任务调度，但不改变中断状态。
  */
static ConfigStoreResult_e prvSaveConfig(const Coffee3Config_t *pxConfig)
{
	uint32_t aulWords[COFFEE3_CONFIG_WORD_COUNT]; /*!< 配置存储字数组。 */
	ConfigStoreResult_e xResult; /*!< 本次保存操作的最终结果。 */
	BaseType_t xScheduled; /*!< 调度器是否已启动的快照。 */

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

/**
  * @brief  读取当前允许使用的储位位图。
  * @retval uint16_t 低十六位储位允许掩码。
  */
uint16_t usCoffee3ConfigStorageMask(void)
{
	return (uint16_t)s_xConfig.ulStorageEnabledMask;
}

/*-----------------------------------------------------------*/
/**
  * @brief  读取指定果奶通道的流量修正系数。
  * @param[in] ucChannel 果奶通道号，范围为 1 至 6。
  * @retval uint16_t 有效通道的配置值；通道无效时返回默认值。
  */
uint16_t usCoffee3ConfigFruitCoefficient(uint8_t ucChannel)
{
	if ((ucChannel == 0U) ||
		(ucChannel > COFFEE3_CONFIG_FRUIT_CHANNEL_COUNT)) {
		return COFFEE3_CONFIG_FRUIT_COEFFICIENT_DEFAULT;
	}
	return (uint16_t)s_xConfig.aulFruitCoefficient[ucChannel - 1U];
}

/*-----------------------------------------------------------*/
/**
  * @brief  读取出冰目标每克对应的开阀时间。
  * @retval uint16_t 每克开阀毫秒数；配置无效时返回默认值。
  */
uint16_t usCoffee3ConfigIceSlopeMsPerGram(void)
{
	if (s_ucValid == 0U) {
		return COFFEE3_CONFIG_ICE_SLOPE_DEFAULT_MS_PER_GRAM;
	}
	return (uint16_t)s_xConfig.ulIceSlopeMsPerGram;
}

/**
  * @brief  保存新的储位允许掩码并在成功后更新运行时镜像。
  * @param[in] usMask 低十六位储位允许掩码。
  * @retval CONFIG_STORE_OK 配置未变化或已保存成功。
  * @retval 其他值 配置存储层拒绝或写入失败。
  */
ConfigStoreResult_e xCoffee3ConfigSetStorageMask(uint16_t usMask)
{
	Coffee3Config_t xNext; /*!< 应用新掩码后的候选配置。 */
	ConfigStoreResult_e xResult; /*!< 候选配置的保存结果。 */

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
/**
  * @brief  校验并保存六个果奶通道的流量修正系数。
  * @param[in] pusCoefficients 六个通道的系数数组；不可为空。
  * @retval CONFIG_STORE_OK 系数未变化或已保存成功。
  * @retval CONFIG_STORE_INVALID 指针为空或任一系数超出允许范围。
  * @retval 其他值 配置存储层写入失败。
  */
ConfigStoreResult_e xCoffee3ConfigSetFruitCoefficients(
	const uint16_t *pusCoefficients)
{
	Coffee3Config_t xNext; /*!< 应用新系数后的候选配置。 */
	ConfigStoreResult_e xResult; /*!< 候选配置的保存结果。 */
	uint8_t ucChanged; /*!< 非零表示至少一个系数或有效状态发生变化。 */
	uint8_t ucIndex; /*!< 当前校验的果奶通道索引。 */

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
/**
  * @brief  校验并保存出冰目标每克对应的开阀时间。
  * @param[in] usSlope 每克开阀毫秒数；零表示恢复默认值。
  * @retval CONFIG_STORE_OK 数值未变化或已保存成功。
  * @retval CONFIG_STORE_INVALID 配置未初始化或数值超出允许范围。
  * @retval 其他值 配置存储层写入失败。
  */
ConfigStoreResult_e xCoffee3ConfigSetIceSlopeMsPerGram(uint16_t usSlope)
{
	Coffee3Config_t xNext; /*!< 应用新斜率后的候选配置。 */
	ConfigStoreResult_e xResult; /*!< 候选配置的保存结果。 */

	/* 兼容旧主机：写入零表示恢复文档规定的默认值。 */
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

/**
  * @brief  从配置存储读取 Coffee3 配置并修复无效字段。
  * @retval CONFIG_STORE_OK 已载入有效配置或已写入默认配置。
  * @retval 其他值 初次读取、字段修复保存或默认配置保存失败。
  * @note 初次读取失败会直接返回，不会用默认值覆盖存储区。
  */
ConfigStoreResult_e xCoffee3ConfigInitialize(void)
{
	uint32_t aulWords[COFFEE3_CONFIG_WORD_COUNT]; /*!< 从存储区读取的原始字数组。 */
	Coffee3Config_t xLoaded; /*!< 校验并修正后的候选配置。 */
	ConfigStoreResult_e xResult; /*!< 当前存储操作结果。 */
	uint8_t ucNeedsSave; /*!< 非零表示至少一个字段已被默认值修复。 */
	uint8_t ucIndex; /*!< 当前处理的果奶通道索引。 */

	/* 步骤 1：读取固定长度记录；读取失败时保留无效状态并直接返回。 */
	s_ucValid = 0U;
	xResult = xConfigStoreRead(aulWords, COFFEE3_CONFIG_WORD_COUNT);
	if (xResult != CONFIG_STORE_OK) {
		return xResult;
	}
	if ((aulWords[0] == COFFEE3_CONFIG_MARK) &&
		(aulWords[1] <= 0xFFFFUL)) {
		/* 步骤 2：载入有效记录，逐字段修复越界的标定参数。 */
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
			/* 步骤 3：把修复后的字段回写，确保下次启动直接得到有效值。 */
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
	/* 步骤 4：格式标记无效时构造默认配置并首次保存。 */
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
