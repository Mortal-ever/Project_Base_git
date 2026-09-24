/**
  * @file      coffee3_device_image.h
  * @brief     定义 Coffee3 自有设备状态镜像与提交接口。
  * @author    WHong
  * @date      2026-09-24
  */

#ifndef COFFEE3_DEVICE_IMAGE_H
#define COFFEE3_DEVICE_IMAGE_H

#ifdef __cplusplus
extern "C" {
#endif

#include "coffee_machine_m50.h"
#include "cup_lid_shengshu.h"
#include "ice_machine_modbus.h"
#include "io_module_modbus_digital.h"
#include "power_meter_ddsu666.h"
#include "scale_bsq_dg_v2.h"
#include "syrup_machine_modbus.h"

/** @brief 保存咖啡机状态寄存器的产品共享镜像。 */
typedef struct {
	uint16_t ausStatus[24]; /*!< M50 状态寄存器值；由 RTU 回调更新。 */
} Coffee3CoffeeMachineImage_t;

/** @brief 保存落杯机与落盖机任务寄存器及线圈镜像。 */
typedef struct {
	uint16_t ausCupTask[2]; /*!< 落杯机两个任务槽的寄存器值。 */
	uint16_t ausLidTask[2]; /*!< 落盖机两个任务槽的寄存器值。 */
	uint8_t aucCupCoils[10]; /*!< 落杯机十路线圈镜像。 */
	uint8_t aucLidCoils[10]; /*!< 落盖机十路线圈镜像。 */
} Coffee3CupLidImage_t;

/** @brief 以下设备镜像由 RTU 写入，工作流与服务端直接读取。M50 在临界区内提交，杯盖镜像分步复制；糖浆、制冰、秤和电表由驱动逐字段写入，没有锁或版本提交保护，跨字段读取不能视为原子快照。 */
extern Coffee3CoffeeMachineImage_t g_xCoffee3CoffeeMachineImage;
extern Coffee3CupLidImage_t g_xCoffee3CupLidImage;
extern SyrupMachineModbusImage_t g_xCoffee3SyrupImage;
extern IceMachineModbusImage_t g_xCoffee3IceImage;
extern ScaleBsqDgV2Image_t g_xCoffee3ScaleImage;
extern PowerMeterDdsu666Image_t g_xCoffee3PowerMeterImage;

void vCoffee3DeviceImageCommitM50(
	const CoffeeMachineM50Image_t *pxStatus);
void vCoffee3DeviceImageCommitCup(const CupLidShengShuImage_t *pxImage,
	uint8_t ucRefresh, uint8_t ucSlot);
void vCoffee3DeviceImageCommitLid(const CupLidShengShuImage_t *pxImage,
	uint8_t ucRefresh, uint8_t ucSlot);

#ifdef __cplusplus
}
#endif

#endif /* COFFEE3_DEVICE_IMAGE_H */
