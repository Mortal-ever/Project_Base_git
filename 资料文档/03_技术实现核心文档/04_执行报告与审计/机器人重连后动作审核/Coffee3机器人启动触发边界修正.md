# Coffee3 机器人启动触发边界修正

日期：2026-09-10。范围：Coffee3Close 私有机器人任务及订单入口；不修改 Coffee2Open、公共 nanoMODBUS、TCP 会话实现和 Flash 分区。本轮未烧录。

## 1. 原因

原机器人任务在 `ucSessionReady == 0` 时，根据 `prvRobotOperational() == 0` 自动执行启动序列，并安排失败重试。这把 TCP 会话恢复与本体运行状态混在一起：停止、下使能使本体不满足运行条件，也可能进入自动启动。

另一个关联点是 `prvServiceInitialization()` 每轮用机器人 READY 重新计算初始化完成状态。即使残杯检查早已完成，手动停止也可能关闭订单入口，使“接到订单后按需启动”无法到达。

## 2. 修正后的边界

| 场景 | 完整启动流程 | 处理 |
| --- | --- | --- |
| TCP 首次连接/重连成功 | 每个新会话尝试一次 | CONNECTING → PROTOCOL_CHECK 设置一次性标记；协议连接检查通过后，由机器人任务消费标记 |
| 上位机停止、开始、下使能等本体指令 | 不触发 | 保留 TCP 连接门禁，执行对应本体命令 |
| 单个机器人协议动作调试 | 不触发 | 必须满足原有协议就绪条件；未就绪返回失败，不自动唤醒 |
| 有效订单进入制作前 | 按需尝试一次 | 刷新本体状态；未运行、未使能、报警或未观察到上电状态时执行启动 |
| 后台状态刷新、普通未运行状态 | 不触发 | 只更新状态 |
| 同一会话启动失败 | 不后台循环重试 | 日志记录失败，等待人工处理；真正新连接或后续有效订单才有对应的新触发机会 |

TCP 会话启动与订单启动是两个独立事件，不把手动停止→开始解释为重连。既有重连时 3100 清零及新鲜就绪判断保持不变。

若重连时存在未完成的机器人事务，调用现有 recovery-safe 启动分支，跳过其中的 STOP 步骤；保留原事务并继续核对命令/结果，不把重连当作原动作已完成，也不直接重新投递原动作。该组合需要真机验证。

## 3. 源码位置与实现

- `Application/UserAPP/Coffee3CloseApp/Robot_Tcp/coffee3_robot_tcp.c`
  - `prvRobotSessionEvent()`：只有新 TCP 连接成功事件设置 `s_ucConnectionStartupPending`。
  - `vCoffee3RobotTcpTask()`：消费一次性标记；取消“未运行则循环启动”的逻辑及启动退避计数。
  - 增加订单内部准备动作的处理，仅接受 WORKFLOW 来源；不进入机器人位置动作的接收/完成线圈握手。
  - 准备前后检查订单取消身份；完成后通过现有命令身份回报结果。
  - `ROBOT_STARTUP_RETRY` 改为 `ROBOT_STARTUP_NOT_READY`，避免日志暗示还会后台重试。
- `Application/UserAPP/Coffee3CloseApp/Device/coffee3_device.h`
  - 新增内部动作 `COFFEE3_ACTION_ROBOT_PREPARE_ORDER = 127`。不是新增上位机寄存器或机器人线圈地址。
- `Application/UserAPP/Coffee3CloseApp/WorkFlow/coffee3_workflow.c`
  - 保留初次残杯检查所需机器人 READY 门禁。
  - 残杯检查通过后，基础状态不再依赖本体运行 READY；仍检查 TCP 连接、门状态、IO 输入设备和故障状态。
  - 订单准入仍检查初始化完成和配方相关设备，但机器人运行状态改由订单开始阶段准备。
  - 通过订单合法性、IO、出餐/存储空位检查后，在步骤 10 投递准备动作，等待上限 30 秒；替代原先机器人 REFRESH 步骤。
  - 状态已满足时只刷新确认，不发送整套启动命令。准备失败不得继续后续生产步骤。

准备成功要求本体运行、使能、无报警、上电反馈及现有严格协议 READY。缺少 3100 就绪时仍会失败，不通过取消门禁强行发运动命令。本轮沿用现有启动命令序列，没有新增厂商“上电”命令地址；上电反馈仍缺失时，订单准备失败。

## 4. 日志

沿用现有 `[订单标识/级别][C3Robot:MBTcpClient]` 日志接口和来源配置。

- `TCP session startup attempt finished`：会话启动尝试结果；成功 INFO，失败 WARN。不是宣称必然启动成功。
- `Order requires robot startup: power/enable/run/alarm state not suitable`：订单准备发现状态不适合，需要启动。
- `Order robot preparation finished: result=...`：订单准备最终结果，携带订单标识。
- `ROBOT_STARTUP_NOT_READY`：一次启动序列结束仍未满足本体条件。

## 5. 验证结果与限制

- Keil Coffee3Close：0 errors，2 warnings。警告为工作流已有的 `s_ucHomeComplete`、`s_xInitRetryTick` 设置后未使用，本次未改其业务。
- Keil：Code 205056、RO-data 8584、RW-data 596、ZI-data 144196。
- GCC Coffee3Close-Debug：编译链接通过；Flash 182656 B、RAM 103328 B、CCMRAM 41064 B。
- 静态触发点检查：`prvStartup()` 调用处恰好两处；新会话 pending 置位处恰好一处；无启动自动重试调度日志。
- `git diff --check` 通过，仅提示 LF/CRLF 转换。
- 没有真机/机器人模拟器运行验证，编译通过不代表本体反馈时序已验收。
- 沿用的启动序列为有界同步执行；执行过程中到达的手动命令需等待当前启动调用返回，本轮没有把它改成可抢占状态机。取消会在返回后复核，不能撤回已经发出的本体命令。

## 6. 上板验收

| 测试 | 预期 |
| --- | --- |
| 首次连接 | 一次启动尝试；3100 新鲜就绪后进行原有初始化 |
| 初始化完成后手动 STOP，保持 TCP 在线 | 不出现新的 ROBOT_STARTUP_BEGIN；机器人保持停止 |
| 同一 TCP 手动 START | 仅发送 START；不清报警、不重复整套使能启动 |
| 手动 DISABLE / ENABLE | 仅执行对应命令，不触发整套启动 |
| 停止状态下提交单步位置调试 | 未就绪拒绝，不触发自动启动 |
| 停止状态下提交有效订单，其他准入条件满足 | 接收后步骤 10 发起一次启动；成功后继续制作 |
| 已运行且上电、使能、协议就绪时提交订单 | 步骤 10 仅检查，无新的启动序列 |
| TCP 断线重连 | 每次真正新连接再次尝试一次，与手动 START 无关 |
| 本体始终未就绪 | 单次尝试后告警，不在同一会话反复清报警/启动 |
| 动作中断网后重连 | 原命令身份和结果核对保留，检查 recovery-safe 分支及机器人程序行为 |

测试时同时记录 TCP 建连计数、启动日志、本体状态反馈及 3100；若停止/开始时实际发生了 TCP 断开，新连接按规则仍会启动一次，应先区分连接变化和本体状态变化。
