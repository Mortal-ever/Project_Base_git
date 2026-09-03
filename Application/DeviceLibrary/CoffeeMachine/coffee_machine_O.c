/**
  * @file      coffee_machine_O.c
  * @brief     Implement the Kalerm O coffee-machine Modbus RTU driver.
  * @author    WHong
  * @date      2026-08-31
  */

#include "coffee_machine_O.h"

#include <stddef.h>
#include "FreeRTOS.h"
#include "task.h"

#define COFFEE_MACHINE_O_POLL_MS 500U
#define COFFEE_MACHINE_O_FAULT_FIRST 1U
#define COFFEE_MACHINE_O_FAULT_LAST 8U

const CoffeeMachineOConfig_t g_xCoffeeMachineOConfig = {
    0x1000U, 16U, 0x2000U, 0x2001U, 0x200CU, 0x200DU, 0x200EU, 0x200BU,
    0x00FFU, 1U, 2U
};

const DeviceDriverDescriptor_t g_xCoffeeMachineODriver = {
    DEVICE_DRIVER_COFFEE_KALERM_O_MODBUS,
    DEVICE_CATEGORY_COFFEE_MACHINE,
    DEVICE_PROTOCOL_MODBUS_RTU
};

static ModbusPortResult_e prvRefresh(const CoffeeMachineOConfig_t *pxConfig,
    ModbusPort_t *pxPort, uint8_t ucUnitId, uint32_t ulTimeoutMs,
    CoffeeMachineOImage_t *pxImage)
{
    ModbusPortResult_e xResult;
    uint16_t usIndex;

    xResult = xModbusPortReadHolding(pxPort, ucUnitId,
        pxConfig->usStatusStart, pxConfig->usStatusCount,
        pxImage->ausStatus, ulTimeoutMs);
    if (xResult != MODBUS_PORT_RESULT_OK) {
        return xResult;
    }
    for (usIndex = COFFEE_MACHINE_O_FAULT_FIRST;
        usIndex <= COFFEE_MACHINE_O_FAULT_LAST; usIndex++) {
        if (pxImage->ausStatus[usIndex] != 0U) {
            return MODBUS_PORT_RESULT_PROTOCOL;
        }
    }
    return MODBUS_PORT_RESULT_OK;
}

static ModbusPortResult_e prvWaitForIdle(
    const CoffeeMachineOConfig_t *pxConfig, ModbusPort_t *pxPort,
    uint8_t ucUnitId, uint32_t ulTimeoutMs, CoffeeMachineOImage_t *pxImage,
    DeviceCancelCheck_t pxCancelCheck, const void *pvCancelContext)
{
    ModbusPortResult_e xResult;
    TickType_t xStart;
    uint8_t ucObservedWorking;

    xStart = xTaskGetTickCount();
    ucObservedWorking = 0U;
    for (;;) {
        if ((pxCancelCheck != NULL) &&
            (pxCancelCheck(pvCancelContext) != 0U)) {
            return MODBUS_PORT_RESULT_CANCELED;
        }
        xResult = xModbusPortReadHolding(pxPort, ucUnitId,
            pxConfig->usStatusStart, pxConfig->usStatusCount,
            pxImage->ausStatus, ulTimeoutMs);
        if (xResult != MODBUS_PORT_RESULT_OK) {
            return xResult;
        }
        if (pxImage->ausStatus[0U] != pxConfig->usIdleValue) {
            ucObservedWorking = 1U;
        } else if (ucObservedWorking != 0U) {
            return MODBUS_PORT_RESULT_OK;
        }
        if ((xTaskGetTickCount() - xStart) >= pdMS_TO_TICKS(ulTimeoutMs)) {
            return MODBUS_PORT_RESULT_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(COFFEE_MACHINE_O_POLL_MS));
    }
}

ModbusPortResult_e xCoffeeMachineOExecute(
    const CoffeeMachineOConfig_t *pxConfig, ModbusPort_t *pxPort,
    uint8_t ucUnitId, CoffeeMachineOAction_e xAction, uint16_t usParameter,
    uint32_t ulTimeoutMs, CoffeeMachineOImage_t *pxImage,
    DeviceCancelCheck_t pxCancelCheck, const void *pvCancelContext)
{
    ModbusPortResult_e xResult;
    uint16_t usIndex;
    uint16_t usValue;

    if ((pxConfig == NULL) || (pxPort == NULL) || (pxImage == NULL) ||
        (ulTimeoutMs == 0U) || (pxConfig->usStatusCount == 0U) ||
        (pxConfig->usStatusCount > COFFEE_MACHINE_O_STATUS_CAPACITY)) {
        return MODBUS_PORT_RESULT_INVALID_ARG;
    }
    if (xAction == COFFEE_MACHINE_O_ACTION_REFRESH) {
        return prvRefresh(pxConfig, pxPort, ucUnitId, ulTimeoutMs, pxImage);
    }
    if (xAction == COFFEE_MACHINE_O_ACTION_MAKE) {
        xResult = xModbusPortWriteRegister(pxPort, ucUnitId,
            pxConfig->usMakeRegister, usParameter, ulTimeoutMs);
        if (xResult != MODBUS_PORT_RESULT_OK) {
            return xResult;
        }
        return prvWaitForIdle(pxConfig, pxPort, ucUnitId, ulTimeoutMs,
            pxImage, pxCancelCheck, pvCancelContext);
    }
    if (xAction == COFFEE_MACHINE_O_ACTION_PAUSE ||
        xAction == COFFEE_MACHINE_O_ACTION_RESUME) {
        usValue = (xAction == COFFEE_MACHINE_O_ACTION_PAUSE) ?
            pxConfig->usPauseValue : pxConfig->usResumeValue;
        return xModbusPortWriteRegister(pxPort, ucUnitId,
            pxConfig->usPauseRegister, usValue, ulTimeoutMs);
    }
    if (xAction == COFFEE_MACHINE_O_ACTION_CLEAN) {
        return xModbusPortWriteRegister(pxPort, ucUnitId,
            pxConfig->usCleanRegister, usParameter, ulTimeoutMs);
    }
    if (xAction == COFFEE_MACHINE_O_ACTION_CANCEL) {
        return xModbusPortWriteRegister(pxPort, ucUnitId,
            pxConfig->usCancelRegister, 0U, ulTimeoutMs);
    }
    if ((xAction == COFFEE_MACHINE_O_ACTION_POWER_OFF) ||
        (xAction == COFFEE_MACHINE_O_ACTION_POWER_RESTART)) {
        usValue = (xAction == COFFEE_MACHINE_O_ACTION_POWER_OFF) ? 1U : 2U;
        return xModbusPortWriteRegister(pxPort, ucUnitId,
            pxConfig->usPowerRegister, usValue, ulTimeoutMs);
    }
    if (xAction == COFFEE_MACHINE_O_ACTION_RESET) {
        usValue = 0U;
        for (usIndex = COFFEE_MACHINE_O_FAULT_FIRST;
            usIndex <= COFFEE_MACHINE_O_FAULT_LAST; usIndex++) {
            if (pxImage->ausStatus[usIndex] != 0U) {
                usValue = usIndex;
                break;
            }
        }
        if (usValue == 0U) {
            return MODBUS_PORT_RESULT_OK;
        }
        return xModbusPortWriteRegister(pxPort, ucUnitId,
            pxConfig->usResetRegister, usValue, ulTimeoutMs);
    }
    return MODBUS_PORT_RESULT_NOT_SUPPORTED;
}
