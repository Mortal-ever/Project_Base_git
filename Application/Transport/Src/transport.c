/**
 * @file      transport.c
 * @brief     与具体 UART/TCP 后端无关的 Transport 公共分派与诊断层。
 * @author    WHong
 * @date      2026-07-28
 *
 * @details
 * Transport 公共层通过 TransportOps_t 函数指针表和 pvContext 把统一 API 分派到具体后端，
 * 上层因此不需要直接依赖 HAL UART、LwIP Netconn 或 Socket 实现。
 *
 * 生命周期关系：
 * - TransportChannel_t 与其后端 Context 均由调用者创建并持有；
 * - 全局注册表只保存 Channel 指针，不复制也不释放 Channel / Context；
 * - pxOps 通常指向后端长期存在的 static const 操作表，pvContext 指向对应实例 Context；
 * - Event Callback 及其 Context 也只保存地址，注册期间必须由调用者保证其持续有效。
 *
 * 本文件同时负责固定长度接收的总截止时间、统一状态/计数器以及后端原生故障快照。
 */

#include "transport.h"

#include <string.h>

#include "task.h"

/** @brief 按注册顺序保存调用者拥有的 Channel 指针。 */
static TransportChannel_t *s_apxChannels[TRANSPORT_MAX_CHANNELS];

/** @brief 当前注册表中的有效 Channel 数量。 */
static uint8_t s_ucChannelCount;

static int32_t prvGetNativeError(TransportChannel_t *pxChannel);

static TransportResult_e prvReceiveOnce(TransportChannel_t *pxChannel,
	uint8_t *pucData, uint16_t usMaxLen, uint16_t *pusReceivedLen,
	uint32_t ulTimeoutMs);

static TickType_t prvMsToTicks(uint32_t ulTimeoutMs);

static uint32_t prvTicksToMsCeil(TickType_t xTicks);

static void prvRecordOperation(TransportChannel_t *pxChannel,
	TransportOperation_e xOperation, TransportResult_e xResult,
	uint16_t usRequestedLength, uint16_t usTransferredLength);

/*-----------------------------------------------------------*/

/**
 * @brief  初始化 Transport 管理器并清空全局通道注册表。
 *
 * @details
 * 该函数只重置 s_apxChannels 与 s_ucChannelCount，不创建、不销毁任何后端 Context。
 * 注册表中保存的是调用者拥有的 TransportChannel_t 指针，因此管理器初始化只解除索引关系，
 * 不接管 Channel / Context 的生命周期。清空过程位于 FreeRTOS 临界区内，避免并发修改注册表。
 */

void vTransportManagerInit(void)
{
	uint8_t ucIndex;

	/* 仅清空注册关系；注册表中的 Channel / Context 内存均由外部持有。 */
	taskENTER_CRITICAL();
	for (ucIndex = 0U; ucIndex < TRANSPORT_MAX_CHANNELS; ucIndex++) {
		s_apxChannels[ucIndex] = NULL; 
	}
	s_ucChannelCount = 0U;
	taskEXIT_CRITICAL();
}

/*-----------------------------------------------------------*/

/**
 * @brief  将一个调用者拥有的 TransportChannel 注册到全局通道表。
 *
 * @details
 * 注册前检查 Channel 名称、操作表 pxOps 与后端 pvContext 是否有效，并拒绝重复名称。
 * 注册表只保存 pxChannel 地址，不复制 TransportChannel_t，也不拥有其内存；因此调用者必须
 * 保证 Channel 及其 pcName、pxOps、pvContext 在注册期间持续有效。注册成功后公共状态被
 * 初始化为 CLOSED，后续实际打开由 xTransportOpen() 分派到具体后端。
 *
 * @param[in,out] pxChannel 待注册的调用者拥有通道。
 *
 * @retval TRANSPORT_RESULT_OK 注册成功。
 * @retval TRANSPORT_RESULT_INVALID_ARG 必要对象或绑定信息无效。
 * @retval TRANSPORT_RESULT_BUSY 已存在同名通道。
 * @retval TRANSPORT_RESULT_NO_RESOURCE 全局注册表已满。
 */

TransportResult_e xTransportRegister(TransportChannel_t *pxChannel)
{
	uint8_t ucIndex;

	if ((pxChannel == NULL) || (pxChannel->pcName == NULL) ||
		(pxChannel->pxOps == NULL) || (pxChannel->pvContext == NULL)) {
		return TRANSPORT_RESULT_INVALID_ARG;
	}

	taskENTER_CRITICAL();
	for (ucIndex = 0U; ucIndex < s_ucChannelCount; ucIndex++) {
		if (strcmp(s_apxChannels[ucIndex]->pcName, pxChannel->pcName) == 0) {
			taskEXIT_CRITICAL();
			return TRANSPORT_RESULT_BUSY;
		}
	}

	if (s_ucChannelCount >= TRANSPORT_MAX_CHANNELS) {
		taskEXIT_CRITICAL();
		return TRANSPORT_RESULT_NO_RESOURCE;
	}

	/* 保存的是 pxChannel 地址而不是对象副本，因此其生命周期必须由调用者保证。 */
	s_apxChannels[s_ucChannelCount] = pxChannel;
	s_ucChannelCount++;
	pxChannel->xState = TRANSPORT_STATE_CLOSED;
	memset(&pxChannel->xStatus, 0, sizeof(pxChannel->xStatus));
	pxChannel->xStatus.xState = TRANSPORT_STATE_CLOSED;
	taskEXIT_CRITICAL();

	return TRANSPORT_RESULT_OK;
}

/*-----------------------------------------------------------*/

/**
 * @brief  按名称查找已经注册的 TransportChannel。
 *
 * @details
 * 遍历全局注册表并返回匹配名称对应的原始 Channel 指针，不创建副本，也不会增加任何
 * 引用计数。调用者取得的仍是注册时保存的同一对象，因此其有效性依赖原 Channel 生命周期。
 *
 * @param[in] pcName 目标通道名称。
 *
 * @retval 非 NULL 匹配到的 TransportChannel_t 指针。
 * @retval NULL 参数无效或未找到同名通道。
 */

TransportChannel_t *pxTransportFind(const char *pcName)
{
	uint8_t ucIndex;

	if (pcName == NULL) {
		return NULL;
	}

	for (ucIndex = 0U; ucIndex < s_ucChannelCount; ucIndex++) {
		if (strcmp(s_apxChannels[ucIndex]->pcName, pcName) == 0) {
			return s_apxChannels[ucIndex];
		}
	}

	return NULL;
}

/*-----------------------------------------------------------*/

/**
 * @brief  通过通道操作表打开具体 Transport 后端。
 *
 * @details
 * 公共层不直接知道 UART、TCP 等实现，而是通过 pxChannel->pxOps->xOpen() 调用后端，
 * 并把 pvContext 作为实例上下文传入。调用结束后同步公共状态并记录一次 OPEN 操作，
 * 包括规范化结果及后端原生错误，形成统一诊断入口。
 *
 * @param[in,out] pxChannel 目标 Transport 通道。
 *
 * @retval TransportResult_e 后端打开结果。
 */

TransportResult_e xTransportOpen(TransportChannel_t *pxChannel)
{
	TransportResult_e xResult;

	if ((pxChannel == NULL) || (pxChannel->pxOps == NULL) ||
		(pxChannel->pxOps->xOpen == NULL)) {
		return TRANSPORT_RESULT_INVALID_ARG;
	}

	/* 函数指针决定“执行什么”，pvContext 决定“操作哪个后端实例”。 */
	xResult = pxChannel->pxOps->xOpen(pxChannel->pvContext);
	pxChannel->xState = (xResult == TRANSPORT_RESULT_OK) ?
		TRANSPORT_STATE_OPEN : TRANSPORT_STATE_ERROR;
	prvRecordOperation(pxChannel, TRANSPORT_OPERATION_OPEN, xResult, 0U, 0U);
	return xResult;
}

/*-----------------------------------------------------------*/

/**
 * @brief  通过通道操作表关闭具体 Transport 后端。
 *
 * @details
 * 调用后端 xClose() 后，仅在关闭成功时把公共状态更新为 CLOSED；无论成功或失败都会
 * 记录本次 CLOSE 操作及原生故障。该函数只分派关闭动作，不释放 caller-owned Channel 本体。
 *
 * @param[in,out] pxChannel 目标 Transport 通道。
 *
 * @retval TransportResult_e 后端关闭结果。
 */

TransportResult_e xTransportClose(TransportChannel_t *pxChannel)
{
	TransportResult_e xResult;

	if ((pxChannel == NULL) || (pxChannel->pxOps == NULL) ||
		(pxChannel->pxOps->xClose == NULL)) {
		return TRANSPORT_RESULT_INVALID_ARG;
	}

	xResult = pxChannel->pxOps->xClose(pxChannel->pvContext);
	if (xResult == TRANSPORT_RESULT_OK) {
		pxChannel->xState = TRANSPORT_STATE_CLOSED;
	}
	prvRecordOperation(pxChannel, TRANSPORT_OPERATION_CLOSE, xResult, 0U,
		0U);
	return xResult;
}

/*-----------------------------------------------------------*/

/**
 * @brief  通过当前 Channel 后端发送一整段字节并记录统一诊断。
 *
 * @details
 * 调用 pxOps->xSend() 后要求“返回 OK 时实际发送长度必须等于请求长度”；若后端返回 OK
 * 但只发送了部分数据，则公共层把结果提升为 IO_ERROR，避免上层把短写误判为完整成功。
 * 最终由 prvRecordOperation() 统一更新计数、状态与最近故障。
 *
 * @param[in,out] pxChannel  目标 Transport 通道。
 * @param[in]     pucData    待发送数据。
 * @param[in]     usDataLen  期望发送字节数。
 * @param[in]     ulTimeoutMs 本次发送允许的超时时间，单位为毫秒。
 *
 * @retval TransportResult_e 规范化发送结果。
 */

TransportResult_e xTransportSend(TransportChannel_t *pxChannel,
								 const uint8_t *pucData,
								 uint16_t usDataLen,
								 uint32_t ulTimeoutMs)
{
	if ((pxChannel == NULL) || (pxChannel->pxOps == NULL) ||
		(pxChannel->pxOps->xSend == NULL) || (pucData == NULL) ||
		(usDataLen == 0U)) {
		return TRANSPORT_RESULT_INVALID_ARG;
	}

	{
		TransportResult_e xResult;
		uint16_t usSentLen;

		usSentLen = 0U;
		xResult = pxChannel->pxOps->xSend(pxChannel->pvContext, pucData,
			usDataLen, &usSentLen, ulTimeoutMs);
		if ((xResult == TRANSPORT_RESULT_OK) &&
			(usSentLen != usDataLen)) {
			xResult = TRANSPORT_RESULT_IO_ERROR;
		}
		prvRecordOperation(pxChannel, TRANSPORT_OPERATION_SEND, xResult,
			usDataLen, usSentLen);
		return xResult;
	}
}

/*-----------------------------------------------------------*/

/**
 * @brief  从后端执行一次接收，并记录本次实际接收长度。
 *
 * @details
 * 该接口只进行一次后端 xReceive() 调用，不保证填满 usMaxLen。pusReceivedLen 返回当前调用
 * 实际获得的字节数，适用于允许短读的场景。需要“凑齐固定长度”时应使用
 * xTransportReceiveExact() / xTransportReceiveExactCancelable()。
 *
 * @param[in,out] pxChannel       目标 Transport 通道。
 * @param[out]    pucData         接收缓冲区。
 * @param[in]     usMaxLen        本次最多接收字节数。
 * @param[out]    pusReceivedLen  实际接收字节数。
 * @param[in]     ulTimeoutMs     本次接收超时时间，单位为毫秒。
 *
 * @retval TransportResult_e 后端接收结果。
 */

TransportResult_e xTransportReceive(TransportChannel_t *pxChannel,
									uint8_t *pucData,
									uint16_t usMaxLen,
									uint16_t *pusReceivedLen,
									uint32_t ulTimeoutMs)
{
	if ((pxChannel == NULL) || (pxChannel->pxOps == NULL) ||
		(pxChannel->pxOps->xReceive == NULL) || (pucData == NULL) ||
		(pusReceivedLen == NULL) || (usMaxLen == 0U)) {
		return TRANSPORT_RESULT_INVALID_ARG;
	}

	{
		TransportResult_e xResult;

		*pusReceivedLen = 0U;
		xResult = prvReceiveOnce(pxChannel, pucData, usMaxLen,
			pusReceivedLen, ulTimeoutMs);
		prvRecordOperation(pxChannel, TRANSPORT_OPERATION_RECEIVE, xResult,
			usMaxLen, *pusReceivedLen);
		return xResult;
	}
}

/*-----------------------------------------------------------*/

/**
 * @brief  在一个总截止时间内累计接收指定字节数，并支持可选抢占检查。
 *
 * @details
 * 流式后端可能一次只返回部分字节，因此本函数循环调用底层 receive，并通过 usOffset
 * 累计进度。总超时从首次进入函数时建立，后续重试不会重新获得完整 timeout，避免碎片接收
 * 无限延长事务。后端中间返回 TIMEOUT 时，只要总预算尚未耗尽就继续尝试。
 *
 * 当 pxCheck 非 NULL 时，每轮接收前都会调用检查函数；返回非零立即以 CANCELED 结束。
 * 为提高前台抢占响应速度，可取消模式会把单次底层等待切成最多 20 ms 的小片，但总截止时间
 * 仍保持不变。ulTimeoutMs == 0 时只执行一次非阻塞式接收。
 *
 * @param[in,out] pxChannel       目标 Transport 通道。
 * @param[out]    pucData         接收缓冲区。
 * @param[in]     usExpectedLen   必须累计得到的目标字节数。
 * @param[out]    pusReceivedLen  实际累计接收字节数。
 * @param[in]     ulTimeoutMs     整个 ReceiveExact 的总超时时间。
 * @param[in]     pxCheck         可选抢占检查函数；NULL 表示不可取消。
 * @param[in]     pvCheckContext  原样传给 pxCheck 的调用者上下文。
 *
 * @retval TRANSPORT_RESULT_OK 已完整接收 usExpectedLen 字节。
 * @retval TRANSPORT_RESULT_TIMEOUT 总预算耗尽或零超时下发生短读。
 * @retval TRANSPORT_RESULT_CANCELED 抢占检查请求终止。
 * @retval 其他 TransportResult_e 后端或参数错误。
 */

TransportResult_e xTransportReceiveExactCancelable(
	TransportChannel_t *pxChannel, uint8_t *pucData, uint16_t usExpectedLen,
	uint16_t *pusReceivedLen, uint32_t ulTimeoutMs,
	TransportPreemptCheck_t pxCheck, void *pvCheckContext)
{
	TransportResult_e xResult;
	TickType_t xStart;
	TickType_t xBudget;
	TickType_t xElapsed;
	TickType_t xRemaining;
	uint32_t ulRemainingMs;
	uint16_t usOffset;
	uint16_t usReceived;

	if ((pxChannel == NULL) || (pxChannel->pxOps == NULL) ||
		(pxChannel->pxOps->xReceive == NULL) || (pucData == NULL) ||
		(pusReceivedLen == NULL) || (usExpectedLen == 0U)) {
		return TRANSPORT_RESULT_INVALID_ARG;
	}

	*pusReceivedLen = 0U;
	usOffset = 0U;
	xResult = TRANSPORT_RESULT_TIMEOUT;

	if (ulTimeoutMs == 0U) {
		usReceived = 0U;
		xResult = prvReceiveOnce(pxChannel, pucData, usExpectedLen,
			&usReceived, 0U);
		if (usReceived > usExpectedLen) {
			usReceived = 0U;
			xResult = TRANSPORT_RESULT_IO_ERROR;
		} else if (usReceived == usExpectedLen) {
			xResult = TRANSPORT_RESULT_OK;
		} else if (xResult == TRANSPORT_RESULT_OK) {
			xResult = TRANSPORT_RESULT_TIMEOUT;
		} else {
		}
		*pusReceivedLen = usReceived;
		prvRecordOperation(pxChannel, TRANSPORT_OPERATION_RECEIVE, xResult,
			usExpectedLen, usReceived);
		return xResult;
	}

	if (xTaskGetSchedulerState() != taskSCHEDULER_RUNNING) {
		return TRANSPORT_RESULT_NOT_READY;
	}

	/* 总截止时间只建立一次；后续短读重试只消耗剩余预算，不刷新 timeout。 */
	xStart = xTaskGetTickCount();
	xBudget = prvMsToTicks(ulTimeoutMs);

	while (usOffset < usExpectedLen) {
		/* 可选抢占检查使用调用者 Context，不要求 Transport 了解具体业务对象。 */
		if ((pxCheck != NULL) && (pxCheck(pvCheckContext) != 0U)) {
			xResult = TRANSPORT_RESULT_CANCELED;
			break;
		}
		xElapsed = xTaskGetTickCount() - xStart;
		if (xElapsed >= xBudget) {
			xResult = TRANSPORT_RESULT_TIMEOUT;
			break;
		}

		xRemaining = xBudget - xElapsed;
		ulRemainingMs = prvTicksToMsCeil(xRemaining);
		if ((pxCheck != NULL) && (ulRemainingMs > 20U)) {

			ulRemainingMs = 20U;
		}
		usReceived = 0U;
		xResult = prvReceiveOnce(pxChannel, &pucData[usOffset],
			(uint16_t)(usExpectedLen - usOffset), &usReceived,
			ulRemainingMs);

		if (usReceived > (uint16_t)(usExpectedLen - usOffset)) {
			xResult = TRANSPORT_RESULT_IO_ERROR;
			break;
		}

		usOffset = (uint16_t)(usOffset + usReceived);
		if (usOffset == usExpectedLen) {
			xResult = TRANSPORT_RESULT_OK;
			break;
		}

		if (xResult == TRANSPORT_RESULT_OK) {
			if (usReceived == 0U) {
				xResult = TRANSPORT_RESULT_IO_ERROR;
				break;
			}
			continue;
		}

		if (xResult == TRANSPORT_RESULT_TIMEOUT) {
			if (usReceived == 0U) {
				xElapsed = xTaskGetTickCount() - xStart;
				if (xElapsed < xBudget) {
					vTaskDelay(1U);
				}
			}
			continue;
		}

		break;
	}

	*pusReceivedLen = usOffset;
	prvRecordOperation(pxChannel, TRANSPORT_OPERATION_RECEIVE, xResult,
		usExpectedLen, usOffset);
	return xResult;
}

/*-----------------------------------------------------------*/

/**
 * @brief  在总截止时间内累计接收指定长度，不启用抢占检查。
 *
 * @details
 * 这是 xTransportReceiveExactCancelable() 的便捷封装，固定传入 NULL 检查函数和 Context。
 * 因此两者具有相同的短读累计、总超时和诊断记录语义，只是不允许中途被业务层取消。
 *
 * @param[in,out] pxChannel       目标 Transport 通道。
 * @param[out]    pucData         接收缓冲区。
 * @param[in]     usExpectedLen   目标字节数。
 * @param[out]    pusReceivedLen  实际累计接收字节数。
 * @param[in]     ulTimeoutMs     总超时时间，单位为毫秒。
 *
 * @retval TransportResult_e 接收结果。
 */

TransportResult_e xTransportReceiveExact(TransportChannel_t *pxChannel,
	uint8_t *pucData, uint16_t usExpectedLen, uint16_t *pusReceivedLen,
	uint32_t ulTimeoutMs)
{
	return xTransportReceiveExactCancelable(pxChannel, pucData, usExpectedLen,
		pusReceivedLen, ulTimeoutMs, NULL, NULL);
}

/*-----------------------------------------------------------*/

/**
 * @brief  向当前 Transport 后端分派一个控制命令。
 *
 * @details
 * 公共层仅负责参数检查、通过 pxOps->xControl() 传递命令及 pvArgument，并记录 CONTROL
 * 操作结果；具体参数类型与命令语义由后端解释，例如 UART 的 RX_FLUSH / GET_BAUD_RATE
 * 或 TCP 的 CONNECTION_RESET。
 *
 * @param[in,out] pxChannel   目标 Transport 通道。
 * @param[in]     xCommand    控制命令。
 * @param[in,out] pvArgument  命令相关参数，可按具体命令为 NULL。
 *
 * @retval TransportResult_e 控制结果。
 */

TransportResult_e xTransportControl(TransportChannel_t *pxChannel,
									TransportControl_e xCommand,
									void *pvArgument)
{
	if ((pxChannel == NULL) || (pxChannel->pxOps == NULL) ||
		(pxChannel->pxOps->xControl == NULL)) {
		return TRANSPORT_RESULT_INVALID_ARG;
	}

	{
		TransportResult_e xResult;

		xResult = pxChannel->pxOps->xControl(pxChannel->pvContext, xCommand,
			pvArgument);
		prvRecordOperation(pxChannel, TRANSPORT_OPERATION_CONTROL, xResult,
			0U, 0U);
		return xResult;
	}
}

/*-----------------------------------------------------------*/

/**
 * @brief  读取具体后端当前生命周期状态。
 *
 * @details
 * 状态查询直接通过 xGetState(pvContext) 从后端获取，而不是只返回公共层缓存，
 * 因而能够反映后端当前真实状态。缺少通道、操作表或状态函数时返回 UNINITIALIZED。
 *
 * @param[in] pxChannel 目标 Transport 通道。
 *
 * @retval TransportState_e 当前后端状态。
 */

TransportState_e xTransportGetState(TransportChannel_t *pxChannel)
{
	if ((pxChannel == NULL) || (pxChannel->pxOps == NULL) ||
		(pxChannel->pxOps->xGetState == NULL)) {
		return TRANSPORT_STATE_UNINITIALIZED;
	}

	return pxChannel->pxOps->xGetState(pxChannel->pvContext);
}

/*-----------------------------------------------------------*/

/**
 * @brief  获取 Transport 公共诊断状态快照。
 *
 * @details
 * 先在临界区内按值复制 pxChannel->xStatus，避免读取计数器过程中被并发更新；随后再通过
 * xTransportGetState() 刷新快照中的 xState。返回给调用者的是独立副本，后续内部状态变化
 * 不会修改已经取得的 pxStatus。
 *
 * @param[in]  pxChannel 目标 Transport 通道。
 * @param[out] pxStatus  调用者提供的状态输出对象。
 *
 * @retval TRANSPORT_RESULT_OK 获取成功。
 * @retval TRANSPORT_RESULT_INVALID_ARG 参数无效。
 */

TransportResult_e xTransportGetStatus(TransportChannel_t *pxChannel,
	TransportStatus_t *pxStatus)
{
	if ((pxChannel == NULL) || (pxStatus == NULL)) {
		return TRANSPORT_RESULT_INVALID_ARG;
	}

	taskENTER_CRITICAL();
	*pxStatus = pxChannel->xStatus;
	taskEXIT_CRITICAL();
	pxStatus->xState = xTransportGetState(pxChannel);
	return TRANSPORT_RESULT_OK;
}

/*-----------------------------------------------------------*/

/**
 * @brief  为一个 TransportChannel 绑定或解除 ISR 事件回调。
 *
 * @details
 * Channel 只保存函数指针和 pvCallbackContext 地址，不拥有 Context 对象。传入 NULL
 * pxCallback 可以解除回调。回调可能由中断路径触发，因此外部 Context 必须在整个注册期间
 * 保持有效，且替换/销毁时需要由上层保证与正在执行的 ISR 不发生生命周期冲突。
 *
 * @param[in,out] pxChannel          目标通道。
 * @param[in]     pxCallback         事件回调；NULL 表示关闭事件回调。
 * @param[in]     pvCallbackContext  调用者拥有的回调 Context。
 */

void vTransportSetEventCallback(TransportChannel_t *pxChannel,
								TransportEventCallback_t pxCallback,
								void *pvCallbackContext)
{
	if (pxChannel == NULL) {
		return;
	}

	taskENTER_CRITICAL();
	/* 仅保存 callback 与 Context 地址；两者的有效期由注册者负责。 */
	pxChannel->pvEventContext = pvCallbackContext;
	pxChannel->pxEventCallback = pxCallback;
	taskEXIT_CRITICAL();
}

/*-----------------------------------------------------------*/

/**
 * @brief  在 ISR 中更新公共事件统计，并把事件转发给已注册回调。
 *
 * @details
 * RX_DATA 会更新最近接收 Tick 与接收字节计数；ERROR / RX_OVERFLOW 会增加错误计数。
 * 公共状态更新使用 ISR 临界区保护。若注册了 pxEventCallback，则在中断上下文中直接调用，
 * 并把 Channel、事件数据、唤醒标志和保存的 pvEventContext 一并传给上层。
 *
 * @param[in,out] pxChannel                 产生事件的通道。
 * @param[in]     xEvent                    Transport 事件类型。
 * @param[in]     pucData                   可选事件数据。
 * @param[in]     usDataLen                 事件数据长度。
 * @param[in,out] pxHigherPriorityTaskWoken FreeRTOS ISR 唤醒标志。
 *
 * @warning 该函数运行于中断上下文，注册回调不得执行阻塞操作。
 */

void vTransportNotifyEventFromISR(TransportChannel_t *pxChannel,
								  TransportEvent_e xEvent,
								  const uint8_t *pucData,
								  uint16_t usDataLen,
								  BaseType_t *pxHigherPriorityTaskWoken)
{
	TransportEventCallback_t pxCallback;

	if (pxChannel == NULL) {
		return;
	}

	pxCallback = pxChannel->pxEventCallback;
	{
		UBaseType_t uxSavedInterruptStatus;

		uxSavedInterruptStatus = taskENTER_CRITICAL_FROM_ISR();
		if (xEvent == TRANSPORT_EVENT_RX_DATA) {
			pxChannel->xStatus.xLastRxTick = xTaskGetTickCountFromISR();
			pxChannel->xStatus.ulRxByteCount += usDataLen;
		} else if ((xEvent == TRANSPORT_EVENT_ERROR) ||
			(xEvent == TRANSPORT_EVENT_RX_OVERFLOW)) {
			pxChannel->xStatus.ulErrorCount++;
		}
		taskEXIT_CRITICAL_FROM_ISR(uxSavedInterruptStatus);
	}
	/* 回调在 ISR 上下文直接执行，pvEventContext 原样返回给注册者。 */
	if (pxCallback != NULL) {
		pxCallback(pxChannel, xEvent, pucData, usDataLen,
			pxHigherPriorityTaskWoken, pxChannel->pvEventContext);
	}
}

/*-----------------------------------------------------------*/

/**
 * @brief  从后端操作表读取最近一次原生错误值。
 *
 * @details
 * 原生错误不参与 TransportResult_e 的统一语义，只作为诊断补充保存。若后端没有提供
 * lGetNativeError()，返回 0。
 *
 * @param[in] pxChannel 目标 Transport 通道。
 *
 * @retval 后端原生错误值；无法读取时返回 0。
 */
static int32_t prvGetNativeError(TransportChannel_t *pxChannel)
{
	if ((pxChannel->pxOps == NULL) ||
		(pxChannel->pxOps->lGetNativeError == NULL)) {
		return 0;
	}
	return pxChannel->pxOps->lGetNativeError(pxChannel->pvContext);
}

/*-----------------------------------------------------------*/

/**
 * @brief  直接调用一次后端接收，不重复记录公共层操作统计。
 *
 * @details
 * ReceiveExact 内部需要多次短读才能组成一笔逻辑接收，因此循环中使用本函数避免每个碎片
 * 都被计作独立 RECEIVE 操作；最终由外层 ReceiveExact 统一调用 prvRecordOperation()。
 *
 * @param[in,out] pxChannel       目标通道。
 * @param[out]    pucData         接收缓冲区。
 * @param[in]     usMaxLen        单次最大接收长度。
 * @param[out]    pusReceivedLen  单次实际接收长度。
 * @param[in]     ulTimeoutMs     单次后端等待时间。
 *
 * @retval TransportResult_e 后端接收结果。
 */
static TransportResult_e prvReceiveOnce(TransportChannel_t *pxChannel,
	uint8_t *pucData, uint16_t usMaxLen, uint16_t *pusReceivedLen,
	uint32_t ulTimeoutMs)
{
	return pxChannel->pxOps->xReceive(pxChannel->pvContext, pucData,
		usMaxLen, pusReceivedLen, ulTimeoutMs);
}

/*-----------------------------------------------------------*/

/**
 * @brief  将正毫秒超时转换为至少一个 FreeRTOS Tick。
 *
 * @details
 * pdMS_TO_TICKS() 在低 Tick 频率下可能把很小的正超时转换为 0，因此这里强制最小返回 1，
 * 防止调用者明明请求了正等待时间却退化成非阻塞调用。
 *
 * @param[in] ulTimeoutMs 正毫秒超时时间。
 *
 * @retval 转换后的 Tick 数，最小为 1。
 */
static TickType_t prvMsToTicks(uint32_t ulTimeoutMs)
{
	TickType_t xTicks;

	xTicks = pdMS_TO_TICKS(ulTimeoutMs);
	return (xTicks == 0U) ? 1U : xTicks;
}

/*-----------------------------------------------------------*/

/**
 * @brief  将剩余 FreeRTOS Tick 向上取整为毫秒。
 *
 * @details
 * 采用向上取整避免把仍然存在的一个不足整毫秒 Tick 预算转换成 0 ms，并在计算结果超过
 * uint32_t 范围时饱和到 UINT32_MAX。
 *
 * @param[in] xTicks 待转换 Tick 数。
 *
 * @retval 向上取整后的毫秒值。
 */
static uint32_t prvTicksToMsCeil(TickType_t xTicks)
{
	uint64_t ullMilliseconds;
	uint32_t ulTickRate;

	ulTickRate = (uint32_t)configTICK_RATE_HZ;
	ullMilliseconds = ((uint64_t)xTicks * 1000ULL) +
		(uint64_t)(ulTickRate - 1U);
	ullMilliseconds /= (uint64_t)ulTickRate;
	if (ullMilliseconds > (uint64_t)UINT32_MAX) {
		return UINT32_MAX;
	}
	return (uint32_t)ullMilliseconds;
}

/*-----------------------------------------------------------*/

/**
 * @brief  统一更新一次 Transport 逻辑操作的状态、统计与故障快照。
 *
 * @details
 * 先读取当前 Tick、后端原生错误及后端状态，再在临界区内更新公共 xStatus。SEND / RECEIVE
 * 会记录请求长度与实际传输长度；成功操作累计次数和字节数，失败操作保存最近故障详情。
 * TIMEOUT 被视为正常可预期的通信结果，因此不会增加 ulErrorCount，其它失败会增加错误计数。
 *
 * @param[in,out] pxChannel            被操作的通道。
 * @param[in]     xOperation           OPEN / CLOSE / SEND / RECEIVE / CONTROL。
 * @param[in]     xResult              本次规范化结果。
 * @param[in]     usRequestedLength    请求长度或接收容量。
 * @param[in]     usTransferredLength  实际完成长度。
 */
static void prvRecordOperation(TransportChannel_t *pxChannel,
	TransportOperation_e xOperation, TransportResult_e xResult,
	uint16_t usRequestedLength, uint16_t usTransferredLength)
{
	TickType_t xNow;
	int32_t lNativeError;

	xNow = xTaskGetTickCount();
	lNativeError = prvGetNativeError(pxChannel);
	if ((pxChannel->pxOps != NULL) &&
		(pxChannel->pxOps->xGetState != NULL)) {
		pxChannel->xState = pxChannel->pxOps->xGetState(
			pxChannel->pvContext);
	}
	/* 从这里开始原子更新公共快照，避免统计字段被任务/ISR 读取到半更新状态。 */
	taskENTER_CRITICAL();
	pxChannel->xStatus.xState = pxChannel->xState;
	if (xOperation == TRANSPORT_OPERATION_SEND) {
		pxChannel->xStatus.usLastTxRequestedLength = usRequestedLength;
		pxChannel->xStatus.usLastTxTransferredLength =
			usTransferredLength;
	} else if (xOperation == TRANSPORT_OPERATION_RECEIVE) {
		pxChannel->xStatus.usLastRxCapacity = usRequestedLength;
		pxChannel->xStatus.usLastRxTransferredLength =
			usTransferredLength;
	} else {
	}
	if (xResult == TRANSPORT_RESULT_OK) {
		if (xOperation == TRANSPORT_OPERATION_OPEN) {
			pxChannel->xStatus.xLastOpenTick = xNow;
			pxChannel->xStatus.ulOpenCount++;
		} else if (xOperation == TRANSPORT_OPERATION_SEND) {
			pxChannel->xStatus.xLastTxTick = xNow;
			pxChannel->xStatus.ulTxOperationCount++;
			pxChannel->xStatus.ulTxByteCount += usTransferredLength;
		} else if (xOperation == TRANSPORT_OPERATION_RECEIVE) {
			pxChannel->xStatus.xLastRxTick = xNow;
			pxChannel->xStatus.ulRxOperationCount++;
			pxChannel->xStatus.ulRxByteCount += usTransferredLength;
		} else {
		}
	} else {
		pxChannel->xStatus.xLastFault.xOperation = xOperation;
		pxChannel->xStatus.xLastFault.xResult = xResult;
		pxChannel->xStatus.xLastFault.lNativeError = lNativeError;
		pxChannel->xStatus.xLastFault.xTimestamp = xNow;
		pxChannel->xStatus.xLastFault.usRequestedLength =
			usRequestedLength;
		pxChannel->xStatus.xLastFault.usTransferredLength =
			usTransferredLength;
		if (xResult != TRANSPORT_RESULT_TIMEOUT) {
			pxChannel->xStatus.ulErrorCount++;
		}
	}
	taskEXIT_CRITICAL();
}
