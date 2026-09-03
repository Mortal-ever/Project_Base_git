/**
  * @file      modbus_port.c
  * @brief     Implement the nanoMODBUS project and Transport integration.
  * @author    WHong
  * @date      2026-07-28
  */

#include "modbus_port.h"

#include <string.h>

#include "task.h"

/**
  * @brief  读取 nanoMODBUS 请求的指定字节段。
  * @param[out] pucData nanoMODBUS 提供的输出缓冲区。
  * @param[in] usCount 需要读取的字节数。
  * @param[in] lTimeoutMs 当前阶段超时时间，单位为毫秒。
  * @param[in] pvArgument ModbusPort_t 上下文指针。
  * @retval 实际读取字节数；失败时返回负的 Transport/协议错误。
  */
static int32_t prvRead(uint8_t *pucData, uint16_t usCount,
	int32_t lTimeoutMs, void *pvArgument);
/**
  * @brief  发送 nanoMODBUS 生成的一段完整字节。
  * @param[in] pucData 待发送数据。
  * @param[in] usCount 待发送字节数。
  * @param[in] lTimeoutMs 当前阶段超时时间，单位为毫秒。
  * @param[in] pvArgument ModbusPort_t 上下文指针。
  * @retval 实际发送字节数；失败时返回负的 Transport 错误。
  */
static int32_t prvWrite(const uint8_t *pucData, uint16_t usCount,
	int32_t lTimeoutMs, void *pvArgument);
/**
  * @brief  在新的 RTU 客户端请求前清理过期接收数据。
  * @param[in] pxNmbs nanoMODBUS 实例，当前仅用于回调上下文关联。
  * @param[in] pvArgument ModbusPort_t 上下文指针。
  */
static void prvFlush(nmbs_t *pxNmbs, void *pvArgument);
/**
  * @brief  初始化通用端口和 nanoMODBUS 平台配置。
  * @param[out] pxPort 调用者拥有的 ModbusPort 对象。
  * @param[in] pxChannel 已注册且生命周期覆盖 pxPort 的 Transport 通道。
  * @param[in] xTransport RTU 或 TCP 帧格式。
  * @param[in] xRole 客户端或 Server 角色。
  * @param[in] ulByteTimeoutMs 单阶段字节超时，单位为毫秒。
  * @param[out] pxPlatform 输出 nanoMODBUS 平台回调配置。
  * @retval MODBUS_PORT_RESULT_OK 初始化成功。
  * @retval 其他 ModbusPortResult_e 参数或 Transport 配置失败。
  */
static ModbusPortResult_e prvInit(ModbusPort_t *pxPort,
	TransportChannel_t *pxChannel, ModbusPortTransport_e xTransport,
	ModbusPortRole_e xRole, uint32_t ulByteTimeoutMs,
	nmbs_platform_conf *pxPlatform);
/**
  * @brief  开始一次客户端事务并建立总截止时间。
  * @param[in,out] pxPort 已初始化的 ModbusPort 客户端。
  * @param[in] ucUnitId 目标 Modbus Unit ID。
  * @param[in] ulTimeoutMs 本次事务总超时时间，单位为毫秒。
  * @retval MODBUS_PORT_RESULT_OK 事务已开始。
  * @retval 其他 ModbusPortResult_e 参数、忙或端口未就绪。
  */
static ModbusPortResult_e prvBegin(ModbusPort_t *pxPort,
	uint8_t ucUnitId, uint32_t ulTimeoutMs);
/**
  * @brief  结束一次事务、映射结果并发布诊断信息。
  * @param[in,out] pxPort 当前事务的 ModbusPort。
  * @param[in] xError nanoMODBUS 原始结果。
  * @retval 规范化 ModbusPortResult_e。
  */
static ModbusPortResult_e prvFinish(ModbusPort_t *pxPort,
	nmbs_error xError);
/**
  * @brief  将 nanoMODBUS 结果映射到稳定的工程错误域。
  * @param[in,out] pxPort 用于保存详细故障的 ModbusPort。
  * @param[in] xError nanoMODBUS 原始错误或异常。
  * @retval 规范化 ModbusPortResult_e。
  */
static ModbusPortResult_e prvMapError(ModbusPort_t *pxPort,
	nmbs_error xError);
/**
  * @brief  将回调超时限制在当前事务剩余时间内。
  * @param[in,out] pxPort 当前事务的 ModbusPort。
  * @param[in] lRequestedMs 回调请求的毫秒超时。
  * @retval 可使用的非零剩余超时时间，单位为毫秒。
  */
static uint32_t prvGetEffectiveTimeout(ModbusPort_t *pxPort,
	int32_t lRequestedMs);
/**
  * @brief  计算当前事务剩余时间。
  * @param[in] pxPort 当前事务的 ModbusPort。
  * @retval 剩余时间，单位为毫秒；没有剩余时间时返回 0。
  */
static uint32_t prvGetRemainingMs(const ModbusPort_t *pxPort);
/**
  * @brief  将毫秒向上转换为至少一个 FreeRTOS Tick。
  * @param[in] ulTimeoutMs 超时时间，单位为毫秒。
  * @retval 向上取整后的 Tick 数。
  */
static TickType_t prvMsToTicks(uint32_t ulTimeoutMs);
/**
  * @brief  将 Tick 数向上取整转换为毫秒。
  * @param[in] xTicks 待转换的 Tick 数。
  * @retval 向上取整后的毫秒数。
  */
static uint32_t prvTicksToMsCeil(TickType_t xTicks);
/**
  * @brief  清空并递增可选事务帧跟踪序号。
  * @param[in,out] pxPort 当前 ModbusPort。
  */
static void prvResetTrace(ModbusPort_t *pxPort);
/**
  * @brief  向有界帧跟踪追加数据并保留完整帧长度。
  * @param[in,out] pxFrame 目标帧跟踪对象。
  * @param[in] pucData 待追加的数据，可以为 NULL（此时只更新长度）。
  * @param[in] usLength 本次追加的字节数。
  */
static void prvAppendFrame(ModbusPortFrame_t *pxFrame,
	const uint8_t *pucData, uint16_t usLength);
/**
  * @brief  保存规范化、协议层和后端原生故障详情。
  * @param[in,out] pxPort 当前 ModbusPort。
  * @param[in] xResult 工程规范化结果。
  * @param[in] xError nanoMODBUS 原始错误或异常。
  */
static void prvUpdateFaultDetail(ModbusPort_t *pxPort,
	ModbusPortResult_e xResult, nmbs_error xError);
/**
  * @brief  在每次 RTU 事务开始前等待满足 Modbus 帧间静默间隔。
  * @details 依据 Modbus RTU 规范，帧与帧之间必须保持至少 3.5 个字符
  *          时间的静默，从站以此划分帧边界。本函数通过 Transport 接口
  *          读取当前串口波特率，动态计算 3.5 字符时间并向上取整等待，
  *          从而避免连续事务（如 IO 模块一次刷新内的多次读）之间
  *          因缺少间隔导致从站无法正确分帧、CRC 校验失败而不响应。
  * @param[in] pxPort 已初始化的 ModbusPort 客户端。
  * @note 仅对 RTU 传输生效；TCP 传输无需帧间静默，直接返回。
  */
static void prvWaitFrameSilence(const ModbusPort_t *pxPort);

/*-----------------------------------------------------------*/
ModbusPortResult_e xModbusPortClientInit(ModbusPort_t *pxPort,
	TransportChannel_t *pxChannel, ModbusPortTransport_e xTransport,
	uint32_t ulByteTimeoutMs)
{
	ModbusPortResult_e xResult;
	nmbs_platform_conf xPlatform; /* nanoMODBUS 平台配置（栈上临时变量）。 */
	nmbs_error xError;
	/* 步骤 1：调用 prvInit() 填充 ModbusPort_t 和平台配置。 */
	xResult = prvInit(pxPort, pxChannel, xTransport,
		MODBUS_PORT_ROLE_CLIENT, ulByteTimeoutMs, &xPlatform);
	if (xResult != MODBUS_PORT_RESULT_OK) {
		return xResult;
	}
	/* 步骤 2：创建 nanoMODBUS 客户端实例。 */
	xError = nmbs_client_create(&pxPort->xNmbs, &xPlatform);
	if (xError != NMBS_ERROR_NONE) {
		return prvFinish(pxPort, xError);
	}
	/* 步骤 3：标记初始化完成。 */
	pxPort->ucInitialized = 1U;
	return MODBUS_PORT_RESULT_OK;
}

#if (NANOMODBUS_CFG_SERVER_ENABLED != 0)
/*-----------------------------------------------------------*/
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
	xResult = prvInit(pxPort, pxChannel, xTransport,
		MODBUS_PORT_ROLE_SERVER, ulByteTimeoutMs, &xPlatform);
	if (xResult != MODBUS_PORT_RESULT_OK) {
		return xResult;
	}
	xError = nmbs_server_create(&pxPort->xNmbs, ucRtuAddress,
		&xPlatform, pxCallbacks);
	if (xError != NMBS_ERROR_NONE) {
		return prvFinish(pxPort, xError);
	}
	pxPort->ucInitialized = 1U;
	return MODBUS_PORT_RESULT_OK;
}

/*-----------------------------------------------------------*/
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
void vModbusPortSetTrace(ModbusPort_t *pxPort,
	ModbusPortTrace_t *pxTrace)
{
	if (pxPort == NULL) {
		return;
	}
	pxPort->pxTrace = pxTrace;
	if (pxTrace != NULL) {
		memset(pxTrace, 0, sizeof(*pxTrace));
	}
}

/*-----------------------------------------------------------*/
void vModbusPortGetLastFault(const ModbusPort_t *pxPort,
	ModbusPortFault_t *pxFault)
{
	if ((pxPort == NULL) || (pxFault == NULL)) {
		return;
	}
	*pxFault = pxPort->xLastFault;
}

/*-----------------------------------------------------------*/
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
	if ((xError == NMBS_ERROR_NONE) &&
		!((pxPort->xTransport == MODBUS_PORT_TRANSPORT_RTU) &&
		(ucUnitId == NMBS_BROADCAST_ADDRESS))) {
		xError = nmbs_receive_raw_pdu_response(&pxPort->xNmbs,
			pucResponseData, ucResponseLength);
	}
	return prvFinish(pxPort, xError);
}

/*-----------------------------------------------------------*/
static int32_t prvRead(uint8_t *pucData, uint16_t usCount,
	int32_t lTimeoutMs, void *pvArgument)
{
	ModbusPort_t *pxPort;
	TransportResult_e xResult;
	uint32_t ulTimeoutMs;
	uint16_t usReceived;

	pxPort = (ModbusPort_t *)pvArgument;
	if ((pxPort == NULL) || (pucData == NULL) || (usCount == 0U)) {
		return -1;
	}
	ulTimeoutMs = prvGetEffectiveTimeout(pxPort, lTimeoutMs);
	usReceived = 0U;
	xResult = xTransportReceiveExact(pxPort->pxChannel, pucData,
		usCount, &usReceived, ulTimeoutMs);
	pxPort->xLastTransportResult = xResult;
	if (pxPort->pxTrace != NULL) {
		prvAppendFrame(&pxPort->pxTrace->xLastRx, pucData, usReceived);
	}
	if (xResult == TRANSPORT_RESULT_OK) {
		return (int32_t)usReceived;
	}
	if (xResult == TRANSPORT_RESULT_TIMEOUT) {
		return (int32_t)usReceived;
	}
	return -1;
}

/*-----------------------------------------------------------*/
static int32_t prvWrite(const uint8_t *pucData, uint16_t usCount,
	int32_t lTimeoutMs, void *pvArgument)
{
	ModbusPort_t *pxPort;
	TransportResult_e xResult;
	uint32_t ulTimeoutMs;

	pxPort = (ModbusPort_t *)pvArgument;
	if ((pxPort == NULL) || (pucData == NULL) || (usCount == 0U)) {
		return -1;
	}
	if (pxPort->xLastTransportResult != TRANSPORT_RESULT_OK) {
		return -1;
	}
	ulTimeoutMs = prvGetEffectiveTimeout(pxPort, lTimeoutMs);
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
static void prvFlush(nmbs_t *pxNmbs, void *pvArgument)
{
	ModbusPort_t *pxPort;
	TransportResult_e xResult;

	(void)pxNmbs;
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
static ModbusPortResult_e prvInit(ModbusPort_t *pxPort,
	TransportChannel_t *pxChannel, ModbusPortTransport_e xTransport,
	ModbusPortRole_e xRole, uint32_t ulByteTimeoutMs,
	nmbs_platform_conf *pxPlatform)
{
	if ((pxPort == NULL) || (pxChannel == NULL) ||
		(pxPlatform == NULL) || (ulByteTimeoutMs == 0U) ||
		(ulByteTimeoutMs > MODBUS_PORT_TIMEOUT_MAX_MS) ||
		((xTransport != MODBUS_PORT_TRANSPORT_RTU) &&
		(xTransport != MODBUS_PORT_TRANSPORT_TCP))) {
		return MODBUS_PORT_RESULT_INVALID_ARG;
	}
	memset(pxPort, 0, sizeof(*pxPort));
	nmbs_platform_conf_create(pxPlatform);
	pxPlatform->transport =
		(xTransport == MODBUS_PORT_TRANSPORT_RTU) ?
		NMBS_TRANSPORT_RTU : NMBS_TRANSPORT_TCP;
	pxPlatform->read = prvRead;
	pxPlatform->write = prvWrite;
	pxPlatform->flush = prvFlush;
	pxPlatform->arg = pxPort;
	pxPort->pxChannel = pxChannel;
	pxPort->xTransport = xTransport;
	pxPort->xRole = xRole;
	pxPort->ulByteTimeoutMs = ulByteTimeoutMs;
	pxPort->xLastFault.xResult = MODBUS_PORT_RESULT_OK;
	pxPort->xLastFault.xTransportResult = TRANSPORT_RESULT_OK;
	pxPort->xLastTransportResult = TRANSPORT_RESULT_OK;
	return MODBUS_PORT_RESULT_OK;
}

/*-----------------------------------------------------------*/
static ModbusPortResult_e prvBegin(ModbusPort_t *pxPort,
	uint8_t ucUnitId, uint32_t ulTimeoutMs)
{
	if ((pxPort == NULL) || (pxPort->ucInitialized == 0U) ||
		(pxPort->xRole != MODBUS_PORT_ROLE_CLIENT) ||
		(ulTimeoutMs == 0U) ||
		(ulTimeoutMs > MODBUS_PORT_TIMEOUT_MAX_MS)) {
		return MODBUS_PORT_RESULT_INVALID_ARG;
	}
	/* 在每次事务开始前等待满足 RTU 帧间静默间隔，避免连续帧粘包。 */
	prvWaitFrameSilence(pxPort);
	pxPort->xOperationStart = xTaskGetTickCount();
	pxPort->xOperationBudget = prvMsToTicks(ulTimeoutMs);
	pxPort->ucOperationActive = 1U;
	pxPort->xLastTransportResult = TRANSPORT_RESULT_OK;
	nmbs_set_read_timeout(&pxPort->xNmbs, (int32_t)ulTimeoutMs);
	nmbs_set_byte_timeout(&pxPort->xNmbs,
		(int32_t)pxPort->ulByteTimeoutMs);
	nmbs_set_destination_rtu_address(&pxPort->xNmbs, ucUnitId);
	prvResetTrace(pxPort);
	return MODBUS_PORT_RESULT_OK;
}

/*-----------------------------------------------------------*/
static void prvWaitFrameSilence(const ModbusPort_t *pxPort)
{
	uint32_t ulBaudRate;
	uint32_t ulSilenceUs;
	uint32_t ulSilenceMs;
	TransportResult_e xResult;

	/* 仅对 RTU 传输需要帧间静默；TCP 传输无需等待，直接返回。 */
	if ((pxPort == NULL) ||
		(pxPort->xTransport != MODBUS_PORT_TRANSPORT_RTU)) {
		return;
	}
	/* 通过 Transport 接口读取当前串口波特率（始终为最新生效值）。 */
	ulBaudRate = 0U;
	xResult = xTransportControl(pxPort->pxChannel,
		TRANSPORT_CTRL_GET_BAUD_RATE, &ulBaudRate);
	if ((xResult != TRANSPORT_RESULT_OK) || (ulBaudRate == 0U)) {
		/* 读取失败或波特率无效时，按最低波特率 9600 保守等待。 */
		ulBaudRate = 9600U;
	}
	/*
	 * Modbus RTU 规范要求帧间静默至少 3.5 个字符时间。
	 * 8N1 格式下每个字符 = 10 bit（1 起始 + 8 数据 + 1 停止），
	 * 故静默时间 = 3.5 × 10 / 波特率，单位为秒。
	 * 此处换算为微秒：3.5 × 10 × 1e6 / 波特率 = 35e6 / 波特率。
	 */
	ulSilenceUs = (35000000UL) / ulBaudRate;
	/* 向上取整到毫秒，保证至少等待 1 个 FreeRTOS Tick。 */
	ulSilenceMs = (ulSilenceUs + 999U) / 1000U;
	vTaskDelay(prvMsToTicks(ulSilenceMs));
}

/*-----------------------------------------------------------*/
static ModbusPortResult_e prvFinish(ModbusPort_t *pxPort,
	nmbs_error xError)
{
	ModbusPortResult_e xResult;

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
static ModbusPortResult_e prvMapError(ModbusPort_t *pxPort,
	nmbs_error xError)
{
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
static uint32_t prvGetEffectiveTimeout(ModbusPort_t *pxPort,
	int32_t lRequestedMs)
{
	uint32_t ulRemainingMs;
	uint32_t ulRequestedMs;

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
static TickType_t prvMsToTicks(uint32_t ulTimeoutMs)
{
	TickType_t xTicks;

	xTicks = pdMS_TO_TICKS(ulTimeoutMs);
	return (xTicks == 0U) ? 1U : xTicks;
}

/*-----------------------------------------------------------*/
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
static void prvResetTrace(ModbusPort_t *pxPort)
{
	if (pxPort->pxTrace == NULL) {
		return;
	}
	memset(pxPort->pxTrace, 0, sizeof(*pxPort->pxTrace));
	pxPort->ulTraceSequence++;
	pxPort->pxTrace->xLastTx.ulSequence = pxPort->ulTraceSequence;
	pxPort->pxTrace->xLastRx.ulSequence = pxPort->ulTraceSequence;
}

/*-----------------------------------------------------------*/
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
static void prvUpdateFaultDetail(ModbusPort_t *pxPort,
	ModbusPortResult_e xResult, nmbs_error xError)
{
	TransportStatus_t xStatus;

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
