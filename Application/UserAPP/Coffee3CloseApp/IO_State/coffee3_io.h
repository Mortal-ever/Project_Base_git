/**
  * @file      coffee3_io.h
  * @brief     定义本机 GPIO 与 Modbus IO 模块共用的全局镜像。
  * @author    WHong
  * @date      2026-09-24
  */

#ifndef COFFEE3_IO_H
#define COFFEE3_IO_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/** @brief STM32 本机输入和输出的点位数量。 */
#define COFFEE3_LOCAL_IO_COUNT               8U
/** @brief 电气设计中每个外部 IO 模块的点位数量。 */
#define COFFEE3_MODBUS_IO_COUNT              16U

/** @brief 定义本机数字输入在镜像数组中的索引。 */
typedef enum {
	COFFEE3_LOCAL_DI_DOOR_UPPER = 0, /*!< 取餐门上限位。 */
	COFFEE3_LOCAL_DI_DOOR_LOWER = 1, /*!< 取餐门下限位。 */
	COFFEE3_LOCAL_DI_COFFEE_WATER_HIGH = 2, /*!< 咖啡供水箱高液位。 */
	COFFEE3_LOCAL_DI_COFFEE_WATER_LOW = 3, /*!< 咖啡供水箱低液位。 */
	COFFEE3_LOCAL_DI_HOT_WATER_HIGH = 4, /*!< 热水箱高液位。 */
	COFFEE3_LOCAL_DI_HOT_WATER_LOW = 5 /*!< 热水箱低液位。 */
} Coffee3LocalInputPoint_e;

/** @brief 定义本机数字输出在镜像数组中的索引。 */
typedef enum {
	COFFEE3_LOCAL_DO_DOOR_UP = 0, /*!< 取餐门上升输出。 */
	COFFEE3_LOCAL_DO_DOOR_DOWN = 1, /*!< 取餐门下降输出。 */
	COFFEE3_LOCAL_DO_HOT_WATER_SUPPLY_VALVE = 2, /*!< 热水补水泵输出。 */
	COFFEE3_LOCAL_DO_COFFEE_WATER_PUMP = 3 /*!< 咖啡供水泵输出。 */
} Coffee3LocalOutputPoint_e;

/** @brief 定义外部 Unit 1 输入在镜像数组中的索引。 */
typedef enum {
	COFFEE3_EXTERNAL_DI_OUTPUT_FRONT_CUP = 0, /*!< 前侧成品杯检测。 */
	COFFEE3_EXTERNAL_DI_OUTPUT_REAR_CUP = 1, /*!< 后侧成品杯检测。 */
	COFFEE3_EXTERNAL_DI_OUTLET_CUP = 2, /*!< 取餐口杯检测。 */
	COFFEE3_EXTERNAL_DI_PURE_WATER_LOW = 3, /*!< 纯水桶低液位。 */
	COFFEE3_EXTERNAL_DI_WASTE_BIN_PRESENT = 4, /*!< 废渣桶在位。 */
	COFFEE3_EXTERNAL_DI_MILK_LOW = 10, /*!< 牛奶箱低液位。 */
	COFFEE3_EXTERNAL_DI_FRUIT_MILK_A_LOW = 11, /*!< 果奶 A 低液位。 */
	COFFEE3_EXTERNAL_DI_FRUIT_MILK_B_LOW = 12 /*!< 果奶 B 低液位。 */
} Coffee3ExternalInputPoint_e;

/** @brief 定义外部 Unit 2 输出在镜像数组中的索引。 */
typedef enum {
	COFFEE3_EXTERNAL_DO_WATER_HEATER_RELAY = 0, /*!< 热水加热继电器。 */
	COFFEE3_EXTERNAL_DO_MILK_VALVE = 3, /*!< 牛奶箱电磁阀。 */
	COFFEE3_EXTERNAL_DO_FRUIT_MILK_A_VALVE = 4, /*!< 果奶 A 电磁阀。 */
	COFFEE3_EXTERNAL_DO_FRUIT_MILK_B_VALVE = 5, /*!< 果奶 B 电磁阀。 */
	COFFEE3_EXTERNAL_DO_FRUIT_MILK_A_PUMP = 9, /*!< 果奶 A 蠕动泵。 */
	COFFEE3_EXTERNAL_DO_FRUIT_MILK_B_PUMP = 10 /*!< 果奶 B 蠕动泵。 */
} Coffee3ExternalOutputPoint_e;

/** @brief 保存工作流和设备任务使用的全部输入点。 */
typedef struct {
	uint8_t aucXPin[COFFEE3_LOCAL_IO_COUNT]; /*!< 低电平有效归一化后的八路本机输入。 */
	uint8_t aucMB1XPin[COFFEE3_MODBUS_IO_COUNT]; /*!< Unit 1 的十六路输入值。 */
	uint8_t aucMB2XPin[COFFEE3_MODBUS_IO_COUNT]; /*!< 保留的第二组输入，不代表已安装模块。 */
} Coffee3InputIo_t;

/** @brief 保存工作流和设备任务使用的全部输出点。 */
typedef struct {
	uint8_t aucYPin[COFFEE3_LOCAL_IO_COUNT]; /*!< 八路本机输出命令电平，不是触点反馈。 */
	uint8_t aucMB1YPin[COFFEE3_MODBUS_IO_COUNT]; /*!< 旧布局保留的第一组模块输出。 */
	uint8_t aucMB2YPin[COFFEE3_MODBUS_IO_COUNT]; /*!< Unit 2 的十六路输出回读值。 */
} Coffee3OutputIo_t;

/** @brief 保存全局 IO 镜像及其更新时间与有效性元数据。 */
typedef struct {
	Coffee3InputIo_t xInput; /*!< 本产品拥有的本机与模块输入镜像。 */
	Coffee3OutputIo_t xOutput; /*!< 本产品拥有的本机与模块输出镜像。 */
	uint32_t ulVersion; /*!< 每次提交后递增；一致读取应使用快照接口。 */
	uint32_t ulLocalUpdateTick; /*!< 最近本机 GPIO 采样或写入的 RTOS 节拍。 */
	uint32_t aulModbusUpdateTick[2]; /*!< 两个模块最近成功提交的 RTOS 节拍。 */
	uint8_t aucModbusValid[2]; /*!< 两个模块的数据有效标志，全零也可能有效。 */
} Coffee3IoState_t;

/** @brief 所有 Coffee3 应用模块可访问的全局 IO 镜像。 */
extern Coffee3IoState_t g_xCoffee3Io;
void vCoffee3IoInitialize(void);
void vCoffee3IoRefreshLocal(void);
uint8_t ucCoffee3IoSetLocalOutput(uint8_t ucIndex, uint8_t ucValue);
uint8_t ucCoffee3IoApplyLocalDebugMask(uint16_t usMask);
void vCoffee3IoCommitModbusInput(const uint8_t *pucInputs);
void vCoffee3IoCommitModbusOutputImage(const uint8_t *pucOutputs);
void vCoffee3IoGetSnapshot(Coffee3IoState_t *pxSnapshot);

#ifdef __cplusplus
}
#endif

#endif /* COFFEE3_IO_H */
