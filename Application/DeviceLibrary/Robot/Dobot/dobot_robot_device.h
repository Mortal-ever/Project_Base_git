/**
  * @file      dobot_robot_device.h
  * @brief     Define static Dobot Modbus robot protocol variants and points.
  * @author    WHong
  * @date      2026-08-20
  */

#ifndef DOBOT_ROBOT_DEVICE_H
#define DOBOT_ROBOT_DEVICE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "device_library.h"

#define DOBOT_ROBOT_SELECTOR_ANY                 0xFFFFU
#define DOBOT_ROBOT_BASE_INPUT_COUNT              16U
#define DOBOT_ROBOT_CONTROL_START                3100U
#define DOBOT_ROBOT_CONTROL_LAST_DEFINED         3159U
#define DOBOT_ROBOT_CONTROL_VALID_END_EXCLUSIVE  3160U
#define DOBOT_ROBOT_CONTROL_RESERVED_ADDRESS     3160U
#define DOBOT_ROBOT_LEGACY_CONTROL_COUNT          40U
#define DOBOT_ROBOT_EXTENDED_CONTROL_COUNT        60U
#define DOBOT_ROBOT_MAX_CONTROL_COUNT             61U

typedef enum {
	DOBOT_ROBOT_PROTOCOL_1 = 0,
	DOBOT_ROBOT_PROTOCOL_2 = 1,
	DOBOT_ROBOT_PROTOCOL_3 = 2
} DobotRobotProtocolVariant_e;

/** @brief Protocol 1 command and result coils from the reviewed workbook. */
typedef enum {
	DOBOT_ROBOT_P1_RESULT_READY = 3100,
	DOBOT_ROBOT_P1_RESULT_HOME = 3101,
	DOBOT_ROBOT_P1_RESULT_HOT_CUP = 3103,
	DOBOT_ROBOT_P1_RESULT_COLD_CUP = 3104,
	DOBOT_ROBOT_P1_RESULT_LID_1 = 3105,
	DOBOT_ROBOT_P1_RESULT_LID_2 = 3106,
	DOBOT_ROBOT_P1_RESULT_ICE = 3107,
	DOBOT_ROBOT_P1_RESULT_COFFEE_FRONT = 3108,
	DOBOT_ROBOT_P1_RESULT_COFFEE_INSIDE = 3109,
	DOBOT_ROBOT_P1_COMMAND_START_SIGNAL = 3110,
	DOBOT_ROBOT_P1_COMMAND_HOME = 3111,
	DOBOT_ROBOT_P1_COMMAND_HOT_CUP = 3113,
	DOBOT_ROBOT_P1_COMMAND_COLD_CUP = 3114,
	DOBOT_ROBOT_P1_COMMAND_LID_1 = 3115,
	DOBOT_ROBOT_P1_COMMAND_LID_2 = 3116,
	DOBOT_ROBOT_P1_COMMAND_ICE = 3117,
	DOBOT_ROBOT_P1_COMMAND_COFFEE_FRONT = 3118,
	DOBOT_ROBOT_P1_COMMAND_COFFEE_INSIDE = 3119,
	DOBOT_ROBOT_P1_RESULT_PRINTER = 3120,
	DOBOT_ROBOT_P1_RESULT_OUTPUT_1 = 3121,
	DOBOT_ROBOT_P1_RESULT_OUTPUT_2 = 3122,
	DOBOT_ROBOT_P1_RESULT_TAKE_COFFEE = 3123,
	DOBOT_ROBOT_P1_RESULT_TAKE_LID = 3124,
	DOBOT_ROBOT_P1_RESULT_COVER_LID = 3125,
	DOBOT_ROBOT_P1_RESULT_PUT_STORAGE = 3126,
	DOBOT_ROBOT_P1_RESULT_TAKE_STORAGE = 3127,
	DOBOT_ROBOT_P1_RESULT_PUT_OUTPUT = 3128,
	DOBOT_ROBOT_P1_RESULT_FRUIT_SYRUP = 3129,
	DOBOT_ROBOT_P1_COMMAND_PRINTER = 3130,
	DOBOT_ROBOT_P1_COMMAND_OUTPUT_1 = 3131,
	DOBOT_ROBOT_P1_COMMAND_OUTPUT_2 = 3132,
	DOBOT_ROBOT_P1_COMMAND_TAKE_COFFEE = 3133,
	DOBOT_ROBOT_P1_COMMAND_TAKE_LID = 3134,
	DOBOT_ROBOT_P1_COMMAND_COVER_LID = 3135,
	DOBOT_ROBOT_P1_COMMAND_PUT_STORAGE = 3136,
	DOBOT_ROBOT_P1_COMMAND_TAKE_STORAGE = 3137,
	DOBOT_ROBOT_P1_COMMAND_PUT_OUTPUT = 3138,
	DOBOT_ROBOT_P1_COMMAND_FRUIT_SYRUP = 3139
} DobotRobotProtocol1Coil_e;

/** @brief Dobot controller coils written with FC05 or FC15. */
typedef enum {
	DOBOT_ROBOT_BODY_COMMAND_START = 0,
	DOBOT_ROBOT_BODY_COMMAND_STOP = 1,
	DOBOT_ROBOT_BODY_COMMAND_PAUSE = 2,
	DOBOT_ROBOT_BODY_COMMAND_ENABLE = 3,
	DOBOT_ROBOT_BODY_COMMAND_DISABLE = 4,
	DOBOT_ROBOT_BODY_COMMAND_CLEAR_ALARM = 5,
	DOBOT_ROBOT_BODY_COMMAND_ENTER_DRAG = 6,
	DOBOT_ROBOT_BODY_COMMAND_EXIT_DRAG = 7,
	DOBOT_ROBOT_BODY_COMMAND_AUTO_MODE = 8,
	DOBOT_ROBOT_BODY_COMMAND_MANUAL_MODE = 9
} DobotRobotBodyCommand_e;

/** @brief Dobot discrete-input indices read from the controller body. */
typedef enum {
	DOBOT_ROBOT_BODY_STATUS_RUNNING = 0,
	DOBOT_ROBOT_BODY_STATUS_STOPPED = 1,
	DOBOT_ROBOT_BODY_STATUS_PAUSED = 2,
	DOBOT_ROBOT_BODY_STATUS_SAFE_ORIGIN = 3,
	DOBOT_ROBOT_BODY_STATUS_SAFETY_PAUSED = 4,
	DOBOT_ROBOT_BODY_STATUS_IDLE = 5,
	DOBOT_ROBOT_BODY_STATUS_POWERED = 6,
	DOBOT_ROBOT_BODY_STATUS_ENABLED = 7,
	DOBOT_ROBOT_BODY_STATUS_ALARM = 8,
	DOBOT_ROBOT_BODY_STATUS_COLLISION = 9,
	DOBOT_ROBOT_BODY_STATUS_DRAG = 10,
	DOBOT_ROBOT_BODY_STATUS_RECOVERY = 11
} DobotRobotBodyStatus_e;

/** @brief Compatibility alias for the original 3100-3139 name. */
#define DOBOT_ROBOT_PROTOCOL_LEGACY_3139   DOBOT_ROBOT_PROTOCOL_1
/** @brief Compatibility alias for the original extended name. */
#define DOBOT_ROBOT_PROTOCOL_EXTENDED_3150 DOBOT_ROBOT_PROTOCOL_2

/**
  * @brief  Describe one static Dobot register-range contract.
  */
typedef struct {
	uint16_t usControlStart;
	uint16_t usControlCount;
	uint16_t usControlValidEndExclusive;
	uint16_t usActionClearStart;
	uint16_t usActionClearCount;
	uint16_t usActionClearSecondStart;
	uint16_t usActionClearSecondCount;
	uint16_t usBaseInputStart;
	uint16_t usBaseInputCount;
	uint8_t ucAllowedRoleMask;
	uint8_t ucPlaceholder;
	DobotRobotProtocolVariant_e xVariant;
} DobotRobotProtocolConfig_t;

/**
  * @brief  Map a product action identifier to a robot command/result pair.
  */
typedef struct {
	uint16_t usTargetId;
	uint16_t usSelector;
	uint16_t usCommandCoil;
	uint16_t usResultCoil;
} DobotRobotPoint_t;

typedef struct {
	const DobotRobotProtocolConfig_t *pxProtocol;
	const DobotRobotPoint_t *pxPoints;
	uint16_t usPointCount;
	DeviceRole_e xRole;
} DobotRobotDriverConfig_t;

/** @brief Selected legacy-compatible Protocol 1 metadata. */
extern const DobotRobotProtocolConfig_t
	g_xDobotRobotProtocol1;
/** @brief Placeholder metadata for the future Robot1 Protocol 2 mapping. */
extern const DobotRobotProtocolConfig_t
	g_xDobotRobotProtocol2;
/** @brief Placeholder metadata for the future Robot2 Protocol 3 mapping. */
extern const DobotRobotProtocolConfig_t
	g_xDobotRobotProtocol3;

/** @brief Protocol 1 driver descriptor. */
extern const DeviceDriverDescriptor_t
	g_xDobotRobotProtocol1Driver;
/** @brief Protocol 2 driver descriptor. */
extern const DeviceDriverDescriptor_t
	g_xDobotRobotProtocol2Driver;
/** @brief Protocol 3 driver descriptor. */
extern const DeviceDriverDescriptor_t
	g_xDobotRobotProtocol3Driver;

/* Compatibility names retained for existing product integrations. */
#define g_xDobotRobotLegacy3139Protocol g_xDobotRobotProtocol1
#define g_xDobotRobotExtended3150Protocol g_xDobotRobotProtocol2
#define g_xDobotRobotLegacy3139Driver g_xDobotRobotProtocol1Driver
#define g_xDobotRobotExtended3150Driver g_xDobotRobotProtocol2Driver
extern const DeviceDriverDescriptor_t
	g_xJakaRobotReservedDriver;

const DobotRobotProtocolConfig_t *pxDobotRobotGetProtocol(
	DobotRobotProtocolVariant_e xVariant);
uint8_t ucDobotRobotIsControlAddressValid(
	const DobotRobotProtocolConfig_t *pxProtocol,
	uint16_t usAddress);
uint8_t ucDobotRobotRoleAllowed(
	const DobotRobotProtocolConfig_t *pxProtocol,
	DeviceRole_e xRole);
uint8_t ucDobotRobotResolvePoint(
	const DobotRobotDriverConfig_t *pxConfig,
	uint16_t usTargetId, uint16_t usSelector,
	uint16_t *pusCommandCoil, uint16_t *pusResultCoil);

#ifdef __cplusplus
}
#endif

#endif /* DOBOT_ROBOT_DEVICE_H */
