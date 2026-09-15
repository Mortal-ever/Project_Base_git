#ifndef COFFEE3_CONFIG_H
#define COFFEE3_CONFIG_H

#include "ConfigStore/config_store.h"

#define COFFEE3_CONFIG_MARK             0x43334331UL
#define COFFEE3_CONFIG_STORAGE_REGISTER 0x001AU
#define COFFEE3_STORAGE_INSTALLED_MASK  0x0003U
#define COFFEE3_CONFIG_FRUIT_CHANNEL_COUNT 6U
#define COFFEE3_CONFIG_FRUIT_COEFFICIENT_DEFAULT 100U
#define COFFEE3_CONFIG_FRUIT_COEFFICIENT_MIN 10U
#define COFFEE3_CONFIG_FRUIT_COEFFICIENT_MAX 1000U

typedef struct {
	uint32_t ulValidMark;
	uint32_t ulStorageEnabledMask;
	uint32_t aulFruitCoefficient[COFFEE3_CONFIG_FRUIT_CHANNEL_COUNT];
} Coffee3Config_t;

ConfigStoreResult_e xCoffee3ConfigInitialize(void);
ConfigStoreResult_e xCoffee3ConfigSetStorageMask(uint16_t usMask);
ConfigStoreResult_e xCoffee3ConfigSetFruitCoefficients(
	const uint16_t *pusCoefficients);
uint16_t usCoffee3ConfigStorageMask(void);
uint16_t usCoffee3ConfigFruitCoefficient(uint8_t ucChannel);

#endif
