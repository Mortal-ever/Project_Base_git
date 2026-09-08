# 16 Coffee2Open 命令、Device、RTU 与 Robot 源码带读

本篇从 `coffee2_device.h` 开始，再读 device.c、rtu_bus.c、robot_tcp.c、device_image.c。类型和函数位置见末尾导航；使用的是本 Target 的私有命令，不是公共 AppCommand。

## 一条命令究竟是什么

`Coffee2Command_t` 是按值复制到队列的32字节消息，头文件的负长度 typedef 在大小错误时令编译失败。它不携带动态字符串，也不携带堆上业务对象指针。

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
2. `pxCoffee2DeviceGetBinding` 查 route，再取 `s_axRouteQueues`。没有注册队列说明 owner 尚未准备。
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

`xCoffee2DeviceWaitCommand(device,epoch,id,ticks)` 不只等事件位，还检查结果身份；`lCoffee2DeviceGetTerminalResult` 用相同键取得原始错误。附录可继续追静态终态history检索。普通 EventGroup 没有携带commandId，故不能只看DONE。

## 协作取消

`vCoffee2OrderCancelRequest` 只接受非零epoch。`ucCoffee2CommandIsCanceled` 对 Workflow 且epoch匹配才返回取消。被识别的安全停机（机器人/咖啡取消、制冰关阀、IO关闭）可以豁免；不是任意加 SAFETY_STOP 都能豁免。

## vCoffee2RtuBusTask 函数体

任务参数是静态 BusConfig。先校验Bus，建立UART通道并Open；Modbus线路再建立ModbusPort。初始化失败保留未创建状态，收到命令会以NOT_READY终结，不会假装成功。

主循环 `xQueueReceive` 取消息 → 校验route/protocol → Started → 取消检查 → 最小间隔等待 → `prvExecute` → 保存结果 → Completed。重试失败时延时50ms再试；成功和取消立即break。咖啡非REFRESH强制 `ucRetryLimit=0`，避免丢响应导致重复制作。

`prvExecute` 把产品枚举翻译成 F200、Cup/Lid、Syrup、Ice、Scale、Energy、IO公共API；驱动成功后提交镜像。没有分支支持的动作返回不支持，不能误诊为断线。

## Robot owner 的分段状态

IDLE=0、PREPARING=1、WAIT_ACCEPT=2、MOVING=3、CLEAR_RESULT=4、RECOVERING=5。私有点位表给出业务action对应的命令与结果线圈，公共 `ucDobotRobotResolvePoint` 负责查表解析，真正Modbus TCP读写在本篇Robot模块。

连接由公共 TcpClientSession 周期推进；任务创建不等于已ONLINE。执行动作要处理原有结果、发命令、等受理、等运动结束、清结果。Workflow读取 `ucRobotAccepted` 后才开始运动计时；恢复或未受理时会更新计时起点。因此模拟器一直不清受理线圈时，运动超时不能被误读为从最初入队一直倒计时。

## DeviceImage 是私有状态投影

`vCoffee2DeviceImageCommitF200` 将F200公共状态投影到Coffee2缓存；Cup/Lid按refresh区分整组任务/线圈复制与单slot更新。由RTU owner完成读取后调用，Workflow/Server消费私有图像。不要因图像的成员类型来自公共设备库就把实例搬进公共层。

## 建议断点与练习

在 submit 后记下32字节消息的id/epoch；在Bus取出时比对；在Completed和WaitCommand验证相同键。把debug IO值设0x0005，沿351分支看整幅输出而非点位编号5。观察队列失败、协议超时、设备未完成的三个不同出口。

下一篇读Workflow，理解“设备命令返回0以后，为什么还必须等传感器或设备状态”。
## 源码导航与版本证据

### [Application/UserAPP/Coffee2OpenApp/Device/coffee2_device_image.c](../../../../Application/UserAPP/Coffee2OpenApp/Device/coffee2_device_image.c)

SHA-256：`374A20B0BF00F134636F7A197C8BDFFC10A72FABB35C72E05E239179B1802554`。以下行号对应 2026-09-08 工作区快照。

| 定义行 | 函数 | 下行调用（词法检索，非运行时跟踪） |
| --- | --- | --- |
| 28 | `vCoffee2DeviceImageCommitF200` | memset |
| 56 | `vCoffee2DeviceImageCommitCup` | memcpy |
| 73 | `vCoffee2DeviceImageCommitLid` | memcpy |
### [Application/UserAPP/Coffee2OpenApp/Device/coffee2_device_image.h](../../../../Application/UserAPP/Coffee2OpenApp/Device/coffee2_device_image.h)

SHA-256：`02F44059AC5489077C68ED93C046AD9BA0D392CF47439E517A4E5EEE7EF85639`。以下行号对应 2026-09-08 工作区快照。

类型与常量原文按头文件顺序列出；正文先解释用途，这里保留全部字段/枚举值，方便逐项对照。API 原型请沿文件链接阅读。

L9
```c
#define COFFEE2_DEVICE_IMAGE_H
```


L23
```c
typedef struct {
	uint16_t ausStatus[24];
} Coffee2CoffeeMachineImage_t;
```


L27
```c
typedef struct {
	uint16_t ausCupTask[2];
	uint16_t ausLidTask[2];
	uint8_t aucCupCoils[10];
	uint8_t aucLidCoils[10];
} Coffee2CupLidImage_t;
```

### [Application/UserAPP/Coffee2OpenApp/Device/coffee2_device.c](../../../../Application/UserAPP/Coffee2OpenApp/Device/coffee2_device.c)

SHA-256：`6E137E54F105B02ACC67A40C8C51E3FEA99777A8BA41EFAC1878A4F05EC3811A`。以下行号对应 2026-09-08 工作区快照。

| 定义行 | 函数 | 下行调用（词法检索，非运行时跟踪） |
| --- | --- | --- |
| 69 | `prvTerminalBits` |  |
| 92 | `lCoffee2DeviceGetTerminalResult` | taskENTER_CRITICAL, taskEXIT_CRITICAL |
| 127 | `xCoffee2DeviceInitialize` | memset, xEventGroupCreateStatic |
| 154 | `vCoffee2DeviceRegisterRoute` |  |
| 180 | `xCoffee2CommandSubmit` | prvSubmit |
| 187 | `xCoffee2CommandSubmitUrgent` | prvSubmit |
| 194 | `vCoffee2OrderCancelRequest` | taskENTER_CRITICAL, taskEXIT_CRITICAL |
| 205 | `ucCoffee2CommandIsCanceled` |  |
| 230 | `prvSubmit` | pxCoffee2DeviceGetBinding, taskENTER_CRITICAL, taskEXIT_CRITICAL, vCoffee2WorkflowReleaseManual, xCoffee2WorkflowAcquireManual, xQueueSend, xQueueSendToFront |
| 284 | `vCoffee2DeviceCommandStarted` | taskENTER_CRITICAL, taskEXIT_CRITICAL, xCoffee2LogWriteFieldOrder, xEventGroupClearBits, xEventGroupSetBits |
| 325 | `vCoffee2DeviceCommandCompleted` | taskENTER_CRITICAL, taskEXIT_CRITICAL, vCoffee2WorkflowReleaseManual, xCoffee2LogWriteFieldOrder, xEventGroupClearBits, xEventGroupSetBits, xTaskGetTickCount |
| 464 | `vCoffee2DeviceSetRobotPhase` | taskENTER_CRITICAL, taskEXIT_CRITICAL |
| 476 | `vCoffee2DeviceSetRobotAccepted` | taskENTER_CRITICAL, taskEXIT_CRITICAL |
| 485 | `vCoffee2DeviceSetReady` | taskENTER_CRITICAL, taskEXIT_CRITICAL, xEventGroupClearBits, xEventGroupSetBits |
| 507 | `vCoffee2DeviceSetRecovering` | taskENTER_CRITICAL, taskEXIT_CRITICAL, xEventGroupClearBits, xEventGroupSetBits |
| 530 | `vCoffee2DeviceSetOnline` | taskENTER_CRITICAL, taskEXIT_CRITICAL, vCoffee2DeviceSetReady, xEventGroupClearBits, xEventGroupSetBits |
| 567 | `xCoffee2DeviceGetEvents` | xEventGroupGetBits |
| 578 | `xCoffee2DeviceWaitCommand` | pdMS_TO_TICKS, prvTerminalBits, taskENTER_CRITICAL, taskEXIT_CRITICAL, vTaskDelay, xEventGroupWaitBits, xTaskGetTickCount |
### [Application/UserAPP/Coffee2OpenApp/Device/coffee2_device.h](../../../../Application/UserAPP/Coffee2OpenApp/Device/coffee2_device.h)

SHA-256：`C4AACAD8AA0DAE4560024F7F501AE5B428EA1BCC81744CC4BE9025193A6C9C01`。以下行号对应 2026-09-08 工作区快照。

类型与常量原文按头文件顺序列出；正文先解释用途，这里保留全部字段/枚举值，方便逐项对照。API 原型请沿文件链接阅读。

L9
```c
#define COFFEE2_DEVICE_H
```


L23
```c
typedef enum {
	COFFEE2_DEVICE_NONE = 0,
	COFFEE2_DEVICE_ROBOT = 1,
	COFFEE2_DEVICE_COFFEE_MACHINE = 2,
	COFFEE2_DEVICE_CUP_MACHINE = 3,
	COFFEE2_DEVICE_SYRUP_MACHINE = 4,
	COFFEE2_DEVICE_LID_MACHINE = 5,
	COFFEE2_DEVICE_ICE_MACHINE = 6,
	COFFEE2_DEVICE_SCALE = 7,
	COFFEE2_DEVICE_POWER_METER = 8,
	COFFEE2_DEVICE_IO_INPUT = 9,
	COFFEE2_DEVICE_IO_OUTPUT = 10,
	COFFEE2_DEVICE_COUNT = 11
} Coffee2DeviceId_e;
```


L39
```c
typedef enum {
	COFFEE2_COMMAND_SOURCE_WORKFLOW = 0,
	COFFEE2_COMMAND_SOURCE_SERVER = 1,
	COFFEE2_COMMAND_SOURCE_MAINTENANCE = 2
} Coffee2CommandSource_e;
```


L46
```c
#define COFFEE2_COMMAND_FLAG_DEBUG 0x04U
```


L49
```c
typedef enum {
	COFFEE2_ROBOT_PHASE_IDLE = 0,
	COFFEE2_ROBOT_PHASE_PREPARING = 1,
	COFFEE2_ROBOT_PHASE_WAIT_ACCEPT = 2,
	COFFEE2_ROBOT_PHASE_MOVING = 3,
	COFFEE2_ROBOT_PHASE_CLEAR_RESULT = 4,
	COFFEE2_ROBOT_PHASE_RECOVERING = 5
} Coffee2RobotPhase_e;
```


L59
```c
typedef enum {
	COFFEE2_ACTION_REFRESH = 1, COFFEE2_ACTION_CANCEL = 2,
	COFFEE2_ACTION_RESET = 3, COFFEE2_ACTION_ROBOT_START = 100,
	COFFEE2_ACTION_ROBOT_STOP = 101, COFFEE2_ACTION_ROBOT_ENABLE = 102,
	COFFEE2_ACTION_ROBOT_CLEAR_ALARM = 103, COFFEE2_ACTION_ROBOT_PAUSE = 104,
	COFFEE2_ACTION_ROBOT_DISABLE = 105, COFFEE2_ACTION_ROBOT_ENTER_DRAG = 106,
	COFFEE2_ACTION_ROBOT_EXIT_DRAG = 107, COFFEE2_ACTION_ROBOT_AUTO_MODE = 108,
	COFFEE2_ACTION_ROBOT_MANUAL_MODE = 109, COFFEE2_ACTION_ROBOT_HOME = 110,
	COFFEE2_ACTION_ROBOT_TAKE_HOT_CUP = 111,
	COFFEE2_ACTION_ROBOT_TAKE_COLD_CUP = 112,
	COFFEE2_ACTION_ROBOT_TO_COFFEE = 113, COFFEE2_ACTION_ROBOT_TO_ICE = 114,
	COFFEE2_ACTION_ROBOT_TO_LID = 115, COFFEE2_ACTION_ROBOT_TAKE_LID = 116,
	COFFEE2_ACTION_ROBOT_COVER_LID = 117, COFFEE2_ACTION_ROBOT_PUT_OUTPUT = 118,
	COFFEE2_ACTION_ROBOT_PUT_STORAGE = 119,
	COFFEE2_ACTION_ROBOT_TO_PRINTER = 120,
	COFFEE2_ACTION_ROBOT_TAKE_OUTPUT_1 = 121,
	COFFEE2_ACTION_ROBOT_TAKE_OUTPUT_2 = 122,
	COFFEE2_ACTION_ROBOT_TAKE_COFFEE = 123,
	COFFEE2_ACTION_ROBOT_TAKE_STORAGE = 124,
	COFFEE2_ACTION_ROBOT_START_SIGNAL = 125,
	COFFEE2_ACTION_ROBOT_TO_FRUIT_SYRUP = 126,
	COFFEE2_ACTION_COFFEE_MAKE = 200, COFFEE2_ACTION_COFFEE_PAUSE = 201,
	COFFEE2_ACTION_COFFEE_RESUME = 202, COFFEE2_ACTION_COFFEE_CLEAN = 203,
	COFFEE2_ACTION_CUP_DROP_1 = 300, COFFEE2_ACTION_CUP_DROP_2 = 301,
	COFFEE2_ACTION_LID_DROP_1 = 310, COFFEE2_ACTION_LID_DROP_2 = 311,
	COFFEE2_ACTION_SYRUP_DISPENSE = 320,
	COFFEE2_ACTION_SYRUP_CLEAN = 321,
	COFFEE2_ACTION_SYRUP_SET_REMAINING = 322,
	COFFEE2_ACTION_ICE_SET_VALVE = 330, COFFEE2_ACTION_SCALE_TARE = 340,
	COFFEE2_ACTION_SCALE_CLEAR_TARE = 341, COFFEE2_ACTION_SCALE_ZERO = 342,
	COFFEE2_ACTION_IO_WRITE = 350,
	COFFEE2_ACTION_IO_WRITE_MASK = 351
} Coffee2Action_e;
```


L94
```c
#define COFFEE2_DEVICE_EVENT_ONLINE          (1UL << 0)
```


L95
```c
#define COFFEE2_DEVICE_EVENT_READY           (1UL << 1)
```


L96
```c
#define COFFEE2_DEVICE_EVENT_BUSY            (1UL << 2)
```


L97
```c
#define COFFEE2_DEVICE_EVENT_COMMAND_DONE    (1UL << 3)
```


L98
```c
#define COFFEE2_DEVICE_EVENT_COMMAND_FAILED  (1UL << 4)
```


L99
```c
#define COFFEE2_DEVICE_EVENT_TIMEOUT         (1UL << 5)
```


L100
```c
#define COFFEE2_DEVICE_EVENT_COMM_FAULT      (1UL << 6)
```


L101
```c
#define COFFEE2_DEVICE_EVENT_DEVICE_FAULT    (1UL << 7)
```


L102
```c
#define COFFEE2_DEVICE_EVENT_CANCELED        (1UL << 8)
```


L103
```c
#define COFFEE2_DEVICE_EVENT_DATA_UPDATED    (1UL << 9)
```


L104
```c
#define COFFEE2_DEVICE_EVENT_RECOVERING      (1UL << 10)
```


L107
```c
#define COFFEE2_COMMAND_RESULT_CANCELED       (-9)
```


L108
```c
#define COFFEE2_COMMAND_RESULT_SUPERSEDED     (-10)
```


L109
```c
#define COFFEE2_COMMAND_FLAG_SAFETY_STOP       0x01U
```


L110
```c
#define COFFEE2_COMMAND_FLAG_MANUAL_RESERVED   0x02U
```


L113
```c
#define COFFEE2_DEVICE_EVENT_TERMINAL         \
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
} Coffee2Command_t;
```


L138
```c
typedef struct {
	Coffee2DeviceId_e xDeviceId;
	uint8_t ucRouteId;
	uint8_t ucUnitId;
	uint16_t usMinimumIntervalMs;
	uint8_t ucCategory;
	uint8_t ucRole;
	uint8_t ucDriverId;
	uint8_t ucProtocolId;
	const char *pcName;
} Coffee2DeviceBinding_t;
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
} Coffee2DeviceStatus_t;
```

### [Application/UserAPP/Coffee2OpenApp/Modbus_Rtu_Bus/coffee2_rtu_bus.c](../../../../Application/UserAPP/Coffee2OpenApp/Modbus_Rtu_Bus/coffee2_rtu_bus.c)

SHA-256：`E4DBF5E9A3F2147A8AE7446FE6371E4C2A06C28EAC8D9974E14EE50C59C146EC`。以下行号对应 2026-09-08 工作区快照。

| 定义行 | 函数 | 下行调用（词法检索，非运行时跟踪） |
| --- | --- | --- |
| 129 | `xCoffee2SerialApplyDefaults` | HAL_UART_Abort, HAL_UART_DeInit, prvConfigureUart |
| 154 | `xCoffee2LogSerialApplyDefault` | prvConfigureUart |
| 160 | `xCoffee2RtuBusInitialize` | memset, vCoffee2DeviceRegisterRoute, xQueueCreateStatic |
| 194 | `vCoffee2RtuBusTask` | memset, pdMS_TO_TICKS, prvExecute, prvFindBusIndex, prvFindModbusPortIndex, prvGetDeviceLogSource, prvGetLogSource, prvLogCommandFailure, prvLogDeviceBindings, pxCoffee2DeviceGetBinding, ucCoffee2CommandIsCanceled, ucModbusPortResultIsLinkFailure, vCoffee2DeviceCommandCompleted, vCoffee2DeviceCommandStarted, vCoffee2DeviceSetOnline, vTaskDelay, vTaskDelete, xCoffee2LogWrite, xCoffee2LogWriteField, xCoffee2LogWriteFieldOrder, xModbusPortClientInit, xQueueReceive, xTaskGetTickCount, xTransportOpen, xTransportUartCreate |
| 370 | `prvConfigureUart` | HAL_UART_Abort, HAL_UART_DeInit, HAL_UART_Init |
| 394 | `prvExecute` | ld, memset, prvLogIoWrite, prvLogIoWriteExpected, prvMapF200Result, ucIceMachineGetFaultMask, vCoffee2DeviceImageCommitCup, vCoffee2DeviceImageCommitF200, vCoffee2DeviceImageCommitLid, vCoffee2IoCommitModbusInput, vCoffee2IoCommitModbusOutputImage, xCoffee2LogPrintfOrder, xCoffeeMachineF200Execute, xCoffeeMachineF200MapRecipe, xCupLidShengShuRefresh, xCupLidShengShuRun, xIceMachineRefresh, xIceMachineSetValve, xIoModuleModbusReadInputs, xIoModuleModbusReadOutputs, xIoModuleModbusWriteOutput, xPowerMeterDdsu666Refresh, xScaleBsqDgV2ClearTare, xScaleBsqDgV2Refresh, xScaleBsqDgV2Tare, xScaleBsqDgV2Zero, xSyrupMachineClean, xSyrupMachineDispense, xSyrupMachineRefresh, xSyrupMachineSetRemaining |
| 598 | `prvCommandCanceled` | ucCoffee2CommandIsCanceled |
| 605 | `prvMapF200Result` |  |
| 620 | `prvGetLogSource` |  |
| 635 | `prvGetDeviceLogSource` |  |
| 713 | `prvLogDeviceBindings` | prvGetBusLinkEvent, prvGetDeviceLogSource, prvGetDeviceProtocolEvent, pxCoffee2DeviceGetBinding, xCoffee2LogWriteField |
| 741 | `prvFindBusIndex` |  |
| 754 | `prvFindModbusPortIndex` |  |
| 775 | `prvLogCommandFailure` | prvGetDeviceLogSource, prvGetDeviceName, xCoffee2LogPrintfOrder |
| 792 | `prvLogIoWriteExpected` | xCoffee2LogWriteFieldOrder |
| 809 | `prvLogIoWrite` | xCoffee2LogWriteFieldOrder |
### [Application/UserAPP/Coffee2OpenApp/Modbus_Rtu_Bus/coffee2_rtu_bus.h](../../../../Application/UserAPP/Coffee2OpenApp/Modbus_Rtu_Bus/coffee2_rtu_bus.h)

SHA-256：`A1679A9C4C61AB08C4A49AE15D175FAA19E06834FADA4FAB773DB479D25F8139`。以下行号对应 2026-09-08 工作区快照。

类型与常量原文按头文件顺序列出；正文先解释用途，这里保留全部字段/枚举值，方便逐项对照。API 原型请沿文件链接阅读。

L9
```c
#define COFFEE2_RTU_BUS_H
```


L23
```c
typedef struct {
    UART_HandleTypeDef *pxUart;
    const char *pcName;
    uint32_t ulDefaultBaudRate;
    uint8_t ucBusId;
    uint8_t ucProtocolId;
} Coffee2RtuBusConfig_t;
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
} Coffee2RtuBusStatus_t;
```

### [Application/UserAPP/Coffee2OpenApp/Robot_Tcp/coffee2_robot_tcp.c](../../../../Application/UserAPP/Coffee2OpenApp/Robot_Tcp/coffee2_robot_tcp.c)

SHA-256：`B2D47621F017F069B58592F45A947074CF93C7889A969A78B148904F3EBF10BB`。以下行号对应 2026-09-08 工作区快照。

| 定义行 | 函数 | 下行调用（词法检索，非运行时跟踪） |
| --- | --- | --- |
| 135 | `prvResolveCoffee2Dobot` | prvCoffee2DobotConfig, ucDobotRobotResolvePoint |
| 310 | `prvRobotLinkFailureConfirmed` | memset, pdMS_TO_TICKS, vModbusPortGetLastFault, vTaskDelay, xCoffee2LogWriteField, xModbusPortReadCoils |
| 382 | `prvFoldServerCommands` | vCoffee2DeviceCommandCompleted, vCoffee2DeviceCommandStarted, xCoffee2LogWriteField, xCoffee2LogWriteFieldOrder, xQueueReceive, xQueueSendToBack |
| 477 | `xCoffee2RobotTcpInitialize` | memset, vCoffee2DeviceRegisterRoute, xQueueCreateStatic |
| 493 | `prvLogRobotConnected` | memcpy, ucTransportTcpFormatIpv4Endpoint, xCoffee2LogWriteField |
| 516 | `vCoffee2RobotTcpTask` | COFFEE2_ROBOT_PROTOCOL_2, COFFEE2_ROBOT_PROTOCOL_3, else, endif, memcpy, memset, pdMS_TO_TICKS, prvAdvanceAction, prvDisconnectRobot, prvExecute, prvFoldServerCommands, prvReconcile, prvRefresh, prvResolveCoffee2Dobot, prvRetryDelayMs, prvRobotActionName, prvRobotBasicAction, prvRobotLinkFailureConfirmed, prvRobotOperational, prvRobotStartupStateMask, prvRobotStrictReady, prvSetRobotReady, prvStartup, ucCoffee2CommandIsCanceled, ucTcpClientSessionIsOnline, vAppTaskManagerWaitNetworkStackReady, vCoffee2DeviceCommandCompleted, vCoffee2DeviceCommandStarted, vCoffee2DeviceSetOnline, vCoffee2DeviceSetReady, vCoffee2DeviceSetRecovering, vCoffee2DeviceSetRobotPhase, vTaskDelay, vTcpClientSessionProcess, xCoffee2LogPrintfOrder, xCoffee2LogWrite, xCoffee2LogWriteField, xCoffee2LogWriteFieldOrder, xModbusPortClientInit, xQueuePeek, xQueueReceive, xTaskGetTickCount, xTcpClientSessionInit, xTransportTcpCreate |
| 1091 | `prvRefresh` | prvCoffee2DobotConfig, xModbusPortReadCoils, xModbusPortReadDiscreteInputs |
| 1126 | `prvRobotSessionNetworkReady` | ucAppTaskManagerIsNetworkReady |
| 1133 | `prvRobotSessionProbe` | prvRefresh |
| 1146 | `prvRobotSessionEvent` | memset, prvLogRobotConnected, prvRobotStartupStateMask, prvSetRobotReady, vCoffee2DeviceSetOnline, vCoffee2DeviceSetReady, vCoffee2LogLwipResourceFailure, vModbusPortGetLastFault, xCoffee2LogWriteField |
| 1264 | `prvRobotOperational` |  |
| 1283 | `prvRobotBasicAction` |  |
| 1292 | `prvRobotStrictReady` | prvRobotOperational |
| 1311 | `prvSetRobotReady` | vCoffee2DeviceSetReady, xCoffee2LogWriteField |
| 1330 | `prvRobotStartupStateMask` |  |
| 1376 | `prvClearActionCoils` | prvClearActionRange, prvCoffee2DobotConfig, xCoffee2LogWriteFieldOrder |
| 1410 | `prvClearActionRange` | xModbusPortReadCoils, xModbusPortWriteCoils |
| 1447 | `prvWriteControlValue` | xModbusPortWriteCoil |
| 1455 | `prvStartup` | pdMS_TO_TICKS, prvRefresh, prvRobotStartupStateMask, prvWriteControlValue, vTaskDelay, xCoffee2LogWriteField, xModbusPortWriteCoils, xTaskGetTickCount |
| 1617 | `prvReconcile` | prvCoffee2DobotConfig, prvRefresh, prvRobotActionName, vCoffee2DeviceSetRobotAccepted, vCoffee2DeviceSetRobotPhase, xCoffee2LogPrintfOrder, xCoffee2LogWriteFieldOrder, xModbusPortReadCoils, xModbusPortWriteCoil, xTaskGetTickCount |
| 1725 | `prvDisconnectRobot` | vTcpClientSessionForceReconnect |
| 1732 | `prvExecute` | prvClearActionCoils, prvRefresh, prvResolveCoffee2Dobot, prvRobotStrictReady, prvWriteControlValue, prvWriteRisingEdge, vCoffee2DeviceSetRobotAccepted, vCoffee2DeviceSetRobotPhase, xCoffee2LogWriteFieldOrder, xModbusPortReadCoils, xModbusPortWriteCoil, xTaskGetTickCount |
| 1848 | `prvWriteRisingEdge` | pdMS_TO_TICKS, vTaskDelay, xModbusPortWriteCoil |
| 1864 | `prvAdvanceAction` | pdMS_TO_TICKS, prvRobotActionName, ucCoffee2CommandIsCanceled, vCoffee2DeviceSetRobotAccepted, vCoffee2DeviceSetRobotPhase, xCoffee2LogPrintfOrder, xCoffee2LogWriteFieldOrder, xModbusPortReadCoils, xModbusPortWriteCoil, xTaskGetTickCount |
| 2016 | `prvRetryDelayMs` |  |
### [Application/UserAPP/Coffee2OpenApp/Robot_Tcp/coffee2_robot_tcp.h](../../../../Application/UserAPP/Coffee2OpenApp/Robot_Tcp/coffee2_robot_tcp.h)

SHA-256：`2CE866A65C12E1DBAED5CCCE13175F3E5189DABB95BEF9AA4467FE6600AC0390`。以下行号对应 2026-09-08 工作区快照。

类型与常量原文按头文件顺序列出；正文先解释用途，这里保留全部字段/枚举值，方便逐项对照。API 原型请沿文件链接阅读。

L9
```c
#define COFFEE2_ROBOT_TCP_H
```


L23
```c
#define COFFEE2_ROBOT_CONTROL_COIL_COUNT  61U
```


L25
```c
#define COFFEE2_ROBOT_CONTROL_COIL_COUNT  40U
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
} Coffee2RobotTcpStatus_t;
```


L43
```c
typedef struct {
	uint8_t aucBaseInputs[16];
	uint8_t aucControlCoils[COFFEE2_ROBOT_CONTROL_COIL_COUNT];
} Coffee2RobotData_t;
```
