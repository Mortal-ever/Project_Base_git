# Coffee2 Keil V5 仿真与源码排查手册

## 2026-08-12 Robot 恢复排查

验证 Robot 路径时，在 `vCoffee2RobotTcpTask`、`prvReconcile` 和 `prvWaitAction` 设置断点。任务不得有终止重试次数：重连退避永久重复并封顶 30 秒。分别观察 `g_xCoffee2RobotTcpStatus.ucConnected/ucReady`，以及 Workflow 命令断链时 `g_axCoffee2DeviceStatus[COFFEE2_DEVICE_ROBOT].ucBusy/ucRecovering`。EventGroup 只能作为通知，不能作为事务存储。

对恢复中的映射动作，验证三次一致 Modbus 快照、结果写零/读零握手和最多一次重放。安全门禁要求命令/结果为零、严格就绪且运行或空闲有效；含义不明确时禁止重放。Robot Modbus 每笔事务超时为 1000 ms，即使轮询间隔为 100 ms。该恢复路径不需要新增上位机寄存器或工程文件。

## 2026-08-12 Coffee1 语义检查点

基础状态 0..15 的观察点是 `xModbusPortReadDiscreteInputs`（FC02），3100..3139 控制/结果观察点是 `xModbusPortReadCoils`（FC01）。禁止在源码中写入 3100；普通动作清零只能是 FC15 `3121U, 19U`，3120 启动信号由独立动作分支直接写入，不得进入普通结果映射。

启动调试应按 200 ms 步进检查 `ROBOT_STARTUP_BEGIN/STEP/DONE/RETRY`，并确认冷启动顺序为基础 0..9 清零、报警 1、停止 1、报警 0、报警 1、退出拖拽 1、使能 1、停止 1、启动 1。恢复中的活动订单先读取保存的 command/result，result=1 直接 reconcile，command=1/result=0 等待，只有均为 0 且映射有效时才跳过两个停止步骤执行恢复启动；不清除动作/结果区。持续运行判据是 enable、无报警以及 ready/run/idle 任一有效；`power_on`、safety、collision、recovery 只记录诊断。

## 1. 目标、工程和事实边界

目标工程为 `MDK-ARM/STM32F407_Base.uvprojx`，编译器为 ARM Compiler V5.06，目标芯片 STM32F407。本文的函数名、全局变量、寄存器和调用关系以当前 Coffee2 源码为准；源码变更后应重新用 `rg` 搜索符号，不能依赖易漂移的行号。

MDK Simulator 只适合纯 C 逻辑、寄存器映射、队列状态和错误分支验证。它不提供真实 ETH、UART、RS485 收发器、Modbus 从站响应或执行器安全反馈，因此 Simulator 中的“完成”不能作为现场动作验收。ST-Link 或 J-Link 连接真实 STM32F407 后，才可验证 Robot TCP、RTU Bus2～Bus5、IO FC05/FC02/FC01 和设备响应；真实调试必须执行急停、隔离电源和现场监护要求。

当前固定 Server 命令仍在 `0x0000`～`0x00AF`；IO 调试只使用 `0x0084`～`0x0086`，状态只使用 `0x1084`～`0x108F`。不存在可测试的 `0x0380`、`0x1380`、`0x0300` 或 `0x1300` 镜像。协议 DOCX 中未接入的命令在 Simulator 和硬件上都禁止尝试。

## 2. 建立 Debug 会话

### 2.1 打开工程与目标

1. 启动与 ARM Compiler V5.06 匹配的 µVision，打开 `MDK-ARM/STM32F407_Base.uvprojx`。
2. 在 Project 树确认 Coffee2 Application、Device、Modbus TCP Server、Modbus RTU Bus、Robot TCP、Diagnostics 均属于当前 Target；不要重新生成 CubeMX 文件。
3. 在 Options for Target 的 Device、Target、Output、Listing、C/C++、Debug 页逐项核对，不改变链接脚本、内存布局和启动文件。
4. Simulator 调试器选择 `Use Simulator`；真实板选择 ST-Link/J-Link 对应驱动，确认 SWD 频率适合板卡。
5. 点击 Rebuild，确认 ARMCC V5.06 没有把 C90/C99 源码当作 C++ 编译；再进入 Debug。

### 2.2 优化与信息设置

逻辑排查优先使用低优化（例如 `-O0` 或工程允许的最小优化）并打开 C source line、Local、Call Stack 和 Symbols。若必须复现发布优化问题，保存一份原配置后再改回原优化级别。不要通过关闭中断、改变 RTOS tick 或随意增加栈来掩盖问题。编译窗口中的 warning 要记录文件和函数，先确认是否为现有生成代码 warning。

### 2.3 运行控制

1. 复位后先运行到任务创建完成，再暂停检查 `g_xCoffee2ServerStatus.ucListening`、设备在线数组和 `g_xCoffee2WorkflowStatus.xState`。
2. 使用 Watch 观察结构体字段，不要在 volatile 总线缓冲区上连续修改值。
3. Call Stack 顶部用于确认当前任务，向下展开到 Server、RtuBus、Robot、ModbusPort 和 Transport。
4. 软件断点应少量放在逻辑边界；硬件断点优先放在 IO 写入和完成回调。断点停留会人为制造超时，结果必须标注“调试暂停影响”。

## 3. 推荐 Watch 窗口

建立一个名为 `Coffee2_Core` 的 Watch，至少加入以下符号：

|符号|重点字段/用途|
|---|---|
|`g_xCoffee2ServerStatus`|`ucOnline`、`ucListening`、`ucActiveClients`、请求/拒绝计数、客户端最后结果|
|`g_axCoffee2DeviceStatus`|每个设备 `ucOnline`、`ucBusy`、`usLastAction`、`lLastResult`、`ulLastCommandId`|
|`g_axCoffee2RtuBusStatus`|当前 Bus、活动设备、波特率切换、帧/错误计数、最后结果|
|`g_xCoffee2Io`|MB1 输入、MB2 输出、`aucModbusValid[2]`、`ulVersion`|
|`g_xCoffee2CoffeeMachineData`|咖啡机 24 个状态寄存器|
|`g_xCoffee2CupLidData`|杯/盖任务寄存器和线圈数组|
|`g_xCoffee2SyrupData`|糖浆寄存器 0～14|
|`g_xCoffee2IceData`|制冰寄存器、故障和阀门寄存器|
|`g_xCoffee2ScaleData`|原始值、小数位、单位、`lWeightDecigram`|
|`g_xCoffee2PowerMeterData`|电压、电流、功率因数、频率、能量|
|`g_xCoffee2WorkflowStatus`|状态、步骤、当前订单、最后错误、取消标志|
|`g_xCoffee2LogStatus`|日志初始化、待发送数、丢弃数、最后传输错误|
|`g_aulAppCrashSavedRegisters`|ARMCC 包装器保存的 R4～R11、MSP、PSP 共 10 个字|

数组观察要带设备索引：Robot=1、Coffee=2、Cup=3、Syrup=4、Lid=5、Ice=6、Scale=7、Power=8、IO Input=9、IO Output=10。

## 4. 完整人工命令断点链

Server 写入入口按以下顺序设置断点，先不在每一层单步：

`prvWriteSingle/prvWriteMultiple` → `prvCommitWrite` → `prvEvaluateManualCommands` → `prvEvaluateIoCommand`（仅 IO）→ `prvSubmitManual` → `xCoffee2CommandSubmit` → `prvSubmit` → 目标路由任务。

RTU 设备的路由链是：

`vCoffee2RtuBusTask` → `prvSelectSerialProfile` → `prvExecute`（`coffee2_rtu_bus.c`）→ `xCoffee2CoffeeMachineExecute`、`xCoffee2CupMachineExecute`、`xCoffee2LidMachineExecute`、`xCoffee2SyrupMachineExecute`、`xCoffee2IceMachineExecute`、`xCoffee2ScaleExecute`、`xCoffee2PowerMeterExecute` 或 `xCoffee2IoModuleExecute`（`coffee2_rtu_protocol.c`）。

Robot 路由链是：

`vCoffee2RobotTcpTask` → `prvExecute`（`coffee2_robot_tcp.c`）→ 动作线圈映射/`prvWriteRisingEdge`/`prvWaitAction` → ModbusPort。

公共收发链是：

`xModbusPortWriteRegister`、`xModbusPortWriteCoil`、`xModbusPortReadHolding`、`xModbusPortReadCoils`、`xModbusPortReadDiscreteInputs` → ModbusPort transport 封装 → `xTransportSend`/`xTransportReceiveExact`/`xTransportControl` → 目标响应解析 → `vCoffee2DeviceCommandCompleted`。

在 `vCoffee2DeviceCommandStarted` 看 `MANUAL_COMMAND_RUNNING` 后，最终回调 `vCoffee2DeviceCommandCompleted` 的 `lResult` 必须原样保留；Server source 的日志会按完成、失败、超时、取消分流。

## 5. 结果码和队列排查表

|结果|首看变量/位置|下一断点|常见原因|
|---:|---|---|---|
|-1 INVALID|`Coffee2Command_t.ausParameter[]`、设备 ID、Unit、地址|`prvSubmit` 参数检查或协议函数入口|point 越界、通道越界、空指针、动作参数错误|
|-2 NOT_READY|`g_axCoffee2DeviceStatus[id].ucOnline`、`g_axCoffee2RtuBusStatus[bus].ucReady`；Bus 私有 context 的 `ucCreated` 仅在任务上下文断点查看|`vCoffee2RtuBusTask` 绑定检查|任务未创建、串口未打开、绑定与 Bus 不一致|
|-3 BUSY|设备 `ucBusy`、ModbusPort 状态|`xModbusPort*` 调用前|同设备命令未结束或端口仍占用|
|-4 TIMEOUT|`ulTimeoutMs`、起始 tick、轮询状态寄存器/结果线圈|`prvWaitAction`、`prvWaitCoffee` 或 `xTransportReceiveExact` 返回处|从站不回、结果位不置位、断点停留过久|
|-5 TRANSPORT|`xTransportGetStatus`、`lLastNativeError`、Bus 统计|`xTransportSend`/`xTransportReceiveExact`|网线、UART、RS485、DMA、socket 或 CRC 传输失败|
|-6 PROTOCOL|帧功能码、长度、设备数据数组、IO 目标位|协议执行函数返回前|状态值非法、FC01 回读不匹配、单位/小数位不支持|
|-7 EXCEPTION|Modbus 异常码和端口最后帧|ModbusPort 异常解析点|远端从站拒绝地址、功能码或参数|
|-8 NOT_SUPPORTED|`Coffee2Command_t.usAction`、目标协议 switch|对应 `default` 分支|DOCX 指令未接入、动作不属于该设备|
|-9 CANCELED|`ulOrderEpoch`、取消标志、EventGroup|`ucCoffee2CommandIsCanceled` 和完成回调|协作取消或安全停机请求|
|Queue full|`xCoffee2CommandSubmit` 返回值、路由 Queue 水位、`g_xCoffee2LogStatus.usPendingCount`|`prvSubmit` 的 `xQueueSend`/`xQueueSendToFront` 返回处|目标路由队列满、工作流占用或提交时机过早|

Queue full 只代表命令没有进入设备任务，不能继续等待设备完成；先确认 `MANUAL_COMMAND_QUEUE_FULL`，再检查是否有未完成命令和生产工作流。

## 6. IO 写校验专项

### 6.1 Server 侧

1. 在 `prvCommitWrite`、`prvEvaluateManualCommands`、`prvEvaluateIoCommand` 依次下断点。
2. 对 operation 1 观察 `usPoint`（1～48）、`usValue`（0/1）及 `Coffee2Command_t.ausParameter[0/1]`；确认内部 point 已变为 point-1。
3. 继续到 `prvSubmitManual`、`xCoffee2CommandSubmit`、`prvSubmit`，确认 `ucDeviceId=10`、`usAction=COFFEE2_ACTION_IO_WRITE`、`ucSource=SERVER`。
4. 对 operation 2/3 只检查目标设备 ID 和 `COFFEE2_ACTION_REFRESH`；point/value 不作为刷新条件。

### 6.2 RTU 与读回

在 `vCoffee2RtuBusTask` 的 `prvExecute` 后进入 `xCoffee2IoModuleExecute`。operation 1 的断点顺序固定为：

`xModbusPortWriteCoil`（FC05，Unit2，point-1）→ `xModbusPortReadDiscreteInputs`（FC02，48 点）→ `xModbusPortReadCoils`（FC01，48 点）→ 数组转换 → `vCoffee2IoCommitModbus` → 目标 FC01 位比较 → `vCoffee2DeviceCommandCompleted`。

Watch 中记录 `abInputs[]`、`abOutputs[]`、`aucInputs[]`、`aucOutputs[]`、`ucPoint`、`ucValue`、`xResult`。成功必须看到 `IO_WRITE_EXPECTED`、`IO_WRITE_OBSERVED`、`IO_WRITE_SUCCESS`；不匹配必须返回 -6 并有 `IO_WRITE_MISMATCH`。周期 refresh 不应触发人工 readback 日志。

### 6.3 人为制造超时的安全方式

只在隔离台架、无运动负载时做。可在 `xTransportReceiveExact` 返回前临时设置一次条件断点，或断开被测 RTU/ETH 链路后运行一个带超时的命令；不要修改业务代码、不要让阀门或机器人保持打开。断点停留本身也会耗尽预算，需在记录中注明。看到 -4 后立即清除断点、恢复链路，再确认 IO 输出线圈和制冰阀门均为安全态。

## 7. 典型设备链路定位

### 7.1 Robot TCP

先看 `g_xCoffee2RobotTcpStatus.ucConnected`、`lLastResult` 和 `g_axCoffee2DeviceStatus[1]`。连接问题在 `xTransportOpen`、`ROBOT_CONNECT_FAILED`、`ROBOT_TCP_DISCONNECTED`；命令问题在 `prvWriteRisingEdge`、`prvWaitAction` 和结果线圈读取。Robot 位置动作是 FC05 线圈，不要在 Server 寄存器上寻找设备 FC06。

### 7.2 RTU Bus2～Bus5

先看 `g_axCoffee2RtuBusStatus[bus_index]` 的活动设备、当前波特率、帧计数和最后结果，再进入 `prvSelectSerialProfile` 与 `prvExecute`。咖啡机、杯/盖、糖浆、制冰、称重、电源表和 IO 的真实协议入口均在 `coffee2_rtu_protocol.c`。Unit 不匹配时应在绑定检查处得到 -2，而不是继续单步到 ModbusPort。

### 7.3 制冰与称重

人工冰路径从 `prvDispenseIce` 进入 `prvRunStep`：先 Scale tare，再 `prvReadStableScale` 三次取中值，随后 `COFFEE2_ACTION_ICE_SET_VALVE` 写 1，等待脉冲，必经写 0 关阀，再称重。看 `COFFEE2_ICE_MAX_PULSE_MS=2000U`、`COFFEE2_ICE_COMPENSATION_FACTOR=1L`、`lWeightDecigram` 和 `ICE_VALVE_PULSE`。任何返回路径都应能看到关阀命令已提交或已尝试。

## 8. HardFault、堆和栈

HardFault 先停止继续运行，读取 `g_aulAppCrashSavedRegisters[0..9]`（R4～R11、MSP、PSP），再从异常栈帧或崩溃日志取得 R0～R3、R12、LR、PC、xPSR；使用反汇编/Map 文件把 PC 映射到函数，不用猜行号。再检查 Call Stack、当前任务名、异常前最后一条日志和 `g_xCoffee2LogStatus.lLastTransportError`。

堆问题检查 `xPortGetFreeHeapSize()` 与 `xPortGetMinimumEverFreeHeapSize()`、调试寄存器 0x1100 区域和任务创建状态。Coffee2 路由使用静态队列/事件存储，出现堆下降时先排查第三方或其他模块，不要新增动态分配。

栈问题检查每个任务的 `uxTaskGetStackHighWaterMark()`（若当前 Target 提供）、Call Stack 深度、递归和大数组局部变量；Modbus 帧缓冲区、`Coffee2Command_t` 和协议数组不应被越界写。HardFault 后先复位并保存寄存器，再验证所有设备输出已关闭。

## 8.1 按结果码展开的现场检查卡

### INVALID（-1）

- 在 `prvSubmit` 入口记下 `ucDeviceId`、`usAction` 和四个参数。
- 在 `coffee2_rtu_protocol.c` 的动作入口确认地址、数量和数组长度。
- Server IO 重点确认外部 point 是 1～48，传入协议的 point 已减一。
- 修正上位机参数后重新发送，不通过改写状态寄存器绕过校验。

### NOT_READY（-2）

- 观察 `g_axCoffee2DeviceStatus[id].ucOnline` 与 `g_axCoffee2RtuBusStatus[bus].ucReady`；需要判断底层创建结果时，在 `vCoffee2RtuBusTask` 上下文查看私有 `Coffee2RtuBusContext_t.ucCreated`。
- 在 `vCoffee2RtuBusTask` 的绑定检查处分辨设备 ID、route ID 和 Unit ID。
- 观察任务管理器的 RTU/Robot ready 位，再决定是否重启任务。
- 真实板先检查串口复用、波特率和 RS485 收发方向。

### BUSY（-3）

- 查看目标设备 `ucBusy`、`ulLastCommandId` 和队列剩余量。
- 在 `xModbusPort*` 调用前断点确认没有同一端口并行事务。
- 让前一命令进入 `vCoffee2DeviceCommandCompleted` 后再重试。
- 不用增加新的队列或任务来掩盖端口占用。

### TIMEOUT（-4）

- 记录 `ulTimeoutMs`、起始 tick、最后一次 FC 发送和当前 Call Stack。
- 断点放在 `prvWaitAction`、`prvWaitCoffee` 或 `xTransportReceiveExact` 返回处。
- 对 Robot 看结果线圈，对 RTU 看设备状态寄存器和 `g_axCoffee2RtuBusStatus`。
- 先确认执行器已关闭，再恢复链路；断点停顿导致的超时必须单独标记。

### TRANSPORT（-5）

- 读取 `xTransportGetStatus` 与 `xPort.xLastFault.lNativeError`。
- Ethernet 检查 socket 状态、远端 IP/端口和网线；UART 检查 DMA、收发方向与终端电阻。
- 在 `xTransportSend` 和 `xTransportReceiveExact` 各设一个短暂停留断点。
- 若链路恢复，等待一次后台 refresh 确认在线位重新置 1。

### PROTOCOL（-6）

- 保存原始功能码、字节数、异常状态和转换前寄存器数组。
- IO 必须同时检查 `abOutputs[ucPoint]` 与期望值，不能只看 FC05 应答。
- Scale 检查小数位、单位和转换乘数；Coffee 检查故障状态寄存器。
- 协议错误重复出现时停止动作，核对从站手册与当前 `switch` 分支。

### EXCEPTION（-7）

- 在 ModbusPort 异常解析处分辨远端异常码与本地传输错误。
- 检查从站是否拒绝功能码、地址或写值；不要把异常码改写成成功。
- 记录 Unit、起始地址、数量和异常码后再恢复设备。

### NOT_SUPPORTED（-8）

- 在协议函数的 `default` 分支查看 `usAction` 和设备 ID。
- 对照 `Coffee2Action_e` 与当前 Server handler；DOCX 中未接入的动作立即停止。
- 不新增寄存器、镜像或临时通用块来“实现”未接入指令。

### CANCELED（-9）

- 观察 `ulOrderEpoch`、取消标志和 `ucCoffee2CommandIsCanceled` 返回值。
- 制冰路径确认最后一次 `COFFEE2_ACTION_ICE_SET_VALVE` 为关闭；Robot/咖啡机确认安全停止。
- 只在执行器安全后重新发起人工命令。

### Queue full

- `prvSubmit` 中区分普通 `xQueueSend` 和 urgent `xQueueSendToFront` 的返回值。
- 查看 `g_xCoffee2LogStatus.usPendingCount`、设备 busy 位和工作流状态。
- 记录 `MANUAL_COMMAND_QUEUE_FULL`，不要等待一个从未入队的命令完成。
- 等队列下降且 `g_xCoffee2WorkflowStatus.xState` 空闲后再重试。

## 8.2 Simulator 与真实板的分层验证

|层级|Simulator 可验证|必须使用 ST-Link/J-Link|
|---|---|---|
|Server 回调|FC06/FC16 范围、operation 清零、非法值日志|TCP 收包、实际客户端并发|
|设备提交|`Coffee2Command_t` 字段、队列 full/拒绝分支|任务实际调度和总线仲裁|
|协议转换|状态数组、结果码 switch、IO 位图公式|真实 Modbus 帧、CRC、从站响应|
|Transport|错误分支和超时预算|ETH/UART 电气质量、DMA、socket|
|执行器|只可看逻辑路径|机器人、阀门、泵、杯盖的机械动作|

Simulator 中可用 Watch 人工填充设备快照验证 `prvRefreshStatusRegisters`，但不应把人工填充的在线位当作现场在线。真实板调试前先读取默认状态，确认没有残留的模拟值。

## 8.3 源码文件定位速查

|文件|入口/重点函数|
|---|---|
|`Application/UserAPP/Coffee2App/Modbus_Tcp_Server/coffee2_server.c`|`prvCommitWrite`、`prvEvaluateManualCommands`、`prvEvaluateIoCommand`、`prvSubmitManual`、`prvRefreshStatusRegisters`|
|`Application/UserAPP/Coffee2App/Device/coffee2_device.c`|`xCoffee2CommandSubmit`、`prvSubmit`、`vCoffee2DeviceCommandStarted`、`vCoffee2DeviceCommandCompleted`|
|`Application/UserAPP/Coffee2App/Modbus_Rtu_Bus/coffee2_rtu_bus.c`|`vCoffee2RtuBusTask`、`prvSelectSerialProfile`、`prvExecute`|
|`Application/UserAPP/Coffee2App/Robot_Tcp/coffee2_robot_tcp.c`|`vCoffee2RobotTcpTask`、`prvExecute`、`prvWriteRisingEdge`、`prvWaitAction`|
|`Application/DeviceProtocol/Coffee2Protocol/coffee2_rtu_protocol.c`|各 `xCoffee2*Execute`、`xCoffee2IoModuleExecute`、`vCoffee2IoCommitModbus` 调用点|
|`Application/ProtocolStack/ModbusPort`|`xModbusPortRead*`、`xModbusPortWrite*`、结果码转换|
|`Application/Transport`|`xTransportSend`、`xTransportReceiveExact`、`xTransportControl`|

以上函数名可通过 `rg -n` 重新定位；断点不要写死源码行号。

## 9. 调试结束清单

1. 删除人为断点和条件断点，恢复优化、Debug 信息和下载设置。
2. 复读 IO `0x108A`、`0x108B`，确认 Unit2 输出处于安全值；制冰阀门、机器人和咖啡机均停止。
3. 保存日志生命周期、结果码、Bus/Transport 状态、Watch 快照和复现条件。
4. 若结果为 -8 或协议 DOCX 地址，停止测试并记录“当前未实现，禁止测试”；不得用新地址绕过 Server 固定协议。
