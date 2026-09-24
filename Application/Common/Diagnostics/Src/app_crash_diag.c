/**
  * @file      app_crash_diag.c
  * @brief     实现不依赖调度器的致命崩溃诊断。
  * @author    WHong
  * @date      2026-09-24
  */

#include "app_crash_diag.h"

#include "app_crash_diag_config.h"
#include "compiler_compat.h"
#include "FreeRTOS.h"
#include "task.h"
#include "stm32f4xx.h"

/** @brief 标识任务记录已经完整发布的魔数。 */
#define APP_CRASH_TASK_VALID_MAGIC       0x43524153UL
/** @brief 仅 CPU 可访问 CCM RAM 的包含式起始地址。 */
#define APP_CRASH_CCM_START              0x10000000UL
/** @brief 仅 CPU 可访问 CCM RAM 的包含式结束地址。 */
#define APP_CRASH_CCM_END                0x1000FFFFUL
/** @brief 主 SRAM 的包含式起始地址。 */
#define APP_CRASH_SRAM_START             0x20000000UL
/** @brief 主 SRAM 的包含式结束地址。 */
#define APP_CRASH_SRAM_END               0x2001FFFFUL
/** @brief 浮点扩展异常栈帧在核心栈帧之前占用的字数。 */
#define APP_CRASH_EXTENDED_FRAME_WORDS   18U
/** @brief Cortex-M 基本异常栈帧的字数。 */
#define APP_CRASH_CORE_FRAME_WORDS        8U

/** @brief 保存调度器失效后仍可独立读取的任务元数据。 */
typedef struct {
	void *pvTaskHandle; /*!< FreeRTOS 任务句柄。 */
	uint8_t *pucStackLow; /*!< 任务栈包含式低地址。 */
	uint8_t *pucStackHigh; /*!< 任务栈登记的最高 StackType_t 地址。 */
	char acTaskName[configMAX_TASK_NAME_LEN]; /*!< 有界复制的任务名。 */
	uint32_t ulPriority; /*!< 创建任务时记录的优先级。 */
	volatile uint32_t ulValid; /*!< 最后写入的完整发布标记。 */
} AppCrashTaskRecord_t;

/** @brief 致命事件后仍可读取的静态任务元数据登记表。 */
static AppCrashTaskRecord_t s_axCrashTasks[APP_CRASH_MAX_TASKS];
/** @brief 静态冻结崩溃报告输出缓冲。 */
static uint8_t s_aucCrashText[APP_CRASH_TEXT_BUFFER_SIZE];
/** @brief 冻结崩溃报告当前有效字节数。 */
static uint16_t s_usCrashTextLength;
/** @brief 防止嵌套致命路径重复改写共享诊断状态。 */
static volatile uint8_t s_ucCrashActive;
/** @brief 任务登记表已满时丢弃的登记次数。 */
static volatile uint32_t s_ulTaskRegistrationDropCount;

/**
  * @brief  提供可由产品目标覆盖的弱致命报告输出接口。
  * @param[in] pucData 冻结报告字节。
  * @param[in] usLength 报告有效字节数。
  * @retval -1 默认实现不提供输出端口。
  */
APP_WEAK int32_t lAppCrashDiagWrite(const uint8_t *pucData,
	uint16_t usLength)
{
	(void)pucData;
	(void)usLength;
	return -1;
}

/** @brief FreeRTOS 切换跟踪 Hook 最近选择的任务。 */
void * volatile g_pvAppCrashCurrentTask;
/** @brief ARMCC 异常汇编包装器保存的 R4 至 R11、MSP 和 PSP。 */
volatile uint32_t g_aulAppCrashSavedRegisters[10];

static void prvEnableCycleCounter(void);
static void prvCrashRun(void);
static void prvCrashDelayRepeatPeriod(void);
static void prvBuildFaultText(uint32_t *pulFaultStack,
	uint32_t ulExcReturn, uint32_t ulReason);
static void prvBuildRtosText(void *pvTaskHandle,
	const char *pcTaskName, uint32_t ulReason);
static void prvBuildAssertText(const char *pcFile,
	uint32_t ulLine, uint32_t ulReason);
static void prvAppendCommonHeader(uint32_t ulReason);
static void prvAppendSavedRegisters(void);
static void prvAppendScbRegisters(void);
static void prvAppendFaultDecode(void);
static void prvAppendTaskRecords(void);
static const AppCrashTaskRecord_t *prvFindTaskRecord(
	void *pvTaskHandle);
static uint32_t prvGetStackHighWaterWords(
	const AppCrashTaskRecord_t *pxTask, uint8_t *pucValid);
static uint8_t prvIsCpuRamRange(uint32_t ulStart,
	uint32_t ulLength);
static uint8_t prvIsTaskStackRangeValid(
	const AppCrashTaskRecord_t *pxTask);
static const char *prvReasonText(uint32_t ulReason);
static void prvTextReset(void);
static void prvAppendText(const char *pcText);
static void prvAppendChar(char cValue);
static void prvAppendHex32(uint32_t ulValue);
static void prvAppendUnsigned(uint32_t ulValue);
static void prvAppendLineEnd(void);

/*-----------------------------------------------------------*/
/** @brief 启用并复位致命诊断使用的 Cortex-M DWT 周期计数器。 */
void vAppCrashDiagInit(void)
{
#if (APP_CRASH_DIAG_ENABLE != 0U)
	prvEnableCycleCounter();
#endif
}

/*-----------------------------------------------------------*/
/**
  * @brief  在不调用 FreeRTOS 接口的情况下登记任务诊断元数据。
  * @param[in] pvTaskHandle FreeRTOS 任务句柄。
  * @param[in] pcTaskName 任务名称，函数内部进行有界复制。
  * @param[in] pvStackLow 任务栈包含式低地址。
  * @param[in] pvStackHigh 任务栈登记的最高 StackType_t 地址。
  * @param[in] ulPriority 创建任务时的优先级。
  */
void vAppCrashDiagTraceTaskCreate(void *pvTaskHandle,
	const char *pcTaskName, void *pvStackLow, void *pvStackHigh,
	uint32_t ulPriority)
{
#if (APP_CRASH_DIAG_ENABLE != 0U)
	AppCrashTaskRecord_t *pxRecord; /*!< 复用或新分配的静态任务记录。 */
	uint32_t ulIndex; /*!< 任务登记表遍历索引。 */
	uint32_t ulNameIndex; /*!< 有界复制任务名称的字符索引。 */

	if ((pvTaskHandle == NULL) || (pcTaskName == NULL) ||
		(pvStackLow == NULL) || (pvStackHigh == NULL)) {
		return;
	}

	/* 优先更新同句柄记录，否则选择最后观察到的未发布槽位。 */
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

	/* 先使记录无效，完整填写后通过内存屏障发布魔数。 */
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

/*-----------------------------------------------------------*/
/**
  * @brief  在 FreeRTOS 释放任务栈和控制块前使对应诊断记录失效。
  * @param[in] pvTaskHandle 正在删除的 FreeRTOS 任务句柄。
  */
void vAppCrashDiagTraceTaskDelete(void *pvTaskHandle)
{
#if (APP_CRASH_DIAG_ENABLE != 0U)
	uint32_t ulIndex; /*!< 任务登记表遍历索引。 */

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

/*-----------------------------------------------------------*/
/**
  * @brief  从汇编包装器保存的 Cortex-M 异常栈帧进入致命诊断。
  * @param[in] pulFaultStack 异常入口选择的 MSP 或 PSP 栈帧地址。
  * @param[in] ulExcReturn LR 中保存的异常返回值。
  * @param[in] ulReason AppCrashReason_e 原因码。
  * @warning 本函数关闭中断并永久重复输出，不会返回。
  */
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

/*-----------------------------------------------------------*/
/**
  * @brief  从 FreeRTOS 栈溢出或内存分配失败 Hook 进入致命诊断。
  * @param[in] pvTaskHandle 受影响任务句柄，未知时可为空。
  * @param[in] pcTaskName 受影响任务名称，当前实现不直接解引用。
  * @param[in] ulReason AppCrashReason_e 原因码。
  * @warning 本函数关闭中断并永久重复输出，不会返回。
  */
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

/*-----------------------------------------------------------*/
/**
  * @brief  使用 configASSERT 提供的源位置进入致命诊断。
  * @param[in] pcFile 触发断言的源文件文本，允许为空。
  * @param[in] ulLine 从 1 开始的源代码行号。
  * @param[in] ulReason APP_CRASH_REASON_ASSERT 原因码。
  * @warning 本函数关闭中断并永久重复输出，不会返回。
  */
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

/*-----------------------------------------------------------*/
/** @brief 按编译配置刷新已经由产品启动的独立看门狗。 */
void vAppCrashDiagWatchdogRefresh(void)
{
#if (APP_CRASH_IWDG_REFRESH_ENABLE != 0U)
	WRITE_REG(IWDG->KR, 0x0000AAAAUL);
#endif
}

/*-----------------------------------------------------------*/
/**
  * @brief  启用并复位 DWT 周期计数器作为致命路径时间基准。
  * @note 不依赖 RTOS 节拍。
  */
static void prvEnableCycleCounter(void)
{
	CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
	DWT->CYCCNT = 0U;
	DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
	__DSB();
	__ISB();
}

/*-----------------------------------------------------------*/
/**
  * @brief  永久重复输出冻结报告并按配置服务独立看门狗。
  * @warning 运行于致命路径，不得依赖调度器恢复。
  */
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

/*-----------------------------------------------------------*/
/** @brief 仅使用 DWT 周期计数器等待一个报告重复周期。 */
static void prvCrashDelayRepeatPeriod(void)
{
	uint32_t ulCycles; /*!< 两次看门狗刷新之间的 CPU 周期数。 */
	uint32_t ulIntervalCount; /*!< 一个报告周期包含的刷新间隔数。 */
	uint32_t ulInterval; /*!< 当前已等待的刷新间隔索引。 */
	uint32_t ulStart; /*!< 当前间隔的 DWT 周期起点。 */

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

/*-----------------------------------------------------------*/
/**
  * @brief  根据硬件异常栈帧构造异常、SCB 和任务诊断报告。
  * @param[in] pulFaultStack 异常入口保存的栈帧起点。
  * @param[in] ulExcReturn 用于判断基本或浮点扩展栈帧的异常返回值。
  * @param[in] ulReason AppCrashReason_e 原因码。
  */
static void prvBuildFaultText(uint32_t *pulFaultStack,
	uint32_t ulExcReturn, uint32_t ulReason)
{
	uint32_t *pulCoreFrame; /*!< 校验通过后的八字核心异常栈帧。 */
	uint32_t ulFrameAddress; /*!< 跳过可选浮点扩展区后的核心帧地址。 */
	uint32_t ulFrameOffset; /*!< 浮点扩展帧在核心帧前占用的字数。 */
	uint8_t ucFrameValid; /*!< 非零表示核心帧完整落在可访问 RAM。 */

	prvAppendCommonHeader(ulReason);
	prvAppendText("[CRASH][CORE] EXC_RETURN=");
	prvAppendHex32(ulExcReturn);
	prvAppendText(" FRAME_SP=");
	prvAppendHex32((uint32_t)pulFaultStack);
	prvAppendLineEnd();

	/* 根据 EXC_RETURN 判断是否需要跳过浮点扩展栈帧。 */
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

	/* 追加汇编保存寄存器、SCB 解码和独立任务栈快照。 */
	prvAppendSavedRegisters();
	prvAppendScbRegisters();
	prvAppendFaultDecode();
	prvAppendTaskRecords();
	prvAppendText("[CRASH][END] repeat_seconds=");
	prvAppendUnsigned(APP_CRASH_REPEAT_SECONDS);
	prvAppendLineEnd();
}

/*-----------------------------------------------------------*/
/**
  * @brief  在不查询内核的情况下构造 FreeRTOS Hook 致命报告。
  * @param[in] pvTaskHandle 触发故障的任务句柄。
  * @param[in] pcTaskName Hook 提供的任务名，当前实现不直接使用。
  * @param[in] ulReason AppCrashReason_e 原因码。
  */
static void prvBuildRtosText(void *pvTaskHandle,
	const char *pcTaskName, uint32_t ulReason)
{
	const AppCrashTaskRecord_t *pxTask; /*!< 与故障句柄匹配的独立任务记录。 */

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

/*-----------------------------------------------------------*/
/**
  * @brief  在不访问 FreeRTOS 内部数据的情况下构造断言报告。
  * @param[in] pcFile 触发断言的源文件文本，允许为空。
  * @param[in] ulLine 触发断言的源代码行号。
  * @param[in] ulReason AppCrashReason_e 原因码。
  */
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

/*-----------------------------------------------------------*/
/**
  * @brief  向报告追加稳定原因文本和当前任务句柄。
  * @param[in] ulReason AppCrashReason_e 原因码。
  */
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

/*-----------------------------------------------------------*/
/** @brief 向报告追加被调用方保存寄存器和进入 C 前的 MSP、PSP。 */
static void prvAppendSavedRegisters(void)
{
	uint32_t ulIndex; /*!< 每行四个寄存器的保存数组起始索引。 */

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

/*-----------------------------------------------------------*/
/** @brief 向报告追加离线故障分析所需的原始 SCB 寄存器。 */
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

/*-----------------------------------------------------------*/
/** @brief 将常用 Cortex-M 故障状态位解码为稳定文本。 */
static void prvAppendFaultDecode(void)
{
	uint32_t ulCfsr; /*!< 组合可配置故障状态寄存器快照。 */
	uint32_t ulHfsr; /*!< HardFault 状态寄存器快照。 */

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

/*-----------------------------------------------------------*/
/** @brief 统计并追加已登记任务元数据和安全校验后的栈高水位。 */
static void prvAppendTaskRecords(void)
{
	const AppCrashTaskRecord_t *pxTask; /*!< 当前输出的已发布任务记录。 */
	uint32_t ulTaskCount; /*!< 当前完整发布的任务记录数。 */
	uint32_t ulIndex; /*!< 任务登记表遍历索引。 */
	uint32_t ulHighWaterWords; /*!< 当前任务未使用栈空间，单位为字。 */
	uint8_t ucHighWaterValid; /*!< 非零表示任务栈范围通过安全校验。 */

	ulTaskCount = 0U;
	/* 步骤 1：只统计带完整发布魔数的任务记录。 */
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

	/* 步骤 2：逐条验证栈范围，再扫描填充值并追加任务详情。 */
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

/*-----------------------------------------------------------*/
/**
  * @brief  在不访问内核的情况下按句柄查找独立任务记录。
  * @param[in] pvTaskHandle 待查找的 FreeRTOS 任务句柄。
  * @retval NULL 没有完整发布的匹配记录。
  * @retval 非空 对应静态任务记录地址。
  */
static const AppCrashTaskRecord_t *prvFindTaskRecord(
	void *pvTaskHandle)
{
	uint32_t ulIndex; /*!< 任务登记表遍历索引。 */

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

/*-----------------------------------------------------------*/
/**
  * @brief  校验栈范围后统计仍保持 FreeRTOS 填充值的栈字数。
  * @param[in] pxTask 待扫描的任务记录。
  * @param[out] pucValid 栈范围校验结果。
  * @retval uint32_t 未被使用的栈空间，单位为 StackType_t 字。
  */
static uint32_t prvGetStackHighWaterWords(
	const AppCrashTaskRecord_t *pxTask, uint8_t *pucValid)
{
	const uint8_t *pucCurrent; /*!< 当前检查的栈填充字节。 */
	const uint8_t *pucEnd; /*!< 栈范围不包含在内的扫描结束地址。 */
	uint32_t ulFreeBytes; /*!< 从栈低端连续保持填充值的字节数。 */

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

/*-----------------------------------------------------------*/
/**
  * @brief  检查完整地址范围是否落在 CCM 或主 SRAM 内。
  * @param[in] ulStart 包含式起始地址。
  * @param[in] ulLength 范围字节数。
  * @retval 1 范围完整落在允许的 CPU RAM 中。
  * @retval 0 长度为零、地址溢出或越出允许 RAM。
  */
static uint8_t prvIsCpuRamRange(uint32_t ulStart,
	uint32_t ulLength)
{
	uint32_t ulEnd; /*!< 计算并检查溢出的包含式结束地址。 */

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

/*-----------------------------------------------------------*/
/**
  * @brief  在扫描任务栈前验证登记地址、对齐、长度和 RAM 范围。
  * @param[in] pxTask 待验证的任务记录。
  * @retval 1 任务栈范围可安全扫描。
  * @retval 0 地址顺序、对齐、长度或 RAM 范围不合法。
  */
static uint8_t prvIsTaskStackRangeValid(
	const AppCrashTaskRecord_t *pxTask)
{
	uint32_t ulLow; /*!< 任务栈包含式低地址。 */
	uint32_t ulHigh; /*!< 任务栈登记的最高字地址。 */
	uint32_t ulLength; /*!< 包含最高字在内的完整栈字节数。 */

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

/*-----------------------------------------------------------*/
/**
  * @brief  将崩溃原因码映射为无需分配内存的静态文本。
  * @param[in] ulReason AppCrashReason_e 原因码。
  * @retval const char* 稳定原因文本，未知值返回 UNKNOWN。
  */
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

/*-----------------------------------------------------------*/
/** @brief 清空有界致命报告缓冲并重新建立字符串结束符。 */
static void prvTextReset(void)
{
	s_usCrashTextLength = 0U;
	s_aucCrashText[0] = '\0';
}

/*-----------------------------------------------------------*/
/**
  * @brief  在容量范围内向致命报告追加字符串。
  * @param[in] pcText 待追加字符串，允许为空。
  */
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

/*-----------------------------------------------------------*/
/**
  * @brief  向致命报告追加一个字符并保留末尾字符串结束字节。
  * @param[in] cValue 待追加字符。
  */
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

/*-----------------------------------------------------------*/
/**
  * @brief  按 0x 前缀和八位大写十六进制追加 32 位数值。
  * @param[in] ulValue 地址或寄存器数值。
  */
static void prvAppendHex32(uint32_t ulValue)
{
	static const char acHex[] = "0123456789ABCDEF"; /*!< 十六进制字符表。 */
	int32_t lShift; /*!< 当前输出半字节相对最低位的右移位数。 */

	prvAppendText("0x");
	for (lShift = 28; lShift >= 0; lShift -= 4) {
		prvAppendChar(acHex[(ulValue >> lShift) & 0x0FU]);
	}
}

/*-----------------------------------------------------------*/
/**
  * @brief  不使用 printf 或 PSP 临时缓冲地追加无符号十进制数。
  * @param[in] ulValue 待格式化数值。
  */
static void prvAppendUnsigned(uint32_t ulValue)
{
	char acDigits[10]; /*!< 按低位到高位暂存的十进制数字。 */
	uint32_t ulCount; /*!< 已暂存且尚未反向输出的数字个数。 */

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

/*-----------------------------------------------------------*/
/** @brief 向报告追加串口终端通用的回车换行。 */
static void prvAppendLineEnd(void)
{
	prvAppendText("\r\n");
}
