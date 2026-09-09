# 18 Coffee3Close 启动、配置、任务与设备绑定

阅读顺序：Config → manager.h → manager.c → Device 初始化 → RTU/Robot 队列。本文适用 Coffee3Close，2026-09-08 当前源码；纳入构建不等于本轮构建通过。

## 从你打开的 manager.c 开始

`xAppTaskManagerCreateTasks()` 是产品的组合入口。函数返回 OK 说明软件对象创建成功；并不说明真实设备已完成残杯检查或机器人已准备好。后一个条件属于 Workflow。

执行顺序如下：

1. 检查 `s_xStatus.ucInfrastructureCreated`，非零立即返回 ALREADY_CREATED，防止重复建任务。
2. 清零状态，`prvCaptureResetCause()` 读取 RCC 标志后清除，保留到 `ulResetCause`。
3. `vTransportManagerInit()` 初始化公共通道管理；应用日志串口配置，然后建立日志缓存/输出通道。早期失败走 `prvWriteRawStartupFailure`，此时不能假设日志消费任务存在。
4. 应用各串口配置。SERIAL_INIT 为 -3，MODULE_INIT 为 -4；每个失败分支立即返回，不继续依赖失败对象。
5. 建立网络 readiness EventGroup。STACK_READY 表示 lwIP 初始化已返回，NETWORK_READY 还要求链路、netif 和 IPv4 有效。
6. 依次初始化 Device、IO、RTU Bus、Robot、Workflow、Server。RTU/Robot 在此建立队列并向 Device 注册 route，任务运行后才打开传输。
7. 保存建任务前剩余堆。Server 优先级 idle+3；Robot、4 路 RTU、Workflow 为 idle+2。每个后续创建受前一步 `pdPASS` 约束。
8. 日志 Transport 可用且核心任务创建成功时才尝试日志任务；日志失败状态单独记录。保存建任务后堆，再置 infrastructure/tasks 标志。

`prvCreateTaskLogged` 包装 `xTaskCreate`，不是创建一个新任务管理线程。栈深度单位是 `StackType_t` 个数；在本 32 位端口上，256 深度对应 1024 字节栈空间。动态任务内存来自 FreeRTOS 堆，不要把静态设备对象误解为全部任务静态创建。

## AppTaskManagerStatus_t 逐字段

| 字段 | 写入和意义 | 读取用途 |
| --- | --- | --- |
| xStartResult | 初始化函数设置的最终分类 | 判断软件启动在哪层失败 |
| ulResetCause | RCC 原因位快照 | 区分软件复位、看门狗、电源等；可能多位同时置位 |
| ulTaskCreatedMask | 成功创建任务累加位 | 对照 LOG/SERVER/ROBOT/BUS2..5/WORKFLOW |
| ulTaskFailedMask | 失败创建任务位 | 找到资源分配失败对象 |
| ulFreeHeapBeforeTasks / ulFreeHeapAfterTasks | 两个采样点 | 差值反映创建消耗，不是实时最小剩余堆 |
| ucInfrastructureCreated / ucTasksCreated | 软件创建过程终态 | 幂等入口、启动诊断 |
| ucLogReady | 输出通道和消费任务 readiness | 不能仅看缓存已经建立 |
| ucDeviceReady / ucRtuReady / ucRobotReady | 模块初始化返回成功 | 不等于实际设备 online |
| ucWorkflowReady / ucServerReady | 软件模块可启动 | 不等于初始化业务通过/客户端在线 |
| ucNetworkStackReady | 默认任务完成栈初始化后发布 | 网络任务启动等待 |
| ucNetworkReady | 默认任务周期重新评估 | 断网后也能退回未就绪 |

`vAppTaskManagerGetStatus` 复制状态给调用者；不要保存函数局部变量地址。RTU 任务参数是长期有效的静态配置表元素指针，不能改成启动函数里的临时配置。

## 默认任务做什么

`vAppTaskManagerRunDefaultTask()` 在 lwIP 初始化之后应用本产品网络地址、发布 STACK_READY，周期检查链路及地址，驱动指示灯。`vAppTaskManagerWaitNetworkStackReady()` 等的是事件，不是固定延时。默认任务循环与 RTU/Robot/Workflow 并行；源码的函数排列顺序不是调度顺序。

## Config 三类文件各管什么

`coffee3_app_config.h` 定义网络地址、串口波特率、任务栈、超时、协议选择和 OTA 分区。`coffee3_device_bindings.h` 将私有设备编号绑定到 route/unit/驱动。`coffee3_io_names.h`（以附录实际文件为准）维护产品点位名称；公共 IO 模块不应知道“出餐门”。

### 设备编号不是 Modbus 从站号

| 设备 ID | 意义 | Route | Unit |
| --- | --- | --- | --- |
| 1 | Robot | 0，Robot TCP | 配置宏 |
| 2 | 咖啡机 M50 | 2 | 1 |
| 3 | Cup | 3 | 1 |
| 4 | Syrup | 3 | 2 |
| 5 | Lid | 3 | 1，与 Cup 共享控制器但角色不同 |
| 6 | Ice | 4 | 1 |
| 7 | Scale | 4 | 2 |
| 8 | EnergyMeter | 3 | 3 |
| 9 | IoInput16 | 5 | 1 |
| 10 | IoOutput16 | 5 | 2 |

DeviceBinding 的 `xDeviceId` 用于产品查表；`ucRouteId` 选队列；`ucUnitId` 才进入下行协议。`usMinimumIntervalMs` 控制事务间隔；Category/Role/Driver/Protocol 四字段分别描述类别、作用、型号和线路协议。`pcName` 是静态诊断名。绑定表不能按编号直接假设数组下标，阅读 `pxCoffee3DeviceGetBinding` 的实际查找逻辑。

## 内存和两个工具链

manager 定义条件编译的 `ucHeap[configTOTAL_HEAP_SIZE]`，使用 CCM 段宏。CCM 可供 CPU 任务栈/堆使用，不能据此把栈上的接收缓冲直接交给 UART DMA。公共 Transport 的 DMA 存储归属见第05册。

本产品的 `USE_COFFEE3` 选中自己的 manager。Coffee2/Coffee3 暴露相同 `xAppTaskManagerCreateTasks` 符号，只能选择一个产品实现进入一次链接；不要同时纳入两个 manager。

## 调试练习

在 `xAppTaskManagerCreateTasks` 入口、每个模块初始化失败 return、`prvCreateTaskLogged`、`prvPublishNetworkReady` 设断点。先观察 task mask 和 heap，再观察 NETWORK_READY，最后观察 Workflow 的初始化完成标志。解释为什么 SERVER 模块已 ready 仍可能没有 TCP 客户端。

下一篇阅读命令对象和 owner 队列，追踪“Device 已初始化”怎样变成一次设备动作。
## 源码导航与版本证据

### [Application/UserAPP/Coffee3CloseApp/Config/coffee3_app_config.h](../../../../Application/UserAPP/Coffee3CloseApp/Config/coffee3_app_config.h)

SHA-256：`1111EE39EAD5CE6F35140A349B8E3B5639914AF6F270CE2D5F3DA3E5B027EF00`。以下行号对应 2026-09-08 工作区快照。

类型与常量原文按头文件顺序列出；正文先解释用途，这里保留全部字段/枚举值，方便逐项对照。API 原型请沿文件链接阅读。

L9
```c
#define COFFEE3_APP_CONFIG_H
```


L14
```c
#define COFFEE3_DEVICE_VERSION_EVENT         "FW_VERSION:Coffee3CloseV3.0.0"
```


L16
```c
#define COFFEE3_COFFEE_RECIPE_MAX            0x0021U
```


L19
```c
#define COFFEE3_CCM_DATA APP_CCM_DATA
```


L22
```c
#define COFFEE3_IP_ADDRESS_0                 192U
```


L23
```c
#define COFFEE3_IP_ADDRESS_1                 168U
```


L24
```c
#define COFFEE3_IP_ADDRESS_2                 5U
```


L25
```c
#define COFFEE3_IP_ADDRESS_3                 10U
```


L26
```c
#define COFFEE3_NETMASK_0                    255U
```


L27
```c
#define COFFEE3_NETMASK_1                    255U
```


L28
```c
#define COFFEE3_NETMASK_2                    255U
```


L29
```c
#define COFFEE3_NETMASK_3                    0U
```


L30
```c
#define COFFEE3_GATEWAY_0                    192U
```


L31
```c
#define COFFEE3_GATEWAY_1                    168U
```


L32
```c
#define COFFEE3_GATEWAY_2                    5U
```


L33
```c
#define COFFEE3_GATEWAY_3                    1U
```


L36
```c
#define COFFEE3_SERVER_PORT                  6001U
```


L37
```c
#define COFFEE3_SERVER_UNIT_ID               1U
```


L38
```c
#define COFFEE3_SERVER_MAX_CLIENTS           4U
```


L39
```c
#define COFFEE3_SERVER_POLL_MS               20U
```


L40
```c
#define COFFEE3_SERVER_BYTE_TIMEOUT_MS       100U
```


L43
```c
#define COFFEE3_ROBOT_IP_0                   192U
```


L44
```c
#define COFFEE3_ROBOT_IP_1                   168U
```


L45
```c
#define COFFEE3_ROBOT_IP_2                   5U
```


L46
```c
#define COFFEE3_ROBOT_IP_3                   1U
```


L47
```c
#define COFFEE3_ROBOT_PORT                   502U
```


L48
```c
#define COFFEE3_ROBOT_UNIT_ID                1U
```


L49
```c
#define COFFEE3_ROBOT_CONNECT_TIMEOUT_MS     3000U
```


L50
```c
#define COFFEE3_ROBOT_IO_TIMEOUT_MS          1000U
```


L51
```c
#define COFFEE3_ROBOT_LOOP_MS                20U
```


L52
```c
#define COFFEE3_ROBOT_ACTION_POLL_MS        100U
```


L53
```c
#define COFFEE3_ROBOT_ACCEPT_LOG_INTERVAL_MS 5000U
```


L54
```c
#define COFFEE3_ROBOT_MOTION_TIMEOUT_MS     60000U
```


L55
```c
#define COFFEE3_ROBOT_EDGE_LOW_MS           50U
```


L56
```c
#define COFFEE3_ROBOT_RETRY_MAX_MS          30000U
```


L57
```c
#define COFFEE3_ROBOT_READY_SAMPLES         3U
```


L60
```c
#define COFFEE3_ROBOT_PROTOCOL_1             0U
```


L61
```c
#define COFFEE3_ROBOT_PROTOCOL_2             1U
```


L62
```c
#define COFFEE3_ROBOT_PROTOCOL_3             2U
```


L63
```c
#define COFFEE3_ROBOT_PROTOCOL_VARIANT       COFFEE3_ROBOT_PROTOCOL_1
```


L66
```c
#define COFFEE3_BUS_PROTOCOL_MODBUS_RTU       1U
```


L67
```c
#define COFFEE3_BUS2_PROTOCOL                 COFFEE3_BUS_PROTOCOL_MODBUS_RTU
```


L68
```c
#define COFFEE3_BUS3_PROTOCOL                 COFFEE3_BUS_PROTOCOL_MODBUS_RTU
```


L69
```c
#define COFFEE3_BUS4_PROTOCOL                 COFFEE3_BUS_PROTOCOL_MODBUS_RTU
```


L70
```c
#define COFFEE3_BUS5_PROTOCOL                 COFFEE3_BUS_PROTOCOL_MODBUS_RTU
```


L71
```c
#define COFFEE3_MODBUS_BUS_COUNT              \
```


L78
```c
#define COFFEE3_RTU_BUS_COUNT                4U
```


L79
```c
#define COFFEE3_COMMAND_QUEUE_LENGTH         4U
```


L80
```c
#define COFFEE3_RTU_IO_TIMEOUT_MS            500U
```


L81
```c
#define COFFEE3_RTU_IDLE_MS                  20U
```


L84
```c
#define COFFEE3_BUS2_DEFAULT_BAUD            115200U
```


L85
```c
#define COFFEE3_BUS3_DEFAULT_BAUD            9600U
```


L86
```c
#define COFFEE3_BUS4_DEFAULT_BAUD            19200U
```


L87
```c
#define COFFEE3_BUS5_DEFAULT_BAUD            38400U
```


L88
```c
#define COFFEE3_LOG_BAUD                     115200U
```


L91
```c
#define COFFEE3_IO_MIN_FRAME_INTERVAL_MS     20U
```


L92
```c
#define COFFEE3_ICE_MIN_FRAME_INTERVAL_MS    100U
```


L93
```c
#define COFFEE3_EXTERNAL_IO_POINT_COUNT       16U
```


L96
```c
#define COFFEE3_LOG_TASK_STACK               256U
```


L97
```c
#define COFFEE3_SERVER_TASK_STACK            1536U
```


L98
```c
#define COFFEE3_ROBOT_TASK_STACK             1024U
```


L99
```c
#define COFFEE3_RTU_TASK_STACK               384U
```


L100
```c
#define COFFEE3_WORKFLOW_TASK_STACK          1024U
```


L103
```c
#define COFFEE3_WORKFLOW_QUEUE_LENGTH        2U
```


L104
```c
#define COFFEE3_WORKFLOW_IO_REFRESH_MS       100U
```


L105
```c
#define COFFEE3_WORKFLOW_DEFAULT_TIMEOUT_MS  30000U
```


L106
```c
#define COFFEE3_COFFEE_ACTION_TIMEOUT_MS      180000U
```


L107
```c
#define COFFEE3_OUTLET_EMPTY_HOLD_MS          30000U
```


L108
```c
#define COFFEE3_DOOR_MOTION_TIMEOUT_MS        5000U
```


L109
```c
#define COFFEE3_IO_STALE_MS                  2000U
```


L111
```c
#define COFFEE3_SYRUP_TIME_PER_VOLUME_UNIT    1U
```


L113
```c
#define COFFEE3_HOT_WATER_FILL_TIMEOUT_MS     120000U
```


L114
```c
#define COFFEE3_HOT_WATER_DEFAULT_HEAT_MIN    30U
```


L115
```c
#define COFFEE3_HOT_WATER_MAX_HEAT_MIN        120U
```


L116
```c
#define COFFEE3_FRUIT_MILK_MS_PER_ML          100U
```


L117
```c
#define COFFEE3_FRUIT_MILK_CLEAN_MS           15000U
```


L118
```c
#define COFFEE3_WORKFLOW_DEVICE_IO_TIMEOUT_MS  1000U
```


L119
```c
#define COFFEE3_WORKFLOW_IO_ACTION_TIMEOUT_MS 5000U
```


L122
```c
#define COFFEE3_ICE_SLOPE_MS_PER_GRAM         18L
```


L123
```c
#define COFFEE3_ICE_OFFSET_MS                 (-300L)
```


L124
```c
#define COFFEE3_ICE_COMPENSATION_FACTOR      1L
```


L125
```c
#define COFFEE3_ICE_MIN_PULSE_MS              200U
```


L126
```c
#define COFFEE3_ICE_MAX_PULSE_MS              2000U
```


L127
```c
#define COFFEE3_ICE_SETTLE_MS                 1000U
```


L128
```c
#define COFFEE3_ICE_TOLERANCE_DECIGRAM        20L
```


L129
```c
#define COFFEE3_ICE_MAX_CORRECTIONS           2U
```

### [Application/UserAPP/Coffee3CloseApp/Config/coffee3_build_config.h](../../../../Application/UserAPP/Coffee3CloseApp/Config/coffee3_build_config.h)

SHA-256：`38D1C5C209B47E07601977254BDC5536AA4D494D71667DC2C88FD87177E1AC9D`。以下行号对应 2026-09-08 工作区快照。

类型与常量原文按头文件顺序列出；正文先解释用途，这里保留全部字段/枚举值，方便逐项对照。API 原型请沿文件链接阅读。

L14
```c
#define COFFEE3_BUILD_CONFIG_H
```


L17
```c
#define NANOMODBUS_CONFIG_H
```


L19
```c
#define NANOMODBUS_CFG_CLIENT_ENABLED       1
```


L20
```c
#define NANOMODBUS_CFG_SERVER_ENABLED       1
```

### [Application/UserAPP/Coffee3CloseApp/Config/coffee3_device_bindings.h](../../../../Application/UserAPP/Coffee3CloseApp/Config/coffee3_device_bindings.h)

SHA-256：`1A4D11A20A15A3A950C74A42BC7577F3A765462F089B240494A3C30F49C345CC`。以下行号对应 2026-09-08 工作区快照。

类型与常量原文按头文件顺序列出；正文先解释用途，这里保留全部字段/枚举值，方便逐项对照。API 原型请沿文件链接阅读。

L8
```c
#define COFFEE3_DEVICE_BINDINGS_H
```


L12
```c
#define COFFEE3_ROBOT_DRIVER_ID \
```


L16
```c
#define COFFEE3_ROBOT_DRIVER_ID \
```


L19
```c
#define COFFEE3_ROBOT_DRIVER_ID \
```

### [Application/UserAPP/Coffee3CloseApp/Config/coffee3_io_names.h](../../../../Application/UserAPP/Coffee3CloseApp/Config/coffee3_io_names.h)

SHA-256：`7FC3707455C46DCF1BCA17AEA3E7D67BB7718B8659FC142F2FF3DA4C4BEECB03`。以下行号对应 2026-09-08 工作区快照。

类型与常量原文按头文件顺序列出；正文先解释用途，这里保留全部字段/枚举值，方便逐项对照。API 原型请沿文件链接阅读。

L7
```c
#define COFFEE3_IO_NAMES_H
```


L9
```c
#define COFFEE3_IO_NAME_LOCAL_DI_1   "DI1_OUTLET_DOOR_UPPER_LIMIT"
```


L10
```c
#define COFFEE3_IO_NAME_LOCAL_DI_2   "DI2_OUTLET_DOOR_LOWER_LIMIT"
```


L11
```c
#define COFFEE3_IO_NAME_LOCAL_DI_3   "DI3_COFFEE_WATER_TANK_HIGH_LEVEL"
```


L12
```c
#define COFFEE3_IO_NAME_LOCAL_DI_4   "DI4_COFFEE_WATER_TANK_LOW_LEVEL"
```


L13
```c
#define COFFEE3_IO_NAME_LOCAL_DI_5   "DI5_HOT_WATER_TANK_HIGH_LEVEL"
```


L14
```c
#define COFFEE3_IO_NAME_LOCAL_DI_6   "DI6_HOT_WATER_TANK_LOW_LEVEL"
```


L15
```c
#define COFFEE3_IO_NAME_LOCAL_DI_7   "DI7_RESERVED"
```


L16
```c
#define COFFEE3_IO_NAME_LOCAL_DI_8   "DI8_RESERVED"
```


L18
```c
#define COFFEE3_IO_NAME_LOCAL_DO_1   "DO1_OUTLET_DOOR_UP"
```


L19
```c
#define COFFEE3_IO_NAME_LOCAL_DO_2   "DO2_OUTLET_DOOR_DOWN"
```


L20
```c
#define COFFEE3_IO_NAME_LOCAL_DO_3   "DO3_HOT_WATER_SUPPLY_PUMP"
```


L21
```c
#define COFFEE3_IO_NAME_LOCAL_DO_4   "DO4_COFFEE_WATER_TANK_SUPPLY_PUMP"
```


L22
```c
#define COFFEE3_IO_NAME_LOCAL_DO_5   "DO5_RESERVED"
```


L23
```c
#define COFFEE3_IO_NAME_LOCAL_DO_6   "DO6_RESERVED"
```


L24
```c
#define COFFEE3_IO_NAME_LOCAL_DO_7   "DO7_RESERVED"
```


L25
```c
#define COFFEE3_IO_NAME_LOCAL_DO_8   "DO8_RESERVED"
```


L27
```c
#define COFFEE3_IO_NAME_MB1_DI_1    "X1_FINISHED_FRONT_CUP"
```


L28
```c
#define COFFEE3_IO_NAME_MB1_DI_2    "X2_FINISHED_REAR_CUP"
```


L29
```c
#define COFFEE3_IO_NAME_MB1_DI_3    "X3_OUTLET_CUP"
```


L30
```c
#define COFFEE3_IO_NAME_MB1_DI_4    "X4_PURE_WATER_BUCKET1_LOW_LEVEL"
```


L31
```c
#define COFFEE3_IO_NAME_MB1_DI_5    "X5_WASTE_RESIDUE_BIN"
```


L32
```c
#define COFFEE3_IO_NAME_MB1_DI_6    "X6_WASTE_LIQUID_BUCKET"
```


L33
```c
#define COFFEE3_IO_NAME_MB1_DI_7    "X7_WASTE_LIQUID_HIGH_LEVEL"
```


L34
```c
#define COFFEE3_IO_NAME_MB1_DI_8    "X8_OUTLET_SAFETY_LIGHT_CURTAIN"
```


L35
```c
#define COFFEE3_IO_NAME_MB1_DI_9    "X9_RESERVED"
```


L36
```c
#define COFFEE3_IO_NAME_MB1_DI_10   "X10_RESERVED"
```


L37
```c
#define COFFEE3_IO_NAME_MB1_DI_11   "X11_MILK_BOX_R1_LEVEL"
```


L38
```c
#define COFFEE3_IO_NAME_MB1_DI_12   "X12_FRUIT_DRINK_A_R2_LEVEL"
```


L39
```c
#define COFFEE3_IO_NAME_MB1_DI_13   "X13_FRUIT_DRINK_B_R3_LEVEL"
```


L40
```c
#define COFFEE3_IO_NAME_MB1_DI_14   "X14_RESERVED"
```


L41
```c
#define COFFEE3_IO_NAME_MB1_DI_15   "X15_RESERVED"
```


L42
```c
#define COFFEE3_IO_NAME_MB1_DI_16   "X16_RESERVED"
```


L44
```c
#define COFFEE3_IO_NAME_MB2_DO_1    "Y1_WATER_HEATER_RELAY"
```


L45
```c
#define COFFEE3_IO_NAME_MB2_DO_2    "Y2_RESERVED"
```


L46
```c
#define COFFEE3_IO_NAME_MB2_DO_3    "Y3_RESERVED"
```


L47
```c
#define COFFEE3_IO_NAME_MB2_DO_4    "Y4_MILK_BOX_R1_SOLENOID_VALVE"
```


L48
```c
#define COFFEE3_IO_NAME_MB2_DO_5    "Y5_FRUIT_DRINK_A_R2_SOLENOID_VALVE"
```


L49
```c
#define COFFEE3_IO_NAME_MB2_DO_6    "Y6_FRUIT_DRINK_B_R3_SOLENOID_VALVE"
```


L50
```c
#define COFFEE3_IO_NAME_MB2_DO_7    "Y7_RESERVED"
```


L51
```c
#define COFFEE3_IO_NAME_MB2_DO_8    "Y8_RESERVED"
```


L52
```c
#define COFFEE3_IO_NAME_MB2_DO_9    "Y9_RESERVED"
```


L53
```c
#define COFFEE3_IO_NAME_MB2_DO_10   "Y10_FRUIT_DRINK_A_PERISTALTIC_PUMP"
```


L54
```c
#define COFFEE3_IO_NAME_MB2_DO_11   "Y11_FRUIT_DRINK_B_PERISTALTIC_PUMP"
```


L55
```c
#define COFFEE3_IO_NAME_MB2_DO_12   "Y12_RESERVED"
```


L56
```c
#define COFFEE3_IO_NAME_MB2_DO_13   "Y13_RESERVED"
```


L57
```c
#define COFFEE3_IO_NAME_MB2_DO_14   "Y14_RESERVED"
```


L58
```c
#define COFFEE3_IO_NAME_MB2_DO_15   "Y15_RESERVED"
```


L59
```c
#define COFFEE3_IO_NAME_MB2_DO_16   "Y16_RESERVED"
```

### [Application/UserAPP/Coffee3CloseApp/Task_Manager/coffee3_manager.c](../../../../Application/UserAPP/Coffee3CloseApp/Task_Manager/coffee3_manager.c)

SHA-256：`21C1C91A59A66A5C72BF66658A0F7355513C793D07E1BF9760006136EBDBE0F8`。以下行号对应 2026-09-08 工作区快照。

| 定义行 | 函数 | 下行调用（词法检索，非运行时跟踪） |
| --- | --- | --- |
| 115 | `xAppTaskManagerCreateTasks` | lCoffee3LogEarlyWrite, memset, prvCaptureResetCause, prvCreateTaskLogged, prvGetBusLogSource, prvWriteRawStartupFailure, pxCoffee3RtuBusGetConfig, vCoffee3IoInitialize, vCoffee3LogSetTaskReady, vTransportManagerInit, xCoffee3DeviceInitialize, xCoffee3LogInitWithTransport, xCoffee3LogSerialApplyDefault, xCoffee3LogWrite, xCoffee3LogWriteField, xCoffee3RobotTcpInitialize, xCoffee3RtuBusInitialize, xCoffee3SerialApplyDefaults, xCoffee3ServerInitialize, xCoffee3WorkflowInitialize, xEventGroupCreateStatic, xPortGetFreeHeapSize |
| 353 | `vAppTaskManagerRunDefaultTask` | pdMS_TO_TICKS, prvApplyNetworkConfiguration, prvIsNetworkReady, prvPublishNetworkReady, prvUpdateNetworkIndicators, taskENTER_CRITICAL, taskEXIT_CRITICAL, vTaskDelay, xCoffee3LogWrite, xCoffee3LogWriteField, xEventGroupSetBits |
| 382 | `vAppTaskManagerWaitNetworkStackReady` | xxxxxxxxxx typedef struct {    uint8_t aucBaseInputs[16];    uint8_t aucControlCoils[COFFEE2_ROBOT_CONTROL_COIL_COUNT];} Coffee2RobotData_t;c |
| 393 | `ucAppTaskManagerIsNetworkReady` | xEventGroupGetBits |
| 405 | `vAppTaskManagerGetStatus` | taskENTER_CRITICAL, taskEXIT_CRITICAL |
| 416 | `prvPublishNetworkReady` | taskENTER_CRITICAL, taskEXIT_CRITICAL, xCoffee3LogWrite, xEventGroupClearBits, xEventGroupSetBits |
| 442 | `prvIsNetworkReady` | ip_addr_isany, netif_ip_addr4, netif_is_link_up, netif_is_up |
| 454 | `prvApplyNetworkConfiguration` | IP4_ADDR, netif_set_addr |
| 476 | `prvCaptureResetCause` | __HAL_RCC_CLEAR_RESET_FLAGS, __HAL_RCC_GET_FLAG |
| 507 | `prvCreateTaskLogged` | xCoffee3LogWriteField, xTaskCreate |
| 531 | `prvWriteRawStartupFailure` | HAL_UART_Transmit |
| 543 | `prvGetBusLogSource` |  |
| 558 | `prvUpdateNetworkIndicators` | HAL_GPIO_WritePin, pdMS_TO_TICKS, xTaskGetTickCount |
### [Application/UserAPP/Coffee3CloseApp/Task_Manager/coffee3_manager.h](../../../../Application/UserAPP/Coffee3CloseApp/Task_Manager/coffee3_manager.h)

SHA-256：`2BE58F4A43CAA494AA0BC9F50A4010A3C15331AD4C8419A541DA5026B82C3463`。以下行号对应 2026-09-08 工作区快照。

类型与常量原文按头文件顺序列出；正文先解释用途，这里保留全部字段/枚举值，方便逐项对照。API 原型请沿文件链接阅读。

L9
```c
#define COFFEE3_MANAGER_H
```


L18
```c
typedef enum {
	APP_TASK_MANAGER_RESULT_OK = 0,
	APP_TASK_MANAGER_RESULT_ALREADY_CREATED = 1,
	APP_TASK_MANAGER_RESULT_NO_RESOURCE = -1,
	APP_TASK_MANAGER_RESULT_LOG_INIT = -2,
	APP_TASK_MANAGER_RESULT_SERIAL_INIT = -3,
	APP_TASK_MANAGER_RESULT_MODULE_INIT = -4
} AppTaskManagerResult_e;
```


L28
```c
#define APP_TASK_MASK_LOG                    (1UL << 0)
```


L29
```c
#define APP_TASK_MASK_SERVER                 (1UL << 1)
```


L30
```c
#define APP_TASK_MASK_ROBOT                  (1UL << 2)
```


L31
```c
#define APP_TASK_MASK_BUS2                   (1UL << 3)
```


L32
```c
#define APP_TASK_MASK_BUS3                   (1UL << 4)
```


L33
```c
#define APP_TASK_MASK_BUS4                   (1UL << 5)
```


L34
```c
#define APP_TASK_MASK_BUS5                   (1UL << 6)
```


L35
```c
#define APP_TASK_MASK_WORKFLOW               (1UL << 7)
```


L36
```c
#define APP_TASK_MASK_ALL                    0x000000FFUL
```


L39
```c
typedef struct {
	AppTaskManagerResult_e xStartResult;
	uint32_t ulResetCause;
	uint32_t ulTaskCreatedMask;
	uint32_t ulTaskFailedMask;
	uint32_t ulFreeHeapBeforeTasks;
	uint32_t ulFreeHeapAfterTasks;
	uint8_t ucInfrastructureCreated;
	uint8_t ucTasksCreated;
	uint8_t ucLogReady; /*!< Log Transport and C3Log task are ready. */
	uint8_t ucDeviceReady;
	uint8_t ucServerReady;
	uint8_t ucRobotReady;
	uint8_t ucRtuReady;
	uint8_t ucWorkflowReady;
	uint8_t ucNetworkStackReady;
	uint8_t ucNetworkReady;
} AppTaskManagerStatus_t;
```

### [Application/UserAPP/Coffee3CloseApp/Comm_Log/app_comm_log_port.h](../../../../Application/UserAPP/Coffee3CloseApp/Comm_Log/app_comm_log_port.h)

SHA-256：`6BAFB1917A290FBAD750CD2651250F2AAAFC0351F82BA9297AE4ED770A3EC400`。以下行号对应 2026-09-08 工作区快照。

类型与常量原文按头文件顺序列出；正文先解释用途，这里保留全部字段/枚举值，方便逐项对照。API 原型请沿文件链接阅读。

L9
```c
#define APP_COMM_LOG_PORT_H
```

### [Application/UserAPP/Coffee3CloseApp/Comm_Log/coffee3_crash_log_port.c](../../../../Application/UserAPP/Coffee3CloseApp/Comm_Log/coffee3_crash_log_port.c)

SHA-256：`E912E6C039DC02B0BC2FD57E08EE2966F8A1C12223F3AEEB2BEBE06CD559026E`。以下行号对应 2026-09-08 工作区快照。

| 定义行 | 函数 | 下行调用（词法检索，非运行时跟踪） |
| --- | --- | --- |
| 18 | `lAppCommLogPortCrashWrite` | CLEAR_BIT, LL_USART_IsActiveFlag_TC, LL_USART_IsActiveFlag_TXE, LL_USART_IsEnabled, LL_USART_TransmitData8, vAppCrashDiagWatchdogRefresh |
| 63 | `lAppCrashDiagWrite` | lAppCommLogPortCrashWrite |
### [Application/UserAPP/Coffee3CloseApp/Comm_Log/coffee3_log.c](../../../../Application/UserAPP/Coffee3CloseApp/Comm_Log/coffee3_log.c)

SHA-256：`53439A021011F8B6D1F8DB2606FCCF345447F76C75F25C838CE66D05FA31B298`。以下行号对应 2026-09-08 工作区快照。

| 定义行 | 函数 | 下行调用（词法检索，非运行时跟踪） |
| --- | --- | --- |
| 53 | `xCoffee3LogInit` | xCoffee3LogInitWithTransport |
| 59 | `xCoffee3LogInitWithTransport` | memset, xAppLogInit, xTransportUartCreate |
| 99 | `xCoffee3LogWrite` | xCoffee3LogWriteOrder |
| 107 | `xCoffee3LogWriteOrder` | xAppLogWriteOrder |
| 117 | `xCoffee3LogWriteField` | xCoffee3LogWriteFieldOrder |
| 127 | `xCoffee3LogWriteFieldOrder` | xAppLogWriteFieldOrder |
| 138 | `xCoffee3LogWriteTextOrder` | xAppLogWriteTextOrder |
| 147 | `xCoffee3LogPrintfOrder` | va_end, va_start, vsnprintf, xCoffee3LogWriteTextOrder |
| 168 | `lCoffee3LogEarlyWrite` | lAppLogEarlyWrite |
| 174 | `vCoffee3LogTask` | vAppLogTask, xCoffee3LogWrite |
| 182 | `vCoffee3LogSetTaskReady` | vAppLogSetTaskReady |
| 188 | `vCoffee3LogGetStatus` | vAppLogGetStatus |
| 194 | `vCoffee3LogLwipResourceFailure` | vAppLwipAlertReportFailure |
### [Application/UserAPP/Coffee3CloseApp/Comm_Log/coffee3_log.h](../../../../Application/UserAPP/Coffee3CloseApp/Comm_Log/coffee3_log.h)

SHA-256：`FC0CC683F012997003EAE58345B23F43EB6102105863E26FEB0F715F32CC8132`。以下行号对应 2026-09-08 工作区快照。

类型与常量原文按头文件顺序列出；正文先解释用途，这里保留全部字段/枚举值，方便逐项对照。API 原型请沿文件链接阅读。

L17
```c
#define COFFEE3_LOG_H
```


L28
```c
typedef enum {
	COFFEE3_LOG_RESULT_OK = 0,
		/*!< The operation completed successfully. */
	COFFEE3_LOG_RESULT_ALREADY_INITIALIZED = 1,
		/*!< The log service was already initialized. */
	COFFEE3_LOG_RESULT_INVALID_ARG = -1,
		/*!< A pointer, level, or source was invalid. */
	COFFEE3_LOG_RESULT_NOT_READY = -2,
		/*!< The log queue or USART1 transport is unavailable. */
	COFFEE3_LOG_RESULT_QUEUE_FULL = -3,
		/*!< The bounded log queue could not accept the record. */
	COFFEE3_LOG_RESULT_TRANSPORT = -4
		/*!< USART1 Transport creation or opening failed. */
} Coffee3LogResult_e;
```


L44
```c
typedef enum {
	COFFEE3_LOG_LEVEL_INFO = 0,
	COFFEE3_LOG_LEVEL_WARNING = 1,
	COFFEE3_LOG_LEVEL_ERROR = 2,
	COFFEE3_LOG_LEVEL_COUNT = 3
} Coffee3LogLevel_e;
```


L52
```c
#define COFFEE3_LOG_ORDER_SYSTEM       0x0000U
```


L53
```c
#define COFFEE3_LOG_ORDER_DEBUG        0xF123U
```


L56
```c
typedef enum {
	COFFEE3_LOG_SOURCE_SYSTEM = 0,
	COFFEE3_LOG_SOURCE_SERVER = 1,
	COFFEE3_LOG_SOURCE_WORKFLOW = 2,
	COFFEE3_LOG_SOURCE_ROBOT = 3,
	COFFEE3_LOG_SOURCE_BUS2 = 4,
	COFFEE3_LOG_SOURCE_BUS3 = 5,
	COFFEE3_LOG_SOURCE_BUS4 = 6,
	COFFEE3_LOG_SOURCE_BUS5 = 7,
	COFFEE3_LOG_SOURCE_IO = 8,
	COFFEE3_LOG_SOURCE_COFFEE = 9,
	COFFEE3_LOG_SOURCE_CUP = 10,
	COFFEE3_LOG_SOURCE_SYRUP = 11,
	COFFEE3_LOG_SOURCE_LID = 12,
	COFFEE3_LOG_SOURCE_ICE = 13,
	COFFEE3_LOG_SOURCE_WEIGH = 14,
	COFFEE3_LOG_SOURCE_ENERGY_METER = 15,
	COFFEE3_LOG_SOURCE_IO_INPUT = 16,
	COFFEE3_LOG_SOURCE_IO_OUTPUT = 17,
	COFFEE3_LOG_SOURCE_COUNT = 18
} Coffee3LogSource_e;
```


L82
```c
#define g_xCoffee3LogStatus g_xAppLogStatus
```

### [Application/UserAPP/Coffee3CloseApp/Comm_Log/coffee3_retarget.c](../../../../Application/UserAPP/Coffee3CloseApp/Comm_Log/coffee3_retarget.c)

SHA-256：`C6F313D7DF0EC68DCCB6D2B0614889DF59221EABF39AD0D327A94E18ED4C997D`。以下行号对应 2026-09-08 工作区快照。

| 定义行 | 函数 | 下行调用（词法检索，非运行时跟踪） |
| --- | --- | --- |
| 12 | `_ttywrch` |  |
