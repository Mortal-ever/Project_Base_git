/**
  * @file      coffee2_device_image.h
  * @brief     Define Coffee2-owned device status images and commit helpers.
  * @author    WHong
  * @date      2026-08-28
  */

#ifndef COFFEE2_DEVICE_IMAGE_H
#define COFFEE2_DEVICE_IMAGE_H

#ifdef __cplusplus
extern "C" {
#endif

#include "coffee_machine_f200.h"
#include "cup_lid_shengshu.h"
#include "ice_machine_modbus.h"
#include "io_module_modbus_digital.h"
#include "power_meter_ddsu666.h"
#include "scale_bsq_dg_v2.h"
#include "syrup_machine_modbus.h"

typedef struct {
	uint16_t ausStatus[24];
} Coffee2CoffeeMachineImage_t;

typedef struct {
	uint16_t ausCupTask[2];
	uint16_t ausLidTask[2];
	uint8_t aucCupCoils[10];
	uint8_t aucLidCoils[10];
} Coffee2CupLidImage_t;

extern Coffee2CoffeeMachineImage_t g_xCoffee2CoffeeMachineImage;
extern Coffee2CupLidImage_t g_xCoffee2CupLidImage;
extern SyrupMachineModbusImage_t g_xCoffee2SyrupImage;
extern IceMachineModbusImage_t g_xCoffee2IceImage;
extern ScaleBsqDgV2Image_t g_xCoffee2ScaleImage;
extern PowerMeterDdsu666Image_t g_xCoffee2PowerMeterImage;

void vCoffee2DeviceImageCommitF200(
	const CoffeeMachineF200Status_t *pxStatus);
void vCoffee2DeviceImageCommitCup(const CupLidShengShuImage_t *pxImage,
	uint8_t ucRefresh, uint8_t ucSlot);
void vCoffee2DeviceImageCommitLid(const CupLidShengShuImage_t *pxImage,
	uint8_t ucRefresh, uint8_t ucSlot);

#ifdef __cplusplus
}
#endif

#endif /* COFFEE2_DEVICE_IMAGE_H */
