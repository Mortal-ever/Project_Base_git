/**
  * @file      app_modbus_rtu_config.h
  * @brief     Configure product Modbus RTU bus task instances.
  * @author    WHong
  * @date      2026-07-28
  */

#ifndef APP_MODBUS_RTU_CONFIG_H
#define APP_MODBUS_RTU_CONFIG_H

/*
 * Keep a bus disabled until its RS485 direction control and device protocol
 * are confirmed. Enabling a bus does not invent missing register addresses.
 */
/** @brief Enable RTU Bus 1 on UART4 when set to one. */
#define APP_MODBUS_RTU_BUS1_ENABLE          0U
/** @brief Enable RTU Bus 2 on UART5 when set to one. */
#define APP_MODBUS_RTU_BUS2_ENABLE          0U
/** @brief Enable RTU Bus 3 on USART2 when set to one. */
#define APP_MODBUS_RTU_BUS3_ENABLE          0U
/** @brief Enable RTU Bus 4 on USART3 when set to one. */
#define APP_MODBUS_RTU_BUS4_ENABLE          0U

/** @brief Total RTU transaction timeout in milliseconds. */
#define APP_MODBUS_RTU_TIMEOUT_MS         500U
/** @brief Enabled bus task loop period in milliseconds. */
#define APP_MODBUS_RTU_LOOP_MS             20U
/** @brief Disabled bus task sleep period in milliseconds. */
#define APP_MODBUS_RTU_DISABLED_MS       1000U

#endif /* APP_MODBUS_RTU_CONFIG_H */
