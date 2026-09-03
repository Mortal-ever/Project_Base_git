; @file      app_crash_fault_armcc.s
; @brief     Capture ARMCC exception and FreeRTOS fatal-hook registers.
; @author    WHong
; @date      2026-07-28
;
; This file contains ARM Compiler V5 assembly entry points. Each entry switches
; to a known stack context before transferring control to the C diagnostic path.

                AREA    |.text|, CODE, READONLY
                THUMB
                PRESERVE8

                IMPORT  vAppCrashDiagFaultEntry
                IMPORT  vAppCrashDiagRtosEntry
                IMPORT  vAppCrashDiagAssertCEntry
                IMPORT  g_aulAppCrashSavedRegisters

                EXPORT  AppHardFault_Handler
                EXPORT  AppMemManage_Handler
                EXPORT  AppBusFault_Handler
                EXPORT  AppUsageFault_Handler
                EXPORT  vApplicationStackOverflowHook
                EXPORT  vApplicationMallocFailedHook
                EXPORT  vAppCrashDiagAssertEntry

; Route HardFault with APP_CRASH_REASON_HARD_FAULT.
AppHardFault_Handler PROC
                MOVS    R2, #1
                B       AppCrashFaultCommon
                ENDP

; Route MemManage with APP_CRASH_REASON_MEM_MANAGE.
AppMemManage_Handler PROC
                MOVS    R2, #2
                B       AppCrashFaultCommon
                ENDP

; Route BusFault with APP_CRASH_REASON_BUS_FAULT.
AppBusFault_Handler PROC
                MOVS    R2, #3
                B       AppCrashFaultCommon
                ENDP

; Route UsageFault with APP_CRASH_REASON_USAGE_FAULT.
AppUsageFault_Handler PROC
                MOVS    R2, #4
                B       AppCrashFaultCommon
                ENDP

; Freeze interrupts, capture R4-R11/MSP/PSP, and select the exception frame.
AppCrashFaultCommon PROC
                CPSID   I
                LDR     R3, =g_aulAppCrashSavedRegisters
                STMIA   R3!, {R4-R11}
                MRS     R4, MSP
                MRS     R5, PSP
                STMIA   R3!, {R4-R5}
                TST     LR, #4
                ITE     EQ
                MRSEQ   R0, MSP
                MRSNE   R0, PSP
                MOV     R1, LR
                B       vAppCrashDiagFaultEntry
                ENDP

; Route the FreeRTOS stack-overflow hook with its task handle and name.
vApplicationStackOverflowHook PROC
                MOVS    R2, #5
                B       AppCrashRtosCommon
                ENDP

; Route the FreeRTOS allocation-failure hook without task arguments.
vApplicationMallocFailedHook PROC
                MOVS    R0, #0
                MOVS    R1, #0
                MOVS    R2, #6
                B       AppCrashRtosCommon
                ENDP

; Switch fatal RTOS hooks to MSP before entering the non-returning C path.
AppCrashRtosCommon PROC
                CPSID   I
                LDR     R3, =g_aulAppCrashSavedRegisters
                STMIA   R3!, {R4-R11}
                MRS     R4, MSP
                MRS     R5, PSP
                STMIA   R3!, {R4-R5}
                MRS     R3, CONTROL
                BIC     R3, R3, #2
                MSR     CONTROL, R3
                ISB
                B       vAppCrashDiagRtosEntry
                ENDP

; Capture assertion context and enter C diagnostics on MSP.
vAppCrashDiagAssertEntry PROC
                MOVS    R2, #7
                CPSID   I
                LDR     R3, =g_aulAppCrashSavedRegisters
                STMIA   R3!, {R4-R11}
                MRS     R4, MSP
                MRS     R5, PSP
                STMIA   R3!, {R4-R5}
                MRS     R3, CONTROL
                BIC     R3, R3, #2
                MSR     CONTROL, R3
                ISB
                B       vAppCrashDiagAssertCEntry
                ENDP

                END
