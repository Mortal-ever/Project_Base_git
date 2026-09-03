/**
  * @file      coffee_machine_O.h
  * @brief     Define the Kalerm O coffee-machine Modbus RTU driver.
  * @author    WHong
  * @date      2026-08-31
  */

#ifndef COFFEE_MACHINE_O_H
#define COFFEE_MACHINE_O_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include "device_library.h"
#include "modbus_port.h"

#define COFFEE_MACHINE_O_STATUS_CAPACITY 16U

typedef enum {
    COFFEE_MACHINE_O_ACTION_REFRESH = 0,
    COFFEE_MACHINE_O_ACTION_MAKE = 1,
    COFFEE_MACHINE_O_ACTION_PAUSE = 2,
    COFFEE_MACHINE_O_ACTION_RESUME = 3,
    COFFEE_MACHINE_O_ACTION_CLEAN = 4,
    COFFEE_MACHINE_O_ACTION_CANCEL = 5,
    COFFEE_MACHINE_O_ACTION_RESET = 6,
    COFFEE_MACHINE_O_ACTION_POWER_OFF = 7,
    COFFEE_MACHINE_O_ACTION_POWER_RESTART = 8
} CoffeeMachineOAction_e;

typedef struct {
    uint16_t ausStatus[COFFEE_MACHINE_O_STATUS_CAPACITY];
} CoffeeMachineOImage_t;

typedef struct {
    uint16_t usStatusStart;
    uint16_t usStatusCount;
    uint16_t usMakeRegister;
    uint16_t usPauseRegister;
    uint16_t usCleanRegister;
    uint16_t usResetRegister;
    uint16_t usCancelRegister;
    uint16_t usPowerRegister;
    uint16_t usIdleValue;
    uint16_t usPauseValue;
    uint16_t usResumeValue;
} CoffeeMachineOConfig_t;

extern const CoffeeMachineOConfig_t g_xCoffeeMachineOConfig;
extern const DeviceDriverDescriptor_t g_xCoffeeMachineODriver;

ModbusPortResult_e xCoffeeMachineOExecute(
    const CoffeeMachineOConfig_t *pxConfig, ModbusPort_t *pxPort,
    uint8_t ucUnitId, CoffeeMachineOAction_e xAction,
    uint16_t usParameter, uint32_t ulTimeoutMs,
    CoffeeMachineOImage_t *pxImage, DeviceCancelCheck_t pxCancelCheck,
    const void *pvCancelContext);

#ifdef __cplusplus
}
#endif

#endif /* COFFEE_MACHINE_O_H */
