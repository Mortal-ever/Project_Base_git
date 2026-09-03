# Coffee2 单机设备测试使用说明书

## 2026-08-12 Robot TCP 断链测试

仅在状态显示已连接且严格就绪后执行 Robot 动作。动作期间断开 Robot 以太网，确认 Workflow 保持 BUSY/RECOVERING，不盲目下发停止/启动，并暂停步骤超时。恢复链路后确认对映射命令/结果线圈进行三次一致采样。只有 command=0、result=0、严格就绪且运行/空闲有效时才允许重放一次；命令/结果含义不明确必须等待恢复窗口失败，禁止重放。Workflow 取消必须在恢复期间以取消结束。Robot 断链、未就绪、BUSY 或 RECOVERING 时，Server 手动写入必须拒绝。

记录 `ROBOT_READY_RISE/FALL`、`ROBOT_COMMAND_LINK_LOST`、`ROBOT_RECOVERY_RECONCILE`、`ROBOT_ACTION_SENT`、`ROBOT_ACTION_REPLAYED`、`ROBOT_RECOVERY_COMPLETED` 和 `ROBOT_RECOVERY_EXPIRED`。验证动作前清理使用 FC15 起始 3121、数量 19 并读回全零；不得清理 3120。活动订单恢复时先读取保存的 command/result：result=1 直接确认，command=1/result=0 继续等待，只有两者都为 0 且映射有效时才允许恢复安全启动。

## 2026-08-12 Coffee1 机器人启动语义

Robot 基础状态 0..15 必须使用 FC02 离散输入读取；3100..3139 控制/结果区使用 FC01 线圈读取。3100 是持续就绪状态，禁止写零；3120 是独立启动信号，禁止纳入普通动作结果映射或 3121..3139 清零。

冷启动按 Coffee1 顺序逐步写入，每步间隔 200 ms：FC15 清基础控制 0..9、清报警 1、停止 1、清报警 0、清报警 1、退出拖拽 1、使能 1、停止 1、启动 1，最后执行一次 FC02+FC01 刷新。订单恢复启动保留动作/结果区并跳过两个停止步骤。持续运行判据为 enable=1、alarm=0 且 ready/run/idle 任一有效；power_on 仅用于诊断，未观察到时记录 `ROBOT_POWER_SIGNAL_NOT_OBSERVED`，不应因该一次性位阻塞服务。

## 1. 范围与唯一事实来源

本手册用于 Coffee2 单机调试。当前仓库中的 `coffee2_server.c/.h`、设备协议、工作流和设备绑定表是唯一实现真相；协议 DOCX 只作历史背景，不能替代源码。测试以无订单、无生产工作流为前提：`g_xCoffee2WorkflowStatus.xState` 应为空闲，`0x0007` 订单存在和 `0x0008` 订单校验均保持为 0。人工冰测试是唯一允许的人工工作流入口，仍不属于订单验收。

上位机连接 Coffee2 Modbus TCP Server（端口 6001，Server Unit ID 1）。命令寄存器为 `0x0000`～`0x00AF`，状态寄存器为 `0x1000`～`0x10AF`，调试寄存器为 `0x1100`～`0x117F`。FC03 与 FC04 在 Server 端读取同一状态回调；写命令使用 FC06 或 FC16。参数先写，触发寄存器最后写，避免半成品参数被提前执行。除本手册明确列出的 IO 字段外，不增加镜像或地址。

### 1.1 每个分路、串口参数与设备

Robot TCP 单列，不属于 RTU 串口；Coffee2 日志使用 UART1。所有 RTU 分路均为 Modbus RTU、8 数据位、无校验、1 停止位、无硬件流控。表中的波特率必须与设备自身配置一致。

|分路/用途|物理接口|串口参数|设备|Unit ID|附加约束|
|---|---|---|---|---:|---|
|Robot TCP|ETH，非串口|TCP，非 RTU|机器人|1|真实设备通过 ETH；命令使用线圈|
|日志|UART1|115200，8N1|日志输出|—|无校验、1 停止位、无硬件流控|
|Bus2|UART2|19200，8N1|咖啡机|1|无校验、1 停止位、无硬件流控|
|Bus3|UART3|9600，8N1|杯机/糖浆机/盖机|1/2/3|无校验、1 停止位、无硬件流控|
|Bus4|UART4|19200，8N1|制冰机/称重模块/电源表|1/2/3|无校验、1 停止位、无硬件流控；制冰最小帧间隔 100 ms|
|Bus5|UART5|38400，8N1|输入 IO/输出 IO|1/2|无校验、1 停止位、无硬件流控；IO 最小帧间隔 20 ms|

同一物理 RTU Bus 的所有设备必须使用同一串口参数；当前 Bus4 三个设备统一为 19200，运行时不应发生正常波特率切换。`prvSelectSerialProfile` 仍是防御性兼容机制，只有绑定表出现不一致时才会尝试切换并增加 `ulBaudSwitchCount`。

### 1.2 通用安全前置

1. 断开订单上位机写入，确认 Server 只有一条人工调试链路；先读 `0x1000`、`0x1018`、`0x1029` 和各设备状态。
2. 机器人测试前清空运动区域，确认急停、护栏、夹具和杯盖通道处于安全状态；移动动作必须有人在场。
3. 咖啡、杯、盖、糖浆和制冰测试前确认对应容器、排液口、余料和防溢措施；任何卡滞立即停止并断电。
4. 制冰人工测试必须先人工放入空杯，确认称重台没有外力；目标值用 0.1 g 为单位，`0x0071=1` 表示 0.1 g。
5. 真实 RTU/ETH 测试使用隔离电源和正确波特率；模拟器只能验证纯逻辑，不能证明现场执行器动作。

## 2. 日志、结果和判定

Server 接受人工命令后依次可能出现：

|日志|含义|判定|
|---|---|---|
|`MANUAL_COMMAND_ACCEPTED`|已复制到绑定路由队列|只代表入队，不代表设备成功|
|`MANUAL_COMMAND_QUEUE_FULL`|路由队列已满|本次未执行，修复负载后重试|
|`MANUAL_COMMAND_REJECTED`|工作流运行/取消中，或参数非法|命令不提交；检查状态与参数|
|`MANUAL_COMMAND_RUNNING`|设备任务开始执行（仅 Server source）|检查设备在线、忙状态|
|`MANUAL_COMMAND_COMPLETED`|`lResult` 原样为 0|动作完成，继续核对设备反馈|
|`MANUAL_COMMAND_FAILED`|非超时、非取消的非零结果|按结果码定位总线/协议/设备问题|
|`MANUAL_COMMAND_TIMEOUT`|设备完成回调标记超时|检查链路、轮询和设备响应|
|`MANUAL_COMMAND_CANCELED`|结果为 -9|命令被取消，确认执行器已安全停止|

设备状态数组 `g_axCoffee2DeviceStatus[device_id]` 至少观察 `ucOnline`、`ucBusy`、`usLastAction`、`lLastResult`、`ulLastCommandId`、`ulCommandCount` 和 `ulErrorCount`。结果码固定如下，日志字段 `result` 与 `lLastResult` 均按原值解释：

|值|宏|含义|首要检查|
|---:|---|---|---|
|-1|`MODBUS_PORT_RESULT_INVALID_ARG`|参数/地址不合法|命令参数、Unit、数组边界|
|-2|`MODBUS_PORT_RESULT_NOT_READY`|端口或任务未就绪|设备在线位、任务启动状态|
|-3|`MODBUS_PORT_RESULT_BUSY`|端口忙|同一设备是否有并发命令|
|-4|`MODBUS_PORT_RESULT_TIMEOUT`|等待响应或完成超时|链路、设备状态轮询|
|-5|`MODBUS_PORT_RESULT_TRANSPORT`|传输层失败|ETH/UART/RS485 收发器和底层错误|
|-6|`MODBUS_PORT_RESULT_PROTOCOL`|响应内容不符合协议/目标|功能码、长度、状态值、IO 回读|
|-7|`MODBUS_PORT_RESULT_EXCEPTION`|远端 Modbus 异常|异常码和设备手册|
|-8|`MODBUS_PORT_RESULT_NOT_SUPPORTED`|当前动作未实现|不要反复重试，回到源码范围|
|-9|`MODBUS_PORT_RESULT_CANCELED`|协作取消|确认阀门、机器人和咖啡机已停机|

## 3. 机器人（Robot TCP）

### 3.1 Server 固定命令

`0x0030` 为控制动作，FC06 写入值后触发：0 启动、1 停止、2 暂停、3 使能、4 禁用、5 清报警、6 进入拖动、7 退出拖动。`0x0031` 为位置动作，值 0 回原点、1 取热杯、2 取冷杯、3/4 到盖工位（内部参数 1/2）、5/6 到咖啡工位（内部参数 0/1）、7 到制冰、8 到打印机、9/10 取出料位 1/2、0x0C 取盖、0x0D 盖盖、0x11～0x14 放出料位 1～4、0x15～0x20 放储位 1～12。非法位置只记录 `MANUAL_ROBOT_POSITION_UNSUPPORTED`。

写序：单值动作直接 FC06 `0x0030` 或 `0x0031`；FC16 只写这一个寄存器。接受后触发寄存器清零。先确认机器人安全区，再写值。预期 Server 日志为 `MANUAL_COMMAND_ACCEPTED`，随后 `MANUAL_COMMAND_RUNNING`，成功为 `MANUAL_COMMAND_COMPLETED`；链路异常还会有 `ROBOT_COMMAND_LINK_LOST`、`ROBOT_COMMAND_FAILED` 或连接重试日志。

### 3.2 Robot Modbus TCP 反馈

启动/停止/暂停/使能/禁用/清报警/拖动使用 FC05 线圈 0～7 先写 0、延时 50 ms、再写 1 的上升沿；位置动作使用 FC05 命令线圈和对应结果线圈（例如原点 3121/3101、热杯 3123/3103、冷杯 3124/3104、咖啡 3128/3108 或 3129/3109、制冰 3127/3107、盖工位 3125/3105 或 3126/3106）。完成后读结果线圈并清零。后台刷新读取 FC01 线圈 0～15 和 3100～3139。不要把这些 Robot 线圈误写成 Server 的 0x0030/0x0031。

成功判据：Server 结果为 0、结果线圈完成且清零、`g_axCoffee2DeviceStatus[1].ucOnline=1`，状态 `0x103C=1` 或动作后回到空闲。报警、结果线圈超时、在线位清零或 `lLastResult` 非 0 均判失败。

## 4. 咖啡机

|Server 写入|值/参数|后端 Modbus 序列|观察|
|---|---|---|---|
|`0x0040`|咖啡类型 `0x0000`～`0x0028`|FC06 保持寄存器 `0x2000=value`；随后 FC03 `0x1000` 起读 16 个状态轮询|`g_xCoffee2CoffeeMachineData.ausStatus[]`、状态 `0x1040`、`0x1042`～`0x104F`|
|`0x0042`|清洗类型仅 1 或 2|FC06 `0x200C=value`|同上；非法值记录 `COFFEE_CLEAN_UNSUPPORTED`|
|`0x0047`|任意非零触发取消|FC06 `0x200E=0`|`MANUAL_COMMAND_CANCELED` 或完成结果|

FC06 写参数后再写触发寄存器；FC16 可写单一寄存器。咖啡类型越界不得测试。刷新动作读 FC03 `0x1000` 数量 24，状态 1～8 任一非零会返回协议错误。成功要求后端状态完成、无故障状态、Server `0x1040=1`（在线且空闲时）或按现场状态回到可用；非零状态、超时、异常均失败。无订单测试不得写订单字段。

## 5. 杯机与盖机

Server `0x0050` 仅接受值 0～3：0 杯机落杯 2、1 杯机落杯 1、2 盖机落盖 2、3 盖机落盖 1。FC06 单写 `0x0050=value`，FC16 仅写该寄存器；接受后清零。设备侧：杯机值 0/1 分别 FC06 保持寄存器 0/1 写 1，轮询对应状态；盖机值 2/3 分别 FC06 保持寄存器 3/2 写 1，轮询对应状态。刷新分别为杯机 FC03 地址 0 数量 2 加 FC01 `0x1004` 数量 10，盖机 FC03 地址 2 数量 2 加 FC01 `0x100E` 数量 10。

检查 `g_xCoffee2CupLidData.ausCupTask[]`、`ausLidTask[]`、`aucCupCoils[]`、`aucLidCoils[]`，以及状态 `0x1050`（低半字节杯状态，高半字节盖状态：1 在线、2 忙、3 故障、4 离线）和 `0x1055`～`0x1058` 故障位。动作区无异物、托盘到位、盖料充足是必要前置。结果 0 且任务状态回到完成/空闲、无故障才算成功。

## 6. 糖浆机

`0x0060` 是通道触发，主机通道只允许 0～3，内部动作参数为通道+1（1～4）；`0x0061` 是该次用量/时间参数。推荐先 FC06 写 `0x0061=amount`，再 FC06 写 `0x0060=channel`；FC16 可从 `0x0060` 连写两个寄存器，但应确保触发值最后到达。后端先 FC06 地址 0 写参数，再 FC06 地址 1～4 写 1 并轮询。

`0x0062` 写剩余量/时间参数，后端 FC06 地址 10；`0x0063` 非零触发清洗，后端 FC06 地址 5 写 1。成功日志包含 `SYRUP_REMAINING_SET`（字段 `time_ds`）或通用命令完成；通道越界记录 `SYRUP_CHANNEL_UNAVAILABLE`。观察 `g_xCoffee2SyrupData.ausRegisters[]`、`0x1060` 聚合状态和 `0x1061`～`0x1064` 的 `ausRegisters[11..14]` 通道剩余值/时间快照；只有 `0x1060` 使用在线 1、忙 2、故障 3 的聚合含义。确认糖浆管路接好、液体充足且废液容器可用。

## 7. 制冰与称重

### 7.1 人工制冰入口

先人工放入空杯，确认杯在称重台中心；FC06 写 `0x0071=目标值`（单位 0.1 g，例如 0.1 g 写 1），再 FC06 写 `0x0070=1` 触发。FC16 可以先写 `0x0071` 再写 `0x0070`；触发接受后 `0x0070` 清零。Server 先记录 `MANUAL_CUP_ASSUMED`，接受后记录 `MANUAL_ICE_ACCEPTED`，拒绝记录 `MANUAL_ICE_REJECTED`。

工作流先去皮，再连续取三次称重中值；随后按目标差值计算脉冲，`COFFEE2_ICE_COMPENSATION_FACTOR=1L`，`COFFEE2_ICE_MAX_PULSE_MS=2000U`，任何开阀路径都会尝试关阀。预期日志为 `MANUAL_ICE_START`、`ICE_VALVE_PULSE`（`pulse_ms`）、`ICE_WEIGHT_SAMPLE`，成功为 `MANUAL_ICE_DONE`，失败常见为 `ICE_TARE_UNSTABLE`、`ICE_WEIGHT_OVER` 或 `MANUAL_ICE_FAILED`。观察 `g_xCoffee2IceData.ausRegisters[]`、`g_xCoffee2ScaleData.lWeightDecigram`、状态 `0x1070`～`0x1074`、`0x1078`、`0x107A`、`0x107C`。

冰仓、阀门、杯口必须无遮挡，操作员手不得进入阀门区域；脉冲不得通过外部脚本延长。订单制冰逻辑不在本次验收范围。

### 7.2 称重协议与限制

称重刷新为 FC03 地址 0 数量 3，结果转换到 `g_xCoffee2ScaleData`。称重动作在设备协议层使用 FC06 地址 `0x0011` 值 1（去皮）、值 2（清除去皮），或地址 `0x0060` 值 1（置零）。当前 Server 没有独立固定寄存器把这些动作暴露给上位机，因此禁止捏造新的主机地址；单机验收通过后台 REFRESH、工作流日志和 Keil 变量观察。`0x107D=1` 表示称重通信/设备/命令故障，非零即失败。

## 8. 电源表

电源表当前只有后台 REFRESH，没有独立固定写命令，禁止编造 FC06/FC16 主机命令。设备协议刷新使用 FC03 地址 `0x2000` 数量 16，再读 `0x4000` 数量 2，结果转换到 `g_xCoffee2PowerMeterData.fVoltage`、`fCurrent`、`fActivePower`、`fReactivePower`、`fApparentPower`、`fPowerFactor`、`fFrequency`、`fEnergy`。验收通过后台在线刷新、`g_axCoffee2DeviceStatus[8]` 的 `ucOnline/lLastResult`、日志和变量值判断；读失败、数据协议错误或在线位为 0 均失败。接线和互感器必须符合额定电压电流，严禁在带电端子上人工改线。

## 9. IO 固定调试协议

### 9.1 主机寄存器与操作

本次只新增以下固定字段，旧地址保持不变：

|地址|名称|取值|
|---:|---|---|
|`0x0084`|operation|0 空闲；1 Unit2 输出写并校验；2 刷新 Unit1 输入；3 刷新 Unit2 输出|
|`0x0085`|point|操作 1 时为 1～48 的外部点号|
|`0x0086`|value|操作 1 时只能 0 或 1|

操作 1：FC06 先写 `0x0085=point`、再写 `0x0086=value`、最后写 `0x0084=1`；FC16 可从 `0x0085` 连写两个参数，再用 FC06 写 operation。提交成功后 `0x0084` 清零；非法 point/value 记录 `MANUAL_IO_REJECTED`，不提交。操作 2/3 只需写 `0x0084=2` 或 `3`，当前实现忽略 `0x0085/0x0086` 的内容，直接提交 REFRESH；成功后 operation 清零。

### 9.2 位图公式与示例

状态 `0x1084`～`0x1086` 是 `g_xCoffee2Io` 的 MB1 输入 48 位图，`0x1087`～`0x1089` 是 MB2 输出 48 位图。对 point `p`（1～48）：`index=p-1`，`word=floor(index/16)`，`bit=index%16`，输入寄存器为 `0x1084+word`，输出寄存器为 `0x1087+word`，掩码为 `1<<bit`。因此 point 1 为第 0 字、第 0 位；point 17 为第 1 字、第 0 位；point 33 为第 2 字、第 0 位。读取时用按位与判断，写 0 期待对应位清零，写 1 期待对应位为 1。

`0x108A` valid：bit0 表示 IO 模块 0（MB1/Unit1）数据有效，bit1 表示模块 1（MB2/Unit2）数据有效。`0x108B` 为 IO 输出设备 `lLastResult` 原值，`0x108C` 为 `usLastAction`，`0x108D/0x108E` 为最近命令 ID 低/高字，`0x108F` 为 IO 版本低字。读回位图前先执行操作 2 或 3，确认相应 valid 位为 1。

### 9.3 FC05 写后校验

操作 1 在 Unit2 上执行 FC05 写线圈 point-1；成功后同一 Unit、同一 Bus 立即执行 FC02 读 48 个离散输入，再执行 FC01 读 48 个线圈。两次读取转换为 0/1 数组并提交 `vCoffee2IoCommitModbus`；最终以 FC01 读回的目标输出位与期望 value 比较。Server source 会记录 `IO_WRITE_EXPECTED`、`IO_WRITE_OBSERVED`，匹配记录 `IO_WRITE_SUCCESS`，不匹配返回 `MODBUS_PORT_RESULT_PROTOCOL` 并记录 `IO_WRITE_MISMATCH`；FC02/FC01 失败记录 `IO_WRITE_READ_FAILED`。周期 REFRESH 只更新位图，不产生上述人工校验噪声。成功判据必须同时满足结果 0、valid bit1、目标位匹配和命令完成日志。

## 10. 当前未接入指令与地址禁用清单

协议 DOCX 中出现但当前 Coffee2 Server 没有 handler 的 `0x0041`、`0x004E`、`0x007E`、`0x007F`、`0x0080`～`0x0083`、全部 `0x009x`、全部 `0x00Ax`：**当前未实现，禁止测试**。本版本也没有新增或镜像 `0x0300`、`0x0380`、`0x1300`、`0x1380`；看到这些地址时应停止脚本并回到源码确认。任何文档或上位机工具若把上述地址显示成可用，均不属于当前 Coffee2 实现。

## 11. 结果记录模板

每个动作至少记录：日期、设备/Unit、Server 写入地址和值、FC06/FC16 写入顺序、FC03/FC04/FC01/FC02 读回、`MANUAL_COMMAND_*` 生命周期、`lLastResult`、在线/忙位、相关 `g_xCoffee2*` 变量和最终安全状态。失败时保留原始结果码与最后一条设备日志，先关闭执行器、确认现场安全，再进行下一次测试。
