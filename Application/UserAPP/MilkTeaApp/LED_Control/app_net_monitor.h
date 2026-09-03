/**
  * @file      app_net_monitor.h
  * @brief     Define Ethernet readiness monitoring and LED control.
  * @author    WHong
  * @date      2026-07-28
  */

#ifndef APP_NET_MONITOR_H
#define APP_NET_MONITOR_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/** @brief A default LwIP netif exists. */
#define APP_NET_STATUS_DEFAULT_NETIF       0x01L
/** @brief The LwIP software interface is up. */
#define APP_NET_STATUS_INTERFACE_UP        0x02L
/** @brief The Ethernet PHY reports an active link. */
#define APP_NET_STATUS_LINK_UP             0x04L
/** @brief A nonzero IPv4 address is configured. */
#define APP_NET_STATUS_IPV4_CONFIGURED     0x08L
/** @brief Mask representing all required network-ready conditions. */
#define APP_NET_STATUS_READY               0x0FL

/**
  * @brief Update Ethernet status, LED state, recovery, and transition logs.
  * @note Call periodically from one task after LwIP initialization.
  */
void AppNetMonitor_Process(void);

/**
  * @brief Read the current network status bit mask.
  * @return Bitwise OR of the APP_NET_STATUS_* flags.
  */
int32_t lAppNetMonitorGetStatusFlags(void);

/**
  * @brief Test whether every network-ready condition is satisfied.
  * @retval 1 The interface, link, and IPv4 configuration are ready.
  * @retval 0 At least one network condition is unavailable.
  */
uint8_t ucAppNetMonitorIsReady(void);

#ifdef __cplusplus
}
#endif

#endif /* APP_NET_MONITOR_H */
