/**
  * @file      compiler_compat.h
  * @brief     Normalize ARMCC V5.06 and GNU Arm compiler attributes.
  * @author    WHong
  * @date      2026-08-04
  */

#ifndef COMPILER_COMPAT_H
#define COMPILER_COMPAT_H

#if defined(__CC_ARM)
#define APP_COMPILER_ARMCC                    1
#define APP_COMPILER_GNU                      0
#define APP_WEAK                              __weak
#define APP_CCM_DATA \
	__attribute__((section("CCM_APP"), zero_init, aligned(8)))
#define APP_CCM_HEAP \
	__attribute__((section("CCM_HEAP"), zero_init, aligned(8)))
#elif defined(__GNUC__)
#define APP_COMPILER_ARMCC                    0
#define APP_COMPILER_GNU                      1
#define APP_WEAK                              __attribute__((weak))
#define APP_CCM_DATA \
	__attribute__((section(".ccm_bss"), aligned(8)))
#define APP_CCM_HEAP \
	__attribute__((section(".ccm_bss"), aligned(8)))
#else
#define APP_COMPILER_ARMCC                    0
#define APP_COMPILER_GNU                      0
#define APP_WEAK
#define APP_CCM_DATA
#define APP_CCM_HEAP
#endif

#endif /* COMPILER_COMPAT_H */
