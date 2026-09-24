/**
  * @file      coffee3_device_image.c
  * @brief     保存 Coffee3 设备状态镜像并实现提交接口。
  * @author    WHong
  * @date      2026-09-24
  */

#include "coffee3_device_image.h"

#include <stddef.h>
#include <string.h>

#include "coffee3_app_config.h"
#include "FreeRTOS.h"
#include "task.h"

COFFEE3_CCM_DATA
Coffee3CoffeeMachineImage_t g_xCoffee3CoffeeMachineImage; /*!< 咖啡机共享状态镜像。 */
COFFEE3_CCM_DATA
Coffee3CupLidImage_t g_xCoffee3CupLidImage; /*!< 落杯机与落盖机共享状态镜像。 */
COFFEE3_CCM_DATA
SyrupMachineModbusImage_t g_xCoffee3SyrupImage; /*!< 糖浆机共享状态镜像；驱动逐字段写入，跨字段读取不是原子快照。 */
COFFEE3_CCM_DATA
IceMachineModbusImage_t g_xCoffee3IceImage; /*!< 制冰机共享状态镜像；驱动逐字段写入，跨字段读取不是原子快照。 */
COFFEE3_CCM_DATA
ScaleBsqDgV2Image_t g_xCoffee3ScaleImage; /*!< 称重设备共享状态镜像；驱动逐字段写入，跨字段读取不是原子快照。 */
COFFEE3_CCM_DATA
PowerMeterDdsu666Image_t g_xCoffee3PowerMeterImage; /*!< 电能表共享状态镜像；驱动逐字段写入，跨字段读取不是原子快照。 */

/**
  * @brief  在临界区内提交咖啡机 M50 的完整状态镜像。
  * @param[in] pxStatus RTU 读取完成的咖啡机镜像；为空时忽略。
  * @note 工作流和服务端直接读取共享镜像，调用方需按自身一致性要求取样。
  */
void vCoffee3DeviceImageCommitM50(
	const CoffeeMachineM50Image_t *pxStatus)
{
	uint8_t ucIndex; /*!< 当前复制的咖啡机状态寄存器索引。 */

	if (pxStatus == NULL) {
		return;
	}
	taskENTER_CRITICAL();
	memset(g_xCoffee3CoffeeMachineImage.ausStatus, 0,
		sizeof(g_xCoffee3CoffeeMachineImage.ausStatus));
	for (ucIndex = 0U; ucIndex < COFFEE_MACHINE_M50_STATUS_CAPACITY; ucIndex++) {
		g_xCoffee3CoffeeMachineImage.ausStatus[ucIndex] =
			pxStatus->ausStatus[ucIndex];
	}
	taskEXIT_CRITICAL();
}

/**
  * @brief  提交落杯机完整刷新或单槽任务寄存器。
  * @param[in] pxImage RTU 事务产生的落杯机镜像；不可为空。
  * @param[in] ucRefresh 非零时提交完整任务与线圈镜像。
  * @param[in] ucSlot 必须为 0 或 1；非完整刷新时还决定更新哪个任务槽。
  * @note 本函数没有临界区保护，共享镜像读取方可能观察到分步复制过程。
  */
void vCoffee3DeviceImageCommitCup(const CupLidShengShuImage_t *pxImage,
	uint8_t ucRefresh, uint8_t ucSlot)
{
	if ((pxImage == NULL) || (ucSlot >= 2U)) {
		return;
	}
	if (ucRefresh != 0U) {
		memcpy(g_xCoffee3CupLidImage.ausCupTask, pxImage->ausTask,
		sizeof(pxImage->ausTask));
		memcpy(g_xCoffee3CupLidImage.aucCupCoils, pxImage->aucCoils,
		sizeof(pxImage->aucCoils));
	} else {
		g_xCoffee3CupLidImage.ausCupTask[ucSlot] =
			pxImage->ausTask[ucSlot];
	}
}

/**
  * @brief  提交落盖机完整刷新或单槽任务寄存器。
  * @param[in] pxImage RTU 事务产生的落盖机镜像；不可为空。
  * @param[in] ucRefresh 非零时提交完整任务与线圈镜像。
  * @param[in] ucSlot 必须为 0 或 1；非完整刷新时还决定更新哪个任务槽。
  * @note 本函数没有临界区保护，共享镜像读取方可能观察到分步复制过程。
  */
void vCoffee3DeviceImageCommitLid(const CupLidShengShuImage_t *pxImage,
	uint8_t ucRefresh, uint8_t ucSlot)
{
	if ((pxImage == NULL) || (ucSlot >= 2U)) {
		return;
	}
	if (ucRefresh != 0U) {
		memcpy(g_xCoffee3CupLidImage.ausLidTask, pxImage->ausTask,
			sizeof(pxImage->ausTask));
		memcpy(g_xCoffee3CupLidImage.aucLidCoils, pxImage->aucCoils,
			sizeof(pxImage->aucCoils));
	} else {
		g_xCoffee3CupLidImage.ausLidTask[ucSlot] =
			pxImage->ausTask[ucSlot];
	}
}
