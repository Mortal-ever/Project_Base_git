/**
  * @file      coffee3_server.h
  * @brief     声明 Coffee3 多客户端 Modbus TCP 服务与寄存器模型。
  * @author    WHong
  * @date      2026-09-24
  */

#ifndef COFFEE3_SERVER_H
#define COFFEE3_SERVER_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "FreeRTOS.h"
#include "coffee3_app_config.h"
#include "coffee3_workflow.h"

/** @brief 命令寄存器镜像数量，地址范围为 0x0000 至 0x00AF。 */
#define COFFEE3_SERVER_COMMAND_COUNT          0x00B0U
/** @brief 主机只读状态寄存器数量，地址范围为 0x1000 至 0x10FF。 */
#define COFFEE3_SERVER_STATUS_COUNT           0x0100U
/** @brief 私有监控寄存器数量，地址范围为 0x1100 至 0x117F。 */
#define COFFEE3_SERVER_DEBUG_COUNT            0x0080U

/** @brief 主机命令区中由本模块直接使用的寄存器地址。 */
/** @brief 订单编号寄存器地址。 */
#define COFFEE3_REG_ORDER_NUMBER              0x0000U
/** @brief 咖啡配方类型寄存器地址。 */
#define COFFEE3_REG_COFFEE_TYPE               0x0001U
/** @brief 加盖使能寄存器地址。 */
#define COFFEE3_REG_LID_ENABLE                0x0002U
/** @brief 一号糖浆量寄存器地址。 */
#define COFFEE3_REG_SYRUP_1                   0x0003U
/** @brief 二号糖浆量寄存器地址。 */
#define COFFEE3_REG_SYRUP_2                   0x0004U
/** @brief 目标冰量寄存器地址。 */
#define COFFEE3_REG_ICE_AMOUNT                0x0005U
/** @brief 订单区保留寄存器地址。 */
#define COFFEE3_REG_RESERVED_0006             0x0006U
/** @brief 订单存在门槛寄存器地址。 */
#define COFFEE3_REG_ORDER_PRESENT             0x0007U
/** @brief 订单核对完成门槛寄存器地址，值 1 才允许提交。 */
#define COFFEE3_REG_ORDER_VERIFIED            0x0008U
/** @brief 储存取杯来源寄存器地址。 */
#define COFFEE3_REG_STORAGE_PICKUP            0x0009U
/** @brief 在线取杯出口寄存器地址。 */
#define COFFEE3_REG_ONLINE_OUTPUT             0x000AU
/** @brief 顾客取餐确认寄存器地址。 */
#define COFFEE3_REG_PICKUP_CONFIRM            0x000BU
/** @brief 离线订单出口寄存器地址。 */
#define COFFEE3_REG_OFFLINE_OUTPUT            0x000CU
/** @brief 一号果奶量寄存器地址。 */
#define COFFEE3_REG_FRUIT_MILK_A              0x000DU
/** @brief 二号果奶量寄存器地址。 */
#define COFFEE3_REG_FRUIT_MILK_B              0x000EU
/** @brief 三号糖浆量寄存器地址。 */
#define COFFEE3_REG_SYRUP_3                   0x0013U
/** @brief 四号糖浆量寄存器地址。 */
#define COFFEE3_REG_SYRUP_4                   0x0014U
/** @brief 清除机器人报警触发寄存器地址。 */
#define COFFEE3_REG_CLEAR_ALARM               0x0021U
/** @brief 取消当前订单触发寄存器地址。 */
#define COFFEE3_REG_CANCEL_ORDER              0x0022U
/** @brief 水桶使能兼容掩码寄存器地址。 */
#define COFFEE3_REG_WATER_BUCKET_ENABLE       0x0023U
/** @brief 水阀调试兼容寄存器地址，当前产品未安装对应阀组。 */
#define COFFEE3_REG_WATER_VALVE_DEBUG         0x0024U
/** @brief 紫外灯兼容寄存器地址，当前产品未安装紫外灯。 */
#define COFFEE3_REG_UV_LAMP                   0x0025U
/** @brief 打印机兼容寄存器地址，当前产品未安装打印机。 */
#define COFFEE3_REG_PRINTER_STATUS            0x002AU
/** @brief 机器人轮询的储存位选择寄存器地址。 */
#define COFFEE3_REG_STORAGE_SELECTOR          0x0032U
/** @brief 热杯高度偏移兼容寄存器地址，当前不应用该值。 */
#define COFFEE3_REG_HOT_CUP_HEIGHT_OFFSET     0x003DU
/** @brief 冷杯高度偏移兼容寄存器地址，当前不应用该值。 */
#define COFFEE3_REG_COLD_CUP_HEIGHT_OFFSET    0x003EU
/** @brief 机器人类型兼容寄存器地址，当前驱动固定。 */
#define COFFEE3_REG_ROBOT_TYPE                0x003FU
/** @brief 咖啡供水泵本地输出控制寄存器地址。 */
#define COFFEE3_REG_COFFEE_WATER_PUMP         0x0041U
/** @brief 咖啡取杯时间兼容寄存器地址，当前未实现。 */
#define COFFEE3_REG_COFFEE_PICKUP_TIME        0x0043U
/** @brief 辅助水箱泵兼容寄存器地址，当前产品未安装。 */
#define COFFEE3_REG_AUXILIARY_TANK_PUMP       0x004EU
/** @brief 咖啡机类型兼容寄存器地址，当前驱动固定。 */
#define COFFEE3_REG_COFFEE_MACHINE_TYPE       0x004FU
/** @brief 持久化制冰系数，单位为毫秒每克；零恢复默认值 10。 */
#define COFFEE3_REG_ICE_COEFFICIENT           0x007EU
/** @brief 制冰机类型兼容寄存器地址，当前驱动固定。 */
#define COFFEE3_REG_ICE_MACHINE_TYPE          0x007FU
/** @brief 热水维护启停触发寄存器地址。 */
#define COFFEE3_REG_HOT_WATER_START            0x0080U
/** @brief 热水维护持续分钟数寄存器地址。 */
#define COFFEE3_REG_HOT_WATER_MINUTES          0x0081U
/** @brief 糖浆清洗触发寄存器地址。 */
#define COFFEE3_REG_SYRUP_CLEAN                0x0082U
/** @brief 咖啡管路清洗触发寄存器地址。 */
#define COFFEE3_REG_COFFEE_PIPE_CLEAN          0x0083U
/** @brief 人工果奶通道类型寄存器地址。 */
#define COFFEE3_REG_MANUAL_FRUIT_TYPE          0x00A1U
/** @brief 人工果奶出料量寄存器地址。 */
#define COFFEE3_REG_MANUAL_FRUIT_AMOUNT        0x00A2U
/** @brief 一号果奶清洗触发寄存器地址。 */
#define COFFEE3_REG_FRUIT_A_CLEAN              0x00A3U
/** @brief 二号果奶清洗触发寄存器地址。 */
#define COFFEE3_REG_FRUIT_B_CLEAN              0x00A4U
/** @brief 果奶系数连续配置区的首地址。 */
#define COFFEE3_REG_FRUIT_COEFFICIENT_FIRST    0x00AAU
/** @brief 果奶系数连续配置区的末地址。 */
#define COFFEE3_REG_FRUIT_COEFFICIENT_LAST     0x00AFU
/** @brief 主机状态区中由本模块直接更新的寄存器地址。 */
/** @brief 主机只读状态区起始地址。 */
#define COFFEE3_REG_STATUS_BASE               0x1000U
/** @brief 制作状态寄存器地址。 */
#define COFFEE3_REG_PRODUCTION_STATUS         0x1008U
/** @brief 当前工作流步骤寄存器地址。 */
#define COFFEE3_REG_WORKFLOW_STEP             0x1018U
/** @brief 最近工作流错误寄存器地址。 */
#define COFFEE3_REG_WORKFLOW_ERROR            0x1029U
/** @brief 整机状态寄存器地址。 */
#define COFFEE3_REG_MACHINE_STATUS             0x1020U
/** @brief 热水维护状态寄存器地址。 */
#define COFFEE3_REG_HOT_WATER_STATUS           0x1082U
/** @brief 咖啡管路清洗状态寄存器地址。 */
#define COFFEE3_REG_COFFEE_PIPE_STATUS         0x1083U
/** @brief 两路果奶汇总状态寄存器地址。 */
#define COFFEE3_REG_FRUIT_STATUS               0x10A0U
/** @brief 一号果奶低液位输入状态寄存器地址。 */
#define COFFEE3_REG_FRUIT_A_LOW                0x10A1U
/** @brief 二号果奶低液位输入状态寄存器地址。 */
#define COFFEE3_REG_FRUIT_B_LOW                0x10A2U
/** @brief 一号果奶工作流状态寄存器地址。 */
#define COFFEE3_REG_FRUIT_A_STATUS             0x10A8U
/** @brief 二号果奶工作流状态寄存器地址。 */
#define COFFEE3_REG_FRUIT_B_STATUS             0x10A9U
/** @brief 主机协议定义的只读 32 通道 IO 页面地址。 */
/** @brief 本地输入通道低位寄存器地址。 */
#define COFFEE3_REG_LOCAL_INPUT_LOW           0x10F0U
/** @brief 本地输入通道高位寄存器地址，当前写零。 */
#define COFFEE3_REG_LOCAL_INPUT_HIGH          0x10F1U
/** @brief 一号外部输入模块低位寄存器地址。 */
#define COFFEE3_REG_EXTERNAL_INPUT_1_LOW      0x10F2U
/** @brief 一号外部输入模块高位寄存器地址，当前写零。 */
#define COFFEE3_REG_EXTERNAL_INPUT_1_HIGH     0x10F3U
/** @brief 二号外部输入模块低位寄存器地址，当前写零。 */
#define COFFEE3_REG_EXTERNAL_INPUT_2_LOW      0x10F4U
/** @brief 二号外部输入模块高位寄存器地址，当前写零。 */
#define COFFEE3_REG_EXTERNAL_INPUT_2_HIGH     0x10F5U
/** @brief 三号外部输入模块低位寄存器地址，当前写零。 */
#define COFFEE3_REG_EXTERNAL_INPUT_3_LOW      0x10F6U
/** @brief 三号外部输入模块高位寄存器地址，当前写零。 */
#define COFFEE3_REG_EXTERNAL_INPUT_3_HIGH     0x10F7U
/** @brief 本地输出通道低位寄存器地址。 */
#define COFFEE3_REG_LOCAL_OUTPUT_LOW          0x10F8U
/** @brief 本地输出通道高位寄存器地址，当前写零。 */
#define COFFEE3_REG_LOCAL_OUTPUT_HIGH         0x10F9U
/** @brief 一号外部输出模块低位寄存器地址。 */
#define COFFEE3_REG_EXTERNAL_OUTPUT_1_LOW     0x10FAU
/** @brief 一号外部输出模块高位寄存器地址，当前写零。 */
#define COFFEE3_REG_EXTERNAL_OUTPUT_1_HIGH    0x10FBU
/** @brief 二号外部输出模块低位寄存器地址，当前写零。 */
#define COFFEE3_REG_EXTERNAL_OUTPUT_2_LOW     0x10FCU
/** @brief 二号外部输出模块高位寄存器地址，当前写零。 */
#define COFFEE3_REG_EXTERNAL_OUTPUT_2_HIGH    0x10FDU
/** @brief 三号外部输出模块低位寄存器地址，当前写零。 */
#define COFFEE3_REG_EXTERNAL_OUTPUT_3_LOW     0x10FEU
/** @brief 三号外部输出模块高位寄存器地址，当前写零。 */
#define COFFEE3_REG_EXTERNAL_OUTPUT_3_HIGH    0x10FFU
/** @brief 主机协议定义的本地与外部输出调试寄存器地址。 */
/** @brief 本地输出调试位掩码寄存器地址。 */
#define COFFEE3_REG_LOCAL_IO_DEBUG             0x0208U
/** @brief 外部输出调试位掩码寄存器地址。 */
#define COFFEE3_REG_EXTERNAL_IO_DEBUG          0x0209U

/** @brief 主机协议定义的制作状态值。 */
/** @brief 当前没有订单在制作。 */
#define COFFEE3_PRODUCTION_IDLE               0U
/** @brief 订单正在制作。 */
#define COFFEE3_PRODUCTION_RUNNING            1U
/** @brief 订单制作已经完成。 */
#define COFFEE3_PRODUCTION_COMPLETED          2U
/** @brief 订单制作失败。 */
#define COFFEE3_PRODUCTION_FAILED             3U

void vCoffee3ServerPublishOrder(const Coffee3Order_t *pxOrder);
void vCoffee3ServerSelectStorage(uint16_t usStorage);
void vCoffee3ServerSelectOutlet(void);
void vCoffee3ServerFinishRequest(uint8_t ucStoragePickup);

/** @brief 保存一个 TCP 客户端槽位的公开连接与请求统计。 */
typedef struct {
	uint32_t ulRemoteIpv4; /*!< 按主机字节序保存的远端 IPv4 地址。 */
	uint32_t ulRequestCount; /*!< 该槽位累计 Modbus 轮询次数，包含错误结果。 */
	uint32_t ulErrorCount; /*!< 该槽位累计不可恢复轮询错误次数。 */
	uint32_t ulDisconnectCount; /*!< 该槽位累计已连接状态关闭次数。 */
	uint32_t ulLastActivityTick; /*!< 最近接入或 Modbus 轮询结束的 RTOS 时钟节拍。 */
	int32_t lLastResult; /*!< 最近一次传输或 Modbus 原生结果。 */
	uint16_t usRemotePort; /*!< 远端客户端端口，不是本地监听端口。 */
	uint8_t ucConnected; /*!< TCP 槽位当前已连接，不表示设备控制已就绪。 */
} Coffee3ServerClientStatus_t;

/** @brief 保存监听器和全部客户端槽位的服务端运行统计。 */
typedef struct {
	Coffee3ServerClientStatus_t axClient[COFFEE3_SERVER_MAX_CLIENTS]; /*!< 最多四个固定客户端槽位。 */
	uint32_t ulAcceptedCount; /*!< 累计成功接入的 TCP 客户端数量。 */
	uint32_t ulRejectedCount; /*!< 无槽位或初始化失败时累计拒绝的连接数。 */
	uint32_t ulListenerErrorCount; /*!< 累计监听器创建或 select 错误次数。 */
	uint32_t ulOnlineTransitionCount; /*!< 服务端在线状态累计变化次数。 */
	uint16_t usListenPort; /*!< 产品配置的 Modbus TCP 监听端口。 */
	uint8_t ucActiveClients; /*!< 当前占用客户端槽位数量，范围为 0 至 4。 */
	uint8_t ucListening; /*!< 非零表示监听 socket 已建立。 */
	uint8_t ucOnline; /*!< 非零表示至少一个客户端已连接。 */
} Coffee3ServerStatus_t;

extern Coffee3ServerStatus_t g_xCoffee3ServerStatus; /*!< 服务端公开运行状态。 */

BaseType_t xCoffee3ServerInitialize(void);
void vCoffee3ServerTask(void *pvArgument);
void vCoffee3ServerPublishWorkflow(uint16_t usOrderId,
	uint16_t usProductionStatus, uint16_t usStep, int32_t lError);
void vCoffee3ServerPublishOutput(uint16_t usOutput, uint16_t usState);
uint16_t usCoffee3ServerGetCommandRegister(uint16_t usAddress);

#ifdef __cplusplus
}
#endif

#endif /* COFFEE3_SERVER_H */
