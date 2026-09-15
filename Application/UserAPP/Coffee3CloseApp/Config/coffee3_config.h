#ifndef COFFEE3_CONFIG_H
#define COFFEE3_CONFIG_H

#include "ConfigStore/config_store.h"

#define COFFEE3_CONFIG_STORAGE_REGISTER 0x001AU
#define COFFEE3_STORAGE_INSTALLED_MASK  0x0003U

typedef struct {
	uint32_t ulValidMark;
	uint32_t ulStorageEnabledMask;
} Coffee3Config_t;

ConfigStoreResult_e xCoffee3ConfigInitialize(void);
ConfigStoreResult_e xCoffee3ConfigSetStorageMask(uint16_t usMask);
uint16_t usCoffee3ConfigStorageMask(void);

#endif
