/**
  * @file      app_ota_http.c
  * @brief     Serve a minimal browser-compatible raw-lwIP OTA endpoint.
  * @author    WHong
  * @date      2026-08-25
  *
  * @details   All lwIP calls execute on the tcpip thread. The sole static
  *            session streams pbuf content into the common flash writer.
  */

#include "Ota/app_ota_http.h"

#include <stddef.h>
#include <string.h>

#include "Ota/app_ota_flash.h"
#include "Log/app_log.h"
#include "lwip/pbuf.h"
#include "lwip/ip4_addr.h"
#include "lwip/ip_addr.h"
#include "lwip/netif.h"
#include "lwip/tcp.h"
#include "lwip/tcpip.h"
#include "lwip/timeouts.h"
#include "main.h"

#define APP_OTA_HTTP_HEADER_CAP             2048U
#define APP_OTA_HTTP_PART_HEADER_CAP        512U
#define APP_OTA_HTTP_BOUNDARY_CAP           70U
#define APP_OTA_HTTP_MARKER_CAP             80U
#define APP_OTA_HTTP_BODY_OVERHEAD          4096UL
#define APP_OTA_HTTP_LOG_TEXT_CAP           72U
#define APP_OTA_HTTP_DIAGNOSTIC_CAP         512U

typedef enum {
	APP_OTA_HTTP_HEADER = 0,
	APP_OTA_HTTP_PART_HEADER = 1,
	APP_OTA_HTTP_DATA = 2,
	APP_OTA_HTTP_COMPLETE = 3,
	APP_OTA_HTTP_RESPONSE = 4
} AppOtaHttpState_e;

typedef struct {
	struct tcp_pcb *pxPcb;
	const char *pcResponse;
	uint16_t usResponseLength;
	uint16_t usResponseOffset;
	uint16_t usHeaderLength;
	uint16_t usPartHeaderLength;
	uint16_t usBoundaryLength;
	uint16_t usMarkerLength;
	uint16_t usHoldLength;
	uint32_t ulContentLength;
	uint32_t ulWrittenBytes;
	uint32_t ulNextProgress;
	uint8_t ucInUse;
	uint8_t ucHeaderDone;
	uint8_t ucPostRequest;
	uint8_t ucUploadActive;
	uint8_t ucResetPending;
	uint8_t ucBoundaryReady;
	AppOtaHttpState_e xState;
	char acHeader[APP_OTA_HTTP_HEADER_CAP];
	char acPartHeader[APP_OTA_HTTP_PART_HEADER_CAP];
	char acBoundary[APP_OTA_HTTP_BOUNDARY_CAP + 1U];
	uint8_t aucMarker[APP_OTA_HTTP_MARKER_CAP];
	uint8_t aucHold[APP_OTA_HTTP_MARKER_CAP];
} AppOtaHttpSession_t;

static struct tcp_pcb *s_pxListener;
static uint8_t s_ucInitRequested;
static AppOtaHttpSession_t s_xSession;

static const char s_acUploadPage[] =
	"HTTP/1.0 200 OK\r\n"
	"Content-Type: text/html\r\n"
	"Connection: close\r\n\r\n"
	"<html><body><h2>OTA</h2>"
	"<form method=\"POST\" action=\"/upload.cgi\" "
	"enctype=\"multipart/form-data\">"
	"<input type=\"file\" name=\"file\" accept=\".bin\">"
	"<input type=\"submit\" value=\"Upload\"></form></body></html>";
static const char s_acSuccessPage[] =
	"HTTP/1.0 200 OK\r\n"
	"Content-Type: text/html\r\n"
	"Connection: close\r\n\r\n"
	"<html><body><h2>Upload complete</h2>"
	"<p>MCU Reset Done</p>"
	"<p>Device will reboot shortly.</p></body></html>";
static const char s_acBadRequestPage[] =
	"HTTP/1.0 400 Bad Request\r\n"
	"Content-Type: text/html\r\n"
	"Connection: close\r\n\r\n"
	"<html><body><h2>OTA rejected</h2></body></html>";
static const char s_acServerErrorPage[] =
	"HTTP/1.0 500 Internal Server Error\r\n"
	"Content-Type: text/html\r\n"
	"Connection: close\r\n\r\n"
	"<html><body><h2>OTA failed</h2></body></html>";

static void prvInitListener(void *pvArgument);
static err_t prvAccept(void *pvArgument, struct tcp_pcb *pxPcb,
	err_t xError);
static err_t prvReceive(void *pvArgument, struct tcp_pcb *pxPcb,
	struct pbuf *pxPbuf, err_t xError);
static err_t prvSent(void *pvArgument, struct tcp_pcb *pxPcb,
	u16_t usLength);
static err_t prvPoll(void *pvArgument, struct tcp_pcb *pxPcb);
static void prvConnectionError(void *pvArgument, err_t xError);
static void prvReset(void *pvArgument);
static void prvClose(struct tcp_pcb *pxPcb, uint8_t ucAbort);
static void prvSetResponse(const char *pcResponse);
static void prvSendResponse(struct tcp_pcb *pxPcb);
static uint8_t prvHeaderComplete(void);
static uint8_t prvParseHeader(void);
static uint8_t prvParseDecimal(const char *pcText, uint32_t *pulValue);
static uint8_t prvPrepareMultipart(void);
static uint8_t prvPartHeaderComplete(void);
static uint8_t prvProcessPartHeaderByte(uint8_t ucByte);
static uint8_t prvProcessDataByte(uint8_t ucByte);
static uint8_t prvFlushHoldByte(void);
static uint8_t prvFinishUpload(struct tcp_pcb *pxPcb);
static uint8_t prvTextContains(const char *pcText, const char *pcNeedle);
static void prvLogRequestUrl(void);
static void prvLogReceivedText(const char *pcPrefix, const char *pcData,
	uint16_t usLength);
static void prvLogRejectedRequest(int32_t lCode);
static void prvFailUpload(struct tcp_pcb *pxPcb, const char *pcLogText,
	int32_t lCode);
static void prvWriteLog(AppLogLevel_e xLevel, const char *pcText,
	int32_t lCode);
static void prvWriteLogField(AppLogLevel_e xLevel, const char *pcText,
	int32_t lCode, const char *pcField, int32_t lFieldValue);
static void prvReportLwipFailure(int32_t lNativeError);

/*-----------------------------------------------------------*/
static void prvWriteLog(AppLogLevel_e xLevel, const char *pcText,
	int32_t lCode)
{
	const AppOtaConfig_t *pxConfig;

	pxConfig = pxAppOtaGetConfig();
	if (pxConfig != NULL) {
		(void)xAppLogWrite(xLevel, pxConfig->xLogSource, pcText, lCode);
	}
}

/*-----------------------------------------------------------*/
static void prvWriteLogField(AppLogLevel_e xLevel, const char *pcText,
	int32_t lCode, const char *pcField, int32_t lFieldValue)
{
	const AppOtaConfig_t *pxConfig;

	pxConfig = pxAppOtaGetConfig();
	if (pxConfig != NULL) {
		(void)xAppLogWriteField(xLevel, pxConfig->xLogSource, pcText, lCode,
			pcField, lFieldValue);
	}
}

/*-----------------------------------------------------------*/
static void prvReportLwipFailure(int32_t lNativeError)
{
	const AppOtaConfig_t *pxConfig;

	pxConfig = pxAppOtaGetConfig();
	if ((pxConfig != NULL) && (pxConfig->pxLwipFailureHook != NULL)) {
		pxConfig->pxLwipFailureHook(pxConfig->xLogSource, lNativeError);
	}
}

/*-----------------------------------------------------------*/
BaseType_t xAppOtaHttpInitialize(void)
{
	err_t xError;

	if (pxAppOtaGetConfig() == NULL) {
		return pdFAIL;
	}
	if ((s_pxListener != NULL) || (s_ucInitRequested != 0U)) {
		return pdPASS;
	}
	s_ucInitRequested = 1U;
	xError = tcpip_callback(prvInitListener, NULL);
	if (xError != ERR_OK) {
		s_ucInitRequested = 0U;
		prvWriteLogField(APP_LOG_LEVEL_ERROR, "OTA_HTTP_READY",
			(int32_t)xError, "tcpip", (int32_t)xError);
		prvReportLwipFailure((int32_t)xError);
		return pdFAIL;
	}
	return pdPASS;
}

/*-----------------------------------------------------------*/
uint8_t ucAppOtaHttpIsActive(void)
{
	return (uint8_t)((s_xSession.ucUploadActive != 0U) ||
		(s_xSession.ucResetPending != 0U) ||
		(ucAppOtaFlashIsActive() != 0U));
}

/*-----------------------------------------------------------*/
static void prvInitListener(void *pvArgument)
{
	struct tcp_pcb *pxPcb;
	err_t xError;
	const AppOtaConfig_t *pxConfig;

	(void)pvArgument;
	pxConfig = pxAppOtaGetConfig();
	if (pxConfig == NULL) {
		s_ucInitRequested = 0U;
		return;
	}
	pxPcb = tcp_new();
	if (pxPcb == NULL) {
		s_ucInitRequested = 0U;
		prvWriteLog(APP_LOG_LEVEL_ERROR, "OTA_HTTP_READY", -1);
		prvReportLwipFailure((int32_t)ERR_MEM);
		return;
	}
	xError = tcp_bind(pxPcb, IP_ADDR_ANY, pxConfig->usHttpPort);
	if (xError != ERR_OK) {
		tcp_abort(pxPcb);
		s_ucInitRequested = 0U;
		prvWriteLogField(APP_LOG_LEVEL_ERROR, "OTA_HTTP_READY",
			(int32_t)xError, "bind", (int32_t)xError);
		prvReportLwipFailure((int32_t)xError);
		return;
	}
	pxPcb = tcp_listen(pxPcb);
	if (pxPcb == NULL) {
		s_ucInitRequested = 0U;
		prvWriteLog(APP_LOG_LEVEL_ERROR, "OTA_HTTP_READY", -2);
		prvReportLwipFailure((int32_t)ERR_MEM);
		return;
	}
	tcp_accept(pxPcb, prvAccept);
	s_pxListener = pxPcb;
	prvWriteLogField(APP_LOG_LEVEL_INFO, "OTA_HTTP_READY", 0, "port",
		(int32_t)pxConfig->usHttpPort);
}

/*-----------------------------------------------------------*/
static err_t prvAccept(void *pvArgument, struct tcp_pcb *pxPcb,
	err_t xError)
{
	(void)pvArgument;
	if (xError != ERR_OK) {
		return xError;
	}
	if ((pxPcb == NULL) || (s_xSession.ucInUse != 0U)) {
		if (pxPcb != NULL) {
			tcp_abort(pxPcb);
		}
		prvWriteLog(APP_LOG_LEVEL_WARNING, "OTA_HTTP_REJECTED", -1);
		return ERR_ABRT;
	}
	memset(&s_xSession, 0, sizeof(s_xSession));
	s_xSession.pxPcb = pxPcb;
	s_xSession.ucInUse = 1U;
	s_xSession.xState = APP_OTA_HTTP_HEADER;
	tcp_arg(pxPcb, &s_xSession);
	tcp_recv(pxPcb, prvReceive);
	tcp_sent(pxPcb, prvSent);
	tcp_err(pxPcb, prvConnectionError);
	tcp_poll(pxPcb, prvPoll, 10U);
	return ERR_OK;
}

/*-----------------------------------------------------------*/
static void prvConnectionError(void *pvArgument, err_t xError)
{
	AppOtaHttpSession_t *pxSession;

	pxSession = (AppOtaHttpSession_t *)pvArgument;
	if ((pxSession != NULL) && (pxSession->ucUploadActive != 0U)) {
		vAppOtaFlashAbort();
		prvWriteLogField(APP_LOG_LEVEL_ERROR, "OTA_HTTP_ABORT",
			(int32_t)xError, "tcp", (int32_t)xError);
	}
	memset(&s_xSession, 0, sizeof(s_xSession));
}

/*-----------------------------------------------------------*/
static void prvClose(struct tcp_pcb *pxPcb, uint8_t ucAbort)
{
	err_t xError;

	if (pxPcb == NULL) {
		return;
	}
	tcp_arg(pxPcb, NULL);
	tcp_recv(pxPcb, NULL);
	tcp_sent(pxPcb, NULL);
	tcp_poll(pxPcb, NULL, 0U);
	tcp_err(pxPcb, NULL);
	if (ucAbort != 0U) {
		tcp_abort(pxPcb);
	} else {
		xError = tcp_close(pxPcb);
		if (xError != ERR_OK) {
			tcp_abort(pxPcb);
		}
	}
	memset(&s_xSession, 0, sizeof(s_xSession));
}

/*-----------------------------------------------------------*/
static void prvSetResponse(const char *pcResponse)
{
	s_xSession.pcResponse = pcResponse;
	s_xSession.usResponseLength = (uint16_t)strlen(pcResponse);
	s_xSession.usResponseOffset = 0U;
	s_xSession.xState = APP_OTA_HTTP_RESPONSE;
}

/*-----------------------------------------------------------*/
static void prvSendResponse(struct tcp_pcb *pxPcb)
{
	u16_t usAvailable;
	u16_t usLength;
	err_t xError;

	if ((pxPcb == NULL) || (s_xSession.pcResponse == NULL) ||
		(s_xSession.usResponseOffset >= s_xSession.usResponseLength)) {
		return;
	}
	usAvailable = tcp_sndbuf(pxPcb);
	if (usAvailable == 0U) {
		return;
	}
	usLength = (u16_t)(s_xSession.usResponseLength -
		s_xSession.usResponseOffset);
	if (usLength > usAvailable) {
		usLength = usAvailable;
	}
	xError = tcp_write(pxPcb,
		s_xSession.pcResponse + s_xSession.usResponseOffset, usLength,
		TCP_WRITE_FLAG_COPY);
	if (xError != ERR_OK) {
		prvWriteLogField((s_xSession.ucResetPending != 0U) ?
			APP_LOG_LEVEL_WARNING : APP_LOG_LEVEL_ERROR,
			(s_xSession.ucResetPending != 0U) ?
			"OTA_HTTP_RESPONSE_FAILED" : "OTA_HTTP_ABORT", (int32_t)xError,
			"write", (int32_t)xError);
		prvClose(pxPcb, 1U);
		return;
	}
	s_xSession.usResponseOffset += usLength;
	xError = tcp_output(pxPcb);
	if (xError != ERR_OK) {
		prvWriteLogField((s_xSession.ucResetPending != 0U) ?
			APP_LOG_LEVEL_WARNING : APP_LOG_LEVEL_ERROR,
			(s_xSession.ucResetPending != 0U) ?
			"OTA_HTTP_RESPONSE_FAILED" : "OTA_HTTP_ABORT", (int32_t)xError,
			"output", (int32_t)xError);
		prvClose(pxPcb, 1U);
	}
}

/*-----------------------------------------------------------*/
static err_t prvSent(void *pvArgument, struct tcp_pcb *pxPcb,
	u16_t usLength)
{
	(void)pvArgument;
	(void)usLength;
	if (s_xSession.xState != APP_OTA_HTTP_RESPONSE) {
		return ERR_OK;
	}
	if (s_xSession.usResponseOffset < s_xSession.usResponseLength) {
		prvSendResponse(pxPcb);
		return ERR_OK;
	}
	prvClose(pxPcb, 0U);
	return ERR_OK;
}

/*-----------------------------------------------------------*/
static err_t prvPoll(void *pvArgument, struct tcp_pcb *pxPcb)
{
	(void)pvArgument;
	if (s_xSession.xState == APP_OTA_HTTP_RESPONSE) {
		prvSendResponse(pxPcb);
	}
	return ERR_OK;
}

/*-----------------------------------------------------------*/
static void prvReset(void *pvArgument)
{
	(void)pvArgument;
	NVIC_SystemReset();
}

/*-----------------------------------------------------------*/
static uint8_t prvHeaderComplete(void)
{
	if (s_xSession.usHeaderLength < 4U) {
		return 0U;
	}
	return (uint8_t)(s_xSession.acHeader[s_xSession.usHeaderLength - 4U] ==
		'\r' && s_xSession.acHeader[s_xSession.usHeaderLength - 3U] == '\n' &&
		s_xSession.acHeader[s_xSession.usHeaderLength - 2U] == '\r' &&
		s_xSession.acHeader[s_xSession.usHeaderLength - 1U] == '\n');
}

/*-----------------------------------------------------------*/
static uint8_t prvParseDecimal(const char *pcText, uint32_t *pulValue)
{
	uint32_t ulValue;
	uint8_t ucDigits;

	if ((pcText == NULL) || (pulValue == NULL)) {
		return 0U;
	}
	while ((*pcText == ' ') || (*pcText == '\t')) {
		pcText++;
	}
	ulValue = 0U;
	ucDigits = 0U;
	while ((*pcText >= '0') && (*pcText <= '9')) {
		if (ulValue > (0xFFFFFFFFUL - (uint32_t)(*pcText - '0')) / 10UL) {
			return 0U;
		}
		ulValue = (ulValue * 10UL) + (uint32_t)(*pcText - '0');
		pcText++;
		ucDigits = 1U;
	}
	if (ucDigits == 0U) {
		return 0U;
	}
	*pulValue = ulValue;
	return 1U;
}

/*-----------------------------------------------------------*/
static uint8_t prvTextContains(const char *pcText, const char *pcNeedle)
{
	uint16_t usText;
	uint16_t usNeedle;
	uint16_t usIndex;
	uint16_t usInner;
	char cLeft;
	char cRight;

	if ((pcText == NULL) || (pcNeedle == NULL)) {
		return 0U;
	}
	usText = (uint16_t)strlen(pcText);
	usNeedle = (uint16_t)strlen(pcNeedle);
	if ((usNeedle == 0U) || (usNeedle > usText)) {
		return 0U;
	}
	for (usIndex = 0U; usIndex <= (uint16_t)(usText - usNeedle);
		usIndex++) {
		for (usInner = 0U; usInner < usNeedle; usInner++) {
			cLeft = pcText[usIndex + usInner];
			cRight = pcNeedle[usInner];
			if ((cLeft >= 'A') && (cLeft <= 'Z')) {
				cLeft = (char)(cLeft + ('a' - 'A'));
			}
			if ((cRight >= 'A') && (cRight <= 'Z')) {
				cRight = (char)(cRight + ('a' - 'A'));
			}
			if (cLeft != cRight) {
				break;
			}
		}
		if (usInner == usNeedle) {
			return 1U;
		}
	}
	return 0U;
}

/*-----------------------------------------------------------*/
static void prvLogRequestUrl(void)
{
	char acIp[16];
	char acUrl[64];
	const char *pcPrefix;
	const char *pcSuffix;
	const ip4_addr_t *pxAddress;
	uint16_t usLength;
	uint16_t usPartLength;
	uint8_t ucAddressReady;

	memset(acIp, 0, sizeof(acIp));
	memset(acUrl, 0, sizeof(acUrl));
	ucAddressReady = 0U;
	pxAddress = NULL;
	if ((netif_default != NULL) && (netif_is_up(netif_default) != 0) &&
		(netif_is_link_up(netif_default) != 0) &&
		(ip_addr_isany(netif_ip_addr4(netif_default)) == 0)) {
		pxAddress = netif_ip4_addr(netif_default);
		if (ip4addr_ntoa_r(pxAddress, acIp, (int)sizeof(acIp)) != NULL) {
			ucAddressReady = 1U;
		}
	}
	if (ucAddressReady == 0U) {
		memcpy(acIp, "0.0.0.0", sizeof("0.0.0.0"));
	}
	pcPrefix = "http://";
	pcSuffix = "/upload.cgi";
	usLength = 0U;
	usPartLength = (uint16_t)strlen(pcPrefix);
	memcpy(acUrl + usLength, pcPrefix, usPartLength);
	usLength += usPartLength;
	usPartLength = (uint16_t)strlen(acIp);
	memcpy(acUrl + usLength, acIp, usPartLength);
	usLength += usPartLength;
	usPartLength = (uint16_t)strlen(pcSuffix);
	memcpy(acUrl + usLength, pcSuffix, usPartLength);
	usLength += usPartLength;
	acUrl[usLength] = '\0';
	prvWriteLog(APP_LOG_LEVEL_ERROR, acUrl, 0);
}

/*-----------------------------------------------------------*/
static void prvLogReceivedText(const char *pcPrefix, const char *pcData,
	uint16_t usLength)
{
	char acLine[APP_OTA_HTTP_LOG_TEXT_CAP];
	uint16_t usOffset;
	uint16_t usOutput;
	uint16_t usPrefixLength;
	uint16_t usLimit;
	uint8_t ucByte;

	if ((pcPrefix == NULL) || (pcData == NULL)) {
		return;
	}
	usLimit = usLength;
	if (usLimit > APP_OTA_HTTP_DIAGNOSTIC_CAP) {
		usLimit = APP_OTA_HTTP_DIAGNOSTIC_CAP;
	}
	usPrefixLength = (uint16_t)strlen(pcPrefix);
	if (usPrefixLength >= (APP_OTA_HTTP_LOG_TEXT_CAP - 1U)) {
		return;
	}
	usOffset = 0U;
	do {
		memset(acLine, 0, sizeof(acLine));
		memcpy(acLine, pcPrefix, usPrefixLength);
		usOutput = usPrefixLength;
		while ((usOffset < usLimit) &&
			(usOutput < (APP_OTA_HTTP_LOG_TEXT_CAP - 1U))) {
			ucByte = (uint8_t)pcData[usOffset];
			usOffset++;
			if ((ucByte == '\r') || (ucByte == '\n')) {
				ucByte = '|';
			} else if ((ucByte < 0x20U) || (ucByte > 0x7EU)) {
				ucByte = '.';
			}
			acLine[usOutput] = (char)ucByte;
			usOutput++;
		}
		acLine[usOutput] = '\0';
		prvWriteLog(APP_LOG_LEVEL_ERROR, acLine, 0);
	} while (usOffset < usLimit);
}

/*-----------------------------------------------------------*/
static void prvLogRejectedRequest(int32_t lCode)
{
	if (lCode == -5) {
		if ((s_xSession.usHeaderLength >= 4U) &&
			(s_xSession.acHeader[s_xSession.usHeaderLength - 4U] == '\0')) {
			s_xSession.acHeader[s_xSession.usHeaderLength - 4U] = '\r';
		}
		prvLogRequestUrl();
		prvLogReceivedText("OTA_HTTP_RX_HEADER>", s_xSession.acHeader,
			s_xSession.usHeaderLength);
	} else if (lCode == -6) {
		if ((s_xSession.usPartHeaderLength >= 4U) &&
			(s_xSession.acPartHeader[
				s_xSession.usPartHeaderLength - 4U] == '\0')) {
			s_xSession.acPartHeader[s_xSession.usPartHeaderLength - 4U] = '\r';
		}
		prvLogRequestUrl();
		prvLogReceivedText("OTA_HTTP_RX_PART>", s_xSession.acPartHeader,
			s_xSession.usPartHeaderLength);
	}
}

/*-----------------------------------------------------------*/
static uint8_t prvParseHeader(void)
{
	char *pcBoundary;
	char *pcEnd;
	uint32_t ulLength;
	uint16_t usLength;
	const AppOtaConfig_t *pxConfig;

	pxConfig = pxAppOtaGetConfig();
	if (pxConfig == NULL) {
		return 0U;
	}
	s_xSession.acHeader[s_xSession.usHeaderLength - 4U] = '\0';
	if (strncmp(s_xSession.acHeader, "GET /", 5U) == 0) {
		s_xSession.ucPostRequest = 0U;
		return 1U;
	}
	if (strncmp(s_xSession.acHeader, "POST /upload.cgi", 16U) != 0) {
		return 0U;
	}
	pcBoundary = strstr(s_xSession.acHeader, "boundary=");
	if (pcBoundary == NULL) {
		return 0U;
	}
	pcBoundary += 9;
	if (*pcBoundary == '"') {
		pcBoundary++;
	}
	pcEnd = pcBoundary;
	while ((*pcEnd != '\0') && (*pcEnd != ';') && (*pcEnd != '\r') &&
		(*pcEnd != '\n') && (*pcEnd != '"') &&
		((uint16_t)(pcEnd - pcBoundary) < APP_OTA_HTTP_BOUNDARY_CAP)) {
		pcEnd++;
	}
	usLength = (uint16_t)(pcEnd - pcBoundary);
	if ((usLength == 0U) || (usLength > APP_OTA_HTTP_BOUNDARY_CAP)) {
		return 0U;
	}
	memcpy(s_xSession.acBoundary, pcBoundary, usLength);
	s_xSession.acBoundary[usLength] = '\0';
	s_xSession.usBoundaryLength = usLength;
	memcpy(s_xSession.aucMarker, "\r\n--", 4U);
	memcpy(s_xSession.aucMarker + 4U, s_xSession.acBoundary, usLength);
	memcpy(s_xSession.aucMarker + 4U + usLength, "--", 2U);
	s_xSession.usMarkerLength = (uint16_t)(usLength + 6U);
	if (s_xSession.usMarkerLength > APP_OTA_HTTP_MARKER_CAP) {
		return 0U;
	}
	pcBoundary = strstr(s_xSession.acHeader, "Content-Length:");
	if (pcBoundary == NULL) {
		return 0U;
	}
	pcBoundary += 15;
	if (prvParseDecimal(pcBoundary, &ulLength) == 0U) {
		return 0U;
	}
	if (ulLength > ((pxConfig->ulApplicationEnd -
		pxConfig->ulApplicationAddress) + APP_OTA_HTTP_BODY_OVERHEAD)) {
		return 0U;
	}
	s_xSession.ulContentLength = ulLength;
	s_xSession.ucPostRequest = 1U;
	return 1U;
}

/*-----------------------------------------------------------*/
static uint8_t prvPartHeaderComplete(void)
{
	if (s_xSession.usPartHeaderLength < 4U) {
		return 0U;
	}
	return (uint8_t)(s_xSession.acPartHeader[
		s_xSession.usPartHeaderLength - 4U] == '\r' &&
		s_xSession.acPartHeader[s_xSession.usPartHeaderLength - 3U] == '\n' &&
		s_xSession.acPartHeader[s_xSession.usPartHeaderLength - 2U] == '\r' &&
		s_xSession.acPartHeader[s_xSession.usPartHeaderLength - 1U] == '\n');
}

/*-----------------------------------------------------------*/
static uint8_t prvPrepareMultipart(void)
{
	s_xSession.usPartHeaderLength = 0U;
	s_xSession.usHoldLength = 0U;
	s_xSession.ulWrittenBytes = 0U;
	s_xSession.ulNextProgress = 4096UL;
	s_xSession.xState = APP_OTA_HTTP_PART_HEADER;
	return 1U;
}

/*-----------------------------------------------------------*/
static uint8_t prvProcessPartHeaderByte(uint8_t ucByte)
{
	AppOtaResult_e xResult;

	if (s_xSession.usPartHeaderLength >=
		(APP_OTA_HTTP_PART_HEADER_CAP - 1U)) {
		return 0U;
	}
	s_xSession.acPartHeader[s_xSession.usPartHeaderLength] = (char)ucByte;
	s_xSession.usPartHeaderLength++;
	if (prvPartHeaderComplete() == 0U) {
		return 1U;
	}
	s_xSession.acPartHeader[s_xSession.usPartHeaderLength - 4U] = '\0';
	if ((prvTextContains(s_xSession.acPartHeader, "filename=") == 0U) ||
		(prvTextContains(s_xSession.acPartHeader, ".bin") == 0U)) {
		return 0U;
	}
	xResult = xAppOtaFlashBegin();
	if (xResult != APP_OTA_RESULT_OK) {
		return 0U;
	}
	s_xSession.ucUploadActive = 1U;
	s_xSession.xState = APP_OTA_HTTP_DATA;
	prvWriteLog(APP_LOG_LEVEL_INFO, "OTA_HTTP_PROGRESS", 0);
	return 1U;
}

/*-----------------------------------------------------------*/
static uint8_t prvFlushHoldByte(void)
{
	AppOtaResult_e xResult;

	xResult = xAppOtaFlashWrite(s_xSession.aucHold, 1U);
	if (xResult != APP_OTA_RESULT_OK) {
		return 0U;
	}
	s_xSession.ulWrittenBytes++;
	if (s_xSession.ulWrittenBytes >= s_xSession.ulNextProgress) {
		prvWriteLogField(APP_LOG_LEVEL_INFO, "OTA_HTTP_PROGRESS", 0,
			"bytes", (int32_t)s_xSession.ulWrittenBytes);
		s_xSession.ulNextProgress += 4096UL;
	}
	return 1U;
}

/*-----------------------------------------------------------*/
static uint8_t prvFinishUpload(struct tcp_pcb *pxPcb)
{
	AppOtaResult_e xResult;
	uint32_t ulCrc;
	uint32_t ulSize;
	const AppOtaConfig_t *pxConfig;

	xResult = xAppOtaFlashFinish(&ulCrc, &ulSize);
	if (xResult != APP_OTA_RESULT_OK) {
		prvFailUpload(pxPcb, "OTA_HTTP_CRC_FAILED", (int32_t)xResult);
		return 0U;
	}
	(void)ulCrc;
	(void)ulSize;
	pxConfig = pxAppOtaGetConfig();
	s_xSession.ucUploadActive = 0U;
	s_xSession.ucResetPending = 1U;
	s_xSession.xState = APP_OTA_HTTP_COMPLETE;
	prvWriteLog(APP_LOG_LEVEL_INFO, "OTA_HTTP_RESET", 0);
	sys_timeout(pxConfig->usResetDelayMs, prvReset, NULL);
	prvSetResponse(s_acSuccessPage);
	prvSendResponse(pxPcb);
	return 1U;
}

/*-----------------------------------------------------------*/
static uint8_t prvProcessDataByte(uint8_t ucByte)
{
	if (s_xSession.usHoldLength >= APP_OTA_HTTP_MARKER_CAP) {
		return 0U;
	}
	s_xSession.aucHold[s_xSession.usHoldLength] = ucByte;
	s_xSession.usHoldLength++;
	while (s_xSession.usHoldLength >= s_xSession.usMarkerLength) {
		if (memcmp(s_xSession.aucHold, s_xSession.aucMarker,
			s_xSession.usMarkerLength) == 0) {
			s_xSession.usHoldLength = 0U;
			return 2U;
		}
		if (prvFlushHoldByte() == 0U) {
			return 0U;
		}
		memmove(s_xSession.aucHold, s_xSession.aucHold + 1U,
			s_xSession.usHoldLength - 1U);
		s_xSession.usHoldLength--;
	}
	return 1U;
}

/*-----------------------------------------------------------*/
static void prvFailUpload(struct tcp_pcb *pxPcb, const char *pcLogText,
	int32_t lCode)
{
	if (s_xSession.ucUploadActive != 0U) {
		vAppOtaFlashAbort();
	}
	s_xSession.ucUploadActive = 0U;
	prvLogRejectedRequest(lCode);
	prvWriteLogField(APP_LOG_LEVEL_ERROR, pcLogText, lCode, "bytes",
		(int32_t)s_xSession.ulWrittenBytes);
	prvSetResponse((lCode == (int32_t)APP_OTA_RESULT_REJECTED) ?
		s_acBadRequestPage : s_acServerErrorPage);
	prvSendResponse(pxPcb);
}

/*-----------------------------------------------------------*/
static err_t prvReceive(void *pvArgument, struct tcp_pcb *pxPcb,
	struct pbuf *pxPbuf, err_t xError)
{
	struct pbuf *pxPart;
	uint16_t usIndex;
	uint16_t usPartLength;
	uint16_t usOffset;
	uint8_t *pucPayload;
	uint8_t ucResult;
	uint8_t ucByte;

	(void)pvArgument;
	if (xError != ERR_OK) {
		if (s_xSession.ucUploadActive != 0U) {
			vAppOtaFlashAbort();
		}
		return xError;
	}
	if (pxPbuf == NULL) {
		if (s_xSession.ucUploadActive != 0U) {
			vAppOtaFlashAbort();
		}
		prvClose(pxPcb, 0U);
		return ERR_OK;
	}
	for (pxPart = pxPbuf; pxPart != NULL; pxPart = pxPart->next) {
		pucPayload = (uint8_t *)pxPart->payload;
		usPartLength = pxPart->len;
		for (usIndex = 0U; usIndex < usPartLength; usIndex++) {
			ucByte = pucPayload[usIndex];
			if (s_xSession.xState == APP_OTA_HTTP_HEADER) {
				if (s_xSession.usHeaderLength >=
					(APP_OTA_HTTP_HEADER_CAP - 1U)) {
					prvFailUpload(pxPcb, "OTA_HTTP_REJECTED", -4);
					break;
				}
				s_xSession.acHeader[s_xSession.usHeaderLength] = (char)ucByte;
				s_xSession.usHeaderLength++;
				if (prvHeaderComplete() == 0U) {
					continue;
				}
				s_xSession.ucHeaderDone = 1U;
				if (prvParseHeader() == 0U) {
					prvFailUpload(pxPcb, "OTA_HTTP_REJECTED", -5);
					break;
				}
				if (s_xSession.ucPostRequest == 0U) {
					prvSetResponse(s_acUploadPage);
					prvSendResponse(pxPcb);
					break;
				}
				(void)prvPrepareMultipart();
				continue;
			}
			if (s_xSession.xState == APP_OTA_HTTP_PART_HEADER) {
				if (prvProcessPartHeaderByte(ucByte) == 0U) {
					prvFailUpload(pxPcb, "OTA_HTTP_REJECTED", -6);
					break;
				}
				continue;
			}
			if (s_xSession.xState == APP_OTA_HTTP_DATA) {
				ucResult = prvProcessDataByte(ucByte);
				if (ucResult == 0U) {
					prvFailUpload(pxPcb, "OTA_HTTP_FLASH_FAILED", -7);
					break;
				}
				if ((ucResult == 2U) && (prvFinishUpload(pxPcb) == 0U)) {
					break;
				}
				continue;
			}
		}
	}
	usOffset = pxPbuf->tot_len;
	tcp_recved(pxPcb, usOffset);
	pbuf_free(pxPbuf);
	return ERR_OK;
}
