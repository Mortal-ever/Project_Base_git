/**
  * @file      coffee_machine_m50.h
  * @brief     Define the M50 coffee-machine Modbus RTU driver.
  * @author    WHong
  * @date      2026-08-31
  */

#ifndef COFFEE_MACHINE_M50_H
#define COFFEE_MACHINE_M50_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include "device_library.h"
#include "modbus_port.h"

#define COFFEE_MACHINE_M50_DEFAULT_UNIT_ID 1U
#define COFFEE_MACHINE_M50_STATUS_CAPACITY 16U

typedef enum {
    COFFEE_MACHINE_M50_ACTION_REFRESH = 0,
    COFFEE_MACHINE_M50_ACTION_MAKE = 1,
    COFFEE_MACHINE_M50_ACTION_CLEAN = 2,
    COFFEE_MACHINE_M50_ACTION_FAULT_ACK = 3,
    COFFEE_MACHINE_M50_ACTION_CANCEL = 4
} CoffeeMachineM50Action_e;

typedef struct {
    uint16_t ausStatus[COFFEE_MACHINE_M50_STATUS_CAPACITY];
} CoffeeMachineM50Image_t;

typedef struct {
    uint16_t usStatusStart;
    uint16_t usStatusCount;
    uint16_t usMakeRegister;
    uint16_t usCleanRegister;
    uint16_t usFaultAckRegister;
    uint16_t usCancelRegister;
    uint16_t usIdleValue;
} CoffeeMachineM50Config_t;

extern const CoffeeMachineM50Config_t g_xCoffeeMachineM50Config;
extern const DeviceDriverDescriptor_t g_xCoffeeMachineM50Driver;

ModbusPortResult_e xCoffeeMachineM50Execute(
    const CoffeeMachineM50Config_t *pxConfig, ModbusPort_t *pxPort,
    uint8_t ucUnitId, CoffeeMachineM50Action_e xAction,
    uint16_t usParameter, uint32_t ulTimeoutMs,
    CoffeeMachineM50Image_t *pxImage, DeviceCancelCheck_t pxCancelCheck,
    const void *pvCancelContext);

#ifdef __cplusplus
}
#endif

#endif /* COFFEE_MACHINE_M50_H */
