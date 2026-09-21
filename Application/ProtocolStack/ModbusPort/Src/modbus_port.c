/**
 * @file      modbus_port.c
 * @brief     nanoMODBUS 与项目 Transport 抽象之间的工程适配层。
 * @author    WHong
 * @date      2026-07-28
 *
 * @details
 * ModbusPort 负责把 nanoMODBUS 的协议读写回调绑定到项目 Transport，
 * 并在协议库之外统一管理事务总超时、RTU 帧间静默、错误映射、Trace 与故障诊断。
 *
 * 生命周期关系：
 * - ModbusPort_t 内嵌并拥有 nmbs_t 实例；
 * - TransportChannel_t 与可选 ModbusPortTrace_t 由调用者拥有，ModbusPort 仅保存其指针；
 * - 因此外部 Channel / Trace 的有效期必须覆盖 ModbusPort 对它们的实际使用期。
 */

#include "modbus_port.h"

#include <string.h>

#include "task.h"

static int32_t prvRead(uint8_t *pucData, uint16_t usCount,
	int32_t lTimeoutMs, void *pvArgument);

static int32_t prvWrite(const uint8_t *pucData, uint16_t usCount,
	int32_t lTimeoutMs, void *pvArgument);

static void prvFlush(nmbs_t *pxNmbs, void *pvArgument);

static ModbusPortResult_e prvInit(ModbusPort_t *pxPort,
	TransportChannel_t *pxChannel, ModbusPortTransport_e xTransport,
	ModbusPortRole_e xRole, uint32_t ulByteTimeoutMs,
	nmbs_platform_conf *pxPlatform);

static ModbusPortResult_e prvBegin(ModbusPort_t *pxPort,
	uint8_t ucUnitId, uint32_t ulTimeoutMs);

static ModbusPortResult_e prvFinish(ModbusPort_t *pxPort,
	nmbs_error xError);

static ModbusPortResult_e prvMapError(ModbusPort_t *pxPort,
	nmbs_error xError);

static uint32_t prvGetEffectiveTimeout(ModbusPort_t *pxPort,
	int32_t lRequestedMs);

static uint32_t prvGetRemainingMs(const ModbusPort_t *pxPort);

static TickType_t prvMsToTicks(uint32_t ulTimeoutMs);

static uint32_t prvTicksToMsCeil(TickType_t xTicks);

static void prvResetTrace(ModbusPort_t *pxPort);

static void prvAppendFrame(ModbusPortFrame_t *pxFrame,
	const uint8_t *pucData, uint16_t usLength);

static void prvUpdateFaultDetail(ModbusPort_t *pxPort,
	ModbusPortResult_e xResult, nmbs_error xError);

static void prvWaitFrameSilence(const ModbusPort_t *pxPort);
static uint8_t prvTransportPreemptCheck(void *pvContext);

/*-----------------------------------------------------------*/

/**
 * @brief  初始化 Modbus Client 端口。
 *
 * @details
 * 先由 prvInit() 建立 ModbusPort 与 Transport / nanoMODBUS Platform 的基础绑定，
 * 再调用 nmbs_client_create() 创建内嵌的 nanoMODBUS Client 实例。
 * xPlatform 是当前函数栈上的临时配置；nanoMODBUS create 接口会按值复制该配置，
 * 因此函数返回后 xPlatform 本身无需继续存在，但其中保存的 pxPort 上下文地址仍必须有效。
 *
 * @param[out] pxPort          待初始化的 ModbusPort 对象。
 * @param[in]  pxChannel       调用者拥有的 Transport 通道；必须覆盖端口使用期。
 * @param[in]  xTransport      Modbus 传输类型：RTU 或 TCP。
 * @param[in]  ulByteTimeoutMs 报文后续字节阶段的最大超时时间，单位为毫秒。
 *
 * @retval MODBUS_PORT_RESULT_OK 初始化成功。
 * @retval 其他 ModbusPortResult_e 基础绑定或 nanoMODBUS 创建失败。
 */
ModbusPortResult_e xModbusPortClientInit(ModbusPort_t *pxPort,
	TransportChannel_t *pxChannel, ModbusPortTransport_e xTransport,
	uint32_t ulByteTimeoutMs)
{
	ModbusPortResult_e xResult;
	nmbs_platform_conf xPlatform;
	nmbs_error xError;
	
	/* 建立 Port/Transport/Platform 基础绑定；此时尚未创建 nanoMODBUS Client。 */
	xResult = prvInit(pxPort, pxChannel, xTransport,
		MODBUS_PORT_ROLE_CLIENT, ulByteTimeoutMs, &xPlatform);
	if (xResult != MODBUS_PORT_RESULT_OK) {
		return xResult;
	}
	
	/* nanoMODBUS 按值复制 xPlatform，实际实例存放在 pxPort 内嵌的 xNmbs 中。 */
	xError = nmbs_client_create(&pxPort->xNmbs, &xPlatform);
	if (xError != NMBS_ERROR_NONE) {
		return prvFinish(pxPort, xError);
	}
	
	/* 只有 nanoMODBUS 创建成功后才对外标记端口可用。 */
	pxPort->ucInitialized = 1U;
	return MODBUS_PORT_RESULT_OK;
}

/*-----------------------------------------------------------*/
void vModbusPortSetPreemptCheck(ModbusPort_t *pxPort,
	TransportPreemptCheck_t pxCheck, void *pvContext)
{
	if (pxPort == NULL) {
		return;
	}
	pxPort->pxPreemptCheck = pxCheck;
	pxPort->pvPreemptContext = pvContext;
	pxPort->ucPreempted = 0U;
}

#if (NANOMODBUS_CFG_SERVER_ENABLED != 0)
/*-----------------------------------------------------------*/

/**
 * @brief  初始化 Modbus Server 端口并注册数据模型回调。
 *
 * @details
 * 先建立 ModbusPort 与 Transport 的平台绑定，再通过 nmbs_server_create() 创建
 * 内嵌 Server 实例并复制 pxCallbacks 回调表。pxCallbacks 结构体本身在 create 返回后
 * 可以结束生命周期，但回调表内部保存的 arg/context 仍只是指针，其目标对象必须在
 * Server 可能继续 Poll 并触发 callback 的整个期间保持有效。
 *
 * @param[out] pxPort          待初始化的 ModbusPort 对象。
 * @param[in]  pxChannel       调用者拥有的 Transport 通道；必须覆盖端口使用期。
 * @param[in]  xTransport      Modbus 传输类型：RTU 或 TCP。
 * @param[in]  ucRtuAddress    RTU Server 地址；TCP 模式下由 nanoMODBUS 按其规则处理。
 * @param[in]  pxCallbacks     Server 数据模型回调配置。
 * @param[in]  ulByteTimeoutMs 报文后续字节阶段的最大超时时间，单位为毫秒。
 *
 * @retval MODBUS_PORT_RESULT_OK 初始化成功。
 * @retval MODBUS_PORT_RESULT_INVALID_ARG 回调表为空或基础参数无效。
 * @retval 其他 ModbusPortResult_e nanoMODBUS 创建失败。
 */
ModbusPortResult_e xModbusPortServerInit(ModbusPort_t *pxPort,
	TransportChannel_t *pxChannel, ModbusPortTransport_e xTransport,
	uint8_t ucRtuAddress, const nmbs_callbacks *pxCallbacks,
	uint32_t ulByteTimeoutMs)
{
	ModbusPortResult_e xResult;
	nmbs_platform_conf xPlatform;
	nmbs_error xError;

	if (pxCallbacks == NULL) {
		return MODBUS_PORT_RESULT_INVALID_ARG;
	}

	/* 第一步：建立 Port/Transport/Platform 基础绑定。 */
	xResult = prvInit(pxPort, pxChannel, xTransport,
		MODBUS_PORT_ROLE_SERVER, ulByteTimeoutMs, &xPlatform);
	if (xResult != MODBUS_PORT_RESULT_OK) {
		return xResult;
	}
	/* 第二步：创建内嵌 Server，并把 Platform 与 callback 配置复制进 xNmbs。 */
	xError = nmbs_server_create(&pxPort->xNmbs, ucRtuAddress,
		&xPlatform, pxCallbacks);
	if (xError != NMBS_ERROR_NONE) {
		return prvFinish(pxPort, xError);
	}
	/* 只有 nanoMODBUS 创建成功后才对外标记端口可用。 */
	pxPort->ucInitialized = 1U;
	return MODBUS_PORT_RESULT_OK;
}

/*-----------------------------------------------------------*/

/**
 * @brief  执行一次 Modbus Server 轮询，最多处理一笔请求。
 *
 * @details
 * 每次调用都会建立独立的 Poll 总时间预算、清空本轮 Trace，并把首字节等待时间和
 * 字节间超时写入 nanoMODBUS。nmbs_server_poll() 本身不是永久循环；持续服务请求应由
 * 上层 Task/循环反复调用本函数。没有收到请求时，nanoMODBUS 可把“首字节等待超时”
 * 视为正常空闲返回；已开始接收后再超时则会作为事务错误处理。
 *
 * @param[in,out] pxPort          已初始化且角色为 Server 的 ModbusPort。
 * @param[in]     ulPollTimeoutMs 本轮等待/处理请求的总时间预算，单位为毫秒。
 *
 * @retval MODBUS_PORT_RESULT_OK 本轮正常结束，可能处理了一笔请求，也可能只是空闲返回。
 * @retval MODBUS_PORT_RESULT_INVALID_ARG 端口、角色或超时参数无效。
 * @retval 其他 ModbusPortResult_e 本轮发生协议或 Transport 错误。
 */
ModbusPortResult_e xModbusPortServerPoll(ModbusPort_t *pxPort,
	uint32_t ulPollTimeoutMs)
{
	nmbs_error xError;

	if ((pxPort == NULL) || (pxPort->ucInitialized == 0U) ||
		(pxPort->xRole != MODBUS_PORT_ROLE_SERVER) ||
		(ulPollTimeoutMs == 0U) ||
		(ulPollTimeoutMs > MODBUS_PORT_TIMEOUT_MAX_MS)) {
		return MODBUS_PORT_RESULT_INVALID_ARG;
	}
	/* 每一次 Poll 都拥有独立的总时间预算，不会无限阻塞在本次调用内部。 */
	pxPort->xOperationStart = xTaskGetTickCount();
	pxPort->xOperationBudget = prvMsToTicks(ulPollTimeoutMs);
	pxPort->ucOperationActive = 1U;
	pxPort->xLastTransportResult = TRANSPORT_RESULT_OK;
	prvResetTrace(pxPort);
	nmbs_set_read_timeout(&pxPort->xNmbs, (int32_t)ulPollTimeoutMs);
	nmbs_set_byte_timeout(&pxPort->xNmbs,
		(int32_t)pxPort->ulByteTimeoutMs);
	xError = nmbs_server_poll(&pxPort->xNmbs);
	return prvFinish(pxPort, xError);
}
#endif

/*-----------------------------------------------------------*/

/**
 * @brief  绑定或解除绑定外部 Modbus Trace 对象。
 *
 * @details
 * ModbusPort 只保存 pxTrace 指针，不拥有 Trace 对象。传入非 NULL 时会先清空 Trace；
 * 传入 NULL 可解除绑定。外部 Trace 在绑定期间必须保持有效，且调用者应避免与正在进行
 * 的事务并发销毁或替换该对象。
 *
 * @param[in,out] pxPort  目标 ModbusPort。
 * @param[in,out] pxTrace 调用者拥有的 Trace 对象；NULL 表示关闭 Trace。
 */
void vModbusPortSetTrace(ModbusPort_t *pxPort,
	ModbusPortTrace_t *pxTrace)
{
	if (pxPort == NULL) {
		return;
	}
	/* 仅保存调用者对象的地址；传入 NULL 即解除绑定。 */
	pxPort->pxTrace = pxTrace;
	if (pxTrace != NULL) {
		memset(pxTrace, 0, sizeof(*pxTrace));
	}
}

/*-----------------------------------------------------------*/

/**
 * @brief  复制最近一次事务的详细故障信息。
 *
 * @details
 * 将端口内部保存的 xLastFault 按值复制到调用者缓冲区。返回的是快照，后续事务更新
 * xLastFault 不会影响已经复制出去的 pxFault。
 *
 * @param[in]  pxPort  目标 ModbusPort。
 * @param[out] pxFault 调用者提供的故障信息输出对象。
 */
void vModbusPortGetLastFault(const ModbusPort_t *pxPort,
	ModbusPortFault_t *pxFault)
{
	if ((pxPort == NULL) || (pxFault == NULL)) {
		return;
	}
	/* 按值复制 Fault 快照，不向外暴露内部对象地址。 */
	*pxFault = pxPort->xLastFault;
}

/*-----------------------------------------------------------*/

/**
 * @brief  判断工程结果是否按当前策略视为链路故障。
 *
 * @details
 * 当前策略把 TIMEOUT、TRANSPORT 和 PROTOCOL 归类为需要关注链路/通信状态的结果，
 * 其它结果返回 0。该函数表达的是项目恢复策略，而不是 Modbus 协议本身的硬性定义。
 *
 * @param[in] xResult 待判断的 ModbusPortResult_e。
 *
 * @retval 1 当前结果被视为链路故障。
 * @retval 0 当前结果不被视为链路故障。
 */
uint8_t ucModbusPortResultIsLinkFailure(ModbusPortResult_e xResult)
{
	switch (xResult) {
	case MODBUS_PORT_RESULT_TIMEOUT:
	case MODBUS_PORT_RESULT_TRANSPORT:
	case MODBUS_PORT_RESULT_PROTOCOL:
		return 1U;

	default:
		return 0U;
	}
}

/*-----------------------------------------------------------*/

/**
 * @brief  使用 FC01 读取一组 Coil，并转换为 bool 数组。
 *
 * @details
 * nanoMODBUS 原生以紧凑 bitfield 返回 Coil；本接口使用 ModbusPort 内嵌的 aucBitfield
 * 作为临时缓冲，事务成功后再逐位展开到调用者的 pbValues。事务总超时由 prvBegin()
 * 建立，最终结果统一由 prvFinish() 映射并更新诊断状态。
 *
 * @param[in,out] pxPort       已初始化的 Client 端口。
 * @param[in]     ucUnitId     目标 Unit ID。
 * @param[in]     usAddress    起始 Coil 地址。
 * @param[in]     usQuantity   读取数量。
 * @param[out]    pbValues     至少包含 usQuantity 个元素的输出数组。
 * @param[in]     ulTimeoutMs  本次事务总超时时间，单位为毫秒。
 *
 * @retval MODBUS_PORT_RESULT_OK 读取并展开成功。
 * @retval MODBUS_PORT_RESULT_INVALID_ARG 输出指针或数量无效。
 * @retval 其他 ModbusPortResult_e 事务失败。
 */
ModbusPortResult_e xModbusPortReadCoils(ModbusPort_t *pxPort,
	uint8_t ucUnitId, uint16_t usAddress, uint16_t usQuantity,
	bool *pbValues, uint32_t ulTimeoutMs)
{
	ModbusPortResult_e xResult;
	nmbs_error xError;
	uint16_t usIndex;

	if ((pbValues == NULL) || (usQuantity == 0U) ||
		(usQuantity > NMBS_BITFIELD_MAX)) {
		return MODBUS_PORT_RESULT_INVALID_ARG;
	}
	xResult = prvBegin(pxPort, ucUnitId, ulTimeoutMs);
	if (xResult != MODBUS_PORT_RESULT_OK) {
		return xResult;
	}
	/* nanoMODBUS 使用紧凑 bitfield，先清空内嵌临时缓冲再接收。 */
	nmbs_bitfield_reset(pxPort->aucBitfield);
	xError = nmbs_read_coils(&pxPort->xNmbs, usAddress, usQuantity,
		pxPort->aucBitfield);
	xResult = prvFinish(pxPort, xError);
	if (xResult == MODBUS_PORT_RESULT_OK) {
		for (usIndex = 0U; usIndex < usQuantity; usIndex++) {
			pbValues[usIndex] =
				nmbs_bitfield_read(pxPort->aucBitfield, usIndex);
		}
	}
	return xResult;
}

/*-----------------------------------------------------------*/

/**
 * @brief  使用 FC02 读取一组 Discrete Input，并转换为 bool 数组。
 *
 * @details
 * 使用内嵌 aucBitfield 接收 nanoMODBUS 的紧凑位数据，只有事务成功后才把位值逐项写入
 * pbValues，从而避免在失败路径上输出不完整的逻辑结果。
 *
 * @param[in,out] pxPort      已初始化的 Client 端口。
 * @param[in]     ucUnitId    目标 Unit ID。
 * @param[in]     usAddress   起始 Discrete Input 地址。
 * @param[in]     usQuantity  读取数量。
 * @param[out]    pbValues    至少包含 usQuantity 个元素的输出数组。
 * @param[in]     ulTimeoutMs 本次事务总超时时间，单位为毫秒。
 *
 * @retval MODBUS_PORT_RESULT_OK 读取成功。
 * @retval MODBUS_PORT_RESULT_INVALID_ARG 输出指针或数量无效。
 * @retval 其他 ModbusPortResult_e 事务失败。
 */
ModbusPortResult_e xModbusPortReadDiscreteInputs(ModbusPort_t *pxPort,
	uint8_t ucUnitId, uint16_t usAddress, uint16_t usQuantity,
	bool *pbValues, uint32_t ulTimeoutMs)
{
	ModbusPortResult_e xResult;
	nmbs_error xError;
	uint16_t usIndex;

	if ((pbValues == NULL) || (usQuantity == 0U) ||
		(usQuantity > NMBS_BITFIELD_MAX)) {
		return MODBUS_PORT_RESULT_INVALID_ARG;
	}
	xResult = prvBegin(pxPort, ucUnitId, ulTimeoutMs);
	if (xResult != MODBUS_PORT_RESULT_OK) {
		return xResult;
	}
	/* nanoMODBUS 使用紧凑 bitfield，先清空内嵌临时缓冲再接收。 */
	nmbs_bitfield_reset(pxPort->aucBitfield);
	xError = nmbs_read_discrete_inputs(&pxPort->xNmbs, usAddress,
		usQuantity, pxPort->aucBitfield);
	xResult = prvFinish(pxPort, xError);
	if (xResult == MODBUS_PORT_RESULT_OK) {
		for (usIndex = 0U; usIndex < usQuantity; usIndex++) {
			pbValues[usIndex] =
				nmbs_bitfield_read(pxPort->aucBitfield, usIndex);
		}
	}
	return xResult;
}

/*-----------------------------------------------------------*/

/**
 * @brief  使用 FC03 读取 Holding Registers。
 *
 * @details
 * 本接口只负责建立统一事务上下文、调用 nanoMODBUS FC03 API，并把协议/Transport 结果
 * 映射回工程错误域。寄存器输出缓冲区由调用者提供并拥有。
 *
 * @param[in,out] pxPort      已初始化的 Client 端口。
 * @param[in]     ucUnitId    目标 Unit ID。
 * @param[in]     usAddress   起始 Holding Register 地址。
 * @param[in]     usQuantity  读取寄存器数量。
 * @param[out]    pusValues   寄存器输出数组。
 * @param[in]     ulTimeoutMs 本次事务总超时时间，单位为毫秒。
 *
 * @retval MODBUS_PORT_RESULT_OK 读取成功。
 * @retval MODBUS_PORT_RESULT_INVALID_ARG 输出指针或事务参数无效。
 * @retval 其他 ModbusPortResult_e 事务失败。
 */
ModbusPortResult_e xModbusPortReadHolding(ModbusPort_t *pxPort,
	uint8_t ucUnitId, uint16_t usAddress, uint16_t usQuantity,
	uint16_t *pusValues, uint32_t ulTimeoutMs)
{
	ModbusPortResult_e xResult;
	nmbs_error xError;

	if (pusValues == NULL) {
		return MODBUS_PORT_RESULT_INVALID_ARG;
	}
	xResult = prvBegin(pxPort, ucUnitId, ulTimeoutMs);
	if (xResult != MODBUS_PORT_RESULT_OK) {
		return xResult;
	}
	xError = nmbs_read_holding_registers(&pxPort->xNmbs, usAddress,
		usQuantity, pusValues);
	return prvFinish(pxPort, xError);
}

/*-----------------------------------------------------------*/

/**
 * @brief  使用 FC04 读取 Input Registers。
 *
 * @details
 * 通过统一的 prvBegin()/prvFinish() 事务模板包装 nanoMODBUS FC04 调用，确保总截止时间、
 * Transport 错误、Trace 与 Fault 诊断保持一致。
 *
 * @param[in,out] pxPort      已初始化的 Client 端口。
 * @param[in]     ucUnitId    目标 Unit ID。
 * @param[in]     usAddress   起始 Input Register 地址。
 * @param[in]     usQuantity  读取寄存器数量。
 * @param[out]    pusValues   寄存器输出数组。
 * @param[in]     ulTimeoutMs 本次事务总超时时间，单位为毫秒。
 *
 * @retval MODBUS_PORT_RESULT_OK 读取成功。
 * @retval MODBUS_PORT_RESULT_INVALID_ARG 输出指针或事务参数无效。
 * @retval 其他 ModbusPortResult_e 事务失败。
 */
ModbusPortResult_e xModbusPortReadInputRegisters(ModbusPort_t *pxPort,
	uint8_t ucUnitId, uint16_t usAddress, uint16_t usQuantity,
	uint16_t *pusValues, uint32_t ulTimeoutMs)
{
	ModbusPortResult_e xResult;
	nmbs_error xError;

	if (pusValues == NULL) {
		return MODBUS_PORT_RESULT_INVALID_ARG;
	}
	xResult = prvBegin(pxPort, ucUnitId, ulTimeoutMs);
	if (xResult != MODBUS_PORT_RESULT_OK) {
		return xResult;
	}
	xError = nmbs_read_input_registers(&pxPort->xNmbs, usAddress,
		usQuantity, pusValues);
	return prvFinish(pxPort, xError);
}

/*-----------------------------------------------------------*/

/**
 * @brief  使用 FC05 写单个 Coil。
 *
 * @details
 * 建立 Client 事务后调用 nanoMODBUS 单线圈写接口，最终统一完成错误映射和诊断更新。
 *
 * @param[in,out] pxPort      已初始化的 Client 端口。
 * @param[in]     ucUnitId    目标 Unit ID。
 * @param[in]     usAddress   Coil 地址。
 * @param[in]     bValue      待写入的逻辑值。
 * @param[in]     ulTimeoutMs 本次事务总超时时间，单位为毫秒。
 *
 * @return ModbusPortResult_e 工程级事务结果。
 */
ModbusPortResult_e xModbusPortWriteCoil(ModbusPort_t *pxPort,
	uint8_t ucUnitId, uint16_t usAddress, bool bValue,
	uint32_t ulTimeoutMs)
{
	ModbusPortResult_e xResult;
	nmbs_error xError;

	xResult = prvBegin(pxPort, ucUnitId, ulTimeoutMs);
	if (xResult != MODBUS_PORT_RESULT_OK) {
		return xResult;
	}
	xError = nmbs_write_single_coil(&pxPort->xNmbs, usAddress, bValue);
	return prvFinish(pxPort, xError);
}

/*-----------------------------------------------------------*/

/**
 * @brief  使用 FC06 写单个 Holding Register。
 *
 * @param[in,out] pxPort      已初始化的 Client 端口。
 * @param[in]     ucUnitId    目标 Unit ID。
 * @param[in]     usAddress   寄存器地址。
 * @param[in]     usValue     待写入的 16 位值。
 * @param[in]     ulTimeoutMs 本次事务总超时时间，单位为毫秒。
 *
 * @return ModbusPortResult_e 工程级事务结果。
 */
ModbusPortResult_e xModbusPortWriteRegister(ModbusPort_t *pxPort,
	uint8_t ucUnitId, uint16_t usAddress, uint16_t usValue,
	uint32_t ulTimeoutMs)
{
	ModbusPortResult_e xResult;
	nmbs_error xError;

	xResult = prvBegin(pxPort, ucUnitId, ulTimeoutMs);
	if (xResult != MODBUS_PORT_RESULT_OK) {
		return xResult;
	}
	xError = nmbs_write_single_register(&pxPort->xNmbs, usAddress,
		usValue);
	return prvFinish(pxPort, xError);
}

/*-----------------------------------------------------------*/

/**
 * @brief  使用 FC15 写多个 Coil。
 *
 * @details
 * 调用者提供 bool 数组；本接口先把每个布尔值压缩到 ModbusPort 内嵌 bitfield，
 * 再交给 nanoMODBUS 构造 FC15 请求。内嵌缓冲避免把临时协议打包内存交给外部管理。
 *
 * @param[in,out] pxPort      已初始化的 Client 端口。
 * @param[in]     ucUnitId    目标 Unit ID。
 * @param[in]     usAddress   起始 Coil 地址。
 * @param[in]     usQuantity  写入数量。
 * @param[in]     pbValues    至少包含 usQuantity 个元素的输入数组。
 * @param[in]     ulTimeoutMs 本次事务总超时时间，单位为毫秒。
 *
 * @retval MODBUS_PORT_RESULT_OK 写入成功。
 * @retval MODBUS_PORT_RESULT_INVALID_ARG 输入指针或数量无效。
 * @retval 其他 ModbusPortResult_e 事务失败。
 */
ModbusPortResult_e xModbusPortWriteCoils(ModbusPort_t *pxPort,
	uint8_t ucUnitId, uint16_t usAddress, uint16_t usQuantity,
	const bool *pbValues, uint32_t ulTimeoutMs)
{
	ModbusPortResult_e xResult;
	nmbs_error xError;
	uint16_t usIndex;

	if ((pbValues == NULL) || (usQuantity == 0U) ||
		(usQuantity > NMBS_BITFIELD_MAX)) {
		return MODBUS_PORT_RESULT_INVALID_ARG;
	}
	xResult = prvBegin(pxPort, ucUnitId, ulTimeoutMs);
	if (xResult != MODBUS_PORT_RESULT_OK) {
		return xResult;
	}
	/* 将调用者的 bool 数组压缩为 Modbus 线圈 bitfield。 */
	nmbs_bitfield_reset(pxPort->aucBitfield);
	for (usIndex = 0U; usIndex < usQuantity; usIndex++) {
		nmbs_bitfield_write(pxPort->aucBitfield, usIndex,
			pbValues[usIndex] ? 1U : 0U);
	}
	xError = nmbs_write_multiple_coils(&pxPort->xNmbs, usAddress,
		usQuantity, pxPort->aucBitfield);
	return prvFinish(pxPort, xError);
}

/*-----------------------------------------------------------*/

/**
 * @brief  使用 FC16 写多个 Holding Registers。
 *
 * @param[in,out] pxPort      已初始化的 Client 端口。
 * @param[in]     ucUnitId    目标 Unit ID。
 * @param[in]     usAddress   起始寄存器地址。
 * @param[in]     usQuantity  写入寄存器数量。
 * @param[in]     pusValues   待写入的寄存器数组。
 * @param[in]     ulTimeoutMs 本次事务总超时时间，单位为毫秒。
 *
 * @return ModbusPortResult_e 工程级事务结果。
 */
ModbusPortResult_e xModbusPortWriteRegisters(ModbusPort_t *pxPort,
	uint8_t ucUnitId, uint16_t usAddress, uint16_t usQuantity,
	const uint16_t *pusValues, uint32_t ulTimeoutMs)
{
	ModbusPortResult_e xResult;
	nmbs_error xError;

	if (pusValues == NULL) {
		return MODBUS_PORT_RESULT_INVALID_ARG;
	}
	xResult = prvBegin(pxPort, ucUnitId, ulTimeoutMs);
	if (xResult != MODBUS_PORT_RESULT_OK) {
		return xResult;
	}
	xError = nmbs_write_multiple_registers(&pxPort->xNmbs, usAddress,
		usQuantity, pusValues);
	return prvFinish(pxPort, xError);
}

/*-----------------------------------------------------------*/

/**
 * @brief  使用 FC20 读取 File Record。
 *
 * @param[in,out] pxPort         已初始化的 Client 端口。
 * @param[in]     ucUnitId       目标 Unit ID。
 * @param[in]     usFileNumber   文件号。
 * @param[in]     usRecordNumber 起始记录号。
 * @param[out]    pusValues      记录数据输出数组。
 * @param[in]     usCount        读取寄存器数量。
 * @param[in]     ulTimeoutMs    本次事务总超时时间，单位为毫秒。
 *
 * @return ModbusPortResult_e 工程级事务结果。
 */
ModbusPortResult_e xModbusPortReadFileRecord(ModbusPort_t *pxPort,
	uint8_t ucUnitId, uint16_t usFileNumber, uint16_t usRecordNumber,
	uint16_t *pusValues, uint16_t usCount, uint32_t ulTimeoutMs)
{
	ModbusPortResult_e xResult;
	nmbs_error xError;

	if (pusValues == NULL) {
		return MODBUS_PORT_RESULT_INVALID_ARG;
	}
	xResult = prvBegin(pxPort, ucUnitId, ulTimeoutMs);
	if (xResult != MODBUS_PORT_RESULT_OK) {
		return xResult;
	}
	xError = nmbs_read_file_record(&pxPort->xNmbs, usFileNumber,
		usRecordNumber, pusValues, usCount);
	return prvFinish(pxPort, xError);
}

/*-----------------------------------------------------------*/

/**
 * @brief  使用 FC21 写 File Record。
 *
 * @param[in,out] pxPort         已初始化的 Client 端口。
 * @param[in]     ucUnitId       目标 Unit ID。
 * @param[in]     usFileNumber   文件号。
 * @param[in]     usRecordNumber 起始记录号。
 * @param[in]     pusValues      待写入的记录数据。
 * @param[in]     usCount        写入寄存器数量。
 * @param[in]     ulTimeoutMs    本次事务总超时时间，单位为毫秒。
 *
 * @return ModbusPortResult_e 工程级事务结果。
 */
ModbusPortResult_e xModbusPortWriteFileRecord(ModbusPort_t *pxPort,
	uint8_t ucUnitId, uint16_t usFileNumber, uint16_t usRecordNumber,
	const uint16_t *pusValues, uint16_t usCount, uint32_t ulTimeoutMs)
{
	ModbusPortResult_e xResult;
	nmbs_error xError;

	if (pusValues == NULL) {
		return MODBUS_PORT_RESULT_INVALID_ARG;
	}
	xResult = prvBegin(pxPort, ucUnitId, ulTimeoutMs);
	if (xResult != MODBUS_PORT_RESULT_OK) {
		return xResult;
	}
	xError = nmbs_write_file_record(&pxPort->xNmbs, usFileNumber,
		usRecordNumber, pusValues, usCount);
	return prvFinish(pxPort, xError);
}

/*-----------------------------------------------------------*/

/**
 * @brief  使用 FC23 在一笔事务中先写多个寄存器，再读取多个寄存器。
 *
 * @details
 * FC23 将写入与读取组合成一个 Modbus 请求。pusWriteValues 提供写入数据，pusReadValues
 * 接收 Server 返回的读取结果；两块缓冲区均由调用者拥有并保证在本函数调用期间有效。
 *
 * @param[in,out] pxPort          已初始化的 Client 端口。
 * @param[in]     ucUnitId        目标 Unit ID。
 * @param[in]     usReadAddress   读取起始地址。
 * @param[in]     usReadQuantity  读取寄存器数量。
 * @param[out]    pusReadValues   读取结果数组。
 * @param[in]     usWriteAddress  写入起始地址。
 * @param[in]     usWriteQuantity 写入寄存器数量。
 * @param[in]     pusWriteValues  待写入数据数组。
 * @param[in]     ulTimeoutMs     本次事务总超时时间，单位为毫秒。
 *
 * @return ModbusPortResult_e 工程级事务结果。
 */
ModbusPortResult_e xModbusPortReadWriteRegisters(ModbusPort_t *pxPort,
	uint8_t ucUnitId, uint16_t usReadAddress, uint16_t usReadQuantity,
	uint16_t *pusReadValues, uint16_t usWriteAddress,
	uint16_t usWriteQuantity, const uint16_t *pusWriteValues,
	uint32_t ulTimeoutMs)
{
	ModbusPortResult_e xResult;
	nmbs_error xError;

	if ((pusReadValues == NULL) || (pusWriteValues == NULL)) {
		return MODBUS_PORT_RESULT_INVALID_ARG;
	}
	xResult = prvBegin(pxPort, ucUnitId, ulTimeoutMs);
	if (xResult != MODBUS_PORT_RESULT_OK) {
		return xResult;
	}
	xError = nmbs_read_write_registers(&pxPort->xNmbs, usReadAddress,
		usReadQuantity, pusReadValues, usWriteAddress, usWriteQuantity,
		pusWriteValues);
	return prvFinish(pxPort, xError);
}

/*-----------------------------------------------------------*/

/**
 * @brief  读取基础设备标识对象。
 *
 * @details
 * 通过 nanoMODBUS Device Identification 接口读取 Vendor Name、Product Code 与 Revision。
 * 输出字符串缓冲区由调用者提供，ucBufferLength 作为各输出缓冲区的容量参数传给协议层。
 *
 * @param[in,out] pxPort         已初始化的 Client 端口。
 * @param[in]     ucUnitId       目标 Unit ID。
 * @param[out]    pcVendorName   Vendor Name 输出缓冲区。
 * @param[out]    pcProductCode  Product Code 输出缓冲区。
 * @param[out]    pcRevision     Revision 输出缓冲区。
 * @param[in]     ucBufferLength 输出缓冲区容量。
 * @param[in]     ulTimeoutMs    本次事务总超时时间，单位为毫秒。
 *
 * @return ModbusPortResult_e 工程级事务结果。
 */
ModbusPortResult_e xModbusPortReadDeviceIdentificationBasic(
	ModbusPort_t *pxPort, uint8_t ucUnitId, char *pcVendorName,
	char *pcProductCode, char *pcRevision, uint8_t ucBufferLength,
	uint32_t ulTimeoutMs)
{
	ModbusPortResult_e xResult;
	nmbs_error xError;

	if ((pcVendorName == NULL) || (pcProductCode == NULL) ||
		(pcRevision == NULL) || (ucBufferLength == 0U)) {
		return MODBUS_PORT_RESULT_INVALID_ARG;
	}
	xResult = prvBegin(pxPort, ucUnitId, ulTimeoutMs);
	if (xResult != MODBUS_PORT_RESULT_OK) {
		return xResult;
	}
	xError = nmbs_read_device_identification_basic(&pxPort->xNmbs,
		pcVendorName, pcProductCode, pcRevision, ucBufferLength);
	return prvFinish(pxPort, xError);
}

/*-----------------------------------------------------------*/

/**
 * @brief  读取常规设备标识对象。
 *
 * @details
 * 读取 Vendor URL、Product Name、Model Name 与 Application Name。所有输出缓冲区均由
 * 调用者拥有，并必须在本次同步事务结束前保持有效。
 *
 * @param[in,out] pxPort             已初始化的 Client 端口。
 * @param[in]     ucUnitId           目标 Unit ID。
 * @param[out]    pcVendorUrl        Vendor URL 输出缓冲区。
 * @param[out]    pcProductName      Product Name 输出缓冲区。
 * @param[out]    pcModelName        Model Name 输出缓冲区。
 * @param[out]    pcApplicationName  Application Name 输出缓冲区。
 * @param[in]     ucBufferLength     各输出缓冲区容量。
 * @param[in]     ulTimeoutMs        本次事务总超时时间，单位为毫秒。
 *
 * @return ModbusPortResult_e 工程级事务结果。
 */
ModbusPortResult_e xModbusPortReadDeviceIdentificationRegular(
	ModbusPort_t *pxPort, uint8_t ucUnitId, char *pcVendorUrl,
	char *pcProductName, char *pcModelName, char *pcApplicationName,
	uint8_t ucBufferLength, uint32_t ulTimeoutMs)
{
	ModbusPortResult_e xResult;
	nmbs_error xError;

	if ((pcVendorUrl == NULL) || (pcProductName == NULL) ||
		(pcModelName == NULL) || (pcApplicationName == NULL) ||
		(ucBufferLength == 0U)) {
		return MODBUS_PORT_RESULT_INVALID_ARG;
	}
	xResult = prvBegin(pxPort, ucUnitId, ulTimeoutMs);
	if (xResult != MODBUS_PORT_RESULT_OK) {
		return xResult;
	}
	xError = nmbs_read_device_identification_regular(&pxPort->xNmbs,
		pcVendorUrl, pcProductName, pcModelName, pcApplicationName,
		ucBufferLength);
	return prvFinish(pxPort, xError);
}

/*-----------------------------------------------------------*/

/**
 * @brief  从指定 Object ID 开始读取扩展设备标识对象集合。
 *
 * @details
 * pucIds 提供可接收的对象 ID 数组，ppcBuffers 指向调用者准备的字符串缓冲区集合，
 * pucObjectsCount 返回实际解析到的对象数量。该接口只借用这些输出缓冲区完成同步调用，
 * 不接管任何外部内存所有权。
 *
 * @param[in,out] pxPort          已初始化的 Client 端口。
 * @param[in]     ucUnitId        目标 Unit ID。
 * @param[in]     ucObjectIdStart 起始 Object ID。
 * @param[out]    pucIds          对象 ID 输出数组。
 * @param[out]    ppcBuffers      对象字符串输出缓冲区数组。
 * @param[in]     ucIdsLength     可接收的对象数量。
 * @param[in]     ucBufferLength  单个对象字符串缓冲区容量。
 * @param[out]    pucObjectsCount 实际返回对象数量。
 * @param[in]     ulTimeoutMs     本次事务总超时时间，单位为毫秒。
 *
 * @return ModbusPortResult_e 工程级事务结果。
 */
ModbusPortResult_e xModbusPortReadDeviceIdentificationExtended(
	ModbusPort_t *pxPort, uint8_t ucUnitId, uint8_t ucObjectIdStart,
	uint8_t *pucIds, char **ppcBuffers, uint8_t ucIdsLength,
	uint8_t ucBufferLength, uint8_t *pucObjectsCount,
	uint32_t ulTimeoutMs)
{
	ModbusPortResult_e xResult;
	nmbs_error xError;

	if ((pucIds == NULL) || (ppcBuffers == NULL) ||
		(pucObjectsCount == NULL) || (ucIdsLength == 0U) ||
		(ucBufferLength == 0U)) {
		return MODBUS_PORT_RESULT_INVALID_ARG;
	}
	xResult = prvBegin(pxPort, ucUnitId, ulTimeoutMs);
	if (xResult != MODBUS_PORT_RESULT_OK) {
		return xResult;
	}
	xError = nmbs_read_device_identification_extended(&pxPort->xNmbs,
		ucObjectIdStart, pucIds, ppcBuffers, ucIdsLength,
		ucBufferLength, pucObjectsCount);
	return prvFinish(pxPort, xError);
}

/*-----------------------------------------------------------*/

/**
 * @brief  读取指定 Object ID 的设备标识字符串。
 *
 * @param[in,out] pxPort         已初始化的 Client 端口。
 * @param[in]     ucUnitId       目标 Unit ID。
 * @param[in]     ucObjectId     待读取的 Object ID。
 * @param[out]    pcBuffer       字符串输出缓冲区。
 * @param[in]     ucBufferLength 输出缓冲区容量。
 * @param[in]     ulTimeoutMs    本次事务总超时时间，单位为毫秒。
 *
 * @return ModbusPortResult_e 工程级事务结果。
 */
ModbusPortResult_e xModbusPortReadDeviceIdentification(
	ModbusPort_t *pxPort, uint8_t ucUnitId, uint8_t ucObjectId,
	char *pcBuffer, uint8_t ucBufferLength, uint32_t ulTimeoutMs)
{
	ModbusPortResult_e xResult;
	nmbs_error xError;

	if ((pcBuffer == NULL) || (ucBufferLength == 0U)) {
		return MODBUS_PORT_RESULT_INVALID_ARG;
	}
	xResult = prvBegin(pxPort, ucUnitId, ulTimeoutMs);
	if (xResult != MODBUS_PORT_RESULT_OK) {
		return xResult;
	}
	xError = nmbs_read_device_identification(&pxPort->xNmbs,
		ucObjectId, pcBuffer, ucBufferLength);
	return prvFinish(pxPort, xError);
}

/*-----------------------------------------------------------*/

/**
 * @brief  发送原始 Modbus PDU 请求，并按需接收原始响应 PDU。
 *
 * @details
 * 本接口仍复用 ModbusPort 的事务 deadline、Transport 适配和错误映射，但不替调用者
 * 解释功能码专有 payload。RTU 广播请求只发送不等待响应；非广播请求在发送成功后调用
 * nmbs_receive_raw_pdu_response() 接收指定长度的响应数据。
 *
 * @param[in,out] pxPort            已初始化的 Client 端口。
 * @param[in]     ucUnitId          目标 Unit ID。
 * @param[in]     ucFunctionCode    原始 Function Code。
 * @param[in]     pucRequestData    PDU 数据区；长度为 0 时可为 NULL。
 * @param[in]     usRequestLength   请求数据区长度。
 * @param[out]    pucResponseData   响应数据输出缓冲区。
 * @param[in]     ucResponseLength  期望接收的响应数据长度。
 * @param[in]     ulTimeoutMs       本次事务总超时时间，单位为毫秒。
 *
 * @return ModbusPortResult_e 工程级事务结果。
 */
ModbusPortResult_e xModbusPortRawRequest(ModbusPort_t *pxPort,
	uint8_t ucUnitId, uint8_t ucFunctionCode,
	const uint8_t *pucRequestData, uint16_t usRequestLength,
	uint8_t *pucResponseData, uint8_t ucResponseLength,
	uint32_t ulTimeoutMs)
{
	ModbusPortResult_e xResult;
	nmbs_error xError;

	if ((usRequestLength > 0U) && (pucRequestData == NULL)) {
		return MODBUS_PORT_RESULT_INVALID_ARG;
	}
	xResult = prvBegin(pxPort, ucUnitId, ulTimeoutMs);
	if (xResult != MODBUS_PORT_RESULT_OK) {
		return xResult;
	}
	xError = nmbs_send_raw_pdu(&pxPort->xNmbs, ucFunctionCode,
		pucRequestData, usRequestLength);
	/* RTU 广播按协议只发送请求，不等待任何 Server 响应。 */
	if ((xError == NMBS_ERROR_NONE) &&
		!((pxPort->xTransport == MODBUS_PORT_TRANSPORT_RTU) &&
		(ucUnitId == NMBS_BROADCAST_ADDRESS))) {
		xError = nmbs_receive_raw_pdu_response(&pxPort->xNmbs,
			pucResponseData, ucResponseLength);
	}
	return prvFinish(pxPort, xError);
}

/*-----------------------------------------------------------*/

/**
 * @brief  实现 nanoMODBUS Platform 的底层读取回调。
 *
 * @details
 * 将 nanoMODBUS 的 read(buffer, count, timeout, arg) 契约适配为项目
 * xTransportReceiveExact()。pvArgument 实际保存当前 ModbusPort_t 地址；函数先把阶段超时
 * 限制在整笔事务剩余 deadline 内，再从绑定的 TransportChannel 精确接收指定字节数。
 *
 * Transport TIMEOUT 时返回已经收到的字节数，使 nanoMODBUS 能通过“返回值小于请求长度”
 * 识别超时；其它 Transport 错误返回负值，同时原始 TransportResult 会保存在端口中，
 * 供后续错误映射和故障诊断使用。
 *
 * @param[out] pucData     nanoMODBUS 提供的接收缓冲区。
 * @param[in]  usCount     本次要求接收的字节数。
 * @param[in]  lTimeoutMs  nanoMODBUS 当前阶段请求的超时时间。
 * @param[in]  pvArgument  Platform 上下文，实际为 ModbusPort_t *。
 *
 * @return 成功时返回实际接收字节数；超时时返回已接收字节数；其它失败返回 -1。
 */
static int32_t prvRead(uint8_t *pucData, uint16_t usCount,
	int32_t lTimeoutMs, void *pvArgument)
{
	ModbusPort_t *pxPort;
	TransportResult_e xResult;
	uint32_t ulTimeoutMs;
	uint16_t usReceived;

	/* Platform arg 保存的是 ModbusPort_t 地址，这里恢复真实上下文类型。 */
	pxPort = (ModbusPort_t *)pvArgument;
	if ((pxPort == NULL) || (pucData == NULL) || (usCount == 0U)) {
		return -1;
	}
	/* 阶段超时不能突破整笔 Modbus 事务的剩余 deadline。 */
	ulTimeoutMs = prvGetEffectiveTimeout(pxPort, lTimeoutMs);

	usReceived = 0U;
	/* 通过 Transport 接口精确接收指定字节数，返回实际接收的字节数和 TransportResult*/
	xResult = xTransportReceiveExactCancelable(pxPort->pxChannel, pucData,
		usCount, &usReceived, ulTimeoutMs, prvTransportPreemptCheck, pxPort);
		
	/* 保存更细粒度的底层结果，并只记录真正接收到的字节。 */
	pxPort->xLastTransportResult = xResult;
	if (pxPort->pxTrace != NULL) {
		prvAppendFrame(&pxPort->pxTrace->xLastRx, pucData, usReceived);
	}
	if (xResult == TRANSPORT_RESULT_OK) {
		return (int32_t)usReceived;
	}
	if (xResult == TRANSPORT_RESULT_CANCELED) {
		return -1;
	}
	if (xResult == TRANSPORT_RESULT_TIMEOUT) {
		/* nanoMODBUS 以“非负但小于请求长度”表示本阶段超时/短读。 */
		return (int32_t)usReceived;
	}
	return -1;
}

/*-----------------------------------------------------------*/

/**
 * @brief  实现 nanoMODBUS Platform 的底层写入回调。
 *
 * @details
 * 把 nanoMODBUS 的输出字节流交给项目 Transport 发送。发送前会限制阶段超时到当前事务
 * 剩余 deadline，并在启用 Trace 时先记录待发送帧；只有 Transport 明确返回 OK 时才设置
 * ucTxSucceeded。若之前的 Transport 阶段已经记录失败，则直接拒绝继续发送。
 *
 * @param[in] pucData      待发送字节缓冲区。
 * @param[in] usCount      待发送字节数。
 * @param[in] lTimeoutMs   nanoMODBUS 当前阶段请求的超时时间。
 * @param[in] pvArgument   Platform 上下文，实际为 ModbusPort_t *。
 *
 * @return 成功返回 usCount；Transport 超时返回 0；其它失败返回 -1。
 */
static int32_t prvWrite(const uint8_t *pucData, uint16_t usCount,
	int32_t lTimeoutMs, void *pvArgument)
{
	ModbusPort_t *pxPort;
	TransportResult_e xResult;
	uint32_t ulTimeoutMs;

	/* Platform arg 保存的是 ModbusPort_t 地址，这里恢复真实上下文类型。 */
	pxPort = (ModbusPort_t *)pvArgument;
	if ((pxPort == NULL) || (pucData == NULL) || (usCount == 0U)) {
		return -1;
	}
	/* 前一 IO 阶段已经失败时不再继续发送，避免覆盖原始故障语义。 */
	if (pxPort->xLastTransportResult != TRANSPORT_RESULT_OK) {
		return -1;
	}
	ulTimeoutMs = prvGetEffectiveTimeout(pxPort, lTimeoutMs);
	/* 先记录“计划发送”的帧；真正成功与否由 ucTxSucceeded 单独标记。 */
	if (pxPort->pxTrace != NULL) {
		prvAppendFrame(&pxPort->pxTrace->xLastTx, pucData, usCount);
	}
	xResult = xTransportSend(pxPort->pxChannel, pucData, usCount,
		ulTimeoutMs);
	pxPort->xLastTransportResult = xResult;
	if (xResult == TRANSPORT_RESULT_OK) {
		if (pxPort->pxTrace != NULL) {
			pxPort->pxTrace->ucTxSucceeded = 1U;
		}
		return (int32_t)usCount;
	}
	if (xResult == TRANSPORT_RESULT_TIMEOUT) {
		return 0;
	}
	return -1;
}

/*-----------------------------------------------------------*/

/**
 * @brief  为 nanoMODBUS 提供接收缓冲清理回调。
 *
 * @details
 * 当前只对 RTU 通道执行 TRANSPORT_CTRL_RX_FLUSH，用于丢弃可能影响下一帧解析的过期接收
 * 数据；TCP 模式直接返回。若 Transport Control 失败，会保存原始结果供后续故障诊断使用。
 *
 * @param[in] pxNmbs      nanoMODBUS 实例；当前实现未直接使用该参数。
 * @param[in] pvArgument  Platform 上下文，实际为 ModbusPort_t *。
 */
static void prvFlush(nmbs_t *pxNmbs, void *pvArgument)
{
	ModbusPort_t *pxPort;
	TransportResult_e xResult;

	(void)pxNmbs;
	/* flush 的实际目标由 Platform arg 中保存的 ModbusPort 上下文确定。 */
	pxPort = (ModbusPort_t *)pvArgument;
	if ((pxPort == NULL) ||
		(pxPort->xTransport != MODBUS_PORT_TRANSPORT_RTU)) {
		return;
	}
	xResult = xTransportControl(pxPort->pxChannel,
		TRANSPORT_CTRL_RX_FLUSH, NULL);
	if (xResult != TRANSPORT_RESULT_OK) {
		pxPort->xLastTransportResult = xResult;
	}
}

/*-----------------------------------------------------------*/

/**
 * @brief  初始化 ModbusPort 基础状态，并构造 nanoMODBUS 平台适配配置。
 *
 * @details
 * 该函数只完成 ModbusPort 与 nanoMODBUS Platform / Transport 之间的基础绑定，
 * 不创建 Client 或 Server 实例。调用者后续还需要根据角色调用
 * nmbs_client_create() 或 nmbs_server_create()。
 *
 * pxChannel 由调用者持有，ModbusPort 仅保存其指针，因此其生命周期必须覆盖
 * ModbusPort 的实际使用期。
 *
 * pxPlatform 由调用者提供，本函数填充 nanoMODBUS 所需的 Transport 类型、
 * read/write/flush 回调及上下文指针。后续 nanoMODBUS create 接口会复制该配置。
 *
 * @param[out] pxPort          待初始化的 ModbusPort 对象。
 * @param[in]  pxChannel       已存在的 Transport 通道，所有权仍属于调用者。
 * @param[in]  xTransport      Modbus 传输类型：RTU 或 TCP。
 * @param[in]  xRole           当前 ModbusPort 角色：Client 或 Server。
 * @param[in]  ulByteTimeoutMs 字节阶段超时时间，单位为毫秒。
 * @param[out] pxPlatform      输出 nanoMODBUS Platform 配置。
 *
 * @retval MODBUS_PORT_RESULT_OK          初始化成功。
 * @retval MODBUS_PORT_RESULT_INVALID_ARG 参数无效。
 */
static ModbusPortResult_e prvInit(ModbusPort_t *pxPort,
    TransportChannel_t *pxChannel, ModbusPortTransport_e xTransport,
    ModbusPortRole_e xRole, uint32_t ulByteTimeoutMs,
    nmbs_platform_conf *pxPlatform)
{
    /* 校验必要对象、超时范围以及当前适配层支持的 Transport 类型。 */
    if ((pxPort == NULL) || (pxChannel == NULL) ||
        (pxPlatform == NULL) || (ulByteTimeoutMs == 0U) ||
        (ulByteTimeoutMs > MODBUS_PORT_TIMEOUT_MAX_MS) ||
        ((xTransport != MODBUS_PORT_TRANSPORT_RTU) &&
        (xTransport != MODBUS_PORT_TRANSPORT_TCP))) {
        return MODBUS_PORT_RESULT_INVALID_ARG;
    }

    /* 清空完整 Port，保证内嵌 xNmbs、状态位和诊断字段从确定状态开始。 */
    memset(pxPort, 0, sizeof(*pxPort));

    /* 建立 nanoMODBUS Platform 默认配置，再覆盖本项目的适配回调。 */
    nmbs_platform_conf_create(pxPlatform);

    /* 把工程侧 RTU/TCP 枚举映射到 nanoMODBUS 的 Transport 枚举。 */
    pxPlatform->transport =
        (xTransport == MODBUS_PORT_TRANSPORT_RTU) ?
        NMBS_TRANSPORT_RTU : NMBS_TRANSPORT_TCP;

    /* 函数指针 + context：nanoMODBUS 后续 IO 会回调到当前 ModbusPort 实例。 */
    pxPlatform->read = prvRead;
    pxPlatform->write = prvWrite;
    pxPlatform->flush = prvFlush;
    pxPlatform->arg = pxPort;

    /* Borrowed reference：只保存 Channel 地址，不复制也不接管其生命周期。 */
    pxPort->pxChannel = pxChannel;

    /* 保存本 Port 自身拥有的运行配置值。 */
    pxPort->xTransport = xTransport;
    pxPort->xRole = xRole;
    pxPort->ulByteTimeoutMs = ulByteTimeoutMs;

    /* 初始化跨层错误与诊断状态。 */
    pxPort->xLastFault.xResult = MODBUS_PORT_RESULT_OK;
    pxPort->xLastFault.xTransportResult = TRANSPORT_RESULT_OK;
    pxPort->xLastTransportResult = TRANSPORT_RESULT_OK;

    return MODBUS_PORT_RESULT_OK;
}

/*-----------------------------------------------------------*/

/**
 * @brief  开始一笔 Modbus Client 事务并建立统一 deadline。
 *
 * @details
 * 校验端口角色与总超时后，RTU 模式先等待帧间静默；该等待发生在事务计时开始之前。
 * 随后记录起始 Tick 与总预算、清除上一笔 Transport 结果，向 nanoMODBUS 配置首字节等待
 * 超时、字节间超时和目标 Unit ID，并重置本笔事务 Trace。
 *
 * @param[in,out] pxPort      已初始化且角色为 Client 的 ModbusPort。
 * @param[in]     ucUnitId    本次请求的目标 Unit ID。
 * @param[in]     ulTimeoutMs 本次事务总时间预算，单位为毫秒。
 *
 * @retval MODBUS_PORT_RESULT_OK 事务上下文建立成功。
 * @retval MODBUS_PORT_RESULT_INVALID_ARG 端口、角色或超时参数无效。
 */
static ModbusPortResult_e prvBegin(ModbusPort_t *pxPort,
	uint8_t ucUnitId, uint32_t ulTimeoutMs)
{
	if ((pxPort == NULL) || (pxPort->ucInitialized == 0U) ||
		(pxPort->xRole != MODBUS_PORT_ROLE_CLIENT) ||
		(ulTimeoutMs == 0U) ||
		(ulTimeoutMs > MODBUS_PORT_TIMEOUT_MAX_MS)) {
		return MODBUS_PORT_RESULT_INVALID_ARG;
	}
	
	/* RTU 帧间静默等待不计入下面建立的事务总预算。 */
	prvWaitFrameSilence(pxPort);
	pxPort->xOperationStart = xTaskGetTickCount();
	pxPort->xOperationBudget = prvMsToTicks(ulTimeoutMs);
	pxPort->ucOperationActive = 1U; // 标记当前事务正在进行
	pxPort->ucPreempted = 0U; // 清除上次事务的抢占标志
	pxPort->xLastTransportResult = TRANSPORT_RESULT_OK;
	nmbs_set_read_timeout(&pxPort->xNmbs, (int32_t)ulTimeoutMs);
	nmbs_set_byte_timeout(&pxPort->xNmbs, (int32_t)pxPort->ulByteTimeoutMs);
	nmbs_set_destination_rtu_address(&pxPort->xNmbs, ucUnitId);
	prvResetTrace(pxPort);
	return MODBUS_PORT_RESULT_OK;
}

/*-----------------------------------------------------------*/

/**
 * @brief  在 RTU Client 新事务前等待至少 3.5 个字符时间的帧间静默。
 *
 * @details
 * 通过 Transport Control 获取当前串口波特率；查询失败或得到 0 时使用 9600 baud 作为
 * 保守回退值。当前实现按 8N1 的 10 bit/字符估算 3.5 字符时间，即 35,000,000/baud us，
 * 再向上取整到毫秒并转换为至少一个 FreeRTOS Tick。TCP 模式无需该帧间静默。
 *
 * @param[in] pxPort 当前 ModbusPort。
 */
static void prvWaitFrameSilence(const ModbusPort_t *pxPort)
{
	uint32_t ulBaudRate;
	uint32_t ulSilenceUs;
	uint32_t ulSilenceMs;
	TransportResult_e xResult;

	/* TCP 没有 RTU 的字符静默分帧要求。 */
	if ((pxPort == NULL) ||
		(pxPort->xTransport != MODBUS_PORT_TRANSPORT_RTU)) {
		return;
	}

	/* 从 Transport 查询当前真实波特率，失败时采用保守回退值。 */
	ulBaudRate = 0U;
	xResult = xTransportControl(pxPort->pxChannel,
		TRANSPORT_CTRL_GET_BAUD_RATE, &ulBaudRate);
	if ((xResult != TRANSPORT_RESULT_OK) || (ulBaudRate == 0U)) {
		ulBaudRate = 9600U;
	}

	/* 8N1 下 3.5 字符 = 35 bit；先算微秒，再向上取整到毫秒。 */
	ulSilenceUs = (35000000UL) / ulBaudRate;
	ulSilenceMs = (ulSilenceUs + 999U) / 1000U;
	vTaskDelay(prvMsToTicks(ulSilenceMs));
}

/*-----------------------------------------------------------*/

/**
 * @brief  结束当前事务并发布统一的工程结果与诊断信息。
 *
 * @details
 * 清除 operation-active 标志，将 nanoMODBUS 原始错误映射为 ModbusPortResult_e；若本轮
 * Trace 确实收到数据且最终为 OK 或 Modbus Exception，则标记 RX 成功。最后更新详细 Fault
 * 快照，供上层诊断使用。
 *
 * @param[in,out] pxPort 当前 ModbusPort。
 * @param[in]     xError nanoMODBUS 返回的原始错误/Exception。
 *
 * @return 映射后的 ModbusPortResult_e。
 */
static ModbusPortResult_e prvFinish(ModbusPort_t *pxPort,
	nmbs_error xError)
{
	ModbusPortResult_e xResult;

	/* 从此刻起 deadline 不再参与 IO 超时计算。 */
	pxPort->ucOperationActive = 0U;
	xResult = prvMapError(pxPort, xError);
	if ((pxPort->pxTrace != NULL) &&
		(pxPort->pxTrace->xLastRx.usLength > 0U) &&
		((xResult == MODBUS_PORT_RESULT_OK) ||
		(xResult == MODBUS_PORT_RESULT_EXCEPTION))) {
		pxPort->pxTrace->ucRxSucceeded = 1U;
	}
	prvUpdateFaultDetail(pxPort, xResult, xError);
	return xResult;
}

/*-----------------------------------------------------------*/

/**
 * @brief  将 nanoMODBUS 错误域映射为稳定的工程错误域。
 *
 * @details
 * Modbus Exception 被归类为 MODBUS_PORT_RESULT_EXCEPTION；nano timeout 与参数错误直接映射。
 * 对 NMBS_ERROR_TRANSPORT，则结合之前保存的 xLastTransportResult 恢复 BUSY、NOT_READY、
 * NOT_SUPPORTED 等更具体的工程语义。其它 nanoMODBUS 错误统一归为协议错误。
 *
 * @param[in] pxPort  当前 ModbusPort，用于读取最近 Transport 结果。
 * @param[in] xError  nanoMODBUS 原始错误/Exception。
 *
 * @return 映射后的 ModbusPortResult_e。
 */
static ModbusPortResult_e prvMapError(ModbusPort_t *pxPort,
	nmbs_error xError)
{
	if ((pxPort != NULL) && (pxPort->ucPreempted != 0U)) {
		return MODBUS_PORT_RESULT_PREEMPTED;
	}
	if (xError == NMBS_ERROR_NONE) {
		return MODBUS_PORT_RESULT_OK;
	}
	if (nmbs_error_is_exception(xError)) {
		return MODBUS_PORT_RESULT_EXCEPTION;
	}
	if (xError == NMBS_ERROR_TIMEOUT) {
		return MODBUS_PORT_RESULT_TIMEOUT;
	}
	if (xError == NMBS_ERROR_INVALID_ARGUMENT) {
		return MODBUS_PORT_RESULT_INVALID_ARG;
	}
	if (xError == NMBS_ERROR_TRANSPORT) {
		switch (pxPort->xLastTransportResult) {
		case TRANSPORT_RESULT_TIMEOUT:
			return MODBUS_PORT_RESULT_TIMEOUT;

		case TRANSPORT_RESULT_BUSY:
			return MODBUS_PORT_RESULT_BUSY;

		case TRANSPORT_RESULT_NOT_OPEN:
		case TRANSPORT_RESULT_NOT_READY:
			return MODBUS_PORT_RESULT_NOT_READY;

		case TRANSPORT_RESULT_NOT_SUPPORTED:
			return MODBUS_PORT_RESULT_NOT_SUPPORTED;

		default:
			return MODBUS_PORT_RESULT_TRANSPORT;
		}
	}
	return MODBUS_PORT_RESULT_PROTOCOL;
}

/*-----------------------------------------------------------*/
static uint8_t prvTransportPreemptCheck(void *pvContext)
{
	ModbusPort_t *pxPort;

	pxPort = (ModbusPort_t *)pvContext;
	if ((pxPort == NULL) || (pxPort->pxPreemptCheck == NULL)) {
		return 0U;
	}
	if (pxPort->pxPreemptCheck(pxPort->pvPreemptContext) != 0U) {
		pxPort->ucPreempted = 1U;
		return 1U;
	}
	return 0U;
}

/*-----------------------------------------------------------*/

/**
 * @brief  计算当前 IO 阶段实际可使用的超时时间。
 *
 * @details
 * 先取得整笔事务剩余时间；若事务预算已经耗尽则返回 0。nanoMODBUS 传入负超时表示该阶段
 * 不设独立上限，此时仍受 ModbusPort 总 deadline 约束；非负阶段超时则与剩余时间取较小值。
 *
 * @param[in] pxPort       当前 ModbusPort。
 * @param[in] lRequestedMs nanoMODBUS 请求的阶段超时，负值表示阶段无限等待。
 *
 * @return 当前阶段可使用的毫秒数；总预算耗尽时返回 0。
 */
static uint32_t prvGetEffectiveTimeout(ModbusPort_t *pxPort,
	int32_t lRequestedMs)
{
	uint32_t ulRemainingMs;
	uint32_t ulRequestedMs;

	/* ModbusPort 的总 deadline 始终优先于协议阶段自身的 timeout。 */
	ulRemainingMs = prvGetRemainingMs(pxPort);
	if (ulRemainingMs == 0U) {
		return 0U;
	}
	if (lRequestedMs < 0) {
		return ulRemainingMs;
	}
	ulRequestedMs = (uint32_t)lRequestedMs;
	return (ulRequestedMs < ulRemainingMs) ?
		ulRequestedMs : ulRemainingMs;
}

/*-----------------------------------------------------------*/

/**
 * @brief  计算当前事务总 deadline 的剩余时间。
 *
 * @details
 * 使用 FreeRTOS Tick 差值计算已经消耗的预算；未处于活动事务或预算已经耗尽时返回 0，
 * 否则把剩余 Tick 向上换算为毫秒，避免因整数截断提前缩短剩余时间。
 *
 * @param[in] pxPort 当前 ModbusPort。
 *
 * @return 剩余毫秒数；无活动事务或已超时时返回 0。
 */
static uint32_t prvGetRemainingMs(const ModbusPort_t *pxPort)
{
	TickType_t xElapsed;

	if ((pxPort == NULL) || (pxPort->ucOperationActive == 0U)) {
		return 0U;
	}
	xElapsed = xTaskGetTickCount() - pxPort->xOperationStart;
	if (xElapsed >= pxPort->xOperationBudget) {
		return 0U;
	}
	return prvTicksToMsCeil(pxPort->xOperationBudget - xElapsed);
}

/*-----------------------------------------------------------*/

/**
 * @brief  将毫秒超时转换为 FreeRTOS Tick，并保证非零毫秒至少得到一个 Tick。
 *
 * @param[in] ulTimeoutMs 超时时间，单位为毫秒。
 *
 * @return 对应 Tick 数；pdMS_TO_TICKS() 结果为 0 时返回 1。
 */
static TickType_t prvMsToTicks(uint32_t ulTimeoutMs)
{
	TickType_t xTicks;

	xTicks = pdMS_TO_TICKS(ulTimeoutMs);
	return (xTicks == 0U) ? 1U : xTicks;
}

/*-----------------------------------------------------------*/

/**
 * @brief  将 FreeRTOS Tick 数向上取整换算为毫秒。
 *
 * @details
 * 使用 64 位中间结果避免乘法溢出，并对最终结果做 UINT32_MAX 饱和处理。
 *
 * @param[in] xTicks 待转换的 Tick 数。
 *
 * @return 向上取整后的毫秒数。
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
 * @brief  为新事务重置可选 Trace，并分配新的事务序号。
 *
 * @details
 * 未绑定 Trace 时直接返回。绑定后清空整个 Trace 对象，递增 ulTraceSequence，并把同一序号
 * 写入本轮 TX/RX 记录，使发送与接收可以按事务关联。
 *
 * @param[in,out] pxPort 当前 ModbusPort。
 */
static void prvResetTrace(ModbusPort_t *pxPort)
{
	if (pxPort->pxTrace == NULL) {
		return;
	}
	/* Trace 为外部对象，但内容由 ModbusPort 在每笔事务开始时重置。 */
	memset(pxPort->pxTrace, 0, sizeof(*pxPort->pxTrace));
	pxPort->ulTraceSequence++;
	pxPort->pxTrace->xLastTx.ulSequence = pxPort->ulTraceSequence;
	pxPort->pxTrace->xLastRx.ulSequence = pxPort->ulTraceSequence;
}

/*-----------------------------------------------------------*/

/**
 * @brief  向有界 Trace 帧中追加本次实际收发的数据。
 *
 * @details
 * aucData 只保留最多 MODBUS_PORT_TRACE_LENGTH 字节，但 usLength 始终累计完整逻辑长度并在
 * UINT16_MAX 处饱和，因此诊断层可以同时知道“总共经过多少字节”和“实际捕获多少字节”。
 * 当前实现对 NULL 数据或 0 长度直接忽略，不更新长度。
 *
 * @param[in,out] pxFrame 目标 Trace 帧。
 * @param[in]     pucData 待追加的有效数据。
 * @param[in]     usLength 本次追加长度。
 */
static void prvAppendFrame(ModbusPortFrame_t *pxFrame,
	const uint8_t *pucData, uint16_t usLength)
{
	uint16_t usAvailable;
	uint16_t usCopyLength;

	if ((pxFrame == NULL) || (pucData == NULL) || (usLength == 0U)) {
		return;
	}
	if (pxFrame->usCapturedLength < MODBUS_PORT_TRACE_LENGTH) {
		usAvailable = (uint16_t)(MODBUS_PORT_TRACE_LENGTH -
			pxFrame->usCapturedLength);
		usCopyLength = (usLength < usAvailable) ?
			usLength : usAvailable;
		memcpy(&pxFrame->aucData[pxFrame->usCapturedLength],
			pucData, usCopyLength);
		pxFrame->usCapturedLength =
			(uint16_t)(pxFrame->usCapturedLength + usCopyLength);
	}
	if (usLength > (uint16_t)(UINT16_MAX - pxFrame->usLength)) {
		pxFrame->usLength = UINT16_MAX;
	} else {
		pxFrame->usLength =
			(uint16_t)(pxFrame->usLength + usLength);
	}
}

/*-----------------------------------------------------------*/

/**
 * @brief  更新最近一次事务的详细故障快照。
 *
 * @details
 * 每次调用先清空旧 Fault，再保存工程结果、nanoMODBUS 原始代码和最近 TransportResult。
 * 若 nano 错误属于 Modbus Exception，则额外记录 Exception Code；若 Transport 确实失败，
 * 再从 Transport Status 中提取后端原生错误码，保留跨层诊断链。
 *
 * @param[in,out] pxPort  当前 ModbusPort。
 * @param[in]     xResult 已映射的工程结果。
 * @param[in]     xError  nanoMODBUS 原始错误/Exception。
 */
static void prvUpdateFaultDetail(ModbusPort_t *pxPort,
	ModbusPortResult_e xResult, nmbs_error xError)
{
	TransportStatus_t xStatus;

	/* Fault 是 ModbusPort 内嵌对象，每次事务结束时覆盖为新的完整快照。 */
	memset(&pxPort->xLastFault, 0, sizeof(pxPort->xLastFault));
	pxPort->xLastFault.xResult = xResult;
	pxPort->xLastFault.xTransportResult =
		pxPort->xLastTransportResult;
	pxPort->xLastFault.lProtocolCode = (int32_t)xError;
	if (nmbs_error_is_exception(xError)) {
		pxPort->xLastFault.ucExceptionCode = (uint8_t)xError;
	}
	if ((pxPort->xLastTransportResult != TRANSPORT_RESULT_OK) &&
		(xTransportGetStatus(pxPort->pxChannel, &xStatus) ==
		TRANSPORT_RESULT_OK)) {
		pxPort->xLastFault.lNativeError =
			xStatus.xLastFault.lNativeError;
	}
}
