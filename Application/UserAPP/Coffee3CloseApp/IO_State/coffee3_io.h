/**
  * @file      coffee3_io.h
  * @brief     Define one global image for GPIO and Modbus IO modules.
  * @author    WHong
  * @date      2026-07-30
  */

#ifndef COFFEE3_IO_H
#define COFFEE3_IO_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/** @brief Number of direct STM32 input and output points. */
#define COFFEE3_LOCAL_IO_COUNT               8U
/** @brief Points fitted on each external IO module in the electrical plan. */
#define COFFEE3_MODBUS_IO_COUNT              16U

typedef enum {
	COFFEE3_LOCAL_DI_DOOR_UPPER = 0,
	COFFEE3_LOCAL_DI_DOOR_LOWER = 1,
	COFFEE3_LOCAL_DI_COFFEE_WATER_HIGH = 2,
	COFFEE3_LOCAL_DI_COFFEE_WATER_LOW = 3,
	COFFEE3_LOCAL_DI_HOT_WATER_HIGH = 4,
	COFFEE3_LOCAL_DI_HOT_WATER_LOW = 5
} Coffee3LocalInputPoint_e;

typedef enum {
	COFFEE3_LOCAL_DO_DOOR_UP = 0,
	COFFEE3_LOCAL_DO_DOOR_DOWN = 1,
	COFFEE3_LOCAL_DO_HOT_WATER_SUPPLY_VALVE = 2,
	COFFEE3_LOCAL_DO_COFFEE_WATER_PUMP = 3
} Coffee3LocalOutputPoint_e;

typedef enum {
	COFFEE3_EXTERNAL_DI_OUTPUT_FRONT_CUP = 0,
	COFFEE3_EXTERNAL_DI_OUTPUT_REAR_CUP = 1,
	COFFEE3_EXTERNAL_DI_OUTLET_CUP = 2,
	COFFEE3_EXTERNAL_DI_PURE_WATER_LOW = 3,
	COFFEE3_EXTERNAL_DI_WASTE_BIN_PRESENT = 4,
	COFFEE3_EXTERNAL_DI_MILK_LOW = 10,
	COFFEE3_EXTERNAL_DI_FRUIT_MILK_A_LOW = 11,
	COFFEE3_EXTERNAL_DI_FRUIT_MILK_B_LOW = 12
} Coffee3ExternalInputPoint_e;

typedef enum {
	COFFEE3_EXTERNAL_DO_WATER_HEATER_RELAY = 0,
	COFFEE3_EXTERNAL_DO_MILK_VALVE = 3,
	COFFEE3_EXTERNAL_DO_FRUIT_MILK_A_VALVE = 4,
	COFFEE3_EXTERNAL_DO_FRUIT_MILK_B_VALVE = 5,
	COFFEE3_EXTERNAL_DO_FRUIT_MILK_A_PUMP = 9,
	COFFEE3_EXTERNAL_DO_FRUIT_MILK_B_PUMP = 10
} Coffee3ExternalOutputPoint_e;

/** @brief Store all input points consumed by workflow and device tasks. */
typedef struct {
	uint8_t aucXPin[COFFEE3_LOCAL_IO_COUNT];
	uint8_t aucMB1XPin[COFFEE3_MODBUS_IO_COUNT];
	uint8_t aucMB2XPin[COFFEE3_MODBUS_IO_COUNT];
} Coffee3InputIo_t;

/** @brief Store all output points consumed by workflow and device tasks. */
typedef struct {
	uint8_t aucYPin[COFFEE3_LOCAL_IO_COUNT];
	uint8_t aucMB1YPin[COFFEE3_MODBUS_IO_COUNT];
	uint8_t aucMB2YPin[COFFEE3_MODBUS_IO_COUNT];
} Coffee3OutputIo_t;

/** @brief Store one coherent global IO image and update metadata. */
typedef struct {
	Coffee3InputIo_t xInput;
	Coffee3OutputIo_t xOutput;
	uint32_t ulVersion;
	uint32_t ulLocalUpdateTick;
	uint32_t aulModbusUpdateTick[2];
	uint8_t aucModbusValid[2];
} Coffee3IoState_t;

/** @brief Global IO image available to all Coffee3 application modules. */
extern Coffee3IoState_t g_xCoffee3Io;

/** @brief Clear the global image and sample local GPIO once. */
void vCoffee3IoInitialize(void);

/** @brief Refresh direct STM32 input and output point values. */
void vCoffee3IoRefreshLocal(void);

/**
  * @brief Set one direct STM32 output and update the global image.
  * @param[in] ucIndex Zero-based direct output index.
  * @param[in] ucValue Zero clears the output; nonzero sets it.
  * @retval 1 The point was written.
  * @retval 0 The index was invalid.
  */
uint8_t ucCoffee3IoSetLocalOutput(uint8_t ucIndex, uint8_t ucValue);

/** @brief Apply one complete debug mask without workflow interlocks. */
uint8_t ucCoffee3IoApplyLocalDebugMask(uint16_t usMask);

/** @brief Commit the dedicated Unit 1 input-module image. */
void vCoffee3IoCommitModbusInput(const uint8_t *pucInputs);

/** @brief Commit the dedicated Unit 2 output-module image. */
void vCoffee3IoCommitModbusOutputImage(const uint8_t *pucOutputs);

/**
  * @brief Copy a coherent global IO snapshot.
  * @param[out] pxSnapshot Caller-owned destination.
  */
void vCoffee3IoGetSnapshot(Coffee3IoState_t *pxSnapshot);

#ifdef __cplusplus
}
#endif

#endif /* COFFEE3_IO_H */
