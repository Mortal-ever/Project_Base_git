/**
  * @file      coffee_machine_f200.c
  * @brief     Implement the Dr.Coffee F200 fixed-frame serial protocol.
  * @author    WHong
  * @date      2026-08-20
  */

#include "coffee_machine_f200.h"

#include <stddef.h>
#include <string.h>

#define F200_HEAD_INDEX          0U
#define F200_LENGTH_INDEX        1U
#define F200_WARNING_INDEX       2U
#define F200_FAULT_INDEX        10U
#define F200_MACHINE_INDEX      18U
#define F200_COMMAND_INDEX      19U
#define F200_APP_STATE_INDEX    20U
#define F200_DRINK_INDEX        21U
#define F200_APP_ID_INDEX       22U
#define F200_RESERVED_INDEX     23U
#define F200_CHECKSUM_INDEX     24U
#define F200_TAIL_INDEX         25U
#define F200_HEAD_VALUE       0x7EU
#define F200_LENGTH_VALUE     0x1AU
#define F200_TAIL_VALUE       0x7EU
#define F200_MACHINE_IDLE     0x02U
#define F200_IO_TIMEOUT_MS    1000U

static uint8_t prvCommandSupported(CoffeeMachineF200Command_e xCommand)
{
	switch (xCommand) {
	case COFFEE_MACHINE_F200_COMMAND_MAKE:
	case COFFEE_MACHINE_F200_COMMAND_BREW_RINSE:
	case COFFEE_MACHINE_F200_COMMAND_MILK_RINSE:
	case COFFEE_MACHINE_F200_COMMAND_POWDER_RINSE:
	case COFFEE_MACHINE_F200_COMMAND_MILK_SYSTEM_CLEAN:
	case COFFEE_MACHINE_F200_COMMAND_QUERY:
	case COFFEE_MACHINE_F200_COMMAND_CANCEL:
	case COFFEE_MACHINE_F200_COMMAND_ONE_KEY_CLEAN:
	case COFFEE_MACHINE_F200_COMMAND_MILK_FORCED_RINSE:
		return 1U;
	default:
		return 0U;
	}
}

const DeviceDriverDescriptor_t g_xCoffeeMachineDrCoffeeF200Driver = {
	DEVICE_DRIVER_COFFEE_DRCOFFEE_F200,
	DEVICE_CATEGORY_COFFEE_MACHINE,
	DEVICE_PROTOCOL_COFFEE_F200_UART
};

static uint64_t prvReadLittleEndian64(const uint8_t *pucData)
{
	uint64_t ullValue;
	uint8_t ucIndex;

	ullValue = 0U;
	for (ucIndex = 0U; ucIndex < 8U; ucIndex++) {
		ullValue |= ((uint64_t)pucData[ucIndex]) << (ucIndex * 8U);
	}
	return ullValue;
}

uint8_t ucCoffeeMachineF200Checksum(const uint8_t *pucFrame)
{
	uint32_t ulSum;
	uint8_t ucIndex;

	if (pucFrame == NULL) {
		return 0U;
	}
	ulSum = 0U;
	for (ucIndex = F200_LENGTH_INDEX;
		ucIndex <= F200_RESERVED_INDEX; ucIndex++) {
		ulSum += pucFrame[ucIndex];
	}
	return (uint8_t)(ulSum & 0xFFU);
}

CoffeeMachineF200Result_e xCoffeeMachineF200BuildFrame(
	CoffeeMachineF200Command_e xCommand, uint8_t ucDrinkId,
	uint8_t *pucFrame, uint16_t usCapacity)
{
	if ((pucFrame == NULL) ||
		(usCapacity < COFFEE_MACHINE_F200_FRAME_LENGTH)) {
		return COFFEE_MACHINE_F200_RESULT_INVALID_ARG;
	}
	if (prvCommandSupported(xCommand) == 0U) {
		return COFFEE_MACHINE_F200_RESULT_INVALID_ARG;
	}
	memset(pucFrame, 0, COFFEE_MACHINE_F200_FRAME_LENGTH);
	pucFrame[F200_HEAD_INDEX] = F200_HEAD_VALUE;
	pucFrame[F200_LENGTH_INDEX] = F200_LENGTH_VALUE;
	pucFrame[F200_COMMAND_INDEX] = (uint8_t)xCommand;
	if (xCommand == COFFEE_MACHINE_F200_COMMAND_MAKE) {
		pucFrame[F200_DRINK_INDEX] = ucDrinkId;
	}
	pucFrame[F200_CHECKSUM_INDEX] =
		ucCoffeeMachineF200Checksum(pucFrame);
	pucFrame[F200_TAIL_INDEX] = F200_TAIL_VALUE;
	return COFFEE_MACHINE_F200_RESULT_OK;
}

CoffeeMachineF200Result_e xCoffeeMachineF200MapRecipe(
	uint16_t usLogicalRecipe, uint8_t *pucDrinkId)
{
	if ((pucDrinkId == NULL) || (usLogicalRecipe > 0x0021U)) {
		return COFFEE_MACHINE_F200_RESULT_INVALID_ARG;
	}
	/* F200 drink identifiers are one based; host recipes are zero based. */
	*pucDrinkId = (uint8_t)(usLogicalRecipe + 1U);
	return COFFEE_MACHINE_F200_RESULT_OK;
}

CoffeeMachineF200Result_e xCoffeeMachineF200ParseFrame(
	const uint8_t *pucFrame, uint16_t usLength,
	CoffeeMachineF200Status_t *pxStatus)
{
	if ((pucFrame == NULL) || (pxStatus == NULL) ||
		(usLength != COFFEE_MACHINE_F200_FRAME_LENGTH)) {
		return COFFEE_MACHINE_F200_RESULT_INVALID_ARG;
	}
	if ((pucFrame[F200_HEAD_INDEX] != F200_HEAD_VALUE) ||
		(pucFrame[F200_LENGTH_INDEX] != F200_LENGTH_VALUE) ||
		(pucFrame[F200_TAIL_INDEX] != F200_TAIL_VALUE) ||
		(pucFrame[F200_CHECKSUM_INDEX] !=
			ucCoffeeMachineF200Checksum(pucFrame))) {
		return COFFEE_MACHINE_F200_RESULT_PROTOCOL;
	}
	pxStatus->ullWarning =
		prvReadLittleEndian64(&pucFrame[F200_WARNING_INDEX]);
	pxStatus->ullFault =
		prvReadLittleEndian64(&pucFrame[F200_FAULT_INDEX]);
	pxStatus->ucMachineState = pucFrame[F200_MACHINE_INDEX];
	pxStatus->ucCommand = pucFrame[F200_COMMAND_INDEX];
	pxStatus->ucApplicationState = pucFrame[F200_APP_STATE_INDEX];
	pxStatus->ucDrinkId = pucFrame[F200_DRINK_INDEX];
	pxStatus->ucApplicationId = pucFrame[F200_APP_ID_INDEX];
	return COFFEE_MACHINE_F200_RESULT_OK;
}

CoffeeMachineF200Result_e xCoffeeMachineF200Exchange(
	TransportChannel_t *pxChannel,
	CoffeeMachineF200Command_e xCommand, uint8_t ucDrinkId,
	uint32_t ulTimeoutMs, CoffeeMachineF200Status_t *pxStatus)
{
	CoffeeMachineF200Result_e xResult;
	TransportResult_e xTransportResult;
	uint16_t usReceived;
	uint8_t aucTx[COFFEE_MACHINE_F200_FRAME_LENGTH];
	uint8_t aucRx[COFFEE_MACHINE_F200_FRAME_LENGTH];

	if ((pxChannel == NULL) || (pxStatus == NULL) ||
		(ulTimeoutMs == 0U)) {
		return COFFEE_MACHINE_F200_RESULT_INVALID_ARG;
	}
	xResult = xCoffeeMachineF200BuildFrame(xCommand, ucDrinkId,
		aucTx, sizeof(aucTx));
	if (xResult != COFFEE_MACHINE_F200_RESULT_OK) {
		return xResult;
	}
	xTransportResult = xTransportSend(pxChannel, aucTx,
		COFFEE_MACHINE_F200_FRAME_LENGTH, ulTimeoutMs);
	if (xTransportResult != TRANSPORT_RESULT_OK) {
		return (xTransportResult == TRANSPORT_RESULT_TIMEOUT) ?
			COFFEE_MACHINE_F200_RESULT_TIMEOUT :
			COFFEE_MACHINE_F200_RESULT_TRANSPORT;
	}
	usReceived = 0U;
	xTransportResult = xTransportReceiveExact(pxChannel, aucRx,
		COFFEE_MACHINE_F200_FRAME_LENGTH, &usReceived, ulTimeoutMs);
	if (xTransportResult != TRANSPORT_RESULT_OK) {
		return (xTransportResult == TRANSPORT_RESULT_TIMEOUT) ?
			COFFEE_MACHINE_F200_RESULT_TIMEOUT :
			COFFEE_MACHINE_F200_RESULT_TRANSPORT;
	}
	xResult = xCoffeeMachineF200ParseFrame(aucRx, usReceived, pxStatus);
	if (xResult != COFFEE_MACHINE_F200_RESULT_OK) {
		return xResult;
	}
	if (pxStatus->ucCommand != (uint8_t)xCommand) {
		return COFFEE_MACHINE_F200_RESULT_PROTOCOL;
	}
	if ((xCommand != COFFEE_MACHINE_F200_COMMAND_QUERY) &&
		(pxStatus->ucApplicationState == 0x04U)) {
		return COFFEE_MACHINE_F200_RESULT_REJECTED;
	}
	return COFFEE_MACHINE_F200_RESULT_OK;
}

CoffeeMachineF200Result_e xCoffeeMachineF200Execute(
	TransportChannel_t *pxChannel, CoffeeMachineF200Action_e xAction,
	uint8_t ucDrinkId, uint32_t ulTimeoutMs,
	CoffeeMachineF200Status_t *pxStatus,
	DeviceCancelCheck_t pxCancelCheck,
	const void *pvCancelContext)
{
	CoffeeMachineF200Result_e xResult;
	uint32_t ulIoTimeoutMs;

	if ((pxChannel == NULL) || (pxStatus == NULL) ||
		(ulTimeoutMs == 0U)) {
		return COFFEE_MACHINE_F200_RESULT_INVALID_ARG;
	}
	ulIoTimeoutMs = (ulTimeoutMs < F200_IO_TIMEOUT_MS) ?
		ulTimeoutMs : F200_IO_TIMEOUT_MS;
	(void)pxCancelCheck;
	(void)pvCancelContext;
	if (xAction == COFFEE_MACHINE_F200_ACTION_REFRESH) {
		return xCoffeeMachineF200Exchange(pxChannel,
			COFFEE_MACHINE_F200_COMMAND_QUERY, 0U,
			ulIoTimeoutMs, pxStatus);
	}
	if (xAction == COFFEE_MACHINE_F200_ACTION_CANCEL) {
		return xCoffeeMachineF200Exchange(pxChannel,
			COFFEE_MACHINE_F200_COMMAND_CANCEL, 0U,
			ulIoTimeoutMs, pxStatus);
	}
	if ((xAction != COFFEE_MACHINE_F200_ACTION_MAKE) &&
		(xAction != COFFEE_MACHINE_F200_ACTION_CLEAN)) {
		return COFFEE_MACHINE_F200_RESULT_INVALID_ARG;
	}
	xResult = xCoffeeMachineF200Exchange(pxChannel,
		COFFEE_MACHINE_F200_COMMAND_QUERY, 0U,
		ulIoTimeoutMs, pxStatus);
	if (xResult != COFFEE_MACHINE_F200_RESULT_OK) {
		return xResult;
	}
	if (pxStatus->ucMachineState != F200_MACHINE_IDLE) {
		return COFFEE_MACHINE_F200_RESULT_REJECTED;
	}
	if (xAction == COFFEE_MACHINE_F200_ACTION_CLEAN) {
		if (prvCommandSupported((CoffeeMachineF200Command_e)ucDrinkId) ==
			0U) {
			return COFFEE_MACHINE_F200_RESULT_INVALID_ARG;
		}
		xResult = xCoffeeMachineF200Exchange(pxChannel,
			(CoffeeMachineF200Command_e)ucDrinkId, 0U,
			ulIoTimeoutMs, pxStatus);
	} else {
		xResult = xCoffeeMachineF200Exchange(pxChannel,
			COFFEE_MACHINE_F200_COMMAND_MAKE, ucDrinkId,
			ulIoTimeoutMs, pxStatus);
	}
	return xResult;
}
