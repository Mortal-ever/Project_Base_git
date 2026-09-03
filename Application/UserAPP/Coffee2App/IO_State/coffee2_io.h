/**
  * @file      coffee2_io.h
  * @brief     Define one global image for GPIO and Modbus IO modules.
  * @author    WHong
  * @date      2026-07-30
  */

#ifndef COFFEE2_IO_H
#define COFFEE2_IO_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/** @brief Number of direct STM32 input and output points. */
#define COFFEE2_LOCAL_IO_COUNT               8U
/** @brief Points fitted on each external IO module in the electrical plan. */
#define COFFEE2_MODBUS_IO_COUNT              16U

typedef enum {
	COFFEE2_LOCAL_DI_HOT_WATER_HIGH = 4,
	COFFEE2_LOCAL_DI_HOT_WATER_LOW = 5
} Coffee2LocalInputPoint_e;

typedef enum {
	COFFEE2_LOCAL_DO_HOT_WATER_SUPPLY_VALVE = 2
} Coffee2LocalOutputPoint_e;

typedef enum {
	COFFEE2_EXTERNAL_DI_OUTPUT_FRONT_CUP = 0,
	COFFEE2_EXTERNAL_DI_OUTPUT_REAR_CUP = 1,
	COFFEE2_EXTERNAL_DI_PURE_WATER_LOW = 3,
	COFFEE2_EXTERNAL_DI_WASTE_BIN_PRESENT = 4,
	COFFEE2_EXTERNAL_DI_MILK_LOW = 10,
	COFFEE2_EXTERNAL_DI_FRUIT_MILK_A_LOW = 11,
	COFFEE2_EXTERNAL_DI_FRUIT_MILK_B_LOW = 12
} Coffee2ExternalInputPoint_e;

typedef enum {
	COFFEE2_EXTERNAL_DO_WATER_HEATER_RELAY = 0,
	COFFEE2_EXTERNAL_DO_MILK_VALVE = 3,
	COFFEE2_EXTERNAL_DO_FRUIT_MILK_A_VALVE = 4,
	COFFEE2_EXTERNAL_DO_FRUIT_MILK_B_VALVE = 5,
	COFFEE2_EXTERNAL_DO_FRUIT_MILK_A_PUMP = 9,
	COFFEE2_EXTERNAL_DO_FRUIT_MILK_B_PUMP = 10,
	COFFEE2_EXTERNAL_DO_BOOSTER_PUMP = 11
} Coffee2ExternalOutputPoint_e;

/** @brief Store all input points consumed by workflow and device tasks. */
typedef struct {
	uint8_t aucXPin[COFFEE2_LOCAL_IO_COUNT];
	uint8_t aucMB1XPin[COFFEE2_MODBUS_IO_COUNT];
	uint8_t aucMB2XPin[COFFEE2_MODBUS_IO_COUNT];
} Coffee2InputIo_t;

/** @brief Store all output points consumed by workflow and device tasks. */
typedef struct {
	uint8_t aucYPin[COFFEE2_LOCAL_IO_COUNT];
	uint8_t aucMB1YPin[COFFEE2_MODBUS_IO_COUNT];
	uint8_t aucMB2YPin[COFFEE2_MODBUS_IO_COUNT];
} Coffee2OutputIo_t;

/** @brief Store one coherent global IO image and update metadata. */
typedef struct {
	Coffee2InputIo_t xInput;
	Coffee2OutputIo_t xOutput;
	uint32_t ulVersion;
	uint32_t ulLocalUpdateTick;
	uint32_t aulModbusUpdateTick[2];
	uint8_t aucModbusValid[2];
} Coffee2IoState_t;

/** @brief Global IO image available to all Coffee2 application modules. */
extern Coffee2IoState_t g_xCoffee2Io;

/** @brief Clear the global image and sample local GPIO once. */
void vCoffee2IoInitialize(void);

/** @brief Refresh direct STM32 input and output point values. */
void vCoffee2IoRefreshLocal(void);

/**
  * @brief Set one direct STM32 output and update the global image.
  * @param[in] ucIndex Zero-based direct output index.
  * @param[in] ucValue Zero clears the output; nonzero sets it.
  * @retval 1 The point was written.
  * @retval 0 The index was invalid.
  */
uint8_t ucCoffee2IoSetLocalOutput(uint8_t ucIndex, uint8_t ucValue);

/** @brief Commit the dedicated Unit 1 input-module image. */
void vCoffee2IoCommitModbusInput(const uint8_t *pucInputs);

/** @brief Commit the dedicated Unit 2 output-module image. */
void vCoffee2IoCommitModbusOutputImage(const uint8_t *pucOutputs);

/**
  * @brief Copy a coherent global IO snapshot.
  * @param[out] pxSnapshot Caller-owned destination.
  */
void vCoffee2IoGetSnapshot(Coffee2IoState_t *pxSnapshot);

#ifdef __cplusplus
}
#endif

#endif /* COFFEE2_IO_H */
