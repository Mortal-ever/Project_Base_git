/**
  * @file      app_modbus_tcp_config.h
  * @brief     Configure Robot and MilkTea Modbus TCP endpoints.
  * @author    WHong
  * @date      2026-07-28
  */

#ifndef APP_MODBUS_TCP_CONFIG_H
#define APP_MODBUS_TCP_CONFIG_H

/** @brief Enable the Robot TCP endpoint when set to one. */
#define APP_ROBOT_TCP_ENABLE                 1U
/** @brief First octet of the Robot server IPv4 address. */
#define APP_ROBOT_TCP_IP_0                 192U
/** @brief Second octet of the Robot server IPv4 address. */
#define APP_ROBOT_TCP_IP_1                 168U
/** @brief Third octet of the Robot server IPv4 address. */
#define APP_ROBOT_TCP_IP_2                   5U
/** @brief Fourth octet of the Robot server IPv4 address. */
#define APP_ROBOT_TCP_IP_3                 100U
/** @brief Robot server TCP port in host byte order. */
#define APP_ROBOT_TCP_PORT                 502U
/** @brief Modbus Unit ID used for Robot requests. */
#define APP_ROBOT_TCP_UNIT_ID                1U
/** @brief Enable periodic Robot holding-register test reads. */
#define APP_ROBOT_TEST_ENABLE                1U
/** @brief Period between Robot test reads in milliseconds. */
#define APP_ROBOT_TEST_MS                  5000U

/** @brief Enable the MilkTea TCP endpoint when set to one. */
#define APP_MILKTEA_TCP_ENABLE               1U
/** @brief First octet of the MilkTea server IPv4 address. */
#define APP_MILKTEA_TCP_IP_0               192U
/** @brief Second octet of the MilkTea server IPv4 address. */
#define APP_MILKTEA_TCP_IP_1               168U
/** @brief Third octet of the MilkTea server IPv4 address. */
#define APP_MILKTEA_TCP_IP_2               	 5U
/** @brief Fourth octet of the MilkTea server IPv4 address. */
#define APP_MILKTEA_TCP_IP_3               100U
/** @brief MilkTea server TCP port in host byte order. */
#define APP_MILKTEA_TCP_PORT              1502U
/** @brief Modbus Unit ID used for MilkTea requests. */
#define APP_MILKTEA_TCP_UNIT_ID              1U
/** @brief Period between MilkTea health reads in milliseconds. */
#define APP_MILKTEA_HEALTH_MS              5000U

/** @brief Total TCP and Modbus transaction timeout in milliseconds. */
#define APP_MODBUS_TCP_IO_TIMEOUT_MS       1000U
/** @brief TCP endpoint state-machine loop period in milliseconds. */
#define APP_MODBUS_TCP_LOOP_MS               20U

#if ((APP_ROBOT_TEST_ENABLE != 0U) && \
	(APP_ROBOT_TEST_ENABLE != 1U))
#error "APP_ROBOT_TEST_ENABLE must be zero or one"
#endif

#if ((APP_MILKTEA_TCP_ENABLE != 0U) && \
	(APP_MILKTEA_TCP_IP_0 == 0U) && \
	(APP_MILKTEA_TCP_IP_1 == 0U) && \
	(APP_MILKTEA_TCP_IP_2 == 0U) && \
	(APP_MILKTEA_TCP_IP_3 == 0U))
#error "Set the MilkTea Server IP before enabling its TCP endpoint"
#endif

#endif /* APP_MODBUS_TCP_CONFIG_H */
