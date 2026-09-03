/**
  * @file      app_crash_diag.c
  * @brief     Implement scheduler-independent fatal crash diagnostics.
  * @author    WHong
  * @date      2026-07-28
  */

#include "app_crash_diag.h"

#include "app_crash_diag_config.h"
#include "compiler_compat.h"
#include "FreeRTOS.h"
#include "task.h"
#include "stm32f4xx.h"

/** @brief Magic value marking a fully published task record. */
#define APP_CRASH_TASK_VALID_MAGIC       0x43524153UL
/** @brief Inclusive start address of CPU-only CCM RAM. */
#define APP_CRASH_CCM_START              0x10000000UL
/** @brief Inclusive end address of CPU-only CCM RAM. */
#define APP_CRASH_CCM_END                0x1000FFFFUL
/** @brief Inclusive start address of main SRAM. */
#define APP_CRASH_SRAM_START             0x20000000UL
/** @brief Inclusive end address of main SRAM. */
#define APP_CRASH_SRAM_END               0x2001FFFFUL
/** @brief Word count in an extended floating-point exception frame. */
#define APP_CRASH_EXTENDED_FRAME_WORDS   18U
/** @brief Word count in the basic Cortex-M exception frame. */
#define APP_CRASH_CORE_FRAME_WORDS        8U

/** @brief Store immutable task metadata required after scheduler failure. */
typedef struct {
	void *pvTaskHandle; /*!< FreeRTOS task handle. */
	uint8_t *pucStackLow; /*!< Inclusive low stack address. */
	uint8_t *pucStackHigh; /*!< Exclusive high stack address. */
	char acTaskName[configMAX_TASK_NAME_LEN]; /*!< Bounded task-name copy. */
	uint32_t ulPriority; /*!< Priority recorded at task creation. */
	volatile uint32_t ulValid; /*!< Publication marker written last. */
} AppCrashTaskRecord_t;

/** @brief Static registry of task metadata available after a fatal event. */
static AppCrashTaskRecord_t s_axCrashTasks[APP_CRASH_MAX_TASKS];
/** @brief Static fatal report output buffer. */
static uint8_t s_aucCrashText[APP_CRASH_TEXT_BUFFER_SIZE];
/** @brief Number of valid bytes currently stored in s_aucCrashText. */
static uint16_t s_usCrashTextLength;
/** @brief Prevent nested fatal paths from rebuilding shared diagnostic state. */
static volatile uint8_t s_ucCrashActive;
/** @brief Count task registrations dropped when the registry is full. */
static volatile uint32_t s_ulTaskRegistrationDropCount;

/* A target may override this weak fatal-output hook. */
APP_WEAK int32_t lAppCrashDiagWrite(const uint8_t *pucData,
	uint16_t usLength)
{
	(void)pucData;
	(void)usLength;
	return -1;
}

/** @brief Task most recently selected by the FreeRTOS switch trace hook. */
void * volatile g_pvAppCrashCurrentTask;
/** @brief Registers captured by the ARMCC exception assembly wrapper. */
volatile uint32_t g_aulAppCrashSavedRegisters[10];

/**
  * @brief  启用并复位 Cortex-M DWT 周期计数器。
  * @note   该计数器是调度器不可用时唯一的崩溃报告时间基准。
  */
static void prvEnableCycleCounter(void);
/**
  * @brief  重复输出冻结的崩溃报告并服务可选独立看门狗。
  * @note   运行于致命错误路径，不得调用会依赖 RTOS 的接口。
  */
static void prvCrashRun(void);
/**
  * @brief  仅使用 DWT 周期计数器等待一个重复输出周期。
  */
static void prvCrashDelayRepeatPeriod(void);
/**
  * @brief  根据 Cortex-M 异常栈帧构造致命错误报告。
  * @param[in] pulFaultStack 异常入口保存的核心寄存器栈指针。
  * @param[in] ulExcReturn 异常返回值，用于判断异常栈格式。
  * @param[in] ulReason 工程崩溃原因码。
  */
static void prvBuildFaultText(uint32_t *pulFaultStack,
	uint32_t ulExcReturn, uint32_t ulReason);
/**
  * @brief  为 FreeRTOS Hook 失败构造致命错误报告。
  * @param[in] pvTaskHandle 触发错误的任务句柄，可以为 NULL。
  * @param[in] pcTaskName 任务名称，可以为 NULL。
  * @param[in] ulReason 工程崩溃原因码。
  */
static void prvBuildRtosText(void *pvTaskHandle,
	const char *pcTaskName, uint32_t ulReason);
/**
  * @brief  为 configASSERT 失败构造致命错误报告。
  * @param[in] pcFile 触发断言的源文件名，可以为 NULL。
  * @param[in] ulLine 触发断言的源代码行号。
  * @param[in] ulReason 工程崩溃原因码。
  */
static void prvBuildAssertText(const char *pcFile,
	uint32_t ulLine, uint32_t ulReason);
/**
  * @brief  追加报告头和致命错误原因。
  * @param[in] ulReason 工程崩溃原因码。
  */
static void prvAppendCommonHeader(uint32_t ulReason);
/**
  * @brief  追加 ARMCC 汇编包装器保存的寄存器。
  */
static void prvAppendSavedRegisters(void);
/**
  * @brief  追加 Cortex-M SCB 故障寄存器。
  */
static void prvAppendScbRegisters(void);
/**
  * @brief  将有效的 SCB 故障位解码为稳定文本。
  */
static void prvAppendFaultDecode(void);
/**
  * @brief  追加已登记任务信息和栈高水位数据。
  */
static void prvAppendTaskRecords(void);
/**
  * @brief  按 FreeRTOS 任务句柄查找不可变任务信息。
  * @param[in] pvTaskHandle 待查找的任务句柄。
  * @retval 匹配的任务记录；找不到时返回 NULL。
  */
static const AppCrashTaskRecord_t *prvFindTaskRecord(
	void *pvTaskHandle);
/**
  * @brief  校验范围后统计未触碰的 FreeRTOS 栈填充字数。
  * @param[in] pxTask 待扫描的任务记录。
  * @param[out] pucValid 输出范围校验结果。
  * @retval 未使用栈空间的字数。
  */
static uint32_t prvGetStackHighWaterWords(
	const AppCrashTaskRecord_t *pxTask, uint8_t *pucValid);
/**
  * @brief  校验地址范围是否位于 CCM 或主 SRAM。
  * @param[in] ulStart 起始地址。
  * @param[in] ulLength 范围长度，单位为字节。
  * @retval 1 范围完全落在允许的 CPU RAM 中。
  * @retval 0 范围越界或发生地址溢出。
  */
static uint8_t prvIsCpuRamRange(uint32_t ulStart,
	uint32_t ulLength);
/**
  * @brief  在扫描任务栈前校验一条已登记的任务栈范围。
  * @param[in] pxTask 待校验的任务记录。
  * @retval 1 栈范围有效且满足字对齐要求。
  * @retval 0 记录无效或范围不可访问。
  */
static uint8_t prvIsTaskStackRangeValid(
	const AppCrashTaskRecord_t *pxTask);
/**
  * @brief  将崩溃原因码映射为稳定报告文本。
  * @param[in] ulReason 工程崩溃原因码。
  * @retval 静态原因文本指针。
  */
static const char *prvReasonText(uint32_t ulReason);
/**
  * @brief  清空有界致命报告文本缓冲区。
  */
static void prvTextReset(void);
/**
  * @brief  追加以 NULL 结尾的文本且不超过报告容量。
  * @param[in] pcText 待追加文本，可以为 NULL。
  */
static void prvAppendText(const char *pcText);
/**
  * @brief  追加一个字符且不超过报告容量。
  * @param[in] cValue 待追加字符。
  */
static void prvAppendChar(char cValue);
/**
  * @brief  将 32 位数值按八位十六进制追加到报告。
  * @param[in] ulValue 待格式化的 32 位数值。
  */
static void prvAppendHex32(uint32_t ulValue);
/**
  * @brief  将一个无符号数按十进制追加到报告。
  * @param[in] ulValue 待格式化的无符号数值。
  */
static void prvAppendUnsigned(uint32_t ulValue);
/**
  * @brief  向报告追加 CRLF 行结束符。
  */
static void prvAppendLineEnd(void);

/* Enables the only fatal-path time base. */
/*-----------------------------------------------------------*/
void vAppCrashDiagInit(void)
{
#if (APP_CRASH_DIAG_ENABLE != 0U)
	prvEnableCycleCounter();
#endif
}

/* Records immutable task metadata without calling a FreeRTOS API. */
/*-----------------------------------------------------------*/
void vAppCrashDiagTraceTaskCreate(void *pvTaskHandle,
	const char *pcTaskName, void *pvStackLow, void *pvStackHigh,
	uint32_t ulPriority)
{
#if (APP_CRASH_DIAG_ENABLE != 0U)
	AppCrashTaskRecord_t *pxRecord;
	uint32_t ulIndex;
	uint32_t ulNameIndex;

	if ((pvTaskHandle == NULL) || (pcTaskName == NULL) ||
		(pvStackLow == NULL) || (pvStackHigh == NULL)) {
		return;
	}

	pxRecord = NULL;
	for (ulIndex = 0U; ulIndex < APP_CRASH_MAX_TASKS; ulIndex++) {
		if ((s_axCrashTasks[ulIndex].ulValid ==
			APP_CRASH_TASK_VALID_MAGIC) &&
			(s_axCrashTasks[ulIndex].pvTaskHandle ==
			pvTaskHandle)) {
			pxRecord = &s_axCrashTasks[ulIndex];
			break;
		}
		if ((pxRecord == NULL) &&
			(s_axCrashTasks[ulIndex].ulValid !=
			APP_CRASH_TASK_VALID_MAGIC)) {
			pxRecord = &s_axCrashTasks[ulIndex];
		}
	}
	if (pxRecord == NULL) {
		s_ulTaskRegistrationDropCount++;
		return;
	}

	pxRecord->ulValid = 0U;
	pxRecord->pvTaskHandle = pvTaskHandle;
	pxRecord->pucStackLow = (uint8_t *)pvStackLow;
	pxRecord->pucStackHigh = (uint8_t *)pvStackHigh;
	pxRecord->ulPriority = ulPriority;
	for (ulNameIndex = 0U;
		ulNameIndex < (configMAX_TASK_NAME_LEN - 1U);
		ulNameIndex++) {
		pxRecord->acTaskName[ulNameIndex] =
			pcTaskName[ulNameIndex];
		if (pcTaskName[ulNameIndex] == '\0') {
			break;
		}
	}
	pxRecord->acTaskName[configMAX_TASK_NAME_LEN - 1U] = '\0';
	__DMB();
	pxRecord->ulValid = APP_CRASH_TASK_VALID_MAGIC;
#else
	(void)pvTaskHandle;
	(void)pcTaskName;
	(void)pvStackLow;
	(void)pvStackHigh;
	(void)ulPriority;
#endif
}

/* Invalidates a record before FreeRTOS releases its stack and TCB. */
/*-----------------------------------------------------------*/
void vAppCrashDiagTraceTaskDelete(void *pvTaskHandle)
{
#if (APP_CRASH_DIAG_ENABLE != 0U)
	uint32_t ulIndex;

	for (ulIndex = 0U; ulIndex < APP_CRASH_MAX_TASKS; ulIndex++) {
		if ((s_axCrashTasks[ulIndex].ulValid ==
			APP_CRASH_TASK_VALID_MAGIC) &&
			(s_axCrashTasks[ulIndex].pvTaskHandle ==
			pvTaskHandle)) {
			s_axCrashTasks[ulIndex].ulValid = 0U;
			__DMB();
			break;
		}
	}
#else
	(void)pvTaskHandle;
#endif
}

/* Handles a Cortex-M exception frame captured by the assembly wrapper. */
/*-----------------------------------------------------------*/
void vAppCrashDiagFaultEntry(uint32_t *pulFaultStack,
	uint32_t ulExcReturn, uint32_t ulReason)
{
	__disable_irq();
	if (s_ucCrashActive == 0U) {
		s_ucCrashActive = 1U;
		prvTextReset();
		prvAppendText("[CRASH][CAPTURE] source=EXCEPTION");
		prvAppendLineEnd();
		prvBuildFaultText(pulFaultStack, ulExcReturn, ulReason);
	}
	prvCrashRun();
}

/* Handles stack overflow and malloc failure without using the damaged PSP. */
/*-----------------------------------------------------------*/
void vAppCrashDiagRtosEntry(void *pvTaskHandle,
	const char *pcTaskName, uint32_t ulReason)
{
	__disable_irq();
	if (s_ucCrashActive == 0U) {
		s_ucCrashActive = 1U;
		prvTextReset();
		prvAppendText("[CRASH][CAPTURE] source=FREERTOS_HOOK");
		prvAppendLineEnd();
		prvBuildRtosText(pvTaskHandle, pcTaskName, ulReason);
	}
	prvCrashRun();
}

/* Handles configASSERT with the source location supplied by the macro. */
/*-----------------------------------------------------------*/
void vAppCrashDiagAssertCEntry(const char *pcFile,
	uint32_t ulLine, uint32_t ulReason)
{
	__disable_irq();
	if (s_ucCrashActive == 0U) {
		s_ucCrashActive = 1U;
		prvTextReset();
		prvAppendText("[CRASH][CAPTURE] source=ASSERT");
		prvAppendLineEnd();
		prvBuildAssertText(pcFile, ulLine, ulReason);
	}
	prvCrashRun();
}

/* Reloads an already-started IWDG; the feature is compile-time selected. */
/*-----------------------------------------------------------*/
void vAppCrashDiagWatchdogRefresh(void)
{
#if (APP_CRASH_IWDG_REFRESH_ENABLE != 0U)
	WRITE_REG(IWDG->KR, 0x0000AAAAUL);
#endif
}

/* Re-enables CYCCNT so the crash loop never depends on an RTOS tick. */
/*-----------------------------------------------------------*/
static void prvEnableCycleCounter(void)
{
	CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
	DWT->CYCCNT = 0U;
	DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
	__DSB();
	__ISB();
}

/* Repeats one frozen log forever using one UART and one time base. */
/*-----------------------------------------------------------*/
static void prvCrashRun(void)
{
	prvEnableCycleCounter();
	for (;;) {
		vAppCrashDiagWatchdogRefresh();
		(void)lAppCrashDiagWrite(s_aucCrashText,
			s_usCrashTextLength);
		prvCrashDelayRepeatPeriod();
	}
}

/* Waits ten seconds in 100 ms watchdog-refresh intervals by default. */
/*-----------------------------------------------------------*/
static void prvCrashDelayRepeatPeriod(void)
{
	uint32_t ulCycles;
	uint32_t ulIntervalCount;
	uint32_t ulInterval;
	uint32_t ulStart;

	ulCycles = SystemCoreClock / APP_CRASH_WATCHDOG_REFRESH_HZ;
	ulIntervalCount = APP_CRASH_REPEAT_SECONDS *
		APP_CRASH_WATCHDOG_REFRESH_HZ;

	for (ulInterval = 0U; ulInterval < ulIntervalCount;
		ulInterval++) {
		ulStart = DWT->CYCCNT;
		while ((uint32_t)(DWT->CYCCNT - ulStart) < ulCycles) {
			__NOP();
		}
		vAppCrashDiagWatchdogRefresh();
	}
}

/* Formats the hardware-stacked frame before any task data. */
/*-----------------------------------------------------------*/
static void prvBuildFaultText(uint32_t *pulFaultStack,
	uint32_t ulExcReturn, uint32_t ulReason)
{
	uint32_t *pulCoreFrame;
	uint32_t ulFrameAddress;
	uint32_t ulFrameOffset;
	uint8_t ucFrameValid;

	prvAppendCommonHeader(ulReason);
	prvAppendText("[CRASH][CORE] EXC_RETURN=");
	prvAppendHex32(ulExcReturn);
	prvAppendText(" FRAME_SP=");
	prvAppendHex32((uint32_t)pulFaultStack);
	prvAppendLineEnd();

	ulFrameOffset = ((ulExcReturn & (1UL << 4)) == 0U) ?
		APP_CRASH_EXTENDED_FRAME_WORDS : 0U;
	ulFrameAddress = (uint32_t)pulFaultStack +
		(ulFrameOffset * sizeof(uint32_t));
	ucFrameValid = prvIsCpuRamRange(ulFrameAddress,
		APP_CRASH_CORE_FRAME_WORDS * sizeof(uint32_t));
	if (ucFrameValid != 0U) {
		pulCoreFrame = (uint32_t *)ulFrameAddress;
		prvAppendText("[CRASH][CORE] R0=");
		prvAppendHex32(pulCoreFrame[0]);
		prvAppendText(" R1=");
		prvAppendHex32(pulCoreFrame[1]);
		prvAppendText(" R2=");
		prvAppendHex32(pulCoreFrame[2]);
		prvAppendText(" R3=");
		prvAppendHex32(pulCoreFrame[3]);
		prvAppendLineEnd();

		prvAppendText("[CRASH][CORE] R12=");
		prvAppendHex32(pulCoreFrame[4]);
		prvAppendText(" LR=");
		prvAppendHex32(pulCoreFrame[5]);
		prvAppendText(" PC=");
		prvAppendHex32(pulCoreFrame[6]);
		prvAppendText(" XPSR=");
		prvAppendHex32(pulCoreFrame[7]);
		prvAppendLineEnd();
	} else {
		prvAppendText("[CRASH][CORE] frame_valid=0");
		prvAppendLineEnd();
	}

	prvAppendSavedRegisters();
	prvAppendScbRegisters();
	prvAppendFaultDecode();
	prvAppendTaskRecords();
	prvAppendText("[CRASH][END] repeat_seconds=");
	prvAppendUnsigned(APP_CRASH_REPEAT_SECONDS);
	prvAppendLineEnd();
}

/* Formats a fatal FreeRTOS hook without querying the kernel. */
/*-----------------------------------------------------------*/
static void prvBuildRtosText(void *pvTaskHandle,
	const char *pcTaskName, uint32_t ulReason)
{
	const AppCrashTaskRecord_t *pxTask;

	(void)pcTaskName;
	pxTask = prvFindTaskRecord(pvTaskHandle);
	prvAppendCommonHeader(ulReason);
	prvAppendText("[CRASH][HOOK] task_handle=");
	prvAppendHex32((uint32_t)pvTaskHandle);
	prvAppendText(" task_name=");
	prvAppendText((pxTask != NULL) ? pxTask->acTaskName : "NA");
	prvAppendLineEnd();
	prvAppendSavedRegisters();
	prvAppendScbRegisters();
	prvAppendTaskRecords();
	prvAppendText("[CRASH][END] repeat_seconds=");
	prvAppendUnsigned(APP_CRASH_REPEAT_SECONDS);
	prvAppendLineEnd();
}

/* Formats an assertion without dereferencing FreeRTOS internals. */
/*-----------------------------------------------------------*/
static void prvBuildAssertText(const char *pcFile,
	uint32_t ulLine, uint32_t ulReason)
{
	prvAppendCommonHeader(ulReason);
	prvAppendText("[CRASH][ASSERT] file=");
	prvAppendText((pcFile != NULL) ? pcFile : "NA");
	prvAppendText(" line=");
	prvAppendUnsigned(ulLine);
	prvAppendLineEnd();
	prvAppendSavedRegisters();
	prvAppendScbRegisters();
	prvAppendTaskRecords();
	prvAppendText("[CRASH][END] repeat_seconds=");
	prvAppendUnsigned(APP_CRASH_REPEAT_SECONDS);
	prvAppendLineEnd();
}

/* Adds the stable reason and active task identity. */
/*-----------------------------------------------------------*/
static void prvAppendCommonHeader(uint32_t ulReason)
{
	prvAppendText("[CRASH][BEGIN] reason=");
	prvAppendText(prvReasonText(ulReason));
	prvAppendText(" code=");
	prvAppendUnsigned(ulReason);
	prvAppendText(" current_handle=");
	prvAppendHex32((uint32_t)g_pvAppCrashCurrentTask);
	prvAppendLineEnd();
}

/* Adds callee-saved registers and exact pre-C MSP/PSP values. */
/*-----------------------------------------------------------*/
static void prvAppendSavedRegisters(void)
{
	uint32_t ulIndex;

	for (ulIndex = 0U; ulIndex < 8U; ulIndex += 4U) {
		prvAppendText("[CRASH][CORE] R");
		prvAppendUnsigned(ulIndex + 4U);
		prvAppendChar('=');
		prvAppendHex32(g_aulAppCrashSavedRegisters[ulIndex]);
		prvAppendText(" R");
		prvAppendUnsigned(ulIndex + 5U);
		prvAppendChar('=');
		prvAppendHex32(g_aulAppCrashSavedRegisters[ulIndex + 1U]);
		prvAppendText(" R");
		prvAppendUnsigned(ulIndex + 6U);
		prvAppendChar('=');
		prvAppendHex32(g_aulAppCrashSavedRegisters[ulIndex + 2U]);
		prvAppendText(" R");
		prvAppendUnsigned(ulIndex + 7U);
		prvAppendChar('=');
		prvAppendHex32(g_aulAppCrashSavedRegisters[ulIndex + 3U]);
		prvAppendLineEnd();
	}

	prvAppendText("[CRASH][STACK] MSP=");
	prvAppendHex32(g_aulAppCrashSavedRegisters[8]);
	prvAppendText(" PSP=");
	prvAppendHex32(g_aulAppCrashSavedRegisters[9]);
	prvAppendText(" CONTROL=");
	prvAppendHex32(__get_CONTROL());
	prvAppendText(" PRIMASK=");
	prvAppendHex32(__get_PRIMASK());
	prvAppendText(" BASEPRI=");
	prvAppendHex32(__get_BASEPRI());
	prvAppendText(" FAULTMASK=");
	prvAppendHex32(__get_FAULTMASK());
	prvAppendLineEnd();
}

/* Adds raw SCB registers needed for offline fault decoding. */
/*-----------------------------------------------------------*/
static void prvAppendScbRegisters(void)
{
	prvAppendText("[CRASH][SCB] CFSR=");
	prvAppendHex32(SCB->CFSR);
	prvAppendText(" HFSR=");
	prvAppendHex32(SCB->HFSR);
	prvAppendText(" DFSR=");
	prvAppendHex32(SCB->DFSR);
	prvAppendText(" AFSR=");
	prvAppendHex32(SCB->AFSR);
	prvAppendLineEnd();

	prvAppendText("[CRASH][SCB] MMFAR=");
	prvAppendHex32(SCB->MMFAR);
	prvAppendText(" BFAR=");
	prvAppendHex32(SCB->BFAR);
	prvAppendText(" SHCSR=");
	prvAppendHex32(SCB->SHCSR);
	prvAppendText(" ICSR=");
	prvAppendHex32(SCB->ICSR);
	prvAppendLineEnd();
}

/* Adds the most useful Cortex-M fault flags without a lookup tool. */
/*-----------------------------------------------------------*/
static void prvAppendFaultDecode(void)
{
	uint32_t ulCfsr;
	uint32_t ulHfsr;

	ulCfsr = SCB->CFSR;
	ulHfsr = SCB->HFSR;
	prvAppendText("[CRASH][DECODE]");
	if ((ulHfsr & SCB_HFSR_FORCED_Msk) != 0U) {
		prvAppendText(" FORCED");
	}
	if ((ulCfsr & (1UL << 9)) != 0U) {
		prvAppendText(" PRECISERR");
	}
	if ((ulCfsr & (1UL << 10)) != 0U) {
		prvAppendText(" IMPRECISERR");
	}
	if ((ulCfsr & (1UL << 15)) != 0U) {
		prvAppendText(" BFARVALID");
	}
	if ((ulCfsr & (1UL << 7)) != 0U) {
		prvAppendText(" MMARVALID");
	}
	if ((ulCfsr & (1UL << 16)) != 0U) {
		prvAppendText(" UNDEFINSTR");
	}
	if ((ulCfsr & (1UL << 17)) != 0U) {
		prvAppendText(" INVSTATE");
	}
	if ((ulCfsr & (1UL << 18)) != 0U) {
		prvAppendText(" INVPC");
	}
	if ((ulCfsr & (1UL << 24)) != 0U) {
		prvAppendText(" UNALIGNED");
	}
	if ((ulCfsr & (1UL << 25)) != 0U) {
		prvAppendText(" DIVBYZERO");
	}
	prvAppendLineEnd();
}

/* Scans registered stack ranges only after the scheduler is abandoned. */
/*-----------------------------------------------------------*/
static void prvAppendTaskRecords(void)
{
	const AppCrashTaskRecord_t *pxTask;
	uint32_t ulTaskCount;
	uint32_t ulIndex;
	uint32_t ulHighWaterWords;
	uint8_t ucHighWaterValid;

	ulTaskCount = 0U;
	for (ulIndex = 0U; ulIndex < APP_CRASH_MAX_TASKS; ulIndex++) {
		if (s_axCrashTasks[ulIndex].ulValid ==
			APP_CRASH_TASK_VALID_MAGIC) {
			ulTaskCount++;
		}
	}
	prvAppendText("[CRASH][RTOS] registered_tasks=");
	prvAppendUnsigned(ulTaskCount);
	prvAppendText(" registration_drops=");
	prvAppendUnsigned(s_ulTaskRegistrationDropCount);
	prvAppendLineEnd();

	for (ulIndex = 0U; ulIndex < APP_CRASH_MAX_TASKS; ulIndex++) {
		pxTask = &s_axCrashTasks[ulIndex];
		if (pxTask->ulValid != APP_CRASH_TASK_VALID_MAGIC) {
			continue;
		}

		ulHighWaterWords = prvGetStackHighWaterWords(pxTask,
			&ucHighWaterValid);
		prvAppendText("[CRASH][TASK] name=");
		prvAppendText(pxTask->acTaskName);
		prvAppendText(" handle=");
		prvAppendHex32((uint32_t)pxTask->pvTaskHandle);
		prvAppendText(" current=");
		prvAppendUnsigned((pxTask->pvTaskHandle ==
			g_pvAppCrashCurrentTask) ? 1U : 0U);
		prvAppendText(" priority=");
		prvAppendUnsigned(pxTask->ulPriority);
		prvAppendText(" stack_low=");
		prvAppendHex32((uint32_t)pxTask->pucStackLow);
		prvAppendText(" stack_high=");
		prvAppendHex32((uint32_t)pxTask->pucStackHigh);
		if (ucHighWaterValid != 0U) {
			prvAppendText(" hwm_words=");
			prvAppendUnsigned(ulHighWaterWords);
			prvAppendText(" hwm_bytes=");
			prvAppendUnsigned(ulHighWaterWords *
				sizeof(StackType_t));
		} else {
			prvAppendText(" hwm=INVALID_RANGE");
		}
		prvAppendLineEnd();
	}
}

/* Finds the independent metadata copy for a task without kernel access. */
/*-----------------------------------------------------------*/
static const AppCrashTaskRecord_t *prvFindTaskRecord(
	void *pvTaskHandle)
{
	uint32_t ulIndex;

	for (ulIndex = 0U; ulIndex < APP_CRASH_MAX_TASKS; ulIndex++) {
		if ((s_axCrashTasks[ulIndex].ulValid ==
			APP_CRASH_TASK_VALID_MAGIC) &&
			(s_axCrashTasks[ulIndex].pvTaskHandle ==
			pvTaskHandle)) {
			return &s_axCrashTasks[ulIndex];
		}
	}
	return NULL;
}

/* Returns FreeRTOS-compatible high-water words from the 0xA5 fill. */
/*-----------------------------------------------------------*/
static uint32_t prvGetStackHighWaterWords(
	const AppCrashTaskRecord_t *pxTask, uint8_t *pucValid)
{
	const uint8_t *pucCurrent;
	const uint8_t *pucEnd;
	uint32_t ulFreeBytes;

	*pucValid = prvIsTaskStackRangeValid(pxTask);
	if (*pucValid == 0U) {
		return 0U;
	}

	pucCurrent = pxTask->pucStackLow;
	pucEnd = pxTask->pucStackHigh + sizeof(StackType_t);
	ulFreeBytes = 0U;
	while ((pucCurrent < pucEnd) &&
		(*pucCurrent == APP_CRASH_STACK_FILL_BYTE)) {
		ulFreeBytes++;
		pucCurrent++;
	}
	return ulFreeBytes / sizeof(StackType_t);
}

/* Accepts only complete ranges inside CCM or the two SRAM banks. */
/*-----------------------------------------------------------*/
static uint8_t prvIsCpuRamRange(uint32_t ulStart,
	uint32_t ulLength)
{
	uint32_t ulEnd;

	if (ulLength == 0U) {
		return 0U;
	}
	ulEnd = ulStart + ulLength - 1U;
	if (ulEnd < ulStart) {
		return 0U;
	}
	if ((ulStart >= APP_CRASH_CCM_START) &&
		(ulEnd <= APP_CRASH_CCM_END)) {
		return 1U;
	}
	if ((ulStart >= APP_CRASH_SRAM_START) &&
		(ulEnd <= APP_CRASH_SRAM_END)) {
		return 1U;
	}
	return 0U;
}

/* Rejects corrupted metadata before scanning any task stack byte. */
/*-----------------------------------------------------------*/
static uint8_t prvIsTaskStackRangeValid(
	const AppCrashTaskRecord_t *pxTask)
{
	uint32_t ulLow;
	uint32_t ulHigh;
	uint32_t ulLength;

	ulLow = (uint32_t)pxTask->pucStackLow;
	ulHigh = (uint32_t)pxTask->pucStackHigh;
	if ((ulLow > ulHigh) ||
		((ulLow & 0x3U) != 0U) ||
		((ulHigh & 0x3U) != 0U)) {
		return 0U;
	}
	ulLength = (ulHigh - ulLow) + sizeof(StackType_t);
	if (ulLength > APP_CRASH_MAX_STACK_BYTES) {
		return 0U;
	}
	return prvIsCpuRamRange(ulLow, ulLength);
}

/* Returns static strings so the crash formatter never allocates memory. */
/*-----------------------------------------------------------*/
static const char *prvReasonText(uint32_t ulReason)
{
	switch ((AppCrashReason_e)ulReason) {
	case APP_CRASH_REASON_HARD_FAULT:
		return "HARD_FAULT";
	case APP_CRASH_REASON_MEM_MANAGE:
		return "MEM_MANAGE";
	case APP_CRASH_REASON_BUS_FAULT:
		return "BUS_FAULT";
	case APP_CRASH_REASON_USAGE_FAULT:
		return "USAGE_FAULT";
	case APP_CRASH_REASON_STACK_OVERFLOW:
		return "STACK_OVERFLOW";
	case APP_CRASH_REASON_MALLOC_FAILED:
		return "MALLOC_FAILED";
	case APP_CRASH_REASON_ASSERT:
		return "ASSERT";
	default:
		return "UNKNOWN";
	}
}

/* Starts a new frozen text image. */
/*-----------------------------------------------------------*/
static void prvTextReset(void)
{
	s_usCrashTextLength = 0U;
	s_aucCrashText[0] = '\0';
}

/* Appends a bounded zero-terminated string. */
/*-----------------------------------------------------------*/
static void prvAppendText(const char *pcText)
{
	if (pcText == NULL) {
		return;
	}
	while (*pcText != '\0') {
		prvAppendChar(*pcText);
		pcText++;
	}
}

/* Keeps one trailing byte reserved for a terminator. */
/*-----------------------------------------------------------*/
static void prvAppendChar(char cValue)
{
	if (s_usCrashTextLength <
		(APP_CRASH_TEXT_BUFFER_SIZE - 1U)) {
		s_aucCrashText[s_usCrashTextLength] =
			(uint8_t)cValue;
		s_usCrashTextLength++;
		s_aucCrashText[s_usCrashTextLength] = '\0';
	}
}

/* Emits a fixed-width address or register value. */
/*-----------------------------------------------------------*/
static void prvAppendHex32(uint32_t ulValue)
{
	static const char acHex[] = "0123456789ABCDEF";
	int32_t lShift;

	prvAppendText("0x");
	for (lShift = 28; lShift >= 0; lShift -= 4) {
		prvAppendChar(acHex[(ulValue >> lShift) & 0x0FU]);
	}
}

/* Emits an unsigned integer without printf or division buffers on the PSP. */
/*-----------------------------------------------------------*/
static void prvAppendUnsigned(uint32_t ulValue)
{
	char acDigits[10];
	uint32_t ulCount;

	if (ulValue == 0U) {
		prvAppendChar('0');
		return;
	}
	ulCount = 0U;
	while ((ulValue != 0U) && (ulCount < sizeof(acDigits))) {
		acDigits[ulCount] = (char)('0' + (ulValue % 10U));
		ulValue /= 10U;
		ulCount++;
	}
	while (ulCount > 0U) {
		ulCount--;
		prvAppendChar(acDigits[ulCount]);
	}
}

/* Uses terminal-friendly line endings for common serial assistants. */
/*-----------------------------------------------------------*/
static void prvAppendLineEnd(void)
{
	prvAppendText("\r\n");
}
