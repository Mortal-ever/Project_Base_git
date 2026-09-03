/**
  * @file      app_net_monitor.c
  * @brief     Monitor Ethernet readiness, recovery, and status indication.
  * @author    WHong
  * @date      2026-07-28
  */

#include "app_net_monitor.h"

#include "app_comm_log.h"
#include "gpio.h"
#include "lwip.h"
#include "lwip/ip4_addr.h"

/** @brief Define the status LED's network lifecycle states. */
typedef enum {
	NET_LED_STATE_OFFLINE = 0, /*!< Link is not ready. */
	NET_LED_STATE_RECONNECTING = 1, /*!< PHY reset window is active. */
	NET_LED_STATE_ONLINE = 2 /*!< Link and IPv4 are ready. */
} NetLedState_t;

/** @brief GPIO port driving the active-low network status LED. */
#define NET_STATUS_LED_GPIO_Port        PA4_LED_TF_GPIO_Port
/** @brief GPIO pin driving the network status LED. */
#define NET_STATUS_LED_Pin              PA4_LED_TF_Pin
/** @brief GPIO level that turns the status LED on. */
#define NET_STATUS_LED_ON_LEVEL         GPIO_PIN_RESET
/** @brief GPIO level that turns the status LED off. */
#define NET_STATUS_LED_OFF_LEVEL        GPIO_PIN_SET

/** @brief Offline time before PHY recovery begins, in milliseconds. */
#define NET_RECONNECT_DELAY_MS          5000U
/** @brief LED toggle period during recovery, in milliseconds. */
#define NET_RECONNECT_BLINK_MS          1000U
/** @brief PHY recovery observation window in milliseconds. */
#define NET_RECONNECT_WINDOW_MS         5000U

/** @brief CubeMX-owned default Ethernet interface. */
extern struct netif gnetif;

/** @brief Test interface, PHY link, and IPv4 communication readiness. */
static uint8_t prvIsCommunicationReady(void)
{
	if (!netif_is_up(&gnetif) || !netif_is_link_up(&gnetif)) {
		return 0U;
	}

#if LWIP_IPV4
	if (ip4_addr_get_u32(netif_ip4_addr(&gnetif)) == 0U) {
		return 0U;
	}
#endif

	return 1U;
}

/* Public readiness query used by the network-management task. */
/*-----------------------------------------------------------*/
uint8_t ucAppNetMonitorIsReady(void)
{
	return prvIsCommunicationReady();
}

/*
 * Bit 0: default netif exists; bit 1: interface up;
 * bit 2: PHY link up; bit 3: IPv4 address configured.
 */
/*-----------------------------------------------------------*/
int32_t lAppNetMonitorGetStatusFlags(void)
{
	int32_t lFlags;

	lFlags = 0;
	if (netif_default != NULL) {
		lFlags |= APP_NET_STATUS_DEFAULT_NETIF;
	}
	if (netif_is_up(&gnetif)) {
		lFlags |= APP_NET_STATUS_INTERFACE_UP;
	}
	if (netif_is_link_up(&gnetif)) {
		lFlags |= APP_NET_STATUS_LINK_UP;
	}
#if LWIP_IPV4
	if (ip4_addr_get_u32(netif_ip4_addr(&gnetif)) != 0U) {
		lFlags |= APP_NET_STATUS_IPV4_CONFIGURED;
	}
#endif
	return lFlags;
}

/*-----------------------------------------------------------*/
/** @brief Drive the active-low network status LED. */
static void prvSetLed(uint8_t ucOn)
{
	HAL_GPIO_WritePin(NET_STATUS_LED_GPIO_Port, NET_STATUS_LED_Pin,
		(ucOn != 0U) ? NET_STATUS_LED_ON_LEVEL : NET_STATUS_LED_OFF_LEVEL);
}

/*-----------------------------------------------------------*/
/** @brief Reset the Ethernet PHY through the board support hook. */
static void prvRecoverEthernet(void)
{
	dp83848_hw_reset();
}

/*
 * Called every 100 ms by CubeMX default task after LwIP initialization.
 * State transitions drive the LED and emit logs; PHY reset is rate limited.
 */
/*-----------------------------------------------------------*/
void AppNetMonitor_Process(void)
{
	static NetLedState_t s_xState = NET_LED_STATE_OFFLINE;
	static uint32_t s_ulStateTick;
	static uint32_t s_ulBlinkTick;
	static uint8_t s_ucBlinkOn;
	uint32_t ulNow;
	uint8_t ucReady;

	ulNow = HAL_GetTick();
	ucReady = prvIsCommunicationReady();
	switch (s_xState) {
	case NET_LED_STATE_ONLINE:
		prvSetLed(1U);
		if (ucReady == 0U) {
			s_xState = NET_LED_STATE_OFFLINE;
			s_ulStateTick = ulNow;
			prvSetLed(0U);
			vAppCommLogWrite(APP_COMM_SOURCE_NETWORK, -1,
				lAppNetMonitorGetStatusFlags());
		}
		break;

	case NET_LED_STATE_OFFLINE:
		prvSetLed(0U);
		if (ucReady != 0U) {
			s_xState = NET_LED_STATE_ONLINE;
			s_ulStateTick = ulNow;
			prvSetLed(1U);
			vAppCommLogWrite(APP_COMM_SOURCE_NETWORK, 2,
				lAppNetMonitorGetStatusFlags());
		} else if ((ulNow - s_ulStateTick) >= NET_RECONNECT_DELAY_MS) {
			s_xState = NET_LED_STATE_RECONNECTING;
			s_ulStateTick = ulNow;
			s_ulBlinkTick = ulNow;
			s_ucBlinkOn = 1U;
			prvSetLed(s_ucBlinkOn);
			vAppCommLogWrite(APP_COMM_SOURCE_NETWORK, -2,
				lAppNetMonitorGetStatusFlags());
			prvRecoverEthernet();
		}
		break;

	case NET_LED_STATE_RECONNECTING:
		if (ucReady != 0U) {
			s_xState = NET_LED_STATE_ONLINE;
			s_ulStateTick = ulNow;
			prvSetLed(1U);
			vAppCommLogWrite(APP_COMM_SOURCE_NETWORK, 2,
				lAppNetMonitorGetStatusFlags());
		} else {
			if ((ulNow - s_ulBlinkTick) >= NET_RECONNECT_BLINK_MS) {
				s_ulBlinkTick = ulNow;
				s_ucBlinkOn = (s_ucBlinkOn == 0U) ? 1U : 0U;
				prvSetLed(s_ucBlinkOn);
			}
			if ((ulNow - s_ulStateTick) >= NET_RECONNECT_WINDOW_MS) {
				s_xState = NET_LED_STATE_OFFLINE;
				s_ulStateTick = ulNow;
				s_ucBlinkOn = 0U;
				prvSetLed(0U);
			}
		}
		break;

	default:
		s_xState = NET_LED_STATE_OFFLINE;
		s_ulStateTick = ulNow;
		prvSetLed(0U);
		break;
	}
}
