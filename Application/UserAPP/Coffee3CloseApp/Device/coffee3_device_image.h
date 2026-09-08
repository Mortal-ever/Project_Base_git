/**
  * @file      coffee3_device_image.h
  * @brief     Define Coffee3-owned device status images and commit helpers.
  * @author    WHong
  * @date      2026-08-28
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

typedef struct {
	uint16_t ausStatus[24];
} Coffee3CoffeeMachineImage_t;

typedef struct {
	uint16_t ausCupTask[2];
	uint16_t ausLidTask[2];
	uint8_t aucCupCoils[10];
	uint8_t aucLidCoils[10];
} Coffee3CupLidImage_t;

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
