/**
  * @file      coffee2_build_config.h
  * @brief     Select Coffee2 target-specific nanoMODBUS features.
  * @author    WHong
  * @date      2026-07-30
  *
  * @details   The Coffee2 Keil target pre-includes this header in every
  *            translation unit. The vendor configuration include guard is
  *            intentionally defined here so client and server structure
  *            layouts remain identical without editing vendored middleware.
  */

#ifndef COFFEE2_BUILD_CONFIG_H
#define COFFEE2_BUILD_CONFIG_H

#ifndef NANOMODBUS_CONFIG_H
#define NANOMODBUS_CONFIG_H

#define NANOMODBUS_CFG_CLIENT_ENABLED       1
#define NANOMODBUS_CFG_SERVER_ENABLED       1

#endif /* NANOMODBUS_CONFIG_H */

#endif /* COFFEE2_BUILD_CONFIG_H */
