# 04_L4_Coffee2 私有应用层源码带读

> 只读 Review 分层文档（2026-09-02）。配套：`00_Coffee2工程架构分析.md`。
> 本层 = `Application/UserAPP/Coffee2App`，Coffee2 产品私有层。

## 1. 本层职责

- Coffee2 业务/Workflow（订单状态机、维护、热水、IO 刷新）。
- Coffee2 命令体系（`Coffee2Action_e`/`Coffee2Command_t`）与命令投递。
- 任务与队列创建（Task_Manager）、设备型号选择、UART/从站地址/IP 绑定。
- 每条物理总线 owner（Bus2-5）、Robot TCP owner、Modbus TCP Server 与寄存器语义。
- `coffee2_device_image` 私有状态、IO 状态采集/投影、Coffee2 OTA 编排。
- Coffee2 日志 source 适配与崩溃日志桥接（强实现公共弱接口）。

## 2. 本层不负责什么

- 不进入公共 CMake/Keil 公共组；不提供可复用公共 API。
- 不拥有设备原生协议实现（在 L3）；不做协议栈（L2）；不做平台（L1）。
- 本层目录中的“IO_State/coffee2_io”是 Coffee2 私有实现，不是公共 `DeviceModel`。

## 3. 当前目录树

```text
Application/UserAPP/Coffee2App/
├─ Config/         coffee2_app_config.h  coffee2_build_config.h  coffee2_io_names.h
├─ Task_Manager/   coffee2_manager.c/.h
├─ WorkFlow/       coffee2_workflow.c/.h
├─ Modbus_Rtu_Bus/ coffee2_rtu_bus.c/.h
├─ Robot_Tcp/      coffee2_robot_tcp.c/.h
├─ Modbus_Tcp_Server/ coffee2_server.c/.h
├─ Device/         coffee2_device.c/.h  coffee2_device_image.c/.h
├─ IO_State/       coffee2_io.c/.h
├─ Ota/            coffee2_ota.c/.h
└─ Comm_Log/       coffee2_log.c/.h  coffee2_crash_log_port.c
                   coffee2_retarget.c  app_comm_log_port.h
```

## 4. 实际进入 Coffee2 构建的文件

- GCC `coffee2_app` OBJECT（12 源，`Application/CMakeLists.txt:153-166`）：
  `coffee2_log.c/coffee2_crash_log_port.c/coffee2_retarget.c/coffee2_device.c/
  coffee2_device_image.c/coffee2_io.c/coffee2_rtu_bus.c/coffee2_server.c/
  coffee2_ota.c/coffee2_robot_tcp.c/coffee2_manager.c/coffee2_workflow.c`。
- Keil `Application/Coffee2App` 组同 12 源（`uvprojx:562-624`）。
- `coffee2_manager.h` 只被 `freertos.c`（经 CommonTargets）include。

## 5. 未进入构建的 legacy

- `Application/UserAPP/MilkTeaApp/*`：遗留，`OUT_OF_SCOPE`，不进 GCC/Keil。
- `DeviceModel/IO_State`（若物理存在）：MilkTea 语义 legacy，Coffee2 用自己的
  `IO_State/coffee2_io.*`。
- 旧 Coffee2 OTA/`DeviceProtocol` 文件：已删除。

## 6. 主要头文件与公共接口

- `coffee2_manager.h`：`xAppTaskManagerCreateTasks`、`vAppTaskManagerRunDefaultTask`、
  `vAppTaskManagerWaitNetworkStackReady`、`ucAppTaskManagerIsNetworkReady`、
  `vAppTaskManagerGetStatus`。
- `coffee2_device.h`：命令/动作枚举/事件组/状态/绑定/结果接口。
- `coffee2_device_image.h`：F200/杯盖/糖浆/冰机/秤/电表 Coffee2 镜像与 commit 接口。
- `coffee2_rtu_bus.h`、`coffee2_robot_tcp.h`、`coffee2_server.h`、
  `coffee2_workflow.h`、`coffee2_io.h`、`coffee2_ota.h`、`coffee2_log.h`。
- `coffee2_app_config.h`：产品级常量（IP/端口/波特率/栈/超时）。

## 7. 本层依赖

全部公共层 + L1：DeviceLibrary（L3）驱动、modbus_port/transport/app_log/app_ota/
app_crash_diag（L2）、HAL/FreeRTOS/lwIP（L1）。禁止被公共层反向依赖。

## 8. 本层调用者

- 启动组合根 `freertos.c`（经 `CommonTargets.h`）调用 manager 两个入口。
- 上位机（外部）：经 Modbus TCP 读写 Coffee2 寄存器。
- 本层内部模块互相调用（Workflow/Server/Bus/Device/IO/OTA/Log）。

## 9. 初始化顺序

`xAppTaskManagerCreateTasks`（`coffee2_manager.c:150-345`）：
`prvCaptureResetCause`（:155）→ `vTransportManagerInit`（:156）→
`xCoffee2LogSerialApplyDefault` + `xCoffee2LogInitWithTransport`（:157-159）→
`xCoffee2SerialApplyDefaults`（:167）→ `xEventGroupCreateStatic`（:202）→
`xCoffee2DeviceInitialize`（:215）→ `vCoffee2IoInitialize`（:228）→
`xCoffee2RtuBusInitialize`（:231）→ `xCoffee2RobotTcpInitialize`（:244）→
`xCoffee2WorkflowInitialize`（:257）→ `xCoffee2ServerInitialize`（:270）→
创建任务：`C2Server`（:286，idle+3）→ `C2Robot`（:291，idle+2）→ Bus2-5 循环
（:298-306，idle+2，任务名取总线配置 `pcName`）→ `C2Workflow`（:308，idle+2）→
`C2Log`（:315，idle+2，仅日志 Transport 就绪时创建）→ `STARTUP_COMPLETE`（:342）。
调度器启动后 defaultTask 先 `MX_LWIP_Init` 再 `vAppTaskManagerRunDefaultTask`
（网络监控 + 就绪发布）。

## 10. 任务、队列、同步对象清单（回答问题 1-3）

| 任务 | 入口 | 优先级 | 栈(StackType_t) | 队列/同步 | 周期/行为 |
| --- | --- | --- | --- | --- | --- |
| defaultTask | `StartDefaultTask`（`freertos.c:127`） | osPriorityNormal | 1024 B | 事件组（网络就绪） | 一次性建 lwIP + 常驻监控（manager） |
| C2Server | `vCoffee2ServerTask`（`coffee2_server.c:266`） | tskIDLE+3 | 1536 | 4 个 Socket 槽位 | accept/select/poll 20 ms |
| C2Robot | `vCoffee2RobotTcpTask`（`coffee2_robot_tcp.c:516`） | tskIDLE+2 | 1024 | 命令队列 4 + TcpClientSession | loop 20 ms、action poll 100 ms |
| C2Bus2..5 | `vCoffee2RtuBusTask`（`coffee2_rtu_bus.c:194`） | tskIDLE+2 | 384 | 每路命令队列 4 | idle 20 ms，逐条执行 |
| C2Workflow | `vCoffee2WorkflowTask`（`coffee2_workflow.c:386`） | tskIDLE+2 | 1024 | 订单队列 2 + EventGroup（设备） | IO 刷新 200 ms、订单串行 |
| C2Log | `vCoffee2LogTask`（`coffee2_log.c:174`） | tskIDLE+2 | 256 | ring 32 + binary sem | 事件驱动发送 |

- 队列内容：Bus/Robot 队列存放 `Coffee2Command_t`（`COFFEE2_COMMAND_QUEUE_LENGTH=4`）；
  Workflow 队列存放 `Coffee2Order_t`（32 寄存器快照，`COFFEE2_WORKFLOW_QUEUE_LENGTH=2`）；
  日志 ring 存放 `AppLogEntry_t`（32 条）。
- 同步对象：每设备独立 `StaticEventGroup_t`（`coffee2_device.c:49-56`）；日志信号量；
  Transport UART 每路 `xTxMutex/xTxDone/xRxStream`。
- 任务创建顺序与掩码：`coffee2_manager.c`（C2Server→Robot→Bus2-5→Workflow→Log），
  状态用 `APP_TASK_MASK_*` 记录（`coffee2_manager.h:28-36`）。

## 11. Config 模块带读（回答问题：如何选择设备/总线/网络）

- 静态 IP/网关：`192.168.5.10/24 gw .1`（`coffee2_app_config.h:21-33`），与 CubeMX
  生成 lwIP 一致（`lwip.c:66-77`），manager 在 LwIP 后 `netif_set_addr` 再应用一次
  （`coffee2_manager.c` `prvApplyNetworkConfiguration`）。
- 协议 owner：`COFFEE2_BUS*_PROTOCOL` 编译期选择（F200=3、RTU=1），
  `COFFEE2_MODBUS_BUS_COUNT` 由宏推导=3（Bus2 F200 不计）。
- Robot 协议变体 `COFFEE2_ROBOT_PROTOCOL_VARIANT=1`（`coffee2_app_config.h:60-63`）。
- `coffee2_build_config.h`：Keil 用 `--preinclude` 保证 nanomodbus 配置（见 L2）。

## 12. Task_Manager 带读（回答问题 4/15 的启动部分）

- `xAppTaskManagerCreateTasks`：静态资源+任务创建；`xAppTaskManagerRunDefaultTask`
  等（`coffee2_manager.c` 后半）负责网络就绪事件、RUN/ALM/TF LED 心跳与复位原因捕获
  （`prvCaptureResetCause`，`coffee2_manager.c:472-500`）。
- `vAppTaskManagerWaitNetworkStackReady` 供 Robot/Server 在 TCP 使用前等待
  `MX_LWIP_Init` 完成（事件组 `APP_TASK_EVENT_NETWORK_STACK_READY`）。
- FreeRTOS 堆放 CCM：`ucHeap[32768]`（`coffee2_manager.c:32-35`，ARMCC `CCM_HEAP`/
  GCC `.ccm_bss`）。


## 13. Config 常量速查（`coffee2_app_config.h` 行号）

- Server：端口 6001、Unit 1、最多 4 客户端、poll 20 ms（`:36-40`）。
- Robot：IP 192.168.5.1:502、Unit 1、connect 3000 ms、action poll 100 ms、
  motion timeout 60000 ms（`:43-57`）。
- Bus：Bus2=F200(115200)、Bus3=RTU(9600)、Bus4=RTU(19200)、Bus5=RTU(38400)
  （`:66-88`）。
- 栈：Log 256 / Server 1536 / Robot 1024 / RTU 384 / Workflow 1024（`:97-101`）。

## 14. Device（coffee2_device.c/.h）带读（回答问题 4/6/8/9/10）

- 绑定表与 route：`COFFEE2_ROUTE_COUNT=6`（Robot route0、Bus2..5 route2..5）
  （`coffee2_device.c:18,136-176`）。命令投递按 `pxBinding->ucRouteId` 找队列。
- `Coffee2Command_t`：device/action/param/orderId/orderEpoch/commandId/source/
  timeout/retry 与命令完成关联（`coffee2_device.h:96-126` 区间）。
- 每设备 EventGroup 位：ONLINE/READY/BUSY/COMM_FAULT/RECOVERING/TERMINAL 等
  （`coffee2_device.h:89-100`）。
- 关键接口：`xCoffee2CommandSubmit/SubmitUrgent`、`vCoffee2DeviceCommandStarted/
  Completed`、`vCoffee2DeviceSetOnline/Ready/Recovering`、`xCoffee2DeviceWaitCommand`、
  `lCoffee2DeviceGetTerminalResult`。
- 终端结果按 `deviceId+orderEpoch+commandId` 精确匹配（`coffee2_device.c:597-672`
  等待逻辑 + 环形历史 `s_aaxTerminalHistory`，`coffee2_device.c:69-74`）。
- 状态表 `g_axCoffee2DeviceStatus[COFFEE2_DEVICE_COUNT]`（CCM）。

## 15. coffee2_device_image（回答问题 10/11）

- `coffee2_device_image.c:15-26`：Coffee2 私有镜像（CCM）——咖啡机 24 状态字、
  杯/盖任务与线圈、糖浆/冰机/秤/电表镜像；`vCoffee2DeviceImageCommitF200/Cup/Lid`
  是 Bus owner 在事务成功后提交的入口（`coffee2_device_image.c:28-88`）。
- 为什么私有：镜像含产品语义且被 Workflow/Server 直接读，公共驱动不引用。

## 16. IO_State（coffee2_io.c/.h）（回答问题 11）

- 本机 X1-8=PE0-7、Y1-8=PE8-15（`coffee2_io.c:31-54`）；外部输入 16 路（Unit1/FC02）、
  外部输出 16 路（Unit2/FC01+FC05）——映射在 `coffee2_io.h:22-49` 与
  `coffee2_io_names.h`。
- 采集：`vCoffee2IoRefreshLocal`（50 ms 可采样）、Workflow `prvServiceIoRefresh`
  200 ms 提交 FC02/FC01 刷新（`coffee2_workflow.c:2142-2189`）。
- 投影：Server 读 4300/4301/4302/…/4336-4351 时即时采样本机并从 Bus5 owner 刷新镜像
  取外部状态（缓存快照按 2026-08-31/09-01 变更记录）。IO 状态变化以英文宏名逐点日志。
- `g_xCoffee2Io` 全局 + `vCoffee2IoGetSnapshot` 供 Server/Workflow 临界区读。

## 17. WorkFlow（coffee2_workflow.c/.h）（回答问题 5/7/9/16）

- 状态机：`Coffee2WorkflowState_e`（IDLE/RUNNING/COMPLETED/FAILED/CANCELING）
  （`coffee2_workflow.h:29-35`）；`Coffee2MachineState_e`、maintenance 枚举。
- 主循环 `vCoffee2WorkflowTask`：初始化自检→接订单（`xQueueReceive` 200 ms 超时兼
  IO/热水服务）→ `prvRunOrder` 串行步骤；失败走 `prvAbortDevices`（安全停止：
  冰阀关/F200 cancel/机器人 cancel）→ 状态 FAILED。
- 步骤模型：`prvRunStep(usStep, device, action, p0, p1, timeout)` 提交命令并等待该
  设备终端事件（提交 `:1622`、等待 `:1627-1648`、内部 5 s 上限 + 服务热水）。
- 订单快照：Server 写入的 32 寄存器复制进 `Coffee2Order_t` 队列
  （`xCoffee2WorkflowSubmitOrder`，`coffee2_workflow.c:292-344`）；新订单覆盖旧订单
  采用 cancel+队列 2 槽替换逻辑（pending 机制）。
- 业务条件等待（杯/盖到位/出货口检测）与设备完成轮询
  （`prvWaitBusinessCondition/prvWaitDeviceReportedComplete`）驱动 Robot/设备状态。
- 失败传播：步骤返回负值→上层按 ERROR_SAFE_STOP 等归一化；`prvPublish` 更新
  `g_xCoffee2WorkflowStatus` 并调 `vCoffee2ServerPublishWorkflow`。

## 18. Modbus_Rtu_Bus（coffee2_rtu_bus.c/.h）（回答问题 4/6/8）

- 4 路 owner：静态队列 + Transport UART 通道 + 3 份 ModbusPort（F200 无 Port）
  （`coffee2_rtu_bus.c:29-63`）。
- `vCoffee2RtuBusTask`：初始化通道/端口→循环取命令→绑定校验→重试循环（含最小帧间隔）
  →`prvExecute`→提交结果/在线状态（`coffee2_rtu_bus.c:194-360`）。
- `prvExecute` 是“Coffee2 动作 → 设备驱动”的映射中枢（`:387-558`）。
- 取消回调 `prvCommandCanceled` 转 `ucCoffee2CommandIsCanceled`（`:562-566`）。

## 19. Robot_Tcp（coffee2_robot_tcp.c/.h）（回答问题 4/6/7/8/12）

- 任务：网络就绪等待→Transport TCP/ModbusPort/TcpClientSession 建立→常驻循环处理
  命令与恢复（`coffee2_robot_tcp.c:516-1088`）。
- 事务状态机 `Coffee2RobotTransaction_t`：action 命令线圈/结果线圈解析
  （`prvResolveCoffee2Dobot`），edge 事件推进，接受轮询 100 ms，60 s 动作预算，
  断线/超时→ reconcile/recovering/重连。
- 手动命令与 Workflow 命令折叠（`prvFoldServerCommands`），位置指令映射
  （`prvRobotBasicAction`）。
- Robot 数据/状态：`g_xCoffee2RobotData/g_xCoffee2RobotTcpStatus`。

## 20. Modbus_Tcp_Server（coffee2_server.c/.h）（回答问题 3/6/11/13/15）

- 寄存器区（`coffee2_server.h:20-88`）：命令区 0x0000-0x00AF（176 字，含订单与维护）、
  状态区 0x1000-0x10FF（映射设备/IO 状态；实际 `STATUS_COUNT=0x0100` +
  IO 页 0x10F0-0x10FF 等由 `prvRefreshStatusRegisters` 刷新）、调试区 0x1100-0x117F、
  OTA/升级区 0x0200-0x0202、IO 调试 0x0208/0x0209。
- 多槽：`COFFEE2_SERVER_MAX_CLIENTS=4`；`vCoffee2ServerTask` 用 select 轮询
  （`coffee2_server.c:266-549`），每槽位为 reusable Socket Transport + ModbusPort
  server（`xModbusPortServerPoll`）。
- nanoMODBUS 回调：`prvReadHolding`（FC03/04，`coffee2_server.c:596-659`）、
  `prvWriteSingle/Multiple`（FC06/10）→ `prvCommitWrite`（地址校验）→
  `prvEvaluateOrder`（订单 latch/核对）与 `prvEvaluateManualCommands`（维护/取消/
  清障/取杯确认映射）→ `prvSubmitManual` 投递设备命令。
- 状态投影：`prvRefreshStatusRegisters`（`:1474+`）从 Workflow/IO/设备镜像写状态区；
  `vCoffee2ServerPublishWorkflow`（`:552-565`）、`vCoffee2ServerPublishOutput`
  （`:568-579`）由 Workflow 调用。
- 上位机地址映射语义由 Coffee2 私有定义（`COFFEE2_REG_*`），不进入公共层。
- 注：`coffee2_server.h:131` 注释“exactly two reusable client slots”与实现
  `MAX_CLIENTS=4` 不一致（P3，见 §30）。

## 21. Ota（coffee2_ota.c/.h）（回答问题 14）

- `coffee2_ota.h:17-24` 定义分区：元数据 0x08004000（magic 0xDEADBEEF）、应用
  0x0800C000-0x08060000、staging 0x08060000-0x080C0000；SRAM 校验范围 0x20000000-
  0x20020000。
- `coffee2_ota.c:20-26` 构造 `AppOtaConfig_t`（metadata sector 1、staging sector 7 起、
  http 端口 80、reset 500 ms、CRC 用 `hcrc`、lwIP 失败走
  `vCoffee2LogLwipResourceFailure`）。
- 接口薄封装 `xAppOtaInitialize/Begin/Write/Finish/Abort` → 公共
  `xAppOtaFlash*`；HTTP 上传经 `xCoffee2OtaHttpInitialize`（公共 raw lwIP HTTP）。
- 入口在上位机通过 Server 升级区写入触发（`prvCommitUpgradeWrite`，Server 延迟复位）。
- Bootloader 搬运约定（metadata 结构）无仓库源码，`UNKNOWN`。

## 22. Comm_Log（coffee2_log.c/.h、coffee2_crash_log_port.c）（回答问题 15）

- `coffee2_log.c:25-45`：18 个 Coffee2 log source 的任务名/模块名表
  （System/Server/Workflow/Robot/Bus2-5/IO/设备）。`xCoffee2LogInitWithTransport`
  创建 USART1 Transport 并交给公共 app_log 核心。
- 所有 Coffee2 `xCoffee2Log*` API 都是公共 `xAppLog*` 的窄适配
  （`coffee2_log.c:99-165`）；`xCoffee2LogPrintfOrder` 有界 `vsnprintf`。
- `coffee2_crash_log_port.c`：`lAppCrashDiagWrite` 强实现 → USART1 LL 轮询
  （关 DMA、刷新 IWDG、`APP_CRASH_UART_SPIN_LIMIT` 有界）。
## 23. 关键函数带读（按任务模板格式）

### xCoffee2CommandSubmit（命令投递，`coffee2_device.c`）

- 文件：`coffee2_device.c`（提交封装区 `:76-77` 附近 prvSubmit；对外
  `xCoffee2CommandSubmit`/`SubmitUrgent`）。
- 层：L4。调用者：Workflow `prvRunStep`、Server `prvSubmitManual`、维护/IO 刷新。
- 被调用者：`xQueueSend`/`xQueueSendToFront`（urgent）、分配 `ulCommandId`。
- 输入：`Coffee2Command_t*`；返回：`pdPASS/pdFAIL`。
- 读取状态：route 队列、初始化标志。修改状态：`s_ulNextCommandId`、队列。
- 阻塞：`xWaitTicks` 由调用者给（Workflow 100 ms / 非阻塞 0）。
- 正常路径：绑定 device→route 找队列→写 commandId→入队→成功。
- 错误：设备/队列无效、满→pdFAIL。日志：入队失败由调用者记。
- RAM/CCM：命令对象由调用者栈持有，入队按值复制（队列存储静态）。
- 断点：入队前、返回前。问题：urgent 用 send-to-front 是否可能打乱同总线顺序？

### vCoffee2DeviceWaitCommand（Workflow 等待完成，`coffee2_device.c:597-672`）

- 输入：deviceId、orderEpoch、commandId、等待 tick。
- 行为：先在终端历史/terminal/previousTerminal 中查精确匹配，否则事件组等待
  TERMINAL 位（分片 100 ms 轮询以服务 cancel/热水中断）。
- 返回：终端位或 0（超时）。问题：为什么结果要同时存在 status 与环形历史？

### vCoffee2RtuBusTask（owner 循环，`coffee2_rtu_bus.c:194-360`）

- 调用者：FreeRTOS。被调用者：`xTransportUartCreate/Open`、`xModbusPortClientInit`、
  `prvExecute`、`vCoffee2DeviceCommandStarted/Completed`、`vCoffee2DeviceSetOnline`。
- 正常：取命令→绑定匹配→重试循环（最小帧间隔 + 50 ms 间隔，retry≤limit）→提交。
- 错误：CANCELED/超时/链路失败映射；失败只记变化状态日志（防刷屏）。
- 问题：为什么 Bus2（F200）也走同任务函数？F200 分支在 prvExecute 内。

### prvRunStep（Workflow 通用步骤，`coffee2_workflow.c:1622-1648` 区域）

- 提交 REFRESH 等命令并轮询设备事件；取消位检查；5 s 内部上限 + 热水服务。
- 正常：命令完成事件→返回 0；错误：取消/超时/设备失败映射负值。
- 问题：5 s 与动作 `ulTimeoutMs` 的关系（动作超时在 Bus/Robot owner，Workflow 侧
  只等待事件/轮询）。

### vCoffee2ServerTask（`coffee2_server.c:266-549`）

- 监听→accept→槽位 attach→select→`xModbusPortServerPoll`；错误关闭槽位并统计。
- 问题：为什么 Server 用 Socket 后端而 Robot 用 Netconn？（Server 多连接 select；
  Robot 单连接会话。）

### prvEvaluateOrder（`coffee2_server.c:890+`）

- 原子写后检查 0x0007/0x0008 核对位→复制 32 寄存器→
  `xCoffee2WorkflowSubmitOrder`；成功 latch 并回写状态区/日志 ORDER_ACCEPTED，
  失败 ORDER_REJECTED_BUSY。问题：order 提交失败后核对位由谁清？

### xCoffee2OtaBegin/Finish（`coffee2_ota.c:33-49`）

- 调用公共 `xAppOtaFlashBegin/Write/Finish`；Finish 校验向量+CRC 后提交 metadata。
- 问题：staging 区擦除（sector 7 起 3 个）与 Coffee2 ld 上限 0x080C0000 是否吻合
  （CONFIRMED：staging 0x08060000..0x080C0000）。

### xAppTaskManagerCreateTasks（`coffee2_manager.c`）

- 见 §12；返回 `AppTaskManagerResult_e`（`coffee2_manager.h:18-25`）。
- 问题：任务全部在调度器启动前创建，栈从 CCM 堆分配，若失败如何上报
  （`APP_TASK_MASK_*` + `xCoffee2LogWriteField`）？

## 24. 端到端调用链（任务要求六条，全部以当前符号/行号支撑）

1. **系统启动链**：`startup Reset_Handler`（GCC `startup:60-102`）→ `main`
   （`main.c:105-171`：NVIC 清理→HAL/时钟→外设→`vAppCcmInit`→诊断→5 s→
   `osKernelInitialize/MX_FREERTOS_Init/osKernelStart`）→ `MX_FREERTOS_Init`
   （`freertos.c:85-118`：defaultTask + `xAppTaskManagerCreateTasks`）→
   `StartDefaultTask`（`freertos.c:127-135`）`MX_LWIP_Init` →
   `vAppTaskManagerRunDefaultTask`（`coffee2_manager.c`）。
2. **RTU 设备命令链**：Workflow `prvRunStep` 或 Server `prvSubmitManual` →
   `Coffee2Command_t` → `xCoffee2CommandSubmit` → route 队列 →
   `vCoffee2RtuBusTask` → `prvExecute` → `DeviceLibrary` 驱动 →
   `modbus_port` → `transport_uart` → HAL UART → 设备响应 → 驱动解析镜像 →
   `vCoffee2DeviceCommandCompleted` → `xCoffee2DeviceWaitCommand` 返回 → Workflow
   推进 / `coffee2_device_image` commit → Server 状态区。
3. **Robot TCP 链**：Workflow 步骤（ROBOT_* action）→ `xCoffee2CommandSubmit` →
   Robot 队列 → `vCoffee2RobotTcpTask`（`coffee2_robot_tcp.c:933-1010`）→
   `prvExecute/prvAdvanceAction/prvWriteRisingEdge` → `modbus_port(TCP)` →
   transport_netconn → lwIP → Dobot → 结果线圈轮询 → 状态更新 → Workflow 推进。
4. **上位机访问链**：Modbus TCP 请求 → `xModbusPortServerPoll` → nanoMODBUS 回调
   `prvReadHolding/prvWriteSingle/prvWriteMultiple`（`coffee2_server.c:596+`）→
   `prvCommitWrite`/`prvEvaluateOrder`/`prvEvaluateManualCommands` → Coffee2 状态或
   IO 镜像 → 响应经回调回传 → Socket 发送。
5. **OTA 链**：上位机写升级区 → `prvCommitUpgradeWrite` → `xCoffee2OtaHttpInitialize`
   /`xAppOtaFlashBegin/Write/Finish`（`coffee2_ota.c:33-49`、`app_ota_flash.c:189+`）
   → staging Flash 写回读 → 向量/CRC 校验 → metadata 提交 → 复位 → Bootloader
   （搬运约定 `UNKNOWN`）。
6. **崩溃日志链**：Fault/Assert/RTOS 钩子 → 汇编入口（`app_crash_fault_gcc.S:30-103`
   或 ARMCC）→ `vAppCrashDiagFaultEntry/RtosEntry/AssertCEntry`
   （`app_crash_diag.c:287+`）→ `prvCrashRun` → `lAppCrashDiagWrite`（公共弱实现）
   → Coffee2 强实现 `coffee2_crash_log_port.c:63-66` → USART1 LL 轮询输出。

## 25. 任务要求的“16 问”集中回答

1. Coffee2 创建哪些任务：defaultTask（平台）+ C2Server/C2Robot/C2Bus2-5/C2Workflow/
   C2Log（`coffee2_manager.c`；§10 表）。
2. 每任务入口/优先级/栈/周期：见 §10 表。
3. 每队列存放什么：Bus/Robot 队列存 `Coffee2Command_t`；Workflow 队列存
   `Coffee2Order_t`（32 寄存器）；日志 ring 存 `AppLogEntry_t`。
4. 每条物理总线由谁拥有：Bus2-5 各一 owner 任务；Robot TCP 一个 owner；
   Server（上位机）无“总线”，多槽在单 Server 任务内轮询（主文档 §9.4）。
5. Coffee2Action/Coffee2Command/Workflow Step 关系：Step（`usStepId`）是业务流程
   位置；Step 生成一个或多个 Coffee2Command（设备+动作+参数）；Command 进入设备
   route 队列执行；`Coffee2Action_e` 是命令的动作字段（`coffee2_device.h:56-87`）。
6. 命令如何进入 RTU 或 Robot owner：`xCoffee2CommandSubmit` 按绑定 route→目标队列
   （`coffee2_device.c`），Bus/Robot owner 各自循环取队列。
7. Workflow 如何等待完成：`prvRunStep`→`xCoffee2DeviceWaitCommand` 事件组等待 +
   终端历史精确匹配；Robot 动作另有 owner 内事务推进（accept/motion 超时）。
8. 设备结果如何关联原命令：`commandId+orderEpoch+deviceId` 三要素；完成写入
   `Coffee2DeviceStatus_t` 的 terminal/previous 与环形历史（`coffee2_device.c:597-672`）。
9. 超时/取消/失败如何传播：命令取消→`vCoffee2OrderCancelRequest`→owner 协作取消
   回调；超时→owner 归一化→terminal 事件；Workflow 收到负值→`prvAbortDevices`
   安全停止→FAILED/ALARM 投影。
10. coffee2_device_image 为何私有：镜像含 Coffee2 产品语义且为 Server/Workflow 直接
    读取，公共驱动禁止引用（§15）。
11. IO 状态如何采集/投影/提供 Server：本机 GPIO 即时采样 + Bus5 输入/输出刷新
    （FC02/FC01），commit 到 `g_xCoffee2Io`，Server 按协议位图/寄存器读取（§16）。
12. 机器人动作如何执行：owner 解析动作→命令/结果线圈→写命令线圈上升沿→轮询结果
    线圈→edge 日志与事务完成（`coffee2_robot_tcp.c` prvAdvanceAction/prvWriteRisingEdge）。
13. Modbus TCP 地址如何映射：`coffee2_server.h` 的 `COFFEE2_REG_*` 分区 +
    `prvReadHolding/prvWriteSingle/Multiple` 回调（§20）。
14. OTA 如何调用公共层：`coffee2_ota.c` 构造 `AppOtaConfig_t` 调 `xAppOta*`（§21）。
15. 日志如何定位设备/动作/失败阶段：source（每设备/总线一个）+ ASCII 事件文本
    （`RTU_DEVICE_ONLINE/ROBOT_ACTION_START/ORDER_START/…`）+ result + field
    （§22；`coffee2_log.h`）。
16. 哪些代码最晦涩：① Workflow 的 pending-order/取消/epoch 交织（`coffee2_workflow.c:
    292-344`）；② Robot 事务 reconcile/recovering/重连三态与手工命令折叠
    （`coffee2_robot_tcp.c:615-1010`）；③ Server 订单 latch + 状态区投影
    （`coffee2_server.c`）；④ 结果精确匹配（terminal 历史）为何不用单个字段。

## 26. 模板对照（统一分层文档 24 项 → 本节位置）

职责=§1；不负责=§2；目录树=§3；进构建=§4；legacy=§5；头文件=§6；依赖=§7；
调用者=§8；初始化顺序=§9；关键数据结构=§14/§15/§16/§10 表；静态全局=§15/§19/§20；
任务上下文=§10；队列同步=§10；关键函数带读=§23；正常路径=§17/§18/§19/§20/§24；
错误超时=§17/§18/§23/§25.9；日志=§22/§25.15；RAM/Flash/栈/DMA=§27；GCC=§27；
Keil=§27；断点=§28；检查表=§29；问题=§30；交叉链接=§31。

## 27. RAM/Flash/栈/DMA 与工具链归属

- CCM 静态：`g_axCoffee2DeviceStatus`/EventGroup/路由队列句柄、终端历史、
  `g_xCoffee2WorkflowStatus`、Server 寄存器镜像（`s_ausCommandRegisters` 等）、
  `g_xCoffee2Io`、设备镜像、FreeRTOS 堆 `ucHeap`（`coffee2_manager.c:32-35`）——
  全部 `APP_CCM_DATA`/`CCM_HEAP` 段（`compiler_compat.h:15-26`）。
- 普通 SRAM：日志 UART Transport（DMA TX 需要）、各 UART Transport 上下文在 Bus 静态
  上下文（普通 .bss）。
- 栈需求差异：Server 1536（FC 处理 + printf）、Robot 1024、Workflow 1024、Bus 384、
  Log 256（`coffee2_app_config.h:97-101`）。
- GCC：Coffee2 12 源进入 `coffee2_app` OBJECT（`Application/CMakeLists.txt:153-166`），
  Debug/Release preset；链接用 Coffee2 OTA ld。
- Keil：`Application/Coffee2App` 组 12 源全含入（`uvprojx:562-624`），Scatter
  `Coffee2_CCM.sct`。
- 资源红线：DMA 缓冲不得在 CCM；UART TX 经普通 SRAM 暂存（Transport）。

## 28. 推荐断点（Coffee2 业务联调）

- Workflow：`coffee2_workflow.c:546`（`prvRunOrder` 返回）、`:1622/:1627`
  （命令提交/等待）。
- Device：`coffee2_device.c` 命令入队/终端完成回调。
- RTU：`coffee2_rtu_bus.c:317`（prvExecute 调用）、`:337`（结果提交）。
- Robot：`coffee2_robot_tcp.c:941`（命令开始）、`:975`（prvExecute）、恢复/重连点。
- Server：`coffee2_server.c:512`（poll）、prvReadHolding/prvEvaluateOrder。
- IO：`coffee2_io.c` commit 函数与 Workflow `prvServiceIoRefresh`。
- OTA：`app_ota_flash.c:58`（prvProgramWord）。
- 崩溃：`app_crash_diag.c:287`、`coffee2_crash_log_port.c:63`。

## 29. 人工 Review 检查表（Coffee2）

- [ ] 任务创建数量/顺序与 manager 注释一致（Server→Robot→Bus2-5→Workflow→Log）？
- [ ] 每设备命令完成是否严格按 deviceId+epoch+commandId 匹配？
- [ ] 每个队列长度与元素大小是否匹配其静态存储？
- [ ] Bus2-5 波特率/协议与 `coffee2_rtu_bus.c:46-56` 表一致？
- [ ] Server 寄存器区是否与上位机协议一致（0x0000/0x1000/0x1100/0x0200/0x10F0）？
- [ ] Workflow 步骤失败是否总是进入 `prvAbortDevices`（安全停止）？
- [ ] Robot 恢复/重连是否保持命令事务不被错误完成？
- [ ] IO 外部输入/输出是否只由 Bus5 owner 串行访问？
- [ ] OTA staging 是否在链接 flash 之外且不与应用/元数据重叠？
- [ ] 公共层是否未 include 本层任何头文件？

## 30. 已发现问题

- P2/CONFLICT（L4/架构边界）：`coffee2_rtu_bus.c:387-558` 的 `prvExecute` 直接
  switch 全部 Coffee2 动作到设备库，是唯一“动作→驱动”映射点；职责集中、改动集中，
  若新设备接入需在此加 case（设计上可接受，但文档需提醒）。本任务是否修改：否。
- P3/CONFLICT（L4/文档）：`coffee2_server.h:131` 注释“exactly two reusable client
  slots”与实际 `COFFEE2_SERVER_MAX_CLIENTS=4` 不一致（实现以 4 为准）。
  本任务是否修改：否。
- P3/UNKNOWN（L4）：`coffee2_rtu_bus.c` 中 F200 Bus2 也创建任务但 `prvExecute`
  F200 分支不使用 ModbusPort——正常；但 Bus 任务栈 384 若未来 F200 长帧/嵌套
  轮询加深需复核水位。本任务是否修改：否。
- P3/一致性：`coffee2_manager.c` 复位原因捕获在 `xAppTaskManagerCreateTasks` 入口
  （`memset` 后）即执行（`:154-155 prvCaptureResetCause`），先于全部模块初始化和任务
  创建；`RESET_CAUSE` 日志紧随其后（`:190-192`）——时序正确，无问题。此处不再列为缺陷。
  本任务是否修改：否。
- P1/CONFLICT（Flash/OTA 前提，承 L1）：MCU 型号 VGT(VG 1MiB) vs 维护 IOC
  VET(512K) 未统一；若烧录到 512K 实物的 staging 区越界风险。禁止未确认烧录。
  本任务是否修改：否。

## 31. 与其他文档的交叉链接

- 主文档 `00_Coffee2工程架构分析.md` §3.4/§4.5/§9。
- L1 文档：启动链与 freertos.c；L2 文档：日志/崩溃/OTA/transport/modbus_port；
  L3 文档：各设备驱动 API。
- 架构定稿与缓存、全局审查索引（见 00_README 与根 CHANGES.md 本日记录）。




