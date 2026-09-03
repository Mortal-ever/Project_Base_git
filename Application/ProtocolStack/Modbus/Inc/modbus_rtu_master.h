#ifndef MODBUS_RTU_MASTER_H
#define MODBUS_RTU_MASTER_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>

#include "transport.h"

#define MODBUS_RTU_MAX_ADU_LENGTH          256U

typedef enum {
	MODBUS_RTU_RESULT_OK = 0,
	MODBUS_RTU_RESULT_INVALID_ARG = -1,
	MODBUS_RTU_RESULT_TIMEOUT = -2,
	MODBUS_RTU_RESULT_TRANSPORT = -3,
	MODBUS_RTU_RESULT_PROTOCOL = -4,
	MODBUS_RTU_RESULT_EXCEPTION = -5
} ModbusRtuResult_e;

typedef struct {
	TransportChannel_t *pxTransport;
	uint8_t ucLastException;
} ModbusRtuMaster_t;

void vModbusRtuMasterInit(ModbusRtuMaster_t *pxMaster,
	TransportChannel_t *pxTransport);
ModbusRtuResult_e xModbusRtuReadCoils(ModbusRtuMaster_t *pxMaster,
	uint8_t ucSlaveId, uint16_t usAddress, uint16_t usQuantity,
	bool *pbData, uint32_t ulTimeoutMs);
ModbusRtuResult_e xModbusRtuReadDiscreteInputs(ModbusRtuMaster_t *pxMaster,
	uint8_t ucSlaveId, uint16_t usAddress, uint16_t usQuantity,
	bool *pbData, uint32_t ulTimeoutMs);
ModbusRtuResult_e xModbusRtuReadHolding(ModbusRtuMaster_t *pxMaster,
	uint8_t ucSlaveId, uint16_t usAddress, uint16_t usQuantity,
	uint16_t *pusData, uint32_t ulTimeoutMs);
ModbusRtuResult_e xModbusRtuReadInputRegisters(ModbusRtuMaster_t *pxMaster,
	uint8_t ucSlaveId, uint16_t usAddress, uint16_t usQuantity,
	uint16_t *pusData, uint32_t ulTimeoutMs);
ModbusRtuResult_e xModbusRtuWriteCoil(ModbusRtuMaster_t *pxMaster,
	uint8_t ucSlaveId, uint16_t usAddress, bool bValue,
	uint32_t ulTimeoutMs);
ModbusRtuResult_e xModbusRtuWriteRegister(ModbusRtuMaster_t *pxMaster,
	uint8_t ucSlaveId, uint16_t usAddress, uint16_t usValue,
	uint32_t ulTimeoutMs);

#ifdef __cplusplus
}
#endif

#endif /* MODBUS_RTU_MASTER_H */
