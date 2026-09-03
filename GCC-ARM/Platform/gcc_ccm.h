/**
  * @file      gcc_ccm.h
  * @brief     Define GNU linker CCM initialization support.
  * @author    WHong
  * @date      2026-08-25
  */

#ifndef GCC_CCM_H
#define GCC_CCM_H

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Clear the GNU `.ccm_bss` region before RTOS object creation. */
void vAppCcmInit(void);

#ifdef __cplusplus
}
#endif

#endif /* GCC_CCM_H */
