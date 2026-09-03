# Coffee2 Open 应用层五层架构终版迁移方案

## 1. 审批结论与产品命名

本方案批准以下方向：公共命令模型；按设备型号组织、按 Target 选源的 `DeviceProtocol` 与 `DeviceLibrary`；产品层只保存配置、绑定、状态投影和业务编排；Coffee2 Keil ARMCC V5.06 先行，GCC 后置；不引入动态注册、运行时 Profile、额外任务、队列或堆分配。

产品命名从本方案起固定为：

- **Coffee2 = 店中店 Open 开放式**；
- **Coffee3 = 店中店 Close 封闭式**。

当前源码目录、C 符号和 Keil Target 继续使用 `Coffee2`。固件版本字符串可以使用 `Coffee2Open`。不通过复制 `Coffee2OpenApp` 或新增同义 Target 表达 Open。

跨产品边界同时固定为：Coffee2 Open 保持单订单工作流；双订单流水线仅是 MilkTea 的产品需求。当前工程中的 MilkTea 源码已确认为错误实现，不作为公共架构、接口兼容性或 Coffee2 迁移的证据；后续须依据 `C:\Users\13193\Desktop\奶茶机评估资料` 单独立项、删除旧实现并重写。MilkTea 的删除和重写不得与本方案的 Coffee2 首次迁移同时进行。

附件原方案不原样执行，终版修正如下：

1. 公共命令第一阶段保持 Coffee2 现有字段、顺序和枚举数值，不提前加入多订单 slot。
2. `WORKFLOW=0`、`SERVER=1`、`MAINTENANCE=2` 保持不变。
3. `DeviceProtocol` 是公共命令到既有驱动 API 的设备调度层，不重复实现 DeviceLibrary 已有协议编解码。
4. Coffee2 寄存器状态投影、日志、取消状态和产品 IO 仍归 Coffee2App，不能下沉到公共协议。
5. F200 保持在 DeviceLibrary 内自含，不因删除旧适配文件丢失命令入口。
6. 当前有效 Modbus 栈只有 `ModbusPort + nanoMODBUS`，不重新启用旧 `ProtocolStack/Modbus`。

## 2. Coffee2 源码基础缓存

### 2.1 构建基线

- MCU：STM32F407VGTx；
- Keil Target：`Coffee2`；
- ARM Compiler：V5.06 update 7；
- Define：`USE_HAL_DRIVER, STM32F407xx, USE_COFFEE2, USER_VECT_TAB_ADDRESS, VECT_TAB_OFFSET=0x0000C000U`；
- 预包含：`Coffee2App/Config/coffee2_build_config.h`；
- Scatter：`MDK-ARM/ScatterFiles/Coffee2_CCM.sct`；
- 最新 Rebuild：`0 Error(s), 0 Warning(s)`；
- 基线尺寸：`Code=190740`、`RO-data=5740`、`RW-data=532`、`ZI-data=150436`。

历史日志不用于否定当前基线。

### 2.2 启动和任务所有权

`freertos.c` 通过 `CommonTargets.h` 调用 Coffee2 Task Manager。Manager 按顺序初始化 Transport、Log、串口默认值、设备状态、IO、串行总线、Robot、Workflow 和 Server，然后创建 C2Server、C2Robot、C2Bus2~5、C2Workflow，以及日志 Transport 可用时的 C2Log。

每个物理 UART2~5 只有一个 owner task 和一个静态命令队列；Robot TCP 有独立静态队列。命令路由由 Coffee2 静态设备绑定表决定。

### 2.3 Coffee2 Open 设备配置

| 设备 | 通道 | 驱动/协议 | 参数 |
| --- | --- | --- | --- |
| 日志 | UART1 | Coffee2 Log Transport | 115200 |
| 咖啡机 | UART2 | Dr.Coffee F200 自有协议 | 115200 |
| 落杯/落盖 | UART3 | ShengShu 合体式协议 | 9600，站号 1 |
| 糖浆机 | UART3 | Current Modbus | 9600，站号 2 |
| 电能表 | UART3 | DDSU666 | 9600，站号 3 |
| 制冰机 | UART4 | Coffee Ice Modbus | 19200，站号 1 |
| 称重 | UART4 | BSQ-DG-V2 | 19200，站号 2 |
| 输入 IO | UART5 | Digital IO Modbus | 38400，站号 1 |
| 输出 IO | UART5 | Digital IO Modbus | 38400，站号 2 |
| Robot | Ethernet | Dobot Protocol 1 / Modbus TCP | 端口 502，Unit 1 |
| 上位机 | Ethernet | Coffee2 Modbus TCP Server | 端口 6001，Unit 1 |

### 2.4 命令与完成语义

当前 `Coffee2Command_t` 贯穿 Server/Workflow、路由队列、Robot/Bus owner 和设备执行。必须保留：

- `ulCommandId` 单调事务号；
- `ulOrderId + ulOrderEpoch` 订单生命周期；
- `(ulOrderEpoch, ulCommandId)` 精确匹配完成记录；
- `WORKFLOW/SERVER/MAINTENANCE` 来源优先级和 Robot 队列折叠语义；
- `ucRetryLimit` 有界重试和 `ucFlags` 兼容字段；
- 取消命令使用 `xQueueSendToFront()`；
- 当前单一 `s_ulCanceledOrderEpoch` 的协作取消模型。

Coffee2 当前是单订单工作流。多 slot 订单、逐 slot 取消表和跨 slot 完成历史不属于本次迁移。

### 2.5 内存事实

- FreeRTOS heap 由 Coffee2 Manager 提供，链接到 CCM 的 `CCM_HEAP`；
- Coffee2 CPU-only 状态、静态队列存储和工作流状态使用 `CCM_APP`；
- DMA/ETH/lwIP/Transport 外设缓冲继续留在 DMA 可访问 SRAM；
- 目录和命令类型迁移不得改变任务栈、队列长度、heap 大小或 DMA 缓冲归属。

## 3. 五层终版架构

| 层 | 目录 | 职责 |
| --- | --- | --- |
| L5 产品应用 | `Application/UserAPP/Coffee2App` | Coffee2 Open 配置、绑定、状态投影、日志 source、Server/Workflow/Robot/Bus 编排 |
| L4 设备能力 | `Application/DeviceProtocol`、`Application/DeviceLibrary` | 公共设备动作调度；型号驱动和协议编解码 |
| L3 公共服务 | `Application/Common`、`Application/Diagnostics` | 命令模型、日志、OTA、TCP Session、lwIP 预警、崩溃诊断 |
| L2 通信 | `Application/Transport`、`Application/ProtocolStack/ModbusPort`、`Application/New_Party/nanoMODBUS` | UART/TCP 字节流和 Modbus 事务 |
| L1 平台 | `CubeMX_Base` | Core、HAL、FreeRTOS、lwIP、BSP、启动和中断 |

依赖只能向下。允许 L4 同时使用 L3 的命令契约和 L2 的通信 API；L4 禁止包含 Coffee2App 头文件。

## 4. 最终 Application 结构

```text
Application/
├─ Common/
│  ├─ compiler_compat.h
│  ├─ CommonTargets.h
│  ├─ Command/app_command.h
│  ├─ Log/app_log.c/.h
│  ├─ LwipAlert/app_lwip_alert.c/.h
│  ├─ Ota/app_ota_flash.c/.h
│  ├─ Ota/app_ota_http.c/.h
│  └─ TcpClientSession/tcp_client_session.c/.h
├─ Diagnostics/Config + Inc + Src
├─ Transport/Inc + Src
├─ ProtocolStack/ModbusPort/Config + Inc + Src
├─ New_Party/nanoMODBUS/Config + Inc + Src
├─ DeviceProtocol/Modbus/
│  ├─ dobot_protocol.c/.h
│  ├─ cup_lid_protocol.c/.h
│  ├─ syrup_protocol.c/.h
│  ├─ ice_protocol.c/.h
│  ├─ scale_protocol.c/.h
│  ├─ power_meter_protocol.c/.h
│  └─ io_protocol.c/.h
├─ DeviceLibrary/
│  ├─ Inc/device_library.h
│  ├─ Robot/Dobot/dobot_robot_device.c/.h
│  ├─ CoffeeMachine/coffee_machine_f200.c/.h
│  ├─ CoffeeMachine/coffee_machine_modbus.c/.h
│  ├─ CupLidController/ShengShu/cup_lid_shengshu.c/.h
│  ├─ SyrupMachine/CurrentModbus/syrup_machine_modbus.c/.h
│  ├─ IceMachine/CurrentModbus/ice_machine_modbus.c/.h
│  ├─ Scale/BSQ_DG_V2/scale_bsq_dg_v2.c/.h
│  ├─ PowerMeter/DDSU666/power_meter_ddsu666.c/.h
│  └─ IoModule/ModbusDigitalIo/io_module_modbus_digital.c/.h
└─ UserAPP/Coffee2App/
   ├─ Config/                 ├─ Comm_Log/
   ├─ Task_Manager/           ├─ WorkFlow/
   ├─ Modbus_Tcp_Server/      ├─ Robot_Tcp/
   ├─ Modbus_Rtu_Bus/         ├─ Device/
   ├─ IO_State/               └─ Ota/
```

本次不移动 Diagnostics、不合并 OTA、不重命名 Bus。这些工作与公共命令/设备协议落地无关，避免扩大同时变化面。

## 5. 公共命令契约

新增 `Application/Common/Command/app_command.h`，第一阶段必须与现有命令 ABI 等价：

```c
typedef enum {
	APP_COMMAND_SOURCE_WORKFLOW = 0,
	APP_COMMAND_SOURCE_SERVER = 1,
	APP_COMMAND_SOURCE_MAINTENANCE = 2
} AppCommandSource_e;

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
} AppCommand_t;
```

约束：

- 不新增 `usSlot`；不改字段名、字段顺序和整数宽度；
- 不用 `#define Coffee2Command_t AppCommand_t`；
- Coffee2 过渡期使用 `typedef AppCommand_t Coffee2Command_t;`；
- 用编译期断言或构建检查确认 `sizeof(AppCommand_t)` 与原类型一致；
- 来源枚举和所有 action 数值保持现有值；
- 公共 action 可改名为 `APP_ACTION_*`，但第一阶段为完整 `COFFEE2_ACTION_*` 提供一对一兼容别名。

`ucDeviceId` 是产品逻辑设备 ID，由 L5 静态绑定表解释；L4 不得假定它在所有 Target 中具有同一枚举值。

### 5.1 公共命令的长期形态

`AppCommand_t` 是通过 FreeRTOS Queue 按值复制的稳定 POD 消息，不采用 C++ 类、继承或虚函数形式，也不携带函数指针、`void *` 产品上下文、可变长度尾部或产品联合体。原因是 Queue 创建端保存固定 item size；若发送端、接收端或静态存储使用了不同的结构体布局，可能产生截断、越界读取和字段错位。

因此长期约束为：

- `AppCommand_t` 只表达一次设备事务所需的公共字段，不承担订单调度器职责；
- `ulOrderId + ulOrderEpoch + ulCommandId` 是跨任务关联键，足以把设备完成事件映射回产品订单上下文；
- 不在 `ucFlags` 中暗藏 slot，不为 MilkTea 增加 `usSlot`，也不把产品资源编号塞入公共保留位；
- `AppCommand_t` 不直接通过上位机协议发送、不写入持久化存储；若未来确有这种需求，另建带显式版本号的 wire/storage DTO，不复用 RTOS 内部消息布局；
- ARMCC V5.06 下用兼容的编译期尺寸检查固定第一阶段 `sizeof(AppCommand_t) == 32U`，不用 C11 `_Static_assert`；
- 任何公共命令字段、顺序、宽度或枚举数值变更都视为跨 Target ABI 变更，必须经过单独架构审批，不能作为某个产品需求的顺带修改。

### 5.2 对象化边界和 MilkTea 双订单隔离

本工程允许“C 风格对象化”，但对象化位置在产品层，而不是公共命令。模块使用静态 `Context_t + API` 组织状态和所有权；固定 Target 优先直接调用，不为形式统一引入运行时工厂、动态注册或每条命令的虚表。

MilkTea 后续重写时应在 `Application/UserAPP/MilkTeaApp` 内定义两个静态订单上下文和一个产品调度器，至少由产品层持有：slot、`orderId`、`orderEpoch`、流程阶段、取消状态、工艺杯、清洗位、等待位以及 Robot1/Robot2 的资源所有权。设备命令仍只携带公共关联键；完成事件按 `ulOrderEpoch` 找回订单上下文。双订单表示 Robot1 前段与 Robot2 后段可以流水并行，不改变“一条物理总线/一个设备连接只有一个 owner”的约束，同一杯体和同一工位同一时刻只能有一个所有者。

这些 MilkTea 类型、状态和资源表不得进入 `Application/Common`、`Application/DeviceProtocol` 或 Coffee2 Target。Coffee2 不提供第二订单槽，不因未来 MilkTea 重写增加 RAM、任务、队列、分支或状态。

### 5.3 跨 Target 防回归门槛

公共头文件或公共源文件一旦变化，禁止只做增量 Build 或只验证需求来源 Target。必须：

1. 清点所有实际包含该公共头的 Target，并分别执行 Keil Rebuild；Coffee2 必须保持 `0 Error(s), 0 Warning(s)`，Coffee3 和重写后的 MilkTea 在各自进入有效状态后纳入同一门槛。
2. 确认 Queue 创建的 item size、静态 queue storage 乘数、发送局部变量和接收局部变量都来自同一个公共类型，不存在手写常量或旧 typedef。
3. 比较每个 Target 的 map、Code/RO/RW/ZI、CCM/SRAM 余量、FreeRTOS heap 最低余量和任务栈 high-water mark，解释全部结构体尺寸引起的变化。
4. 检查 Keil 活动源清单：Coffee2 不得编入 MilkTea Workflow/Scheduler；MilkTea 不得通过公共层引用 Coffee2App 符号。
5. 对订单关联、取消、超时、重试、stale completion 和队列满路径做 Target 内回归；禁止以“其他 Target 可以编译”代替行为验收。

## 6. DeviceProtocol 和 DeviceLibrary 边界

### 6.1 DeviceLibrary

DeviceLibrary 继续拥有设备寄存器/线圈地址、串行帧和校验、Modbus 读写序列、型号原生 action/status/image、超时、取消回调和设备结果。不得把这些实现复制到 DeviceProtocol。

### 6.2 DeviceProtocol

DeviceProtocol 是轻量、无状态的公共命令执行入口：

- 接收 `AppCommand_t`、通信上下文、Unit ID、原生 image 指针和通用取消回调；
- 校验 device action 和参数，将公共 action 映射到型号驱动 API，返回公共事务结果；
- 不持有任务、队列、EventGroup、heap、Coffee2 日志或 Coffee2 寄存器状态镜像；
- 不包含 `coffee2_*.h`。

代码来源：

- `dobot_protocol`：从 Coffee2 Robot TCP 迁移 Dobot action-to-coil 和结果地址映射；TCP 连接、重连和 owner task 留 Coffee2App/Robot_Tcp。
- 其余六个协议文件：从旧 `coffee2_rtu_protocol` 迁移公共 action 调度，调用现有型号驱动。

### 6.3 F200 特例

F200 保持在 `coffee_machine_f200.c/.h` 自含。删除旧 `coffee2_rtu_protocol` 前，为它提供接收 `AppCommand_t` 的公共执行入口，保留 Refresh、Make、Clean、Cancel、配方参数、取消回调和结果映射。串口通道和波特率继续由 Coffee2App Config/Bus owner 绑定。

### 6.4 Coffee2 产品状态投影

旧 `coffee2_rtu_protocol` 中以下内容迁回 `Coffee2App/Device`，可新增 `coffee2_device_image.c/.h`；它是产品状态存储，不是 action adapter：

- Coffee2 咖啡机 24-word 状态投影；
- Coffee2 cup/lid 独立状态投影；
- Syrup/Ice/Scale/PowerMeter 原生 image 的 Coffee2 持有实例；
- Coffee2 IO 提交和 host 寄存器同步；
- Coffee2 日志 source 和设备在线/完成状态发布。

公共 DeviceProtocol 通过调用方传入的原生 image 指针工作，不能引用 Coffee2 全局对象。

## 7. Keil Coffee2 分组和选源

保留 Target 名 `Coffee2`，因为 Coffee2 已明确代表 Open。Application 虚拟分组终态：

```text
Application/Common
Application/Diagnostics
Application/Transport
Application/ProtocolStack
Application/New_Party
Application/DeviceProtocol
Application/DeviceLibrary
Application/Coffee2/App
```

选择规则：

- `app_crash_fault_armcc.s` 编译，`app_crash_fault_gcc.S` 不进入 Keil；
- 七个 Coffee2 Open 所需 DeviceProtocol 文件编译；
- Dobot P1、F200、ShengShu、Syrup、Ice、BSQ-DG-V2、DDSU666、Digital IO 编译；
- `coffee_machine_modbus.c` 设置 `Include in Target Build = false`；
- MilkTea 相关组保持原样且禁用，不分析、不改路径；
- 每个活动 `.c/.s` 在 Target 中只出现一次，不建立 `DeviceLibrary_Unselected`；
- CubeMX、Drivers、Middlewares 和 LWIP 分组完全不动。

## 8. 分阶段实施与验收

### P0 基线冻结

保存当前 Coffee2 AXF/BIN、map、build log、活动源清单、宏、include、scatter 和程序尺寸。

### P1 公共命令

新增 `app_command.h`，Coffee2 使用 typedef 过渡，不改业务调用和字段访问。增加 ARMCC V5.06 兼容的 32-byte 编译期尺寸检查，并核对所有 Queue 的 item size、静态 storage 和收发局部变量均使用同一 typedef。Keil Coffee2 Rebuild 必须 `0 Error(s), 0 Warning(s)`，程序尺寸和队列存储不得改变。

### P2 逐设备迁移

按 Scale、Power Meter、IO、Ice、Syrup、Cup/Lid、Dobot、F200 顺序，一次迁移一个执行单元，每一步 Rebuild 0/0。每一步先新增并切换调用，再确认旧函数无引用，不得同时删除全部旧实现。

### P3 废弃旧 Coffee2Protocol

只有所有 action 有唯一新归属、Coffee2 状态投影已迁入产品层、F200 和 Robot 执行链闭合、全文无旧符号引用、Keil Rebuild 0/0 后，才从 Target 排除 `coffee2_rtu_protocol.c/.h`。

具体文件删除须按项目安全规则另行确认；未确认时从 Target 排除并保留文件。

### P4 Keil 终验

- ARM Compiler V5.06 update 7，`0 Error(s), 0 Warning(s)`；
- 无 duplicate input，map 中无 `coffee_machine_modbus.o`；
- `_ttywrch` 和 Crash Port 各一个活动定义；
- Code/RO/RW/ZI 与基线对比并解释全部差值；
- CCM heap、CCM_APP、SRAM 和主栈区间保持合法；
- 无新增任务、队列、EventGroup 或动态分配。

### P5 硬件最小闭环

- 启动日志包含 Coffee2Open 版本；
- Server、Robot、Bus2~5、Workflow、Log 任务正常；
- UART1~5 参数与基线一致；
- Dobot P1、F200 和每条 Modbus 总线至少完成一次命令/状态反馈；
- Cancel、超时、重试和 stale completion 过滤各验证一次；
- Coffee2 host 状态寄存器投影不变；OTA 初始化和 active 状态不回归。

### P6 GCC 后置

Keil P4/P5 通过后再更新 CMake。GCC 编译 `app_crash_fault_gcc.S`，不编译 ARMCC fault 汇编；DeviceProtocol/DeviceLibrary 选源与 Keil 一致；Coffee2 Debug/Release 全量构建并检查 map。

MilkTea 和 Coffee3 均不属于本次构建验收。

## 9. 明确后置

- Coffee3 Close 产品层设计；
- MilkTea 旧错误源码的受控删除和完整重写；
- MilkTea 产品层双订单 Context/Scheduler、资源所有权表、逐订单取消和并发完成历史；
- OTA 合并、Bus 目录改名、Diagnostics 目录迁移；
- 旧自研 Modbus和历史材料清理。

这些能力不得混入 Coffee2 Open 首次架构迁移。尤其不得为了 MilkTea 双订单修改 `AppCommand_t` 布局、给 Coffee2 预留 slot，或在公共层引入运行时 OOP 框架。

## 10. 最终交付

1. Coffee2 源码与构建基线；
2. 新增/修改/排除的文件清单；
3. 公共命令 ABI 和 action 数值兼容表；
4. 每个 DeviceProtocol 到 DeviceLibrary API 的映射；
5. Coffee2 状态投影归属清单；
6. 每阶段 Keil 0/0 日志和尺寸差值；
7. 硬件最小闭环记录；
8. GCC Debug/Release 结果；
9. `CHANGES.md`，注明“Coffee2 Open 应用层架构迁移，不改变协议和业务行为”。
