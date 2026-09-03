/**
  * @file      modbus_port.h
  * @brief     Bind nanoMODBUS to the project Transport abstraction.
  * @author    WHong
  * @date      2026-07-28
  */

#ifndef MODBUS_PORT_H
#define MODBUS_PORT_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>

#include "FreeRTOS.h"
#include "modbus_port_config.h"
#include "nanomodbus.h"
#include "transport.h"

/** @brief Select the Modbus application data unit transport format. */
typedef enum {
	MODBUS_PORT_TRANSPORT_RTU = 0, /*!< Serial RTU with CRC footer. */
	MODBUS_PORT_TRANSPORT_TCP = 1 /*!< TCP with MBAP header. */
} ModbusPortTransport_e;

/** @brief Select the local Modbus protocol role. */
typedef enum {
	MODBUS_PORT_ROLE_CLIENT = 0, /*!< Initiate requests and parse responses. */
	MODBUS_PORT_ROLE_SERVER = 1 /*!< Parse requests and send responses. */
} ModbusPortRole_e;

/** @brief Define project-level Modbus transaction results. */
typedef enum {
	MODBUS_PORT_RESULT_OK = 0, /*!< Transaction completed successfully. */
	MODBUS_PORT_RESULT_INVALID_ARG = -1, /*!< Parameter validation failed. */
	MODBUS_PORT_RESULT_NOT_READY = -2, /*!< Port or Transport is not ready. */
	MODBUS_PORT_RESULT_BUSY = -3, /*!< Required resource is busy. */
	MODBUS_PORT_RESULT_TIMEOUT = -4, /*!< Transaction deadline expired. */
	MODBUS_PORT_RESULT_TRANSPORT = -5, /*!< Byte transport failed. */
	MODBUS_PORT_RESULT_PROTOCOL = -6, /*!< Response validation failed. */
	MODBUS_PORT_RESULT_EXCEPTION = -7, /*!< Peer returned a valid exception. */
	MODBUS_PORT_RESULT_NOT_SUPPORTED = -8, /*!< Requested ability is unavailable. */
	MODBUS_PORT_RESULT_CANCELED = -9 /*!< Owning workflow canceled the transaction. */
} ModbusPortResult_e;

/** @brief Store a bounded copy and total length of one Modbus frame. */
typedef struct {
	uint32_t ulSequence; /*!< Transaction sequence associated with the frame. */
	uint16_t usLength; /*!< Total frame bytes observed by callbacks. */
	uint16_t usCapturedLength; /*!< Bytes retained in aucData. */
	uint8_t aucData[MODBUS_PORT_TRACE_LENGTH]; /*!< Bounded frame copy. */
} ModbusPortFrame_t;

/** @brief Store the latest request and response frame trace. */
typedef struct {
	ModbusPortFrame_t xLastTx; /*!< Latest transmitted ADU fragments. */
	ModbusPortFrame_t xLastRx; /*!< Latest received ADU fragments. */
	uint8_t ucTxSucceeded; /*!< Nonzero after complete request transmission. */
	uint8_t ucRxSucceeded; /*!< Nonzero after a valid received response. */
} ModbusPortTrace_t;

/** @brief Preserve normalized and native details of the latest transaction. */
typedef struct {
	ModbusPortResult_e xResult; /*!< Project-level transaction result. */
	TransportResult_e xTransportResult; /*!< Latest Transport result. */
	int32_t lProtocolCode; /*!< Native nanoMODBUS error or exception value. */
	int32_t lNativeError; /*!< HAL, LwIP, or Socket backend error. */
	uint8_t ucExceptionCode; /*!< Positive Modbus exception code, or zero. */
} ModbusPortFault_t;

/**
  * @brief Store one nanoMODBUS instance and its project integration state.
  * @warning One owner may execute a transaction on this object at a time.
  */
typedef struct {
	nmbs_t xNmbs; /*!< Embedded upstream protocol instance. */
	nmbs_bitfield aucBitfield; /*!< Scratch storage for coil operations. */
	TransportChannel_t *pxChannel; /*!< Bound caller-owned byte channel. */
	ModbusPortTrace_t *pxTrace; /*!< Optional caller-owned frame trace. */
	ModbusPortFault_t xLastFault; /*!< Latest detailed transaction fault. */
	TickType_t xOperationStart; /*!< Tick at transaction start. */
	TickType_t xOperationBudget; /*!< Total transaction budget in ticks. */
	uint32_t ulByteTimeoutMs; /*!< Inter-stage timeout in milliseconds. */
	uint32_t ulTraceSequence; /*!< Monotonic trace transaction sequence. */
	ModbusPortTransport_e xTransport; /*!< RTU or TCP framing selection. */
	ModbusPortRole_e xRole; /*!< Client or server role. */
	TransportResult_e xLastTransportResult; /*!< Latest callback IO result. */
	uint8_t ucOperationActive; /*!< Nonzero while a deadline is active. */
	uint8_t ucInitialized; /*!< Nonzero after nanoMODBUS creation. */
} ModbusPort_t;

/**
  * @brief Initialize a nanoMODBUS client over an existing Transport channel.
  * @param[out] pxPort Caller-owned port object.
  * @param[in] pxChannel Registered Transport channel that outlives pxPort.
  * @param[in] xTransport RTU or TCP framing selection.
  * @param[in] ulByteTimeoutMs Maximum IO-stage timeout in milliseconds.
  * @return Project-level initialization result.
  */
ModbusPortResult_e xModbusPortClientInit(ModbusPort_t *pxPort,
	TransportChannel_t *pxChannel, ModbusPortTransport_e xTransport,
	uint32_t ulByteTimeoutMs);

#if (NANOMODBUS_CFG_SERVER_ENABLED != 0)
/**
  * @brief Initialize a nanoMODBUS server over an existing Transport channel.
  * @param[out] pxPort Caller-owned port object.
  * @param[in] pxChannel Registered Transport channel that outlives pxPort.
  * @param[in] xTransport RTU or TCP framing selection.
  * @param[in] ucRtuAddress RTU server address; ignored for TCP routing.
  * @param[in] pxCallbacks Persistent server data-model callbacks.
  * @param[in] ulByteTimeoutMs Maximum IO-stage timeout in milliseconds.
  * @return Project-level initialization result.
  */
ModbusPortResult_e xModbusPortServerInit(ModbusPort_t *pxPort,
	TransportChannel_t *pxChannel, ModbusPortTransport_e xTransport,
	uint8_t ucRtuAddress, const nmbs_callbacks *pxCallbacks,
	uint32_t ulByteTimeoutMs);
/**
  * @brief Poll and process at most one Modbus server request.
  * @param[in,out] pxPort Initialized server port.
  * @param[in] ulPollTimeoutMs Total polling budget in milliseconds.
  * @return Project-level request-processing result.
  */
ModbusPortResult_e xModbusPortServerPoll(ModbusPort_t *pxPort,
	uint32_t ulPollTimeoutMs);
#endif

/**
  * @brief Attach or detach an optional frame trace object.
  * @param[in,out] pxPort Initialized or zeroed port.
  * @param[out] pxTrace Persistent trace object, or NULL to disable tracing.
  */
void vModbusPortSetTrace(ModbusPort_t *pxPort,
	ModbusPortTrace_t *pxTrace);

/**
  * @brief Copy detailed information about the latest transaction result.
  * @param[in] pxPort Initialized port.
  * @param[out] pxFault Caller-owned fault destination.
  */
void vModbusPortGetLastFault(const ModbusPort_t *pxPort,
	ModbusPortFault_t *pxFault);

/**
  * @brief Test whether a result requires rebuilding a stream connection.
  * @param[in] xResult Project-level Modbus result.
  * @retval 1 The link should be rebuilt.
  * @retval 0 The result does not prove a link failure.
  */
uint8_t ucModbusPortResultIsLinkFailure(ModbusPortResult_e xResult);

/**
  * @brief Read coil values with FC01.
  * @param[in,out] pxPort Initialized client port.
  * @param[in] ucUnitId Destination Unit ID.
  * @param[in] usAddress First coil address.
  * @param[in] usQuantity Number of coils to read.
  * @param[out] pbValues Boolean output array with usQuantity entries.
  * @param[in] ulTimeoutMs Total transaction timeout in milliseconds.
  * @return Project-level Modbus transaction result.
  */
ModbusPortResult_e xModbusPortReadCoils(ModbusPort_t *pxPort,
	uint8_t ucUnitId, uint16_t usAddress, uint16_t usQuantity,
	bool *pbValues, uint32_t ulTimeoutMs);
/**
  * @brief Read discrete-input values with FC02.
  * @param[in,out] pxPort Initialized client port.
  * @param[in] ucUnitId Destination Unit ID.
  * @param[in] usAddress First discrete-input address.
  * @param[in] usQuantity Number of inputs to read.
  * @param[out] pbValues Boolean output array with usQuantity entries.
  * @param[in] ulTimeoutMs Total transaction timeout in milliseconds.
  * @return Project-level Modbus transaction result.
  */
ModbusPortResult_e xModbusPortReadDiscreteInputs(ModbusPort_t *pxPort,
	uint8_t ucUnitId, uint16_t usAddress, uint16_t usQuantity,
	bool *pbValues, uint32_t ulTimeoutMs);
/**
  * @brief Read holding registers with FC03.
  * @param[in,out] pxPort Initialized client port.
  * @param[in] ucUnitId Destination Unit ID.
  * @param[in] usAddress First holding-register address.
  * @param[in] usQuantity Number of registers to read.
  * @param[out] pusValues Output array with usQuantity entries.
  * @param[in] ulTimeoutMs Total transaction timeout in milliseconds.
  * @return Project-level Modbus transaction result.
  */
ModbusPortResult_e xModbusPortReadHolding(ModbusPort_t *pxPort,
	uint8_t ucUnitId, uint16_t usAddress, uint16_t usQuantity,
	uint16_t *pusValues, uint32_t ulTimeoutMs);
/**
  * @brief Read input registers with FC04.
  * @param[in,out] pxPort Initialized client port.
  * @param[in] ucUnitId Destination Unit ID.
  * @param[in] usAddress First input-register address.
  * @param[in] usQuantity Number of registers to read.
  * @param[out] pusValues Output array with usQuantity entries.
  * @param[in] ulTimeoutMs Total transaction timeout in milliseconds.
  * @return Project-level Modbus transaction result.
  */
ModbusPortResult_e xModbusPortReadInputRegisters(ModbusPort_t *pxPort,
	uint8_t ucUnitId, uint16_t usAddress, uint16_t usQuantity,
	uint16_t *pusValues, uint32_t ulTimeoutMs);
/**
  * @brief Write one coil with FC05.
  * @param[in,out] pxPort Initialized client port.
  * @param[in] ucUnitId Destination Unit ID.
  * @param[in] usAddress Coil address.
  * @param[in] bValue Requested logical value.
  * @param[in] ulTimeoutMs Total transaction timeout in milliseconds.
  * @return Project-level Modbus transaction result.
  */
ModbusPortResult_e xModbusPortWriteCoil(ModbusPort_t *pxPort,
	uint8_t ucUnitId, uint16_t usAddress, bool bValue,
	uint32_t ulTimeoutMs);
/**
  * @brief Write one holding register with FC06.
  * @param[in,out] pxPort Initialized client port.
  * @param[in] ucUnitId Destination Unit ID.
  * @param[in] usAddress Holding-register address.
  * @param[in] usValue Requested 16-bit register value.
  * @param[in] ulTimeoutMs Total transaction timeout in milliseconds.
  * @return Project-level Modbus transaction result.
  */
ModbusPortResult_e xModbusPortWriteRegister(ModbusPort_t *pxPort,
	uint8_t ucUnitId, uint16_t usAddress, uint16_t usValue,
	uint32_t ulTimeoutMs);
/**
  * @brief Write multiple coils with FC15.
  * @param[in,out] pxPort Initialized client port.
  * @param[in] ucUnitId Destination Unit ID.
  * @param[in] usAddress First coil address.
  * @param[in] usQuantity Number of coils to write.
  * @param[in] pbValues Boolean input array with usQuantity entries.
  * @param[in] ulTimeoutMs Total transaction timeout in milliseconds.
  * @return Project-level Modbus transaction result.
  */
ModbusPortResult_e xModbusPortWriteCoils(ModbusPort_t *pxPort,
	uint8_t ucUnitId, uint16_t usAddress, uint16_t usQuantity,
	const bool *pbValues, uint32_t ulTimeoutMs);
/**
  * @brief Write multiple holding registers with FC16.
  * @param[in,out] pxPort Initialized client port.
  * @param[in] ucUnitId Destination Unit ID.
  * @param[in] usAddress First holding-register address.
  * @param[in] usQuantity Number of registers to write.
  * @param[in] pusValues Input array with usQuantity entries.
  * @param[in] ulTimeoutMs Total transaction timeout in milliseconds.
  * @return Project-level Modbus transaction result.
  */
ModbusPortResult_e xModbusPortWriteRegisters(ModbusPort_t *pxPort,
	uint8_t ucUnitId, uint16_t usAddress, uint16_t usQuantity,
	const uint16_t *pusValues, uint32_t ulTimeoutMs);
/**
  * @brief Read one file record with FC20.
  * @param[in,out] pxPort Initialized client port.
  * @param[in] ucUnitId Destination Unit ID.
  * @param[in] usFileNumber File number in the Modbus file space.
  * @param[in] usRecordNumber First record number.
  * @param[out] pusValues Output array with usCount entries.
  * @param[in] usCount Number of 16-bit record values.
  * @param[in] ulTimeoutMs Total transaction timeout in milliseconds.
  * @return Project-level Modbus transaction result.
  */
ModbusPortResult_e xModbusPortReadFileRecord(ModbusPort_t *pxPort,
	uint8_t ucUnitId, uint16_t usFileNumber, uint16_t usRecordNumber,
	uint16_t *pusValues, uint16_t usCount, uint32_t ulTimeoutMs);
/**
  * @brief Write one file record with FC21.
  * @param[in,out] pxPort Initialized client port.
  * @param[in] ucUnitId Destination Unit ID.
  * @param[in] usFileNumber File number in the Modbus file space.
  * @param[in] usRecordNumber First record number.
  * @param[in] pusValues Input array with usCount entries.
  * @param[in] usCount Number of 16-bit record values.
  * @param[in] ulTimeoutMs Total transaction timeout in milliseconds.
  * @return Project-level Modbus transaction result.
  */
ModbusPortResult_e xModbusPortWriteFileRecord(ModbusPort_t *pxPort,
	uint8_t ucUnitId, uint16_t usFileNumber, uint16_t usRecordNumber,
	const uint16_t *pusValues, uint16_t usCount, uint32_t ulTimeoutMs);
/**
  * @brief Execute combined register read and write with FC23.
  * @param[in,out] pxPort Initialized client port.
  * @param[in] ucUnitId Destination Unit ID.
  * @param[in] usReadAddress First register to read.
  * @param[in] usReadQuantity Number of registers to read.
  * @param[out] pusReadValues Read result array.
  * @param[in] usWriteAddress First register to write.
  * @param[in] usWriteQuantity Number of registers to write.
  * @param[in] pusWriteValues Values written by the request.
  * @param[in] ulTimeoutMs Total transaction timeout in milliseconds.
  * @return Project-level Modbus transaction result.
  */
ModbusPortResult_e xModbusPortReadWriteRegisters(ModbusPort_t *pxPort,
	uint8_t ucUnitId, uint16_t usReadAddress, uint16_t usReadQuantity,
	uint16_t *pusReadValues, uint16_t usWriteAddress,
	uint16_t usWriteQuantity, const uint16_t *pusWriteValues,
	uint32_t ulTimeoutMs);
/**
  * @brief Read the basic FC43/14 device-identification objects.
  * @param[in,out] pxPort Initialized client port.
  * @param[in] ucUnitId Destination Unit ID.
  * @param[out] pcVendorName Vendor-name buffer.
  * @param[out] pcProductCode Product-code buffer.
  * @param[out] pcRevision Revision buffer.
  * @param[in] ucBufferLength Capacity of each output buffer in bytes.
  * @param[in] ulTimeoutMs Total transaction timeout in milliseconds.
  * @return Project-level Modbus transaction result.
  */
ModbusPortResult_e xModbusPortReadDeviceIdentificationBasic(
	ModbusPort_t *pxPort, uint8_t ucUnitId, char *pcVendorName,
	char *pcProductCode, char *pcRevision, uint8_t ucBufferLength,
	uint32_t ulTimeoutMs);
/**
  * @brief Read the regular FC43/14 device-identification objects.
  * @param[in,out] pxPort Initialized client port.
  * @param[in] ucUnitId Destination Unit ID.
  * @param[out] pcVendorUrl Vendor-URL buffer.
  * @param[out] pcProductName Product-name buffer.
  * @param[out] pcModelName Model-name buffer.
  * @param[out] pcApplicationName User-application-name buffer.
  * @param[in] ucBufferLength Capacity of each output buffer in bytes.
  * @param[in] ulTimeoutMs Total transaction timeout in milliseconds.
  * @return Project-level Modbus transaction result.
  */
ModbusPortResult_e xModbusPortReadDeviceIdentificationRegular(
	ModbusPort_t *pxPort, uint8_t ucUnitId, char *pcVendorUrl,
	char *pcProductName, char *pcModelName, char *pcApplicationName,
	uint8_t ucBufferLength, uint32_t ulTimeoutMs);
/**
  * @brief Read extended FC43/14 device-identification objects.
  * @param[in,out] pxPort Initialized client port.
  * @param[in] ucUnitId Destination Unit ID.
  * @param[in] ucObjectIdStart First extended object ID.
  * @param[out] pucIds Output array of object IDs.
  * @param[out] ppcBuffers Array of caller-owned value buffers.
  * @param[in] ucIdsLength Number of ID and buffer entries.
  * @param[in] ucBufferLength Capacity of each value buffer in bytes.
  * @param[out] pucObjectsCount Number of returned objects.
  * @param[in] ulTimeoutMs Total transaction timeout in milliseconds.
  * @return Project-level Modbus transaction result.
  */
ModbusPortResult_e xModbusPortReadDeviceIdentificationExtended(
	ModbusPort_t *pxPort, uint8_t ucUnitId, uint8_t ucObjectIdStart,
	uint8_t *pucIds, char **ppcBuffers, uint8_t ucIdsLength,
	uint8_t ucBufferLength, uint8_t *pucObjectsCount,
	uint32_t ulTimeoutMs);
/**
  * @brief Read one individual FC43/14 device-identification object.
  * @param[in,out] pxPort Initialized client port.
  * @param[in] ucUnitId Destination Unit ID.
  * @param[in] ucObjectId Requested object ID.
  * @param[out] pcBuffer Caller-owned value buffer.
  * @param[in] ucBufferLength Buffer capacity in bytes.
  * @param[in] ulTimeoutMs Total transaction timeout in milliseconds.
  * @return Project-level Modbus transaction result.
  */
ModbusPortResult_e xModbusPortReadDeviceIdentification(
	ModbusPort_t *pxPort, uint8_t ucUnitId, uint8_t ucObjectId,
	char *pcBuffer, uint8_t ucBufferLength, uint32_t ulTimeoutMs);
/**
  * @brief Exchange a caller-defined Modbus function PDU.
  * @param[in,out] pxPort Initialized client port.
  * @param[in] ucUnitId Destination Unit ID.
  * @param[in] ucFunctionCode Requested function code.
  * @param[in] pucRequestData PDU data bytes, or NULL when length is zero.
  * @param[in] usRequestLength Request-data length, at most 252 bytes.
  * @param[out] pucResponseData Response buffer, or NULL to discard data.
  * @param[in] ucResponseLength Expected response-data length.
  * @param[in] ulTimeoutMs Total transaction timeout in milliseconds.
  * @return Project-level Modbus transaction result.
  * @warning The caller owns byte order and function-specific validation.
  */
ModbusPortResult_e xModbusPortRawRequest(ModbusPort_t *pxPort,
	uint8_t ucUnitId, uint8_t ucFunctionCode,
	const uint8_t *pucRequestData, uint16_t usRequestLength,
	uint8_t *pucResponseData, uint8_t ucResponseLength,
	uint32_t ulTimeoutMs);

#ifdef __cplusplus
}
#endif

#endif /* MODBUS_PORT_H */
