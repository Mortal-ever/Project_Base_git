/**
  * @file      coffee_machine_m50.c
  * @brief     Implement the M50 coffee-machine Modbus RTU driver.
  * @author    WHong
  * @date      2026-08-31
  *
  * @details   Register addresses follow the Coffee1 M50 implementation:
  *            status 0x1000..0x100F and control registers 0x2000,
  *            0x200C, 0x200D, and 0x200E.  MQTT and order behavior are
  *            intentionally outside this device library.
  */

#include "coffee_machine_m50.h"
#include "FreeRTOS.h"
#include "task.h"

#define COFFEE_MACHINE_M50_POLL_MS 500U

const CoffeeMachineM50Config_t g_xCoffeeMachineM50Config = {
    0x1000U, 16U, 0x2000U, 0x200CU, 0x200DU, 0x200EU, 0x00FFU
};

const DeviceDriverDescriptor_t g_xCoffeeMachineM50Driver = {
    DEVICE_DRIVER_COFFEE_M50_MODBUS,
    DEVICE_CATEGORY_COFFEE_MACHINE,
    DEVICE_PROTOCOL_MODBUS_RTU
};

static ModbusPortResult_e prvRefresh(const CoffeeMachineM50Config_t *pxConfig,
    ModbusPort_t *pxPort, uint8_t ucUnitId, uint32_t ulTimeoutMs,
    CoffeeMachineM50Image_t *pxImage)
{
    return xModbusPortReadHolding(pxPort, ucUnitId, pxConfig->usStatusStart,
        pxConfig->usStatusCount, pxImage->ausStatus, ulTimeoutMs);
}

static ModbusPortResult_e prvWaitForIdle(
    const CoffeeMachineM50Config_t *pxConfig, ModbusPort_t *pxPort,
    uint8_t ucUnitId, uint32_t ulTimeoutMs, CoffeeMachineM50Image_t *pxImage,
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
        xResult = prvRefresh(pxConfig, pxPort, ucUnitId, ulTimeoutMs, pxImage);
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
        vTaskDelay(pdMS_TO_TICKS(COFFEE_MACHINE_M50_POLL_MS));
    }
}

ModbusPortResult_e xCoffeeMachineM50Execute(
    const CoffeeMachineM50Config_t *pxConfig, ModbusPort_t *pxPort,
    uint8_t ucUnitId, CoffeeMachineM50Action_e xAction, uint16_t usParameter,
    uint32_t ulTimeoutMs, CoffeeMachineM50Image_t *pxImage,
    DeviceCancelCheck_t pxCancelCheck, const void *pvCancelContext)
{
    ModbusPortResult_e xResult;

    if ((pxConfig == NULL) || (pxPort == NULL) || (pxImage == NULL) ||
        (ulTimeoutMs == 0U) || (pxConfig->usStatusCount == 0U) ||
        (pxConfig->usStatusCount > COFFEE_MACHINE_M50_STATUS_CAPACITY)) {
        return MODBUS_PORT_RESULT_INVALID_ARG;
    }
    if (xAction == COFFEE_MACHINE_M50_ACTION_REFRESH) {
        return prvRefresh(pxConfig, pxPort, ucUnitId, ulTimeoutMs, pxImage);
    }
    if (xAction == COFFEE_MACHINE_M50_ACTION_MAKE) {
        xResult = xModbusPortWriteRegister(pxPort, ucUnitId,
            pxConfig->usMakeRegister, usParameter, ulTimeoutMs);
        if (xResult != MODBUS_PORT_RESULT_OK) {
            return xResult;
        }
        return prvWaitForIdle(pxConfig, pxPort, ucUnitId, ulTimeoutMs,
            pxImage, pxCancelCheck, pvCancelContext);
    }
    if (xAction == COFFEE_MACHINE_M50_ACTION_CLEAN) {
        return xModbusPortWriteRegister(pxPort, ucUnitId,
            pxConfig->usCleanRegister, usParameter, ulTimeoutMs);
    }
    if (xAction == COFFEE_MACHINE_M50_ACTION_FAULT_ACK) {
        return xModbusPortWriteRegister(pxPort, ucUnitId,
            pxConfig->usFaultAckRegister, usParameter, ulTimeoutMs);
    }
    if (xAction == COFFEE_MACHINE_M50_ACTION_CANCEL) {
        return xModbusPortWriteRegister(pxPort, ucUnitId,
            pxConfig->usCancelRegister, 0U, ulTimeoutMs);
    }
    return MODBUS_PORT_RESULT_NOT_SUPPORTED;
}
