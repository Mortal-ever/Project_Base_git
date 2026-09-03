/**
  * @file      app_ccm.c
  * @brief     Initialize CPU-only CCM storage defined by the GNU linker.
  * @author    WHong
  * @date      2026-08-04
  */

#include "gcc_ccm.h"

#include <stdint.h>

extern uint32_t __ccm_bss_start__;
extern uint32_t __ccm_bss_end__;

/*-----------------------------------------------------------*/
void vAppCcmInit(void)
{
	uint32_t *pulCurrent;

	pulCurrent = &__ccm_bss_start__;
	while (pulCurrent < &__ccm_bss_end__) {
		*pulCurrent = 0U;
		pulCurrent++;
	}
}
