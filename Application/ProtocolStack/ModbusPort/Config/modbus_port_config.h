/**
  * @file      modbus_port_config.h
  * @brief     Configure the project nanoMODBUS integration boundary.
  * @author    WHong
  * @date      2026-07-28
  */

#ifndef MODBUS_PORT_CONFIG_H
#define MODBUS_PORT_CONFIG_H

#include "nanomodbus_config.h"

/** @brief Maximum number of TX or RX frame bytes retained for diagnostics. */
#define MODBUS_PORT_TRACE_LENGTH             260U
/** @brief Maximum accepted public transaction timeout in milliseconds. */
#define MODBUS_PORT_TIMEOUT_MAX_MS         60000U

#if (NANOMODBUS_CFG_CLIENT_ENABLED != 1)
#error "The current product requires nanoMODBUS client support"
#endif

#endif /* MODBUS_PORT_CONFIG_H */
