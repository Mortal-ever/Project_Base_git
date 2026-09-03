/**
  * @file      dobot_robot_device.c
  * @brief     Implement static Dobot robot variant and point validation.
  * @author    WHong
  * @date      2026-08-20
  */

#include "dobot_robot_device.h"

#include <stddef.h>

#define DOBOT_ROLE_MASK_ROBOT_1   (1U << 0)
#define DOBOT_ROLE_MASK_ROBOT_2   (1U << 1)

const DobotRobotProtocolConfig_t g_xDobotRobotProtocol1 = {
	DOBOT_ROBOT_CONTROL_START,
	DOBOT_ROBOT_LEGACY_CONTROL_COUNT,
	3140U,
	3111U,
	9U,
	3130U,
	10U,
	0U,
	DOBOT_ROBOT_BASE_INPUT_COUNT,
	DOBOT_ROLE_MASK_ROBOT_1 | DOBOT_ROLE_MASK_ROBOT_2,
	0U,
	DOBOT_ROBOT_PROTOCOL_1
};

const DobotRobotProtocolConfig_t g_xDobotRobotProtocol2 = {
	DOBOT_ROBOT_CONTROL_START,
	DOBOT_ROBOT_LEGACY_CONTROL_COUNT,
	3140U,
	3111U,
	9U,
	3130U,
	10U,
	0U,
	DOBOT_ROBOT_BASE_INPUT_COUNT,
	DOBOT_ROLE_MASK_ROBOT_1,
	1U,
	DOBOT_ROBOT_PROTOCOL_2
};

const DobotRobotProtocolConfig_t g_xDobotRobotProtocol3 = {
	DOBOT_ROBOT_CONTROL_START,
	DOBOT_ROBOT_LEGACY_CONTROL_COUNT,
	3140U,
	3111U,
	9U,
	3130U,
	10U,
	0U,
	DOBOT_ROBOT_BASE_INPUT_COUNT,
	DOBOT_ROLE_MASK_ROBOT_2,
	1U,
	DOBOT_ROBOT_PROTOCOL_3
};

const DeviceDriverDescriptor_t g_xDobotRobotProtocol1Driver = {
	DEVICE_DRIVER_ROBOT_DOBOT_PROTOCOL_1,
	DEVICE_CATEGORY_ROBOT,
	DEVICE_PROTOCOL_MODBUS_TCP
};

const DeviceDriverDescriptor_t g_xDobotRobotProtocol2Driver = {
	DEVICE_DRIVER_ROBOT_DOBOT_PROTOCOL_2,
	DEVICE_CATEGORY_ROBOT,
	DEVICE_PROTOCOL_MODBUS_TCP
};

const DeviceDriverDescriptor_t g_xDobotRobotProtocol3Driver = {
	DEVICE_DRIVER_ROBOT_DOBOT_PROTOCOL_3,
	DEVICE_CATEGORY_ROBOT,
	DEVICE_PROTOCOL_MODBUS_TCP
};

const DeviceDriverDescriptor_t g_xJakaRobotReservedDriver = {
	DEVICE_DRIVER_ROBOT_JAKA_RESERVED,
	DEVICE_CATEGORY_ROBOT,
	DEVICE_PROTOCOL_JAKA_RESERVED
};

const DobotRobotProtocolConfig_t *pxDobotRobotGetProtocol(
	DobotRobotProtocolVariant_e xVariant)
{
	if (xVariant == DOBOT_ROBOT_PROTOCOL_1) {
		return &g_xDobotRobotProtocol1;
	}
	if (xVariant == DOBOT_ROBOT_PROTOCOL_2) {
		return &g_xDobotRobotProtocol2;
	}
	if (xVariant == DOBOT_ROBOT_PROTOCOL_3) {
		return &g_xDobotRobotProtocol3;
	}
	return NULL;
}

uint8_t ucDobotRobotIsControlAddressValid(
	const DobotRobotProtocolConfig_t *pxProtocol,
	uint16_t usAddress)
{
	if (pxProtocol == NULL) {
		return 0U;
	}
	if ((usAddress < pxProtocol->usControlStart) ||
		(usAddress >= pxProtocol->usControlValidEndExclusive)) {
		return 0U;
	}
	return 1U;
}

uint8_t ucDobotRobotRoleAllowed(
	const DobotRobotProtocolConfig_t *pxProtocol,
	DeviceRole_e xRole)
{
	uint8_t ucMask;

	if (pxProtocol == NULL) {
		return 0U;
	}
	if (xRole == DEVICE_ROLE_ROBOT_1) {
		ucMask = DOBOT_ROLE_MASK_ROBOT_1;
	} else if (xRole == DEVICE_ROLE_ROBOT_2) {
		ucMask = DOBOT_ROLE_MASK_ROBOT_2;
	} else {
		return 0U;
	}
	return ((pxProtocol->ucAllowedRoleMask & ucMask) != 0U) ? 1U : 0U;
}

uint8_t ucDobotRobotResolvePoint(
	const DobotRobotDriverConfig_t *pxConfig,
	uint16_t usTargetId, uint16_t usSelector,
	uint16_t *pusCommandCoil, uint16_t *pusResultCoil)
{
	const DobotRobotPoint_t *pxFallback;
	const DobotRobotPoint_t *pxPoint;
	uint16_t usIndex;

	if ((pxConfig == NULL) || (pxConfig->pxProtocol == NULL) ||
		(pxConfig->pxPoints == NULL) || (pusCommandCoil == NULL) ||
		(pusResultCoil == NULL) ||
		(ucDobotRobotRoleAllowed(pxConfig->pxProtocol,
			pxConfig->xRole) == 0U)) {
		return 0U;
	}
	pxPoint = NULL;
	pxFallback = NULL;
	for (usIndex = 0U; usIndex < pxConfig->usPointCount; usIndex++) {
		pxPoint = &pxConfig->pxPoints[usIndex];
		if (pxPoint->usTargetId != usTargetId) {
			continue;
		}
		if (pxPoint->usSelector == usSelector) {
			break;
		}
		if (pxPoint->usSelector == DOBOT_ROBOT_SELECTOR_ANY) {
			pxFallback = pxPoint;
		}
	}
	if (usIndex >= pxConfig->usPointCount) {
		pxPoint = pxFallback;
	}
	if (pxPoint == NULL) {
		return 0U;
	}
	if ((ucDobotRobotIsControlAddressValid(pxConfig->pxProtocol,
		pxPoint->usCommandCoil) == 0U) ||
		(ucDobotRobotIsControlAddressValid(pxConfig->pxProtocol,
			pxPoint->usResultCoil) == 0U)) {
		return 0U;
	}
	*pusCommandCoil = pxPoint->usCommandCoil;
	*pusResultCoil = pxPoint->usResultCoil;
	return 1U;
}
