/**
  * @file      app_ota_flash.h
  * @brief     定义与产品无关的固件暂存区 Flash 写入接口。
  * @author    WHong
  * @date      2026-09-24
  *
  * @details   调用方提供固定的目标分区与诊断绑定。本模块不持有 RTOS
  *            对象，只为当前上传保留一个不足四字节的流式尾部。
  */

#ifndef APP_OTA_FLASH_H
#define APP_OTA_FLASH_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "FreeRTOS.h"
#include "Log/app_log.h"

/** @brief 表示固件暂存写入和校验的结果。 */
typedef enum {
	APP_OTA_RESULT_OK = 0, /*!< 操作成功完成。 */
	APP_OTA_RESULT_INVALID_ARG = 1, /*!< 参数、会话状态或长度不合法。 */
	APP_OTA_RESULT_BUSY = 2, /*!< 已有上传会话占用写入器。 */
	APP_OTA_RESULT_REJECTED = 3, /*!< 请求被上层策略拒绝。 */
	APP_OTA_RESULT_HAL_ERROR = 4, /*!< Flash 或 CRC 的 HAL 操作失败。 */
	APP_OTA_RESULT_INVALID_IMAGE = 5, /*!< 固件向量表不符合目标内存约束。 */
	APP_OTA_RESULT_CRC_ERROR = 6 /*!< 两次 CRC 计算结果不一致。 */
} AppOtaResult_e;

/** @brief 通过产品诊断端口报告一次 LwIP 资源故障。 */
typedef void (*AppOtaLwipFailureHook_t)(AppLogSourceId_t xSource,
	int32_t lNativeError);

/**
  * @brief  保存一个产品目标不可变的 OTA 分区和服务绑定。
  * @note   名称以 End 结尾的地址均为不包含在范围内的上界。
  */
typedef struct {
	uint32_t ulMetadataAddress; /*!< 引导元数据记录的起始地址。 */
	uint32_t ulStagingAddress; /*!< 接收固件暂存槽的首地址。 */
	uint32_t ulStagingEnd; /*!< 暂存槽不包含在内的结束地址。 */
	uint32_t ulApplicationAddress; /*!< 引导程序复制应用的目标首地址。 */
	uint32_t ulApplicationEnd; /*!< 应用区不包含在内的结束地址。 */
	uint32_t ulSramStart; /*!< 合法主栈指针的最低 SRAM 地址。 */
	uint32_t ulSramEnd; /*!< 合法主栈范围不包含在内的上界。 */
	uint32_t ulMetadataMagic; /*!< 引导程序识别的首个元数据字。 */
	uint32_t ulMetadataSector; /*!< 元数据所在的 HAL Flash 扇区号。 */
	uint32_t ulStagingFirstSector; /*!< 暂存区需要擦除的首个 HAL 扇区。 */
	uint32_t ulStagingSectorCount; /*!< 暂存区连续擦除的扇区数量。 */
	uint16_t usHttpPort; /*!< Raw LwIP HTTP 监听端口。 */
	uint16_t usResetDelayMs; /*!< 成功响应开始后到复位的延时，单位为毫秒。 */
	AppLogSourceId_t xLogSource; /*!< OTA 记录使用的产品日志来源。 */
	void *pvCrc; /*!< 已初始化的产品 HAL CRC 句柄。 */
	AppOtaLwipFailureHook_t pxLwipFailureHook;
		/*!< 可选的产品 LwIP 资源故障诊断回调。 */
} AppOtaConfig_t;

AppOtaResult_e xAppOtaInitialize(const AppOtaConfig_t *pxConfig);

AppOtaResult_e xAppOtaFlashBegin(void);

AppOtaResult_e xAppOtaFlashWrite(const uint8_t *pucData,
	uint32_t ulLength);

AppOtaResult_e xAppOtaFlashFinish(uint32_t *pulCrc32, uint32_t *pulSize);

void vAppOtaFlashAbort(void);

uint8_t ucAppOtaFlashIsActive(void);

const AppOtaConfig_t *pxAppOtaGetConfig(void);

#ifdef __cplusplus
}
#endif

#endif /* APP_OTA_FLASH_H */
