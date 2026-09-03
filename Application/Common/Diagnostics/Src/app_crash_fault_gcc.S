/**
  * @file      app_crash_fault_gcc.S
  * @brief     Capture GNU Cortex-M exception and FreeRTOS fatal context.
  * @author    WHong
  * @date      2026-08-03
  */

.syntax unified
.cpu cortex-m4
.fpu fpv4-sp-d16
.thumb

.extern vAppCrashDiagFaultEntry
.extern vAppCrashDiagRtosEntry
.extern vAppCrashDiagAssertCEntry
.extern g_aulAppCrashSavedRegisters

.global AppHardFault_Handler
.global AppMemManage_Handler
.global AppBusFault_Handler
.global AppUsageFault_Handler
.global vApplicationStackOverflowHook
.global vApplicationMallocFailedHook
.global vAppCrashDiagAssertEntry

.section .text.AppCrashFault, "ax", %progbits
.align 2

.thumb_func
AppHardFault_Handler:
	movs r2, #1
	b AppCrashFaultCommon

.thumb_func
AppMemManage_Handler:
	movs r2, #2
	b AppCrashFaultCommon

.thumb_func
AppBusFault_Handler:
	movs r2, #3
	b AppCrashFaultCommon

.thumb_func
AppUsageFault_Handler:
	movs r2, #4
	b AppCrashFaultCommon

.thumb_func
AppCrashFaultCommon:
	cpsid i
	ldr r3, =g_aulAppCrashSavedRegisters
	stmia r3!, {r4-r11}
	mrs r4, msp
	mrs r5, psp
	stmia r3!, {r4-r5}
	tst lr, #4
	ite eq
	mrseq r0, msp
	mrsne r0, psp
	mov r1, lr
	b vAppCrashDiagFaultEntry

.thumb_func
vApplicationStackOverflowHook:
	movs r2, #5
	b AppCrashRtosCommon

.thumb_func
vApplicationMallocFailedHook:
	movs r0, #0
	movs r1, #0
	movs r2, #6
	b AppCrashRtosCommon

.thumb_func
AppCrashRtosCommon:
	cpsid i
	ldr r3, =g_aulAppCrashSavedRegisters
	stmia r3!, {r4-r11}
	mrs r4, msp
	mrs r5, psp
	stmia r3!, {r4-r5}
	mrs r3, control
	bic r3, r3, #2
	msr control, r3
	isb
	b vAppCrashDiagRtosEntry

.thumb_func
vAppCrashDiagAssertEntry:
	movs r2, #7
	cpsid i
	ldr r3, =g_aulAppCrashSavedRegisters
	stmia r3!, {r4-r11}
	mrs r4, msp
	mrs r5, psp
	stmia r3!, {r4-r5}
	mrs r3, control
	bic r3, r3, #2
	msr control, r3
	isb
	b vAppCrashDiagAssertCEntry

