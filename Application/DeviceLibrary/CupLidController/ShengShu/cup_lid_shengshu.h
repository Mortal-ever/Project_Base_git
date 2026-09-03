/**
  * @file      cup_lid_shengshu.h
  * @brief     Define the ShengShu cup and lid Modbus device driver.
  * @author    WHong
  * @date      2026-08-21
  *
  * @details   The combined and split mechanical assemblies share this
  *            register protocol.  The selected Unit ID is supplied by the
  *            caller, so topology remains outside this protocol driver.
  *
  * @attention The caller owns the Modbus port and image.  No task, queue,
  *            transport setting, or Unit ID is stored by this module.
  */

#ifndef CUP_LID_SHENGSHU_H
#define CUP_LID_SHENGSHU_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "device_library.h"
#include "modbus_port.h"

#define CUP_LID_SHENGSHU_TASK_COUNT       2U
#define CUP_LID_SHENGSHU_COIL_COUNT       10U
#define CUP_LID_SHENGSHU_STATUS_WORKING   2U
#define CUP_LID_SHENGSHU_STATUS_SUCCESS   3U
#define CUP_LID_SHENGSHU_STATUS_FAILED    4U
#define CUP_LID_SHENGSHU_POLL_MS          100U

/** @brief Select the mechanical role represented by one Unit ID. */
typedef enum {
	CUP_LID_ROLE_CUP = 0,
	CUP_LID_ROLE_LID = 1
} CupLidRole_e;

/**
  * @brief  Store one role's task registers and raw diagnostic coils.
  */
typedef struct {
	uint16_t ausTask[CUP_LID_SHENGSHU_TASK_COUNT];
	uint8_t aucCoils[CUP_LID_SHENGSHU_COIL_COUNT];
} CupLidShengShuImage_t;

/** @brief Describe the ShengShu cup-side protocol. */
extern const DeviceDriverDescriptor_t g_xShengShuCupDriver;
/** @brief Describe the ShengShu lid-side protocol. */
extern const DeviceDriverDescriptor_t g_xShengShuLidDriver;

/**
  * @brief  Refresh task and diagnostic coil data for one mechanical role.
  * @param[in,out] pxPort Initialized Modbus client port.
  * @param[in] ucUnitId Device Unit ID selected by the target topology.
  * @param[in] xRole Cup or lid role.
  * @param[in] ulTimeoutMs Modbus transaction timeout in milliseconds.
  * @param[out] pxImage Caller-owned destination image.
  * @retval MODBUS_PORT_RESULT_OK Refresh completed.
  * @retval MODBUS_PORT_RESULT_INVALID_ARG A pointer, role, or timeout is bad.
  */
ModbusPortResult_e xCupLidShengShuRefresh(ModbusPort_t *pxPort,
	uint8_t ucUnitId, CupLidRole_e xRole, uint32_t ulTimeoutMs,
	CupLidShengShuImage_t *pxImage);

/**
  * @brief  Start one cup or lid task and return after the FC06 response.
  * @param[in,out] pxPort Initialized Modbus client port.
  * @param[in] ucUnitId Device Unit ID selected by the target topology.
  * @param[in] xRole Cup or lid role.
  * @param[in] ucSlot Task slot, 0 or 1.
  * @param[in] ulTimeoutMs Modbus transaction timeout in milliseconds.
  * @param[in,out] pxImage Optional image marked as command accepted.
  * @param[in] pxCancelCheck Reserved for API compatibility.
  * @param[in] pvCancelContext Reserved for API compatibility.
  * @retval MODBUS_PORT_RESULT_OK The slave acknowledged the command.
  * @retval MODBUS_PORT_RESULT_TIMEOUT The slave did not reply in time.
  */
ModbusPortResult_e xCupLidShengShuRun(ModbusPort_t *pxPort,
	uint8_t ucUnitId, CupLidRole_e xRole, uint8_t ucSlot,
	uint32_t ulTimeoutMs, CupLidShengShuImage_t *pxImage,
	DeviceCancelCheck_t pxCancelCheck, const void *pvCancelContext);

#ifdef __cplusplus
}
#endif

#endif /* CUP_LID_SHENGSHU_H */
