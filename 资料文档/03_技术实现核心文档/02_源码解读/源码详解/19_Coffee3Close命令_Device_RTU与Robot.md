# 19 Coffee3Close 命令、Device、RTU 与 Robot 源码带读

本篇从 `coffee3_device.h` 开始，再读 device.c、rtu_bus.c、robot_tcp.c、device_image.c。类型和函数位置见末尾导航；使用的是本 Target 的私有命令，不是公共 AppCommand。

## 一条命令究竟是什么

`Coffee3Command_t` 是按值复制到队列的32字节消息，头文件的负长度 typedef 在大小错误时令编译失败。它不携带动态字符串，也不携带堆上业务对象指针。

| 字段 | 含义及流向 |
| --- | --- |
| ulCommandId | submit 为0时分配非零递增序号；回写调用者，供后续精确等待 |
| ulOrderId | 日志关联单号，不作为抢占优先级依据 |
| ulOrderEpoch | 业务代次；取消某一代命令，避免重用订单号串结果 |
| ulTimeoutMs | 传给 owner 的设备事务/动作预算，不等于所有等待总时长 |
| usStepId | Workflow 步骤诊断号，不能当设备编号 |
| usAction | 产品动作枚举，owner 转成公共设备动作 |
| ausParameter[4] | 四个16位参数；具体含义由 action 分支解释 |
| ucDeviceId | 1..10逻辑设备，查绑定表，不是unit |
| ucSource | 0 Workflow、1 Server、2 Maintenance |
| ucRetryLimit | owner 最大附加重试次数；咖啡非刷新动作强制不重试 |
| ucFlags | bit0 safety stop；bit1 manual reservation；bit2 debug |

### Action 编号的阅读方法

1=REFRESH、2=CANCEL、3=RESET；100..126 是机器人动作；200..203 为 MAKE/PAUSE/RESUME/CLEAN；300/301落杯，310/311落盖；320..322糖浆；330制冰阀；340..342秤；350单点IO、351整幅IO。不是所有设备都支持每个动作，必须读 `prvExecute` 的设备分支。完整逐值原文在本篇头文件附录。

## prvSubmit 函数体：从消息到队列

1. NULL、NONE、越界 device 直接 `pdFAIL`，不会发送。
2. `pxCoffee3DeviceGetBinding` 查 route，再取 `s_axRouteQueues`。没有注册队列说明 owner 尚未准备。
3. `ulCommandId == 0` 才分配序号；临界区保护自增，溢出跳过0。重用非零对象不会自动成为新命令。
4. Server 且未标 DEBUG 时调用 WorkflowAcquireManual，成功才加 MANUAL_RESERVED。非REFRESH手动请求投队首；REFRESH仍普通队列。
5. `xQueueSendToFront` 或 `xQueueSend` 按值复制。队列满时只等待传入 ticks，不在这里访问串口。
6. 入队失败释放刚取得的手动预留，并清掉flag；否则完成路径负责释放。

```c
xResult = (ucUrgent != 0U) ?
    xQueueSendToFront(xQueue, pxCommand, xWaitTicks) :
    xQueueSend(xQueue, pxCommand, xWaitTicks);
```

队首插入不是中断正在执行的串口事务。DEBUG 绕过这一手动预留分支，不能仅从“Server来源”推断所有命令都有相同优先规则。

## 事件位和状态字段怎样配合

ONLINE=bit0，READY=bit1，BUSY=bit2；DONE=bit3、FAILED=bit4、TIMEOUT=bit5、CANCELED=bit8 组成 TERMINAL。COMM_FAULT=bit6、DEVICE_FAULT=bit7、DATA_UPDATED=bit9、RECOVERING=bit10。

`DeviceCommandStarted` 清旧终态事件，写 LastCommandId/LastOrderEpoch/LastAction，busy=1，累计 command count；Robot还清 accepted和phase。`DeviceCommandCompleted` 分类结果、记录终态和历史、更新事件，并释放手动预留。

| DeviceStatus 字段组 | 谁写、谁读 |
| --- | --- |
| ulLastCommandId / ulLastOrderEpoch / usLastAction | owner started写；Workflow确认当前机器人计时身份 |
| ulLastSuccessTick / ulCommandCount / ulErrorCount / lLastResult | owner完成写；Server和日志读 |
| ucOnline / ucReady / ucBusy / ucRecovering | 通信/owner分别更新；业务不能用Online替代Ready |
| ucRobotPhase / ucRobotAccepted | Robot事务推进写；Workflow用accepted启动运动计时 |
| ucTerminalValid及ulTerminalCommandId/OrderEpoch、lTerminalResult、usTerminalAction、ucTerminalTimedOut | 最新终态快照，精确匹配后消费 |
| ucPreviousTerminalValid及Previous对应字段 | 保留前一次结果，减小下一命令覆盖的窗口 |

`xCoffee3DeviceWaitCommand(device,epoch,id,ticks)` 不只等事件位，还检查结果身份；`lCoffee3DeviceGetTerminalResult` 用相同键取得原始错误。附录可继续追静态终态history检索。普通 EventGroup 没有携带commandId，故不能只看DONE。

## 协作取消

`vCoffee3OrderCancelRequest` 只接受非零epoch。`ucCoffee3CommandIsCanceled` 对 Workflow 且epoch匹配才返回取消。被识别的安全停机（机器人/咖啡取消、制冰关阀、IO关闭）可以豁免；不是任意加 SAFETY_STOP 都能豁免。

## vCoffee3RtuBusTask 函数体

任务参数是静态 BusConfig。先校验Bus，建立UART通道并Open；Modbus线路再建立ModbusPort。初始化失败保留未创建状态，收到命令会以NOT_READY终结，不会假装成功。

主循环 `xQueueReceive` 取消息 → 校验route/protocol → Started → 取消检查 → 最小间隔等待 → `prvExecute` → 保存结果 → Completed。重试失败时延时50ms再试；成功和取消立即break。咖啡非REFRESH强制 `ucRetryLimit=0`，避免丢响应导致重复制作。

`prvExecute` 把产品枚举翻译成 M50、Cup/Lid、Syrup、Ice、Scale、Energy、IO公共API；驱动成功后提交镜像。没有分支支持的动作返回不支持，不能误诊为断线。

## Robot owner 的分段状态

IDLE=0、PREPARING=1、WAIT_ACCEPT=2、MOVING=3、CLEAR_RESULT=4、RECOVERING=5。私有点位表给出业务action对应的命令与结果线圈，公共 `ucDobotRobotResolvePoint` 负责查表解析，真正Modbus TCP读写在本篇Robot模块。

连接由公共 TcpClientSession 周期推进；任务创建不等于已ONLINE。执行动作要处理原有结果、发命令、等受理、等运动结束、清结果。Workflow读取 `ucRobotAccepted` 后才开始运动计时；恢复或未受理时会更新计时起点。因此模拟器一直不清受理线圈时，运动超时不能被误读为从最初入队一直倒计时。

## DeviceImage 是私有状态投影

`vCoffee3DeviceImageCommitM50` 校验NULL，在临界区清私有ausStatus后逐项复制公共M50状态。Cup/Lid的refresh分支复制任务数组和线圈；非refresh仅更新ucSlot任务值。这两个Cup/Lid提交函数没有与M50相同的临界区，不能声称所有设备镜像都是原子快照。

## 建议断点与练习

在 submit 后记下32字节消息的id/epoch；在Bus取出时比对；在Completed和WaitCommand验证相同键。把debug IO值设0x0005，沿351分支看整幅输出而非点位编号5。观察队列失败、协议超时、设备未完成的三个不同出口。

下一篇读Workflow，理解“设备命令返回0以后，为什么还必须等传感器或设备状态”。
## 源码导航与版本证据

### [Application/UserAPP/Coffee3CloseApp/Device/coffee3_device_image.c](../../../../Application/UserAPP/Coffee3CloseApp/Device/coffee3_device_image.c)

SHA-256：`2C1EB9E3DEA81E55731C73FA16F046B08CA68F4D5CE35BD850CCBD20D762044A`。以下行号对应 2026-09-08 工作区快照。

| 定义行 | 函数 | 下行调用（词法检索，非运行时跟踪） |
| --- | --- | --- |
| 30 | `vCoffee3DeviceImageCommitM50` | memset, taskENTER_CRITICAL, taskEXIT_CRITICAL |
| 48 | `vCoffee3DeviceImageCommitCup` | memcpy |
| 65 | `vCoffee3DeviceImageCommitLid` | memcpy |
### [Application/UserAPP/Coffee3CloseApp/Device/coffee3_device_image.h](../../../../Application/UserAPP/Coffee3CloseApp/Device/coffee3_device_image.h)

SHA-256：`D1DE90D4087EC60F9E3813211810CD5DF645B1B17D613EEB1BB0584C58C7F515`。以下行号对应 2026-09-08 工作区快照。

类型与常量原文按头文件顺序列出；正文先解释用途，这里保留全部字段/枚举值，方便逐项对照。API 原型请沿文件链接阅读。

L9
```c
#define COFFEE3_DEVICE_IMAGE_H
```


L23
```c
typedef struct {
	uint16_t ausStatus[24];
} Coffee3CoffeeMachineImage_t;
```


L27
```c
typedef struct {
	uint16_t ausCupTask[2];
	uint16_t ausLidTask[2];
	uint8_t aucCupCoils[10];
	uint8_t aucLidCoils[10];
} Coffee3CupLidImage_t;
```

### [Application/UserAPP/Coffee3CloseApp/Device/coffee3_device.c](../../../../Application/UserAPP/Coffee3CloseApp/Device/coffee3_device.c)

SHA-256：`81E25F29A3A11C8FD578FD7785F09F8E459B36E36E0E7849565A911A81A40625`。以下行号对应 2026-09-08 工作区快照。

| 定义行 | 函数 | 下行调用（词法检索，非运行时跟踪） |
| --- | --- | --- |
| 69 | `prvTerminalBits` |  |
| 92 | `lCoffee3DeviceGetTerminalResult` | taskENTER_CRITICAL, taskEXIT_CRITICAL |
| 127 | `xCoffee3DeviceInitialize` | memset, xEventGroupCreateStatic |
| 154 | `vCoffee3DeviceRegisterRoute` |  |
| 180 | `xCoffee3CommandSubmit` | prvSubmit |
| 187 | `xCoffee3CommandSubmitUrgent` | prvSubmit |
| 194 | `vCoffee3OrderCancelRequest` | taskENTER_CRITICAL, taskEXIT_CRITICAL |
| 205 | `ucCoffee3CommandIsCanceled` |  |
| 230 | `prvSubmit` | pxCoffee3DeviceGetBinding, taskENTER_CRITICAL, taskEXIT_CRITICAL, vCoffee3WorkflowReleaseManual, xCoffee3WorkflowAcquireManual, xQueueSend, xQueueSendToFront |
| 284 | `vCoffee3DeviceCommandStarted` | taskENTER_CRITICAL, taskEXIT_CRITICAL, xCoffee3LogWriteFieldOrder, xEventGroupClearBits, xEventGroupSetBits |
| 325 | `vCoffee3DeviceCommandCompleted` | taskENTER_CRITICAL, taskEXIT_CRITICAL, vCoffee3WorkflowReleaseManual, xCoffee3LogWriteFieldOrder, xEventGroupClearBits, xEventGroupSetBits, xTaskGetTickCount |
| 464 | `vCoffee3DeviceSetRobotPhase` | taskENTER_CRITICAL, taskEXIT_CRITICAL |
| 476 | `vCoffee3DeviceSetRobotAccepted` | taskENTER_CRITICAL, taskEXIT_CRITICAL |
| 485 | `vCoffee3DeviceSetReady` | taskENTER_CRITICAL, taskEXIT_CRITICAL, xEventGroupClearBits, xEventGroupSetBits |
| 507 | `vCoffee3DeviceSetRecovering` | taskENTER_CRITICAL, taskEXIT_CRITICAL, xEventGroupClearBits, xEventGroupSetBits |
| 530 | `vCoffee3DeviceSetOnline` | taskENTER_CRITICAL, taskEXIT_CRITICAL, vCoffee3DeviceSetReady, xEventGroupClearBits, xEventGroupSetBits |
| 567 | `xCoffee3DeviceGetEvents` | xEventGroupGetBits |
| 578 | `xCoffee3DeviceWaitCommand` | pdMS_TO_TICKS, prvTerminalBits, taskENTER_CRITICAL, taskEXIT_CRITICAL, vTaskDelay, xEventGroupWaitBits, xTaskGetTickCount |
### [Application/UserAPP/Coffee3CloseApp/Device/coffee3_device.h](../../../../Application/UserAPP/Coffee3CloseApp/Device/coffee3_device.h)

SHA-256：`87696091E0010FB70C3257F27DAFE18697DF5C5A9E616D7F53CB39AC522B3A0B`。以下行号对应 2026-09-08 工作区快照。

类型与常量原文按头文件顺序列出；正文先解释用途，这里保留全部字段/枚举值，方便逐项对照。API 原型请沿文件链接阅读。

L9
```c
#define COFFEE3_DEVICE_H
```


L23
```c
typedef enum {
	COFFEE3_DEVICE_NONE = 0,
	COFFEE3_DEVICE_ROBOT = 1,
	COFFEE3_DEVICE_COFFEE_MACHINE = 2,
	COFFEE3_DEVICE_CUP_MACHINE = 3,
	COFFEE3_DEVICE_SYRUP_MACHINE = 4,
	COFFEE3_DEVICE_LID_MACHINE = 5,
	COFFEE3_DEVICE_ICE_MACHINE = 6,
	COFFEE3_DEVICE_SCALE = 7,
	COFFEE3_DEVICE_POWER_METER = 8,
	COFFEE3_DEVICE_IO_INPUT = 9,
	COFFEE3_DEVICE_IO_OUTPUT = 10,
	COFFEE3_DEVICE_COUNT = 11
} Coffee3DeviceId_e;
```


L39
```c
typedef enum {
	COFFEE3_COMMAND_SOURCE_WORKFLOW = 0,
	COFFEE3_COMMAND_SOURCE_SERVER = 1,
	COFFEE3_COMMAND_SOURCE_MAINTENANCE = 2
} Coffee3CommandSource_e;
```


L46
```c
#define COFFEE3_COMMAND_FLAG_DEBUG 0x04U
```


L49
```c
typedef enum {
	COFFEE3_ROBOT_PHASE_IDLE = 0,
	COFFEE3_ROBOT_PHASE_PREPARING = 1,
	COFFEE3_ROBOT_PHASE_WAIT_ACCEPT = 2,
	COFFEE3_ROBOT_PHASE_MOVING = 3,
	COFFEE3_ROBOT_PHASE_CLEAR_RESULT = 4,
	COFFEE3_ROBOT_PHASE_RECOVERING = 5
} Coffee3RobotPhase_e;
```


L59
```c
typedef enum {
	COFFEE3_ACTION_REFRESH = 1, COFFEE3_ACTION_CANCEL = 2,
	COFFEE3_ACTION_RESET = 3, COFFEE3_ACTION_ROBOT_START = 100,
	COFFEE3_ACTION_ROBOT_STOP = 101, COFFEE3_ACTION_ROBOT_ENABLE = 102,
	COFFEE3_ACTION_ROBOT_CLEAR_ALARM = 103, COFFEE3_ACTION_ROBOT_PAUSE = 104,
	COFFEE3_ACTION_ROBOT_DISABLE = 105, COFFEE3_ACTION_ROBOT_ENTER_DRAG = 106,
	COFFEE3_ACTION_ROBOT_EXIT_DRAG = 107, COFFEE3_ACTION_ROBOT_AUTO_MODE = 108,
	COFFEE3_ACTION_ROBOT_MANUAL_MODE = 109, COFFEE3_ACTION_ROBOT_HOME = 110,
	COFFEE3_ACTION_ROBOT_TAKE_HOT_CUP = 111,
	COFFEE3_ACTION_ROBOT_TAKE_COLD_CUP = 112,
	COFFEE3_ACTION_ROBOT_TO_COFFEE = 113, COFFEE3_ACTION_ROBOT_TO_ICE = 114,
	COFFEE3_ACTION_ROBOT_TO_LID = 115, COFFEE3_ACTION_ROBOT_TAKE_LID = 116,
	COFFEE3_ACTION_ROBOT_COVER_LID = 117, COFFEE3_ACTION_ROBOT_PUT_OUTPUT = 118,
	COFFEE3_ACTION_ROBOT_PUT_STORAGE = 119,
	COFFEE3_ACTION_ROBOT_TO_PRINTER = 120,
	COFFEE3_ACTION_ROBOT_TAKE_OUTPUT_1 = 121,
	COFFEE3_ACTION_ROBOT_TAKE_OUTPUT_2 = 122,
	COFFEE3_ACTION_ROBOT_TAKE_COFFEE = 123,
	COFFEE3_ACTION_ROBOT_TAKE_STORAGE = 124,
	COFFEE3_ACTION_ROBOT_START_SIGNAL = 125,
	COFFEE3_ACTION_ROBOT_TO_FRUIT_SYRUP = 126,
	COFFEE3_ACTION_COFFEE_MAKE = 200, COFFEE3_ACTION_COFFEE_PAUSE = 201,
	COFFEE3_ACTION_COFFEE_RESUME = 202, COFFEE3_ACTION_COFFEE_CLEAN = 203,
	COFFEE3_ACTION_CUP_DROP_1 = 300, COFFEE3_ACTION_CUP_DROP_2 = 301,
	COFFEE3_ACTION_LID_DROP_1 = 310, COFFEE3_ACTION_LID_DROP_2 = 311,
	COFFEE3_ACTION_SYRUP_DISPENSE = 320,
	COFFEE3_ACTION_SYRUP_CLEAN = 321,
	COFFEE3_ACTION_SYRUP_SET_REMAINING = 322,
	COFFEE3_ACTION_ICE_SET_VALVE = 330, COFFEE3_ACTION_SCALE_TARE = 340,
	COFFEE3_ACTION_SCALE_CLEAR_TARE = 341, COFFEE3_ACTION_SCALE_ZERO = 342,
	COFFEE3_ACTION_IO_WRITE = 350,
	COFFEE3_ACTION_IO_WRITE_MASK = 351
} Coffee3Action_e;
```


L94
```c
#define COFFEE3_DEVICE_EVENT_ONLINE          (1UL << 0)
```


L95
```c
#define COFFEE3_DEVICE_EVENT_READY           (1UL << 1)
```


L96
```c
#define COFFEE3_DEVICE_EVENT_BUSY            (1UL << 2)
```


L97
```c
#define COFFEE3_DEVICE_EVENT_COMMAND_DONE    (1UL << 3)
```


L98
```c
#define COFFEE3_DEVICE_EVENT_COMMAND_FAILED  (1UL << 4)
```


L99
```c
#define COFFEE3_DEVICE_EVENT_TIMEOUT         (1UL << 5)
```


L100
```c
#define COFFEE3_DEVICE_EVENT_COMM_FAULT      (1UL << 6)
```


L101
```c
#define COFFEE3_DEVICE_EVENT_DEVICE_FAULT    (1UL << 7)
```


L102
```c
#define COFFEE3_DEVICE_EVENT_CANCELED        (1UL << 8)
```


L103
```c
#define COFFEE3_DEVICE_EVENT_DATA_UPDATED    (1UL << 9)
```


L104
```c
#define COFFEE3_DEVICE_EVENT_RECOVERING      (1UL << 10)
```


L107
```c
#define COFFEE3_COMMAND_RESULT_CANCELED       (-9)
```


L108
```c
#define COFFEE3_COMMAND_RESULT_SUPERSEDED     (-10)
```


L109
```c
#define COFFEE3_COMMAND_FLAG_SAFETY_STOP       0x01U
```


L110
```c
#define COFFEE3_COMMAND_FLAG_MANUAL_RESERVED   0x02U
```


L113
```c
#define COFFEE3_DEVICE_EVENT_TERMINAL         \
```


L120
```c
typedef struct {
	uint32_t ulCommandId;
	uint32_t ulOrderId;
	uint32_t ulOrderEpoch;
	uint32_t ulTimeoutMs;
	uint16_t usStepId;
	uint16_t usAction;
	uint16_t ausParameter[4];
	uint8_t ucDeviceId;
	uint8_t ucSource;
	uint8_t ucRetryLimit;
	uint8_t ucFlags;
} Coffee3Command_t;
```


L138
```c
typedef struct {
	Coffee3DeviceId_e xDeviceId;
	uint8_t ucRouteId;
	uint8_t ucUnitId;
	uint16_t usMinimumIntervalMs;
	uint8_t ucCategory;
	uint8_t ucRole;
	uint8_t ucDriverId;
	uint8_t ucProtocolId;
	const char *pcName;
} Coffee3DeviceBinding_t;
```


L151
```c
typedef struct {
	uint32_t ulLastCommandId;
	uint32_t ulLastOrderEpoch;
	uint32_t ulLastSuccessTick;
	uint32_t ulCommandCount;
	uint32_t ulErrorCount;
	int32_t lLastResult;
	uint16_t usLastAction;
	uint8_t ucOnline;
	uint8_t ucBusy;
	uint8_t ucReady;
	uint8_t ucRecovering;
	uint8_t ucRobotPhase;
	uint8_t ucRobotAccepted;
	uint8_t ucTerminalValid;
	uint8_t ucPreviousTerminalValid;
	uint32_t ulTerminalCommandId;
	uint32_t ulTerminalOrderEpoch;
	int32_t lTerminalResult;
	uint16_t usTerminalAction;
	uint8_t ucTerminalTimedOut;
	uint32_t ulPreviousTerminalCommandId;
	uint32_t ulPreviousTerminalOrderEpoch;
	int32_t lPreviousTerminalResult;
	uint16_t usPreviousTerminalAction;
	uint8_t ucPreviousTerminalTimedOut;
} Coffee3DeviceStatus_t;
```

### [Application/UserAPP/Coffee3CloseApp/Modbus_Rtu_Bus/coffee3_rtu_bus.c](../../../../Application/UserAPP/Coffee3CloseApp/Modbus_Rtu_Bus/coffee3_rtu_bus.c)

SHA-256：`6EF28FCA7C5BE2918609C09C84ABBECEDB602EEFEBB9F8B54891034C32B6AE55`。以下行号对应 2026-09-08 工作区快照。

| 定义行 | 函数 | 下行调用（词法检索，非运行时跟踪） |
| --- | --- | --- |
| 128 | `xCoffee3SerialApplyDefaults` | HAL_UART_Abort, HAL_UART_DeInit, prvConfigureUart |
| 153 | `xCoffee3LogSerialApplyDefault` | prvConfigureUart |
| 159 | `xCoffee3RtuBusInitialize` | memset, vCoffee3DeviceRegisterRoute, xQueueCreateStatic |
| 193 | `vCoffee3RtuBusTask` | memset, pdMS_TO_TICKS, prvExecute, prvFindBusIndex, prvFindModbusPortIndex, prvGetDeviceLogSource, prvGetLogSource, prvLogCommandFailure, prvLogDeviceBindings, pxCoffee3DeviceGetBinding, ucCoffee3CommandIsCanceled, ucModbusPortResultIsLinkFailure, vCoffee3DeviceCommandCompleted, vCoffee3DeviceCommandStarted, vCoffee3DeviceSetOnline, vTaskDelay, vTaskDelete, xCoffee3LogWrite, xCoffee3LogWriteField, xCoffee3LogWriteFieldOrder, xModbusPortClientInit, xQueueReceive, xTaskGetTickCount, xTransportOpen, xTransportUartCreate |
| 371 | `prvConfigureUart` | HAL_UART_Abort, HAL_UART_DeInit, HAL_UART_Init |
| 395 | `prvExecute` | ld, memset, prvLogIoWrite, prvLogIoWriteExpected, ucIceMachineGetFaultMask, vCoffee3DeviceImageCommitCup, vCoffee3DeviceImageCommitLid, vCoffee3DeviceImageCommitM50, vCoffee3IoCommitModbusInput, vCoffee3IoCommitModbusOutputImage, xCoffee3LogPrintfOrder, xCoffeeMachineM50Execute, xCupLidShengShuRefresh, xCupLidShengShuRun, xIceMachineRefresh, xIceMachineSetValve, xIoModuleModbusReadInputs, xIoModuleModbusReadOutputs, xIoModuleModbusWriteOutput, xPowerMeterDdsu666Refresh, xScaleBsqDgV2ClearTare, xScaleBsqDgV2Refresh, xScaleBsqDgV2Tare, xScaleBsqDgV2Zero, xSyrupMachineClean, xSyrupMachineDispense, xSyrupMachineRefresh, xSyrupMachineSetRemaining |
| 586 | `prvCommandCanceled` | ucCoffee3CommandIsCanceled |
| 594 | `prvGetLogSource` |  |
| 609 | `prvGetDeviceLogSource` |  |
| 687 | `prvLogDeviceBindings` | prvGetBusLinkEvent, prvGetDeviceLogSource, prvGetDeviceProtocolEvent, pxCoffee3DeviceGetBinding, xCoffee3LogWriteField |
| 715 | `prvFindBusIndex` |  |
| 728 | `prvFindModbusPortIndex` |  |
| 761 | `prvLogCommandFailure` | prvGetDeviceLogSource, prvGetDeviceName, prvModbusResultName, pxCoffee3DeviceGetBinding, xCoffee3LogPrintfOrder |
| 788 | `prvLogIoWriteExpected` | xCoffee3LogWriteFieldOrder |
| 805 | `prvLogIoWrite` | xCoffee3LogWriteFieldOrder |
### [Application/UserAPP/Coffee3CloseApp/Modbus_Rtu_Bus/coffee3_rtu_bus.h](../../../../Application/UserAPP/Coffee3CloseApp/Modbus_Rtu_Bus/coffee3_rtu_bus.h)

SHA-256：`0F3002FE4389230DA76EB299192956814503B10717F4A48B19433BEDE18E0C0C`。以下行号对应 2026-09-08 工作区快照。

类型与常量原文按头文件顺序列出；正文先解释用途，这里保留全部字段/枚举值，方便逐项对照。API 原型请沿文件链接阅读。

L9
```c
#define COFFEE3_RTU_BUS_H
```


L23
```c
typedef struct {
    UART_HandleTypeDef *pxUart;
    const char *pcName;
    uint32_t ulDefaultBaudRate;
    uint8_t ucBusId;
    uint8_t ucProtocolId;
} Coffee3RtuBusConfig_t;
```


L32
```c
typedef struct {
	uint32_t ulCommandCount;
	uint32_t ulErrorCount;
	uint32_t ulCurrentBaudRate;
	int32_t lLastResult;
	uint8_t ucReady;
	uint8_t ucActiveDevice;
} Coffee3RtuBusStatus_t;
```

### [Application/UserAPP/Coffee3CloseApp/Robot_Tcp/coffee3_robot_tcp.c](../../../../Application/UserAPP/Coffee3CloseApp/Robot_Tcp/coffee3_robot_tcp.c)

SHA-256：`F8974C38C788059F7AA766B05203D524E5841D2AC374F8E484356BA15747AFE7`。以下行号对应 2026-09-08 工作区快照。

| 定义行 | 函数 | 下行调用（词法检索，非运行时跟踪） |
| --- | --- | --- |
| 136 | `prvResolveCoffee3Dobot` | prvCoffee3DobotConfig, ucDobotRobotResolvePoint |
| 311 | `prvRobotLinkFailureConfirmed` | memset, pdMS_TO_TICKS, vModbusPortGetLastFault, vTaskDelay, xCoffee3LogWriteField, xModbusPortReadCoils |
| 383 | `prvFoldServerCommands` | vCoffee3DeviceCommandCompleted, vCoffee3DeviceCommandStarted, xCoffee3LogWriteField, xCoffee3LogWriteFieldOrder, xQueueReceive, xQueueSendToBack |
| 478 | `xCoffee3RobotTcpInitialize` | memset, vCoffee3DeviceRegisterRoute, xQueueCreateStatic |
| 494 | `prvLogRobotConnected` | memcpy, ucTransportTcpFormatIpv4Endpoint, xCoffee3LogWriteField |
| 517 | `vCoffee3RobotTcpTask` | COFFEE3_ROBOT_PROTOCOL_2, COFFEE3_ROBOT_PROTOCOL_3, else, endif, memcpy, memset, pdMS_TO_TICKS, prvAdvanceAction, prvDisconnectRobot, prvExecute, prvFoldServerCommands, prvReconcile, prvRefresh, prvResolveCoffee3Dobot, prvRetryDelayMs, prvRobotActionName, prvRobotBasicAction, prvRobotLinkFailureConfirmed, prvRobotOperational, prvRobotStartupStateMask, prvRobotStrictReady, prvSetRobotReady, prvStartup, ucCoffee3CommandIsCanceled, ucTcpClientSessionIsOnline, vAppTaskManagerWaitNetworkStackReady, vCoffee3DeviceCommandCompleted, vCoffee3DeviceCommandStarted, vCoffee3DeviceSetOnline, vCoffee3DeviceSetReady, vCoffee3DeviceSetRecovering, vCoffee3DeviceSetRobotPhase, vTaskDelay, vTcpClientSessionProcess, xCoffee3LogPrintfOrder, xCoffee3LogWrite, xCoffee3LogWriteField, xCoffee3LogWriteFieldOrder, xModbusPortClientInit, xQueuePeek, xQueueReceive, xTaskGetTickCount, xTcpClientSessionInit, xTransportTcpCreate |
| 1092 | `prvRefresh` | prvCoffee3DobotConfig, xModbusPortReadCoils, xModbusPortReadDiscreteInputs |
| 1127 | `prvRobotSessionNetworkReady` | ucAppTaskManagerIsNetworkReady |
| 1134 | `prvRobotSessionProbe` | prvRefresh |
| 1147 | `prvRobotSessionEvent` | memset, prvLogRobotConnected, prvRobotStartupStateMask, prvSetRobotReady, vCoffee3DeviceSetOnline, vCoffee3DeviceSetReady, vCoffee3LogLwipResourceFailure, vModbusPortGetLastFault, xCoffee3LogWriteField |
| 1265 | `prvRobotOperational` |  |
| 1284 | `prvRobotBasicAction` |  |
| 1293 | `prvRobotStrictReady` | prvRobotOperational |
| 1312 | `prvSetRobotReady` | vCoffee3DeviceSetReady, xCoffee3LogWriteField |
| 1331 | `prvRobotStartupStateMask` |  |
| 1377 | `prvClearActionCoils` | prvClearActionRange, prvCoffee3DobotConfig, xCoffee3LogWriteFieldOrder |
| 1411 | `prvClearActionRange` | xModbusPortReadCoils, xModbusPortWriteCoils |
| 1448 | `prvWriteControlValue` | xModbusPortWriteCoil |
| 1456 | `prvStartup` | pdMS_TO_TICKS, prvRefresh, prvRobotStartupStateMask, prvWriteControlValue, vTaskDelay, xCoffee3LogWriteField, xModbusPortWriteCoils, xTaskGetTickCount |
| 1618 | `prvReconcile` | prvCoffee3DobotConfig, prvRefresh, prvRobotActionName, vCoffee3DeviceSetRobotAccepted, vCoffee3DeviceSetRobotPhase, xCoffee3LogPrintfOrder, xCoffee3LogWriteFieldOrder, xModbusPortReadCoils, xModbusPortWriteCoil, xTaskGetTickCount |
| 1726 | `prvDisconnectRobot` | vTcpClientSessionForceReconnect |
| 1733 | `prvExecute` | prvClearActionCoils, prvRefresh, prvResolveCoffee3Dobot, prvRobotStrictReady, prvWriteControlValue, prvWriteRisingEdge, vCoffee3DeviceSetRobotAccepted, vCoffee3DeviceSetRobotPhase, vCoffee3ServerSelectStorage, xCoffee3LogWriteFieldOrder, xModbusPortReadCoils, xModbusPortWriteCoil, xTaskGetTickCount |
| 1857 | `prvWriteRisingEdge` | pdMS_TO_TICKS, vTaskDelay, xModbusPortWriteCoil |
| 1873 | `prvAdvanceAction` | pdMS_TO_TICKS, prvRobotActionName, ucCoffee3CommandIsCanceled, vCoffee3DeviceSetRobotAccepted, vCoffee3DeviceSetRobotPhase, xCoffee3LogPrintfOrder, xCoffee3LogWriteFieldOrder, xModbusPortReadCoils, xModbusPortWriteCoil, xTaskGetTickCount |
| 2025 | `prvRetryDelayMs` |  |
### [Application/UserAPP/Coffee3CloseApp/Robot_Tcp/coffee3_robot_tcp.h](../../../../Application/UserAPP/Coffee3CloseApp/Robot_Tcp/coffee3_robot_tcp.h)

SHA-256：`4D53C553EB2B33288D37E613170B71876D7AA8AD2F9420BF8CD7FFBDA15BD083`。以下行号对应 2026-09-08 工作区快照。

类型与常量原文按头文件顺序列出；正文先解释用途，这里保留全部字段/枚举值，方便逐项对照。API 原型请沿文件链接阅读。

L9
```c
#define COFFEE3_ROBOT_TCP_H
```


L23
```c
#define COFFEE3_ROBOT_CONTROL_COIL_COUNT  61U
```


L25
```c
#define COFFEE3_ROBOT_CONTROL_COIL_COUNT  40U
```


L29
```c
typedef struct {
	uint32_t ulConnectAttemptCount;
	uint32_t ulConnectSuccessCount;
	uint32_t ulDisconnectCount;
	uint32_t ulCommandCount;
	uint32_t ulErrorCount;
	uint32_t ulConsecutiveFailures;
	uint32_t ulNextRetryDelayMs;
	int32_t lLastResult;
	uint8_t ucConnected;
	uint8_t ucReady;
} Coffee3RobotTcpStatus_t;
```


L43
```c
typedef struct {
	uint8_t aucBaseInputs[16];
	uint8_t aucControlCoils[COFFEE3_ROBOT_CONTROL_COIL_COUNT];
} Coffee3RobotData_t;
```
