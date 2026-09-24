/**
  * @file      compiler_compat.h
  * @brief     统一 ARMCC V5.06 与 GNU Arm 编译器属性写法。
  * @author    WHong
  * @date      2026-09-24
  */

#ifndef COMPILER_COMPAT_H
#define COMPILER_COMPAT_H

#if defined(__CC_ARM)
/** @brief 非零表示当前使用 ARMCC V5 编译。 */
#define APP_COMPILER_ARMCC                    1
/** @brief 非零表示当前使用 GNU Arm 编译。 */
#define APP_COMPILER_GNU                      0
/** @brief 将符号声明为可由产品目标覆盖的弱定义。 */
#define APP_WEAK                              __weak
/** @brief 将零初始化数据放入 8 字节对齐的 CCM_APP 区。 */
#define APP_CCM_DATA \
	__attribute__((section("CCM_APP"), zero_init, aligned(8)))
/** @brief 将堆存储放入 8 字节对齐的 CCM_HEAP 区。 */
#define APP_CCM_HEAP \
	__attribute__((section("CCM_HEAP"), zero_init, aligned(8)))
#elif defined(__GNUC__)
/** @brief 非零表示当前使用 ARMCC V5 编译。 */
#define APP_COMPILER_ARMCC                    0
/** @brief 非零表示当前使用 GNU Arm 编译。 */
#define APP_COMPILER_GNU                      1
/** @brief 将符号声明为可由产品目标覆盖的弱定义。 */
#define APP_WEAK                              __attribute__((weak))
/** @brief 将零初始化数据放入 8 字节对齐的 .ccm_bss 区。 */
#define APP_CCM_DATA \
	__attribute__((section(".ccm_bss"), aligned(8)))
/** @brief 将堆存储放入 8 字节对齐的 .ccm_bss 区。 */
#define APP_CCM_HEAP \
	__attribute__((section(".ccm_bss"), aligned(8)))
#else
/** @brief 未识别编译器时保持为零的 ARMCC 标志。 */
#define APP_COMPILER_ARMCC                    0
/** @brief 未识别编译器时保持为零的 GNU 标志。 */
#define APP_COMPILER_GNU                      0
/** @brief 未识别编译器时退化为空的弱定义属性。 */
#define APP_WEAK
/** @brief 未识别编译器时退化为空的 CCM 数据属性。 */
#define APP_CCM_DATA
/** @brief 未识别编译器时退化为空的 CCM 堆属性。 */
#define APP_CCM_HEAP
#endif

#endif /* COMPILER_COMPAT_H */
