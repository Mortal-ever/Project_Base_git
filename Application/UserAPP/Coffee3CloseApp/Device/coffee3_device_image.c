/**
  * @file      coffee3_device_image.c
  * @brief     Store Coffee3-owned device images and commit helpers.
  * @author    WHong
  * @date      2026-08-28
  */

#include "coffee3_device_image.h"

#include <stddef.h>
#include <string.h>

#include "coffee3_app_config.h"
#include "FreeRTOS.h"
#include "task.h"

COFFEE3_CCM_DATA
Coffee3CoffeeMachineImage_t g_xCoffee3CoffeeMachineImage;
COFFEE3_CCM_DATA
Coffee3CupLidImage_t g_xCoffee3CupLidImage;
COFFEE3_CCM_DATA
SyrupMachineModbusImage_t g_xCoffee3SyrupImage;
COFFEE3_CCM_DATA
IceMachineModbusImage_t g_xCoffee3IceImage;
COFFEE3_CCM_DATA
ScaleBsqDgV2Image_t g_xCoffee3ScaleImage;
COFFEE3_CCM_DATA
PowerMeterDdsu666Image_t g_xCoffee3PowerMeterImage;

void vCoffee3DeviceImageCommitM50(
	const CoffeeMachineM50Image_t *pxStatus)
{
	uint8_t ucIndex;

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
