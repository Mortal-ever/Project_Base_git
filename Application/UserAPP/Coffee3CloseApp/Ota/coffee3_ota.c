/**
  * @file      coffee3_ota.c
  * @brief     把 Coffee3 OTA 分区参数绑定到公共升级服务。
  * @author    WHong
  * @date      2026-09-24
  */

#include "coffee3_ota.h"

#include "coffee3_log.h"
#include "Ota/app_ota_http.h"
#include "crc.h"

#define COFFEE3_OTA_SRAM_START 0x20000000UL
#define COFFEE3_OTA_SRAM_END   0x20020000UL

/**
  * @brief  把公共 OTA 网络故障转交 Coffee3 日志服务。
  * @param[in] xSource 公共日志来源编号。
  * @param[in] lNativeError LwIP 或套接字原生错误码。
  */
static void prvReportLwipFailure(AppLogSourceId_t xSource, int32_t lNativeError)
{
	vCoffee3LogLwipResourceFailure((Coffee3LogSource_e)xSource, lNativeError);
}

/** @brief Coffee3 分区、CRC、超时与日志回调的固定 OTA 配置。 */
static const AppOtaConfig_t s_xCoffee3OtaConfig = {
	COFFEE3_OTA_METADATA_ADDRESS, COFFEE3_OTA_STAGING_ADDRESS,
	COFFEE3_OTA_STAGING_END, COFFEE3_OTA_APPLICATION_ADDRESS,
	COFFEE3_OTA_APPLICATION_END, COFFEE3_OTA_SRAM_START, COFFEE3_OTA_SRAM_END,
	COFFEE3_OTA_METADATA_MAGIC, FLASH_SECTOR_1, FLASH_SECTOR_7, 3U, 80U, 500U,
	(AppLogSourceId_t)COFFEE3_LOG_SOURCE_SYSTEM, &hcrc, prvReportLwipFailure
};

/**
  * @brief  使用 Coffee3 固定参数初始化公共 OTA 服务。
  * @retval APP_OTA_RESULT_OK 初始化成功或配置已经匹配。
  * @retval 其他值 公共 OTA 服务拒绝该配置。
  */
Coffee3OtaResult_e xCoffee3OtaInitialize(void)
{
	return xAppOtaInitialize(&s_xCoffee3OtaConfig);
}

/**
  * @brief  初始化 OTA 服务并开始写入暂存分区。
  * @retval APP_OTA_RESULT_OK 暂存写入会话已开始。
  * @retval APP_OTA_RESULT_BUSY 初始化失败或已有冲突操作。
  * @retval 其他值 公共 Flash OTA 开始操作失败。
  */
Coffee3OtaResult_e xCoffee3OtaBegin(void)
{
	if (xCoffee3OtaInitialize() != APP_OTA_RESULT_OK) return APP_OTA_RESULT_BUSY;
	return xAppOtaFlashBegin();
}

/**
  * @brief  向当前 OTA 暂存会话追加固件数据。
  * @param[in] pucData 固件数据缓冲区；ulLength 为零时可为空。
  * @param[in] ulLength 本次写入长度，单位字节。
  * @retval APP_OTA_RESULT_OK 数据已经写入暂存区。
  * @retval APP_OTA_RESULT_BUSY 初始化失败或服务不可用。
  * @retval 其他值 公共 Flash OTA 写入失败。
  */
Coffee3OtaResult_e xCoffee3OtaWrite(const uint8_t *pucData, uint32_t ulLength)
{
	if (xCoffee3OtaInitialize() != APP_OTA_RESULT_OK) return APP_OTA_RESULT_BUSY;
	return xAppOtaFlashWrite(pucData, ulLength);
}

/**
  * @brief  完成 OTA 暂存写入并输出镜像 CRC 与长度。
  * @param[out] pulCrc32 接收镜像 CRC32 的可选缓冲区，可为空。
  * @param[out] pulSize 接收镜像字节数的可选缓冲区，可为空。
  * @retval APP_OTA_RESULT_OK 镜像校验与元数据提交完成。
  * @retval APP_OTA_RESULT_BUSY 初始化失败或服务不可用。
  * @retval 其他值 公共 Flash OTA 完成操作失败。
  */
Coffee3OtaResult_e xCoffee3OtaFinish(uint32_t *pulCrc32, uint32_t *pulSize)
{
	if (xCoffee3OtaInitialize() != APP_OTA_RESULT_OK) return APP_OTA_RESULT_BUSY;
	return xAppOtaFlashFinish(pulCrc32, pulSize);
}

/**
  * @brief  终止当前 OTA 写入并释放公共 OTA 会话状态。
  */
void vCoffee3OtaAbort(void)
{
	(void)xCoffee3OtaInitialize();
	vAppOtaFlashAbort();
}

/**
  * @brief  查询 Flash OTA 写入会话是否活动。
  * @retval 1 OTA 写入会话活动。
  * @retval 0 没有活动写入会话。
  */
uint8_t ucCoffee3OtaIsActive(void)
{
	return ucAppOtaFlashIsActive();
}

/**
  * @brief  初始化 Coffee3 OTA 配置并创建公共 HTTP OTA 服务。
  * @retval pdPASS HTTP 回调已排队或监听服务已经存在。
  * @retval pdFAIL OTA 配置或 HTTP 服务初始化失败。
  * @note pdPASS 不表示 HTTP 监听套接字已成功接受连接。
  */
BaseType_t xCoffee3OtaHttpInitialize(void)
{
	if (xCoffee3OtaInitialize() != APP_OTA_RESULT_OK) return pdFAIL;
	return xAppOtaHttpInitialize();
}

/**
  * @brief  查询 HTTP 上传、复位等待或 Flash 写入会话是否占用 OTA。
  * @retval 1 上述任一会话活动。
  * @retval 0 没有活动会话；仅创建 HTTP 监听服务也可能返回零。
  */
uint8_t ucCoffee3OtaHttpIsActive(void)
{
	return ucAppOtaHttpIsActive();
}
