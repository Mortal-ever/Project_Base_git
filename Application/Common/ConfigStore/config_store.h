/**
  * @file      config_store.h
  * @brief     定义单副本配置区的读写接口与结果状态。
  * @author    WHong
  * @date      2026-09-24
  */

#ifndef CONFIG_STORE_H
#define CONFIG_STORE_H

#include <stdint.h>

/** @brief 配置区在内部 Flash 中的起始地址。 */
#define CONFIG_STORE_ADDRESS 0x08008000UL
/** @brief 配置区容量，单位为字节。 */
#define CONFIG_STORE_SIZE    0x00004000UL

/** @brief 表示配置区读写操作的完成状态。 */
typedef enum {
	CONFIG_STORE_OK = 0, /*!< 读写和回读校验均成功。 */
	CONFIG_STORE_INVALID = 1, /*!< 参数、长度或有效标记不合法。 */
	CONFIG_STORE_WRITE_FAILED = 2, /*!< 擦除、写入、加锁或回读校验失败。 */
	CONFIG_STORE_BUSY = 3 /*!< 保留给调用方的忙状态。 */
} ConfigStoreResult_e;

ConfigStoreResult_e xConfigStoreRead(uint32_t *pulWords, uint32_t ulCount);
ConfigStoreResult_e xConfigStoreWrite(const uint32_t *pulWords,
	uint32_t ulCount);

#endif
