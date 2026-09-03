#ifndef MODBUS_TCP_CLIENT_H
#define MODBUS_TCP_CLIENT_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>

#include "transport.h"

#define MODBUS_TCP_MAX_ADU_LENGTH          260U
#define MODBUS_TCP_MAX_PDU_LENGTH          253U
#define MODBUS_TCP_DEBUG_LENGTH            260U

typedef enum {
	MODBUS_TCP_RESULT_OK = 0,
	MODBUS_TCP_RESULT_INVALID_ARG = -1,
	MODBUS_TCP_RESULT_TIMEOUT = -2,
	MODBUS_TCP_RESULT_TRANSPORT = -3,
	MODBUS_TCP_RESULT_PROTOCOL = -4,
	MODBUS_TCP_RESULT_EXCEPTION = -5
} ModbusTcpResult_e;

typedef struct {
	uint32_t ulSequence;
	uint16_t usLength;
	uint16_t usCapturedLength;
	uint8_t aucData[MODBUS_TCP_DEBUG_LENGTH];
} ModbusTcpFrame_t;

typedef struct {
	ModbusTcpFrame_t xLastTx;
	ModbusTcpFrame_t xLastRx;
	uint8_t ucTxSucceeded;
	uint8_t ucRxSucceeded;
} ModbusTcpDebug_t;

typedef struct {
	TransportChannel_t *pxTransport;
	ModbusTcpDebug_t *pxDebug;
	uint16_t usNextTransactionId;
	uint8_t ucLastException;
} ModbusTcpClient_t;

void vModbusTcpClientInit(ModbusTcpClient_t *pxClient,
	TransportChannel_t *pxTransport);
void vModbusTcpClientSetDebug(ModbusTcpClient_t *pxClient,
	ModbusTcpDebug_t *pxDebug);

ModbusTcpResult_e xModbusTcpReadCoils(ModbusTcpClient_t *pxClient,
	uint8_t ucUnitId, uint16_t usAddress, uint16_t usQuantity,
	bool *pbData, uint32_t ulTimeoutMs);
ModbusTcpResult_e xModbusTcpReadHolding(ModbusTcpClient_t *pxClient,
	uint8_t ucUnitId, uint16_t usAddress, uint16_t usQuantity,
	uint16_t *pusData, uint32_t ulTimeoutMs);
ModbusTcpResult_e xModbusTcpWriteRegister(ModbusTcpClient_t *pxClient,
	uint8_t ucUnitId, uint16_t usAddress, uint16_t usValue,
	uint32_t ulTimeoutMs);
ModbusTcpResult_e xModbusTcpWriteRegisters(ModbusTcpClient_t *pxClient,
	uint8_t ucUnitId, uint16_t usAddress, uint16_t usQuantity,
	const uint16_t *pusData, uint32_t ulTimeoutMs);

#ifdef __cplusplus
}
#endif

#endif /* MODBUS_TCP_CLIENT_H */
