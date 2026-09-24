/**
  * @file      coffee3_build_config.h
  * @brief     选择 Coffee3 目标使用的 nanoMODBUS 功能。
  * @author    WHong
  * @date      2026-09-24
  *
  * @details   Coffee3 Keil 目标会在每个编译单元中预包含本文件。这里主动定义
  *            厂商配置保护宏，使客户端与服务端结构布局一致且无需修改中间件。
  */

#ifndef COFFEE3_BUILD_CONFIG_H
#define COFFEE3_BUILD_CONFIG_H

#ifndef NANOMODBUS_CONFIG_H
#define NANOMODBUS_CONFIG_H

#define NANOMODBUS_CFG_CLIENT_ENABLED       1
#define NANOMODBUS_CFG_SERVER_ENABLED       1

#endif /* NANOMODBUS_CONFIG_H */

#endif /* COFFEE3_BUILD_CONFIG_H */
