/**
  * @file      scale_bsq_dg_v2.h
  * @brief     Define the BSQ-DG-V2 weighing-module Modbus driver.
  * @author    WHong
  * @date      2026-08-21
  *
  * @details   The normalized weight is expressed in 0.1 gram units.  The
  *            driver accepts only the documented gram and kilogram unit
  *            codes and keeps the raw protocol fields for diagnostics.
  */

#ifndef SCALE_BSQ_DG_V2_H
#define SCALE_BSQ_DG_V2_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "device_library.h"
#include "modbus_port.h"

#define SCALE_BSQ_DG_V2_UNIT_KG           2U
#define SCALE_BSQ_DG_V2_UNIT_G            4U
#define SCALE_BSQ_DG_V2_REGISTER_COUNT    3U

/** @brief Store raw and normalized BSQ-DG-V2 weight values. */
typedef struct {
	int16_t sRawValue;
	int32_t lWeightTenthGram;
	uint16_t usDecimalPlaces;
	uint16_t usUnit;
} ScaleBsqDgV2Image_t;

/** @brief Describe the BSQ-DG-V2 Modbus protocol. */
extern const DeviceDriverDescriptor_t g_xScaleBsqDgV2Driver;

/**
  * @brief  Read and normalize the BSQ-DG-V2 weight registers.
  * @param[in,out] pxPort Initialized Modbus client port.
  * @param[in] ucUnitId Device Unit ID.
  * @param[in] ulTimeoutMs Modbus transaction timeout in milliseconds.
  * @param[out] pxImage Caller-owned destination image.
  * @retval MODBUS_PORT_RESULT_OK Data read and normalized.
  * @retval MODBUS_PORT_RESULT_PROTOCOL Decimal or unit code is unsupported.
  */
ModbusPortResult_e xScaleBsqDgV2Refresh(ModbusPort_t *pxPort,
	uint8_t ucUnitId, uint32_t ulTimeoutMs,
	ScaleBsqDgV2Image_t *pxImage);

/**
  * @brief  Tare the weighing module.
  * @param[in,out] pxPort Initialized Modbus client port.
  * @param[in] ucUnitId Device Unit ID.
  * @param[in] ulTimeoutMs Modbus transaction timeout in milliseconds.
  * @retval MODBUS_PORT_RESULT_OK Command completed.
  */
ModbusPortResult_e xScaleBsqDgV2Tare(ModbusPort_t *pxPort,
	uint8_t ucUnitId, uint32_t ulTimeoutMs);

/**
  * @brief  Clear the weighing-module tare value.
  * @param[in,out] pxPort Initialized Modbus client port.
  * @param[in] ucUnitId Device Unit ID.
  * @param[in] ulTimeoutMs Modbus transaction timeout in milliseconds.
  * @retval MODBUS_PORT_RESULT_OK Command completed.
  */
ModbusPortResult_e xScaleBsqDgV2ClearTare(ModbusPort_t *pxPort,
	uint8_t ucUnitId, uint32_t ulTimeoutMs);

/**
  * @brief  Zero the weighing module.
  * @param[in,out] pxPort Initialized Modbus client port.
  * @param[in] ucUnitId Device Unit ID.
  * @param[in] ulTimeoutMs Modbus transaction timeout in milliseconds.
  * @retval MODBUS_PORT_RESULT_OK Command completed.
  */
ModbusPortResult_e xScaleBsqDgV2Zero(ModbusPort_t *pxPort,
	uint8_t ucUnitId, uint32_t ulTimeoutMs);

#ifdef __cplusplus
}
#endif

#endif /* SCALE_BSQ_DG_V2_H */
