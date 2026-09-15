#ifndef CONFIG_STORE_H
#define CONFIG_STORE_H

#include <stdint.h>

#define CONFIG_STORE_ADDRESS 0x08008000UL
#define CONFIG_STORE_SIZE    0x00004000UL

typedef enum {
	CONFIG_STORE_OK = 0,
	CONFIG_STORE_INVALID = 1,
	CONFIG_STORE_WRITE_FAILED = 2,
	CONFIG_STORE_BUSY = 3
} ConfigStoreResult_e;

/* Caller owns serialization. Word zero is the format validity marker. */
ConfigStoreResult_e xConfigStoreRead(uint32_t *pulWords, uint32_t ulCount);
ConfigStoreResult_e xConfigStoreWrite(const uint32_t *pulWords,
	uint32_t ulCount);

#endif
