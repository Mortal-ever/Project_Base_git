/**
  * @file      coffee_machine_f200.h
  * @brief     Define the Dr.Coffee F200 fixed-frame serial protocol.
  * @author    WHong
  * @date      2026-08-20
  */

#ifndef COFFEE_MACHINE_F200_H
#define COFFEE_MACHINE_F200_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "device_library.h"
#include "transport.h"

#define COFFEE_MACHINE_F200_FRAME_LENGTH       26U
#define COFFEE_MACHINE_F200_QUERY_INTERVAL_MS  200U
#define COFFEE_MACHINE_F200_DEFAULT_BAUD        115200U

typedef enum {
	COFFEE_MACHINE_F200_RESULT_OK = 0,
	COFFEE_MACHINE_F200_RESULT_INVALID_ARG = -1,
	COFFEE_MACHINE_F200_RESULT_TIMEOUT = -4,
	COFFEE_MACHINE_F200_RESULT_TRANSPORT = -5,
	COFFEE_MACHINE_F200_RESULT_PROTOCOL = -6,
	COFFEE_MACHINE_F200_RESULT_REJECTED = -7,
	COFFEE_MACHINE_F200_RESULT_CANCELED = -9
} CoffeeMachineF200Result_e;

typedef enum {
	COFFEE_MACHINE_F200_COMMAND_MAKE = 0x01,
	COFFEE_MACHINE_F200_COMMAND_BREW_RINSE = 0x02,
	COFFEE_MACHINE_F200_COMMAND_MILK_RINSE = 0x03,
	COFFEE_MACHINE_F200_COMMAND_POWDER_RINSE = 0x04,
	COFFEE_MACHINE_F200_COMMAND_MILK_SYSTEM_CLEAN = 0x07,
	COFFEE_MACHINE_F200_COMMAND_QUERY = 0x10,
	COFFEE_MACHINE_F200_COMMAND_CANCEL = 0x11,
	COFFEE_MACHINE_F200_COMMAND_ONE_KEY_CLEAN = 0x17,
	COFFEE_MACHINE_F200_COMMAND_MILK_FORCED_RINSE = 0x1BU
} CoffeeMachineF200Command_e;

typedef enum {
	COFFEE_MACHINE_F200_ACTION_REFRESH = 0,
	COFFEE_MACHINE_F200_ACTION_MAKE = 1,
	COFFEE_MACHINE_F200_ACTION_CANCEL = 2,
	COFFEE_MACHINE_F200_ACTION_CLEAN = 3
} CoffeeMachineF200Action_e;

typedef struct {
	uint64_t ullWarning;
	uint64_t ullFault;
	uint8_t ucMachineState;
	uint8_t ucCommand;
	uint8_t ucApplicationState;
	uint8_t ucDrinkId;
	uint8_t ucApplicationId;
} CoffeeMachineF200Status_t;

extern const DeviceDriverDescriptor_t
	g_xCoffeeMachineDrCoffeeF200Driver;

uint8_t ucCoffeeMachineF200Checksum(const uint8_t *pucFrame);
CoffeeMachineF200Result_e xCoffeeMachineF200BuildFrame(
	CoffeeMachineF200Command_e xCommand, uint8_t ucDrinkId,
	uint8_t *pucFrame, uint16_t usCapacity);
CoffeeMachineF200Result_e xCoffeeMachineF200MapRecipe(
	uint16_t usLogicalRecipe, uint8_t *pucDrinkId);
CoffeeMachineF200Result_e xCoffeeMachineF200ParseFrame(
	const uint8_t *pucFrame, uint16_t usLength,
	CoffeeMachineF200Status_t *pxStatus);
CoffeeMachineF200Result_e xCoffeeMachineF200Exchange(
	TransportChannel_t *pxChannel,
	CoffeeMachineF200Command_e xCommand, uint8_t ucDrinkId,
	uint32_t ulTimeoutMs, CoffeeMachineF200Status_t *pxStatus);
CoffeeMachineF200Result_e xCoffeeMachineF200Execute(
	TransportChannel_t *pxChannel, CoffeeMachineF200Action_e xAction,
	uint8_t ucDrinkId, uint32_t ulTimeoutMs,
	CoffeeMachineF200Status_t *pxStatus,
	DeviceCancelCheck_t pxCancelCheck,
	const void *pvCancelContext);

#ifdef __cplusplus
}
#endif

#endif /* COFFEE_MACHINE_F200_H */
