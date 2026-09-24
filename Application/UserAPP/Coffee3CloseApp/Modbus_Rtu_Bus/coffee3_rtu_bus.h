/**
  * @file      coffee3_rtu_bus.h
  * @brief     定义 Coffee3 四路串口总线拥有者任务的公共接口。
  * @author    WHong
  * @date      2026-09-24
  */

#ifndef COFFEE3_RTU_BUS_H
#define COFFEE3_RTU_BUS_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "FreeRTOS.h"
#include "coffee3_app_config.h"
#include "modbus_port.h"
#include "stm32f4xx_hal.h"

/** @brief 保存一路 Coffee3 物理串口总线的固定配置。 */
typedef struct {
    UART_HandleTypeDef *pxUart; /*!< 借用的 HAL UART 句柄，在任务生命周期内保持有效。 */
    const char *pcName; /*!< 借用的静态诊断名称，存储期长于总线任务。 */
    uint32_t ulDefaultBaudRate; /*!< 串口默认波特率，单位为 bit/s。 */
    uint8_t ucBusId; /*!< 物理路由编号 Bus2 至 Bus5，不是数组下标。 */
    uint8_t ucProtocolId; /*!< 该总线唯一使用的线协议编号。 */
} Coffee3RtuBusConfig_t;

/** @brief 保存一路 RTU 总线拥有者任务对外公开的运行状态。 */
typedef struct {
	uint32_t ulCommandCount; /*!< 通过路由和协议校验、由总线拥有者开始处理的前台命令累计数量。 */
	uint32_t ulErrorCount; /*!< 前台非取消、非维护命令的累计失败数量。 */
	uint32_t ulCurrentBaudRate; /*!< 总线初始化后记录的当前波特率，单位为 bit/s。 */
	int32_t lLastResult; /*!< 最近一条前台命令的 ModbusPortResult_e 原始结果。 */
	uint8_t ucReady; /*!< 总线协议通道已创建并可接收命令时为 1。 */
	uint8_t ucActiveDevice; /*!< 活动设备编号；空闲也写 0，与设备 0 的值相同。 */
} Coffee3RtuBusStatus_t;

/** @brief Bus2 至 Bus5 按紧凑下标排列的公开运行状态。 */
extern Coffee3RtuBusStatus_t
	g_axCoffee3RtuBusStatus[COFFEE3_RTU_BUS_COUNT];

HAL_StatusTypeDef xCoffee3SerialApplyDefaults(void);

HAL_StatusTypeDef xCoffee3LogSerialApplyDefault(void);

BaseType_t xCoffee3RtuBusInitialize(void);

const Coffee3RtuBusConfig_t *pxCoffee3RtuBusGetConfig(uint8_t ucIndex);

void vCoffee3RtuBusTask(void *pvArgument);

#ifdef __cplusplus
}
#endif

#endif /* COFFEE3_RTU_BUS_H */
