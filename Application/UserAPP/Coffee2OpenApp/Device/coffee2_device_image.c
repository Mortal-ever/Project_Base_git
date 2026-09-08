/**
  * @file      coffee2_device_image.c
  * @brief     Store Coffee2-owned device images and commit helpers.
  * @author    WHong
  * @date      2026-08-28
  */

#include "coffee2_device_image.h"

#include <stddef.h>
#include <string.h>

#include "coffee2_app_config.h"

COFFEE2_CCM_DATA
Coffee2CoffeeMachineImage_t g_xCoffee2CoffeeMachineImage;
COFFEE2_CCM_DATA
Coffee2CupLidImage_t g_xCoffee2CupLidImage;
COFFEE2_CCM_DATA
SyrupMachineModbusImage_t g_xCoffee2SyrupImage;
COFFEE2_CCM_DATA
IceMachineModbusImage_t g_xCoffee2IceImage;
COFFEE2_CCM_DATA
ScaleBsqDgV2Image_t g_xCoffee2ScaleImage;
COFFEE2_CCM_DATA
PowerMeterDdsu666Image_t g_xCoffee2PowerMeterImage;

void vCoffee2DeviceImageCommitF200(
	const CoffeeMachineF200Status_t *pxStatus)
{
	uint8_t ucIndex;

	if (pxStatus == NULL) {
		return;
	}
	memset(g_xCoffee2CoffeeMachineImage.ausStatus, 0,
		sizeof(g_xCoffee2CoffeeMachineImage.ausStatus));
	g_xCoffee2CoffeeMachineImage.ausStatus[0] =
		(uint16_t)pxStatus->ucMachineState;
	g_xCoffee2CoffeeMachineImage.ausStatus[1] =
		(uint16_t)pxStatus->ucApplicationState;
	g_xCoffee2CoffeeMachineImage.ausStatus[2] =
		(uint16_t)pxStatus->ucCommand;
	g_xCoffee2CoffeeMachineImage.ausStatus[3] =
		(uint16_t)pxStatus->ucDrinkId;
	g_xCoffee2CoffeeMachineImage.ausStatus[4] =
		(uint16_t)pxStatus->ucApplicationId;
	for (ucIndex = 0U; ucIndex < 4U; ucIndex++) {
		g_xCoffee2CoffeeMachineImage.ausStatus[5U + ucIndex] =
			(uint16_t)(pxStatus->ullWarning >> (ucIndex * 16U));
		g_xCoffee2CoffeeMachineImage.ausStatus[9U + ucIndex] =
			(uint16_t)(pxStatus->ullFault >> (ucIndex * 16U));
	}
}

void vCoffee2DeviceImageCommitCup(const CupLidShengShuImage_t *pxImage,
	uint8_t ucRefresh, uint8_t ucSlot)
{
	if ((pxImage == NULL) || (ucSlot >= 2U)) {
		return;
	}
	if (ucRefresh != 0U) {
		memcpy(g_xCoffee2CupLidImage.ausCupTask, pxImage->ausTask,
		sizeof(pxImage->ausTask));
		memcpy(g_xCoffee2CupLidImage.aucCupCoils, pxImage->aucCoils,
		sizeof(pxImage->aucCoils));
	} else {
		g_xCoffee2CupLidImage.ausCupTask[ucSlot] =
			pxImage->ausTask[ucSlot];
	}
}

void vCoffee2DeviceImageCommitLid(const CupLidShengShuImage_t *pxImage,
	uint8_t ucRefresh, uint8_t ucSlot)
{
	if ((pxImage == NULL) || (ucSlot >= 2U)) {
		return;
	}
	if (ucRefresh != 0U) {
		memcpy(g_xCoffee2CupLidImage.ausLidTask, pxImage->ausTask,
			sizeof(pxImage->ausTask));
		memcpy(g_xCoffee2CupLidImage.aucLidCoils, pxImage->aucCoils,
			sizeof(pxImage->aucCoils));
	} else {
		g_xCoffee2CupLidImage.ausLidTask[ucSlot] =
			pxImage->ausTask[ucSlot];
	}
}
