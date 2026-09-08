# 20 Coffee3Close Workflow、Server、IO 与 OTA 业务闭环

适用2026-09-08源码。先打开本篇附录的workflow.h和server.h，再读本文函数分解。本文区分协议回包、命令终态、设备物理完成、订单完成与客户取走五种不同结果。

## 一、先认识业务对象

`Coffee3Order_t.ausRegister[32]` 是接受订单时的寄存器快照。Workflow队列按值复制它，后续上位机修改命令寄存器不应改变已接订单。

WorkflowState 的 IDLE=0、RUNNING=1、COMPLETED=2、FAILED=3、CANCELING=4 描述一条工作流。MachineState 的 DEFAULT=0、IDLE=1、INITIALIZING=2、BUSY=3、ALARM=4 描述整机；两组不能按数字相等互换。维护类型0无、1糖浆清洗、2咖啡清洗、3果乳出液、4果乳清洗，运行状态另用0闲、1运行、2完成、3失败、4报警。

### WorkflowStatus 字段如何联动

| 字段 | 意义、写者和读者 |
| --- | --- |
| ulCompletedOrderCount / ulFailedOrderCount | Workflow处理终态累加，Server调试读取 |
| ulOrderEpoch | 每个业务代次标识，下传command并用于取消 |
| usCurrentOrderId / usCurrentStep / lLastError | 当前单、当前步骤、业务错误；发布到Server |
| xState / xMachineState | 工作流与整机分别发布 |
| ucCancelRequested | 请求函数置位，等待循环读取 |
| ucOrderAdmissionOpen | 接单许可，与设备online不同 |
| ucHotWaterState / ucCoffeeCleanState / aucFruitState[2] | 并行服务/维护的对外状态 |
| ausOutputState[2] / ausOutputOrderId[2] | 出口事务状态和归属订单；不能被下一单直接覆盖 |
| usActiveOutput / usAction / ucDeviceId | 当前资源、动作、设备诊断信息 |
| ucCommandSent / ucDeviceDone / ucPhysicalVerified | 分别为入队、命令完成、物理后置条件通过 |
| ucPositionUncertain / ucRecoveryRequired | 动作途中失败可能导致位置不确定与恢复锁 |
| lSafetyResult | 失败后停止动作结果，不能覆盖原始制作失败 |

Coffee3额外 `ucStorageReserved` 为预留储位，`ucContentComplete` 标记内容制作完成。字段存在是源码事实，不代表该字段处处拥有原子快照保证；检查实际写入和Server读法。

## 二、prvRunStep 逐段读

先检查取消，再建立局部command。步骤小于0xF000作为订单步骤；0xFD00..0xFDFF作为初始化步骤，日志去重规则不同。用 `usStepId` 关联业务阶段，用device ID查诊断名。

1. 把action、两个参数、step、订单ID、epoch写入command。非订单/维护步骤epoch为0，不能拿它参与订单代次取消。
2. 按设备选择command.ulTimeoutMs；机器人使用动作预算，RTU一般使用单事务预算。外层等待预算另算，不能把两者当成同一个timeout。
3. submit最多等待100ms入队，失败返回QUEUE=-1001。成功置ucCommandSent；位置动作还置ucPositionUncertain。
4. 循环每次最多等100ms，调用DeviceWaitCommand(device,epoch,id)。CANCELED检查是否被手动命令替代；DONE置ucDeviceDone然后返回0。
5. 其他terminal读取原始设备结果，用可读日志打印步骤、设备名、action、result；函数向业务返回DEVICE=-1003。原始通信result仍需在Device终态查询。
6. 每轮检查cancel和外层timeout，分别返回-1004与-1002；等待间隙推进热水和IO服务。
7. 机器人位置动作必须本次id/epoch相同且accepted已置位才开始运动计时；未受理或recovering会刷新起点。这意味着当前运动计时并非整个请求的绝对总期限。

### 为什么还要 WaitBusinessCondition / WaitDeviceReportedComplete

例如落杯写寄存器返回成功只是设备接受动作，下一步 `prvWaitBusinessCondition` 刷新对应设备/IO并验证物理条件。糖浆则循环读设备完成状态。条件不满足要继续等，通信失败要返回失败，超时和取消也有独立出口。把 `prvRunStep == 0` 直接当落杯成功，会跳过这些后置条件。

## 三、订单和初始化的本产品分支

### Coffee3：初始化先找空储位，再探残杯

`prvRunInitialization` 停门并关产品输出。三次source依次为“当前手上”（pickup action=0）、TAKE_COFFEE、TAKE_LID。每次 `prvProbeResidualCup` 先SelectStorage；没有经验证的空位立即失败，然后按需取杯、PUT_STORAGE、等待500ms同时服务后台、刷新输入。检测到杯输出WARN，但函数返回0，允许下一来源另选空位；无杯同样通过。只用两个储位，不把出餐口当第三储位。

全部探测通过→机器人HOME→StartDoor(1)上升关门→循环推进服务等待DI1。门故障返回IO=-1010。当前初始化与旧业务草稿不同之处以这条源码路径为准。

### Coffee3：每单开始先检查资源

订单号高4位0xD识别线下。先刷新有效IO，要求X4有水、X5/X6有桶且X7不高液位；线下检查出餐口，线上SelectStorage并保存ucStorageReserved。资源不合格在生产动作之前返回。

之后回原点、取杯、落杯并验证，按需制冰、果乳、糖浆、咖啡、压盖。冰量非零选冷杯/盖道2，零选热杯/盖道1。咖啡类型0xFFFF跳过咖啡站。M50 MAKE在80步骤使用咖啡动作预算；当前路径没有Coffee2的F200步骤85等待器，是否已完成须沿M50 Execute内部检查，不能给Coffee3复制虚构的85步骤。

### Coffee3：线上/线下完成分叉

线上放杯前再次刷新并检查预留储位仍空，ServerSelectStorage发布选择，180 PUT_STORAGE→185储位传感器→保存储位订单号。线下先置ucContentComplete，发布production=2、step=179，源码日志明确no host ACK needed。随后查出餐口→发布出口2→PUT_OUTPUT→验证有杯→出口5，并将OutletPhase设1、清空计时和ACK、StartDoor(2)下降开门。失败出口3。两种成功路径最后都执行190 HOME。

### Coffee3：prvServicePickup的两个状态机

DoorDirection 0停止/1上升/2下降。StartDoor先关闭DO1/DO2、记录方向和start tick。推进时若故障锁、DI1/DI2同时有效或超时，停输出、锁门故障、关接单；若对应限位有效则停，否则打开该方向。当前配置运动超时5000ms。

OutletPhase 0无事务/1等待观察有杯/2等待连续无杯/3已上报取走等ACK。phase1见X3有杯转2；phase2杯出现或门仍动即重置空计时。无杯持续30秒后发布0x15，StartDoor(1)。phase3还需pickup pending、门停止、DI1有效、杯仍无，才清pending/phase并发布出口0。上报取走、关门到位、消耗ACK是不同条件。

### 缺水和取储位订单

初始化完成且IO有效后X4失水置water alarm，日志说明本单继续、阻止下一单直到恢复确认。`xCoffee3WorkflowSubmitStoragePickup` 为储位转出餐请求，`prvRunStoragePickup` 负责实际路径；ServerFinishRequest按请求种类清触发，不用新制作订单覆盖储位订单归属。

## 四、Server地址不是一整块任意内存

先读 `prvReadHolding`：检查NULL和quantity=0返回异常03；用32位加法校验整个请求是否落在某个合法窗口，再拷贝。跨两个窗口的请求即使各端点合法也可能返回异常02。

| 窗口 | 内容与处理 |
| --- | --- |
| 0x0000..0x00AF | 命令块；读回命令值不表示动作完成 |
| 0x1000..0x10FF | 状态块，读取前prvRefreshStatusRegisters投影 |
| 0x1100..0x117F | 私有诊断块，prvBuildDebugRegisters临时构建 |
| 0x0208..0x0209 | 单独IO调试窗口；十进制520、521 |
| 升级窗口 | 由配置宏决定；查看附录准确常量 |

`prvEvaluateOrder` 读取命令快照，检查触发/验证和保留订单号，然后调用WorkflowSubmitOrder；成功才锁存。接收函数返回成功表示进入软件流程，不代表已经出咖啡。

FC06和FC16最终进入写回调。调试寄存器不是整个命令数组的一部分，所以添加一个宏不够，还要读回调/写回调/合法区间都支持。

### IO状态页逐地址

| 地址 | 显示32位组的含义 | 当前实际数据 |
| --- | --- | --- |
| 0x10F0/0x10F1 | 本机DI低/高16位 | 低字bit0..7，余位0 |
| 0x10F2/0x10F3 | 外部DI第1组 | Unit1的16路，第二字0 |
| 0x10F4..0x10F7 | 外部DI第2/3组 | 未装配，0 |
| 0x10F8/0x10F9 | 本机DO | 低字bit0..7，余位0 |
| 0x10FA/0x10FB | 外部DO第1组 | Unit2的16路，第二字0 |
| 0x10FC..0x10FF | 外部DO第2/3组 | 未装配，0 |

Unit2输出图像在私有结构中叫 `aucMB2YPin`，并不意味着必须放到“外部DO第2组”寄存器。物理从站、结构字段命名、展示页分组是三种编号。

### 一次写0x0208=0x0005的代码路径

`prvCommitIoDebugWrite` 先检查初始化完成；再检查高8位全0，否则异常03；调用 `ucCoffee3IoApplyLocalDebugMask`。该函数循环8个点，bit为1输出SET，bit为0输出RESET，最后刷新本机镜像。因此写5是DO1和DO3开，其余关，不是“追加打开两点”。

0x0209则投递 IO_WRITE_MASK 到Bus5，执行结果和读回由设备owner随后产生。Modbus成功应答与外部模块物理完成的时刻不同。多个寄存器写入顺序执行，不提供跨本机GPIO/RTU的硬件原子提交。

## 五、IO_State函数体和极性

`vCoffee3IoInitialize` 清镜像、清baseline，立即采样。`vCoffee3IoRefreshLocal` 在临界区外采GPIO，再在临界区复制旧值、新值、tick、version，退出临界区后打印变化。第一次只建立基线，不把全体点位当作变化。

```c
/* 输入逻辑值：高为0，低为1。 */
(HAL_GPIO_ReadPin(port, pin) == GPIO_PIN_SET) ? 0U : 1U;
```

输出读ODR并用SET表示1，不能把输入低有效规则无条件应用到输出。输出镜像证明软件输出电平，不证明继电器触点或机构已动作。外部图像只有成功读取后才commit，更新时间和valid可以区分有效零与尚无样本。

`Coffee3IoState_t` 的xInput/xOutput分别含本机8点和两个16点数组；ulVersion每次提交增加；ulLocalUpdateTick标本机采样时间；aulModbusUpdateTick[2]/aucModbusValid[2]标模块有效性。`GetSnapshot`临界区复制，不返回内部可写指针。

Coffee3普通SetLocalOutput在打开DO1/DO2前检查另一方向，冲突返回0。调试ApplyLocalDebugMask直接写全部GPIO，绕过这条互锁；所以不能声称调试也受门互锁保护。这是当前代码行为，教材不替源码增加保护。

## 六、维护、制冰与取消要怎样读

`prvDispenseIce` 先tare、等待、取稳定秤值，按目标0.1g计算阀脉冲；开阀后计时并检查取消，随后始终尝试关阀，再复测和有限次数修正。关阀失败单独处理，不能因为达到重量就忽略关闭错误。

`prvRunFruit` 用通道选阀和泵，数量换算运行时间，关闭输出和失败返回由本函数维护。`prvRunMaintenance` 依据维护类型串行执行糖浆/咖啡/果乳操作。热水服务以状态推进方式穿插在Workflow等待中，并非新增的独立任务。

`prvAbortDevices` 对活跃设备发安全停止。取消是协作式检查，不能保证已发出的动作从未生效。读ucPositionUncertain和ucRecoveryRequired判断是否需要恢复，不要根据取消标志立即宣布物理安全。

## 七、OTA私有接入

`coffee3_ota.c` 保存 `static const AppOtaConfig_t`：metadata/staging/application起止地址、SRAM范围、magic、Flash扇区、HTTP配置、CRC句柄与日志回调。地址属于产品组合，擦写和传输实现位于公共Common/Ota。

Initialize把配置交给公共服务；Begin/Write/Finish每次先确认初始化，再调用对应公共函数。初始化失败被门面转换为BUSY，排查时还要看公共初始错误。Abort调用公共终止。WorkflowAcquireOta是业务准入，必须从Server/升级入口一起追，不能只看这个短门面认为任意时刻可擦Flash。

## 八、日志与崩溃如何对接

Comm_Log维护本产品source与串口参数，调用公共Text/Printf/Field接口。正文既有EVENT，也有可读字符串，教材照实际代码解释。IO日志是首次baseline后逐点比较，仅变化时打印；不能把“无变化日志”理解成没有刷新。

`coffee3_crash_log_port.c` 提供公共诊断弱接口的产品输出实现，retarget承接工具链运行库输出。崩溃时不应假设调度器和普通日志消费仍工作；具体同步输出路径见第04册。两个产品只选择自己的强实现。

## 九、读懂后的检查题

1. 为什么FC03返回全0不能单独证明模块离线？找valid/tick。
2. 为什么一个设备BUSY不等于整机只能处理这一件事情？找到等待循环推进的服务。
3. 何处区分订单完成、放杯完成和客户取走？逐个定位寄存器写入。
4. 0x0208写入成功后为何其他位被关掉？读mask循环。
5. 哪个返回值是原始设备result，哪个是Workflow归一化错误？查terminal键。
## 源码导航与版本证据

### [Application/UserAPP/Coffee3CloseApp/WorkFlow/coffee3_workflow.c](../../../../Application/UserAPP/Coffee3CloseApp/WorkFlow/coffee3_workflow.c)

SHA-256：`AC2C4116CF88DB50FEB50706BEE9E0666A556AB66510D2FB92C6230DBFE7A5CF`。以下行号对应 2026-09-08 工作区快照。

| 定义行 | 函数 | 下行调用（词法检索，非运行时跟踪） |
| --- | --- | --- |
| 220 | `xCoffee3WorkflowAcquireManual` | taskENTER_CRITICAL, taskEXIT_CRITICAL |
| 249 | `vCoffee3WorkflowReleaseManual` | taskENTER_CRITICAL, taskEXIT_CRITICAL |
| 259 | `xCoffee3WorkflowAcquireOta` | pdMS_TO_TICKS, taskENTER_CRITICAL, taskEXIT_CRITICAL, vCoffee3IoGetSnapshot, vCoffee3IoRefreshLocal, xCoffee3WorkflowAcquireManual, xTaskGetTickCount |
| 300 | `vCoffee3WorkflowReleaseOta` | taskENTER_CRITICAL, taskEXIT_CRITICAL, vCoffee3WorkflowReleaseManual |
| 311 | `vCoffee3WorkflowConfirmPickup` | taskENTER_CRITICAL, taskEXIT_CRITICAL |
| 321 | `prvIoValid` | pdMS_TO_TICKS, xTaskGetTickCount |
| 329 | `prvStartDoor` | ucCoffee3IoSetLocalOutput, xTaskGetTickCount |
| 338 | `prvServicePickup` | pdMS_TO_TICKS, prvIoValid, prvPublishOutput, prvStartDoor, taskENTER_CRITICAL, taskEXIT_CRITICAL, ucCoffee3IoSetLocalOutput, vCoffee3IoGetSnapshot, vCoffee3IoRefreshLocal, xCoffee3LogPrintfOrder, xTaskGetTickCount |
| 424 | `prvPublishOutput` | taskENTER_CRITICAL, taskEXIT_CRITICAL, vCoffee3ServerPublishOutput |
| 437 | `prvCheckOutputEmpty` | prvIoValid, prvRefreshDeviceQuiet, prvServicePickup, vCoffee3IoGetSnapshot |
| 457 | `prvSelectStorage` | prvIoValid, prvRefreshDeviceQuiet, vCoffee3IoGetSnapshot |
| 477 | `xCoffee3WorkflowInitialize` | memset, xQueueCreateStatic |
| 512 | `ucCoffee3WorkflowInitializationComplete` |  |
| 518 | `xCoffee3WorkflowSubmitMaintenance` | taskENTER_CRITICAL, taskEXIT_CRITICAL |
| 545 | `xCoffee3WorkflowSetHotWater` | memset, taskENTER_CRITICAL, taskEXIT_CRITICAL, xCoffee3LogWriteFieldOrder |
| 580 | `vCoffee3WorkflowAcknowledgeAlarm` | prvIoValid, taskENTER_CRITICAL, taskEXIT_CRITICAL, vCoffee3IoGetSnapshot |
| 611 | `xCoffee3WorkflowSubmitOrder` | taskENTER_CRITICAL, taskEXIT_CRITICAL, xQueueSend |
| 634 | `xCoffee3WorkflowSubmitStoragePickup` | taskENTER_CRITICAL, taskEXIT_CRITICAL |
| 654 | `xCoffee3WorkflowSubmitManualIce` | taskENTER_CRITICAL, taskEXIT_CRITICAL, uxQueueMessagesWaiting |
| 677 | `vCoffee3WorkflowRequestCancel` | taskENTER_CRITICAL, taskEXIT_CRITICAL, vCoffee3OrderCancelRequest, xCoffee3LogWriteFieldOrder |
| 694 | `vCoffee3WorkflowTask` | failed, memset, pdMS_TO_TICKS, prvAbortDevices, prvDispenseIce, prvPublish, prvQueuePendingOrder, prvRunInitialization, prvRunMaintenance, prvRunOrder, prvRunStoragePickup, prvServiceHotWater, prvServiceIoRefresh, taskENTER_CRITICAL, taskEXIT_CRITICAL, vCoffee3ServerFinishRequest, vCoffee3ServerPublishOrder, vTaskDelay, xCoffee3LogPrintfOrder, xCoffee3LogWrite, xCoffee3LogWriteFieldOrder, xCoffee3LogWriteOrder, xQueueReceive |
| 963 | `prvRobotPositionAction` |  |
| 971 | `prvQueuePendingOrder` | taskENTER_CRITICAL, taskEXIT_CRITICAL, xCoffee3LogWriteFieldOrder, xQueueSendToFront |
| 1006 | `prvRunStep` | lCoffee3DeviceGetTerminalResult, memset, pdMS_TO_TICKS, prvPublish, prvRobotPositionAction, prvServiceHotWater, prvServiceIoRefresh, pxCoffee3DeviceGetBinding, xCoffee3CommandSubmit, xCoffee3DeviceWaitCommand, xCoffee3LogPrintfOrder, xCoffee3LogWriteFieldOrder, xTaskGetTickCount |
| 1231 | `prvRunOrder` | prvCheckOutputEmpty, prvDispenseIce, prvIoValid, prvOrderValid, prvPublishOutput, prvRefreshDeviceQuiet, prvRunFruit, prvRunStep, prvSelectStorage, prvStartDoor, prvWaitBusinessCondition, prvWaitDeviceReportedComplete, vCoffee3IoGetSnapshot, vCoffee3ServerPublishWorkflow, vCoffee3ServerSelectStorage, xCoffee3LogPrintfOrder, xCoffee3LogWriteFieldOrder |
| 1537 | `prvDispenseIce` | pdMS_TO_TICKS, prvCalculateIcePulseMs, prvReadStableScale, prvRunStep, vTaskDelay, xCoffee3LogWriteField |
| 1629 | `prvCalculateIcePulseMs` |  |
| 1646 | `prvReadStableScale` | pdMS_TO_TICKS, prvRunStep, vTaskDelay |
| 1686 | `prvRunIoOutput` | lCoffee3DeviceGetTerminalResult, memset, pdMS_TO_TICKS, prvServiceHotWater, xCoffee3CommandSubmit, xCoffee3DeviceWaitCommand, xTaskGetTickCount |
| 1739 | `prvSetProductOutputsOff` | prvRunIoOutput, ucCoffee3IoSetLocalOutput |
| 1766 | `prvRunInitialization` | pdMS_TO_TICKS, prvProbeResidualCup, prvRunStep, prvServiceIoRefresh, prvSetProductOutputsOff, prvStartDoor, upper, vTaskDelay, xCoffee3LogPrintfOrder |
| 1806 | `prvProbeResidualCup` | failed, prvDelayWithServices, prvIoValid, prvRefreshDeviceQuiet, prvRunStep, prvSelectStorage, vCoffee3IoGetSnapshot, vCoffee3ServerSelectStorage, xCoffee3LogPrintfOrder |
| 1852 | `prvRunFruit` | prvDelayWithServices, prvRunIoOutput, prvRunStep, vCoffee3IoGetSnapshot, xCoffee3LogWriteFieldOrder |
| 1927 | `prvRunMaintenance` | prvRunFruit, prvRunStep, prvWaitDeviceReportedComplete, prvWaitM50Clean |
| 1972 | `prvOrderValid` |  |
| 2000 | `prvRefreshDeviceQuiet` | lCoffee3DeviceGetTerminalResult, memset, pdMS_TO_TICKS, prvServiceHotWater, xCoffee3CommandSubmit, xCoffee3DeviceWaitCommand, xTaskGetTickCount |
| 2063 | `prvBusinessConditionActive` | prvIoValid, vCoffee3IoGetSnapshot |
| 2092 | `prvWaitBusinessCondition` | pdMS_TO_TICKS, prvBusinessConditionActive, prvPublish, prvRefreshDeviceQuiet, prvServiceHotWater, vTaskDelay, xCoffee3LogWriteFieldOrder, xTaskGetTickCount |
| 2166 | `prvWaitDeviceReportedComplete` | pdMS_TO_TICKS, prvPublish, prvRefreshDeviceQuiet, prvServiceHotWater, vTaskDelay, xCoffee3LogWriteFieldOrder, xTaskGetTickCount |
| 2248 | `prvDelayWithServices` | pdMS_TO_TICKS, prvServiceHotWater, prvServiceIoRefresh, vTaskDelay, xTaskGetTickCount |
| 2261 | `prvSubmitHotWaterIo` | memset, xCoffee3CommandSubmit |
| 2286 | `prvPollHotWaterIo` | lCoffee3DeviceGetTerminalResult |
| 2311 | `prvSetHotWaterPublicState` | xCoffee3LogWriteFieldOrder |
| 2331 | `prvServiceCoffeeFill` | pdMS_TO_TICKS, prvIoValid, taskENTER_CRITICAL, taskEXIT_CRITICAL, ucCoffee3IoSetLocalOutput, vCoffee3IoGetSnapshot, xCoffee3LogPrintfOrder, xTaskGetTickCount |
| 2380 | `prvServiceHotWater` | pdMS_TO_TICKS, prvIoValid, prvPollHotWaterIo, prvServiceCoffeeFill, prvServiceIoRefresh, prvSetHotWaterPublicState, prvSubmitHotWaterIo, ucCoffee3IoSetLocalOutput, vCoffee3IoGetSnapshot, vCoffee3IoRefreshLocal, xCoffee3LogWriteFieldOrder, xTaskGetTickCount |
| 2540 | `prvAbortDevices` | lCoffee3DeviceGetTerminalResult, memset, pdMS_TO_TICKS, prvPublish, prvSetProductOutputsOff, ucCoffee3IoSetLocalOutput, vCoffee3OrderCancelRequest, xCoffee3CommandSubmitUrgent, xCoffee3DeviceWaitCommand, xCoffee3LogPrintfOrder, xCoffee3LogWriteFieldOrder, xCoffee3LogWriteOrder |
| 2649 | `prvServiceIoRefresh` | memset, pdMS_TO_TICKS, prvServicePickup, vCoffee3IoRefreshLocal, xCoffee3CommandSubmit, xCoffee3DeviceWaitCommand, xTaskGetTickCount |
| 2712 | `prvPublish` | taskENTER_CRITICAL, taskEXIT_CRITICAL, vCoffee3ServerPublishWorkflow |
| 2765 | `prvWaitM50Clean` | pdMS_TO_TICKS, prvDelayWithServices, prvRefreshDeviceQuiet, xTaskGetTickCount |
| 2795 | `prvRunStoragePickup` | order, prvCheckOutputEmpty, prvIoValid, prvPublishOutput, prvRunStep, prvStartDoor, prvWaitBusinessCondition, vCoffee3IoGetSnapshot, vCoffee3ServerSelectStorage, xCoffee3LogPrintfOrder |
### [Application/UserAPP/Coffee3CloseApp/WorkFlow/coffee3_workflow.h](../../../../Application/UserAPP/Coffee3CloseApp/WorkFlow/coffee3_workflow.h)

SHA-256：`A32D0EC1FBFCF6D63C52B41D733028D9B0B24FDD819F922DABBCA06362303192`。以下行号对应 2026-09-08 工作区快照。

类型与常量原文按头文件顺序列出；正文先解释用途，这里保留全部字段/枚举值，方便逐项对照。API 原型请沿文件链接阅读。

L9
```c
#define COFFEE3_WORKFLOW_H
```


L21
```c
#define COFFEE3_ORDER_REGISTER_COUNT          32U
```


L24
```c
typedef struct {
	uint16_t ausRegister[COFFEE3_ORDER_REGISTER_COUNT];
} Coffee3Order_t;
```


L29
```c
typedef enum {
	COFFEE3_WORKFLOW_IDLE = 0,
	COFFEE3_WORKFLOW_RUNNING = 1,
	COFFEE3_WORKFLOW_COMPLETED = 2,
	COFFEE3_WORKFLOW_FAILED = 3,
	COFFEE3_WORKFLOW_CANCELING = 4
} Coffee3WorkflowState_e;
```


L38
```c
typedef enum {
	COFFEE3_MACHINE_DEFAULT = 0,
	COFFEE3_MACHINE_IDLE = 1,
	COFFEE3_MACHINE_INITIALIZING = 2,
	COFFEE3_MACHINE_BUSY = 3,
	COFFEE3_MACHINE_ALARM = 4
} Coffee3MachineState_e;
```


L47
```c
typedef enum {
	COFFEE3_MAINTENANCE_NONE = 0,
	COFFEE3_MAINTENANCE_SYRUP_CLEAN = 1,
	COFFEE3_MAINTENANCE_COFFEE_CLEAN = 2,
	COFFEE3_MAINTENANCE_FRUIT_DISPENSE = 3,
	COFFEE3_MAINTENANCE_FRUIT_CLEAN = 4
} Coffee3MaintenanceType_e;
```


L56
```c
typedef enum {
	COFFEE3_MAINTENANCE_IDLE = 0,
	COFFEE3_MAINTENANCE_RUNNING = 1,
	COFFEE3_MAINTENANCE_COMPLETED = 2,
	COFFEE3_MAINTENANCE_FAILED = 3,
	COFFEE3_MAINTENANCE_ALARM = 4
} Coffee3MaintenanceState_e;
```


L65
```c
typedef struct {
	uint32_t ulCompletedOrderCount;
	uint32_t ulFailedOrderCount;
	uint32_t ulOrderEpoch;
	uint16_t usCurrentOrderId;
	uint16_t usCurrentStep;
	int32_t lLastError;
	Coffee3WorkflowState_e xState;
	Coffee3MachineState_e xMachineState;
	uint8_t ucCancelRequested;
	uint8_t ucOrderAdmissionOpen;
	uint8_t ucHotWaterState;
	uint8_t ucCoffeeCleanState;
	uint8_t aucFruitState[2];
	uint16_t ausOutputState[2];
	uint16_t ausOutputOrderId[2];
	uint16_t usActiveOutput;
	uint16_t usAction;
	uint8_t ucDeviceId;
	uint8_t ucCommandSent;
	uint8_t ucDeviceDone;
	uint8_t ucPhysicalVerified;
	uint8_t ucPositionUncertain;
	uint8_t ucRecoveryRequired;
	int32_t lSafetyResult;
	uint8_t ucStorageReserved;
	uint8_t ucContentComplete;
} Coffee3WorkflowStatus_t;
```

### [Application/UserAPP/Coffee3CloseApp/Modbus_Tcp_Server/coffee3_server.c](../../../../Application/UserAPP/Coffee3CloseApp/Modbus_Tcp_Server/coffee3_server.c)

SHA-256：`0DF14C48F10ED176CB8FE9B8292BF3BD2659E4D48044C912FA9409D684207428`。以下行号对应 2026-09-08 工作区快照。

| 定义行 | 函数 | 下行调用（词法检索，非运行时跟踪） |
| --- | --- | --- |
| 210 | `xCoffee3ServerInitialize` | memset, nmbs_callbacks_create |
| 235 | `prvLogClientConnected` | memcpy, ucTransportTcpFormatIpv4Endpoint, xCoffee3LogWriteField |
| 263 | `vCoffee3ServerTask` | FD_ISSET, FD_SET, FD_ZERO, lwip_accept, lwip_close, lwip_fcntl, lwip_select, memset, ntohl, ntohs, NVIC_SystemReset, pdMS_TO_TICKS, prvCloseSlot, prvCreateListener, prvLogClientConnected, prvUpdateActiveClientCount, ucAppTaskManagerIsNetworkReady, uxTaskGetStackHighWaterMark, vAppTaskManagerWaitNetworkStackReady, vCoffee3LogLwipResourceFailure, vTaskDelay, xCoffee3LogWrite, xCoffee3LogWriteField, xModbusPortServerInit, xModbusPortServerPoll, xTaskGetTickCount, xTransportTcpSocketAttach, xTransportTcpSocketCreate |
| 549 | `vCoffee3ServerPublishOrder` | taskENTER_CRITICAL, taskEXIT_CRITICAL |
| 585 | `vCoffee3ServerPublishWorkflow` | taskENTER_CRITICAL, taskEXIT_CRITICAL |
| 601 | `vCoffee3ServerPublishOutput` | taskENTER_CRITICAL, taskEXIT_CRITICAL |
| 614 | `usCoffee3ServerGetCommandRegister` | taskENTER_CRITICAL, taskEXIT_CRITICAL |
| 628 | `vCoffee3ServerSelectStorage` | taskENTER_CRITICAL, taskEXIT_CRITICAL |
| 640 | `vCoffee3ServerFinishRequest` | taskENTER_CRITICAL, taskEXIT_CRITICAL |
| 656 | `prvReadHolding` | memcpy, prvBuildDebugRegisters, prvReadIoDebugValue, prvRefreshStatusRegisters, taskENTER_CRITICAL, taskEXIT_CRITICAL |
| 722 | `prvCommitIoDebugWrite` | prvSubmitManual, ucCoffee3IoApplyLocalDebugMask, ucCoffee3WorkflowInitializationComplete, xCoffee3LogPrintfOrder, xCoffee3LogWrite |
| 753 | `prvReadIoDebugValue` | vCoffee3IoGetSnapshot, vCoffee3IoRefreshLocal |
| 785 | `prvCommitIoDebugWriteRange` | prvCommitIoDebugWrite, ucCoffee3WorkflowInitializationComplete, xCoffee3LogWrite |
| 819 | `prvWriteSingle` | prvCommitWrite |
| 828 | `prvWriteMultiple` | prvCommitWrite |
| 838 | `prvCommitWrite` | memcpy, prvCommitIoDebugWriteRange, prvCommitUpgradeWrite, prvEvaluateManualCommands, prvEvaluateOrder, taskENTER_CRITICAL, taskEXIT_CRITICAL, ucCoffee3OtaHttpIsActive, xCoffee3LogWrite |
| 891 | `prvCommitUpgradeWrite` | memcpy, vCoffee3WorkflowReleaseOta, xCoffee3LogPrintfOrder, xCoffee3LogWrite, xCoffee3OtaHttpInitialize, xCoffee3WorkflowAcquireOta, xTaskGetTickCount |
| 955 | `prvEvaluateOrder` | taskENTER_CRITICAL, taskEXIT_CRITICAL, xCoffee3LogWriteFieldOrder, xCoffee3WorkflowSubmitOrder |
| 1008 | `prvEvaluateManualCommands` | prvSubmitManual, prvSubmitRobotPosition, vCoffee3WorkflowAcknowledgeAlarm, vCoffee3WorkflowConfirmPickup, vCoffee3WorkflowRequestCancel, xCoffee3LogPrintfOrder, xCoffee3LogWriteFieldOrder, xCoffee3WorkflowSetHotWater, xCoffee3WorkflowSubmitMaintenance, xCoffee3WorkflowSubmitManualIce, xCoffee3WorkflowSubmitStoragePickup |
| 1276 | `prvSubmitRobotPosition` | prvSubmitManual, xCoffee3LogWriteFieldOrder |
| 1343 | `prvSubmitManual` | memset, ucCoffee3WorkflowInitializationComplete, vCoffee3WorkflowRequestCancel, xCoffee3CommandSubmit, xCoffee3CommandSubmitUrgent, xCoffee3LogWriteFieldOrder |
| 1403 | `prvBuildDebugRegisters` | memset, vAppTaskManagerGetStatus, vCoffee3LogGetStatus, xCoffee3DeviceGetEvents, xPortGetFreeHeapSize, xPortGetMinimumEverFreeHeapSize |
| 1542 | `prvRefreshStatusRegisters` | taskENTER_CRITICAL, taskEXIT_CRITICAL, vCoffee3IoGetSnapshot, vCoffee3IoRefreshLocal, xCoffee3DeviceGetEvents |
| 1838 | `prvCreateListener` | htons, lwip_bind, lwip_close, lwip_fcntl, lwip_listen, lwip_setsockopt, lwip_socket, memset, PP_HTONL |
| 1869 | `prvCloseSlot` | xCoffee3LogWriteField, xTransportClose |
| 1895 | `prvUpdateActiveClientCount` | xCoffee3LogWriteField |
### [Application/UserAPP/Coffee3CloseApp/Modbus_Tcp_Server/coffee3_server.h](../../../../Application/UserAPP/Coffee3CloseApp/Modbus_Tcp_Server/coffee3_server.h)

SHA-256：`0C647E8120C6C3CCFF59C045A1BC77D2DAAD6B5B66C6DD99B8D3FC12AFAD149A`。以下行号对应 2026-09-08 工作区快照。

类型与常量原文按头文件顺序列出；正文先解释用途，这里保留全部字段/枚举值，方便逐项对照。API 原型请沿文件链接阅读。

L9
```c
#define COFFEE3_SERVER_H
```


L22
```c
#define COFFEE3_SERVER_COMMAND_COUNT          0x00B0U
```


L24
```c
#define COFFEE3_SERVER_STATUS_COUNT           0x0100U
```


L26
```c
#define COFFEE3_SERVER_DEBUG_COUNT            0x0080U
```


L29
```c
#define COFFEE3_REG_ORDER_NUMBER              0x0000U
```


L30
```c
#define COFFEE3_REG_COFFEE_TYPE               0x0001U
```


L31
```c
#define COFFEE3_REG_LID_ENABLE                0x0002U
```


L32
```c
#define COFFEE3_REG_SYRUP_1                   0x0003U
```


L33
```c
#define COFFEE3_REG_SYRUP_2                   0x0004U
```


L34
```c
#define COFFEE3_REG_ICE_AMOUNT                0x0005U
```


L35
```c
#define COFFEE3_REG_RESERVED_0006             0x0006U
```


L36
```c
#define COFFEE3_REG_ORDER_PRESENT             0x0007U
```


L37
```c
#define COFFEE3_REG_ORDER_VERIFIED            0x0008U
```


L38
```c
#define COFFEE3_REG_STORAGE_PICKUP            0x0009U
```


L39
```c
#define COFFEE3_REG_ONLINE_OUTPUT             0x000AU
```


L40
```c
#define COFFEE3_REG_PICKUP_CONFIRM            0x000BU
```


L41
```c
#define COFFEE3_REG_OFFLINE_OUTPUT            0x000CU
```


L42
```c
#define COFFEE3_REG_FRUIT_MILK_A              0x000DU
```


L43
```c
#define COFFEE3_REG_FRUIT_MILK_B              0x000EU
```


L44
```c
#define COFFEE3_REG_SYRUP_3                   0x0013U
```


L45
```c
#define COFFEE3_REG_SYRUP_4                   0x0014U
```


L46
```c
#define COFFEE3_REG_CLEAR_ALARM               0x0021U
```


L47
```c
#define COFFEE3_REG_CANCEL_ORDER              0x0022U
```


L48
```c
#define COFFEE3_REG_RESERVED_002A             0x002AU
```


L49
```c
#define COFFEE3_REG_HOT_WATER_START            0x0080U
```


L50
```c
#define COFFEE3_REG_HOT_WATER_MINUTES          0x0081U
```


L51
```c
#define COFFEE3_REG_SYRUP_CLEAN                0x0082U
```


L52
```c
#define COFFEE3_REG_COFFEE_PIPE_CLEAN          0x0083U
```


L53
```c
#define COFFEE3_REG_MANUAL_FRUIT_TYPE          0x00A1U
```


L54
```c
#define COFFEE3_REG_MANUAL_FRUIT_AMOUNT        0x00A2U
```


L55
```c
#define COFFEE3_REG_FRUIT_A_CLEAN              0x00A3U
```


L56
```c
#define COFFEE3_REG_FRUIT_B_CLEAN              0x00A4U
```


L58
```c
#define COFFEE3_REG_STATUS_BASE               0x1000U
```


L59
```c
#define COFFEE3_REG_PRODUCTION_STATUS         0x1008U
```


L60
```c
#define COFFEE3_REG_WORKFLOW_STEP             0x1018U
```


L61
```c
#define COFFEE3_REG_WORKFLOW_ERROR            0x1029U
```


L62
```c
#define COFFEE3_REG_MACHINE_STATUS             0x1020U
```


L63
```c
#define COFFEE3_REG_HOT_WATER_STATUS           0x1082U
```


L64
```c
#define COFFEE3_REG_COFFEE_PIPE_STATUS         0x1083U
```


L65
```c
#define COFFEE3_REG_FRUIT_STATUS               0x10A0U
```


L66
```c
#define COFFEE3_REG_FRUIT_A_LOW                0x10A1U
```


L67
```c
#define COFFEE3_REG_FRUIT_B_LOW                0x10A2U
```


L68
```c
#define COFFEE3_REG_FRUIT_A_STATUS             0x10A8U
```


L69
```c
#define COFFEE3_REG_FRUIT_B_STATUS             0x10A9U
```


L71
```c
#define COFFEE3_REG_LOCAL_INPUT_LOW           0x10F0U
```


L72
```c
#define COFFEE3_REG_LOCAL_INPUT_HIGH          0x10F1U
```


L73
```c
#define COFFEE3_REG_EXTERNAL_INPUT_1_LOW      0x10F2U
```


L74
```c
#define COFFEE3_REG_EXTERNAL_INPUT_1_HIGH     0x10F3U
```


L75
```c
#define COFFEE3_REG_EXTERNAL_INPUT_2_LOW      0x10F4U
```


L76
```c
#define COFFEE3_REG_EXTERNAL_INPUT_2_HIGH     0x10F5U
```


L77
```c
#define COFFEE3_REG_EXTERNAL_INPUT_3_LOW      0x10F6U
```


L78
```c
#define COFFEE3_REG_EXTERNAL_INPUT_3_HIGH     0x10F7U
```


L79
```c
#define COFFEE3_REG_LOCAL_OUTPUT_LOW          0x10F8U
```


L80
```c
#define COFFEE3_REG_LOCAL_OUTPUT_HIGH         0x10F9U
```


L81
```c
#define COFFEE3_REG_EXTERNAL_OUTPUT_1_LOW     0x10FAU
```


L82
```c
#define COFFEE3_REG_EXTERNAL_OUTPUT_1_HIGH    0x10FBU
```


L83
```c
#define COFFEE3_REG_EXTERNAL_OUTPUT_2_LOW     0x10FCU
```


L84
```c
#define COFFEE3_REG_EXTERNAL_OUTPUT_2_HIGH    0x10FDU
```


L85
```c
#define COFFEE3_REG_EXTERNAL_OUTPUT_3_LOW     0x10FEU
```


L86
```c
#define COFFEE3_REG_EXTERNAL_OUTPUT_3_HIGH    0x10FFU
```


L88
```c
#define COFFEE3_REG_LOCAL_IO_DEBUG             0x0208U
```


L89
```c
#define COFFEE3_REG_EXTERNAL_IO_DEBUG          0x0209U
```


L92
```c
#define COFFEE3_PRODUCTION_IDLE               0U
```


L93
```c
#define COFFEE3_PRODUCTION_RUNNING            1U
```


L94
```c
#define COFFEE3_PRODUCTION_COMPLETED          2U
```


L95
```c
#define COFFEE3_PRODUCTION_FAILED             3U
```


L105
```c
typedef struct {
	uint32_t ulRemoteIpv4;
	uint32_t ulRequestCount;
	uint32_t ulErrorCount;
	uint32_t ulDisconnectCount;
	uint32_t ulLastActivityTick;
	int32_t lLastResult;
	uint16_t usRemotePort;
	uint8_t ucConnected;
} Coffee3ServerClientStatus_t;
```


L117
```c
typedef struct {
	Coffee3ServerClientStatus_t axClient[COFFEE3_SERVER_MAX_CLIENTS];
	uint32_t ulAcceptedCount;
	uint32_t ulRejectedCount;
	uint32_t ulListenerErrorCount;
	uint32_t ulOnlineTransitionCount;
	uint16_t usListenPort;
	uint8_t ucActiveClients;
	uint8_t ucListening;
	uint8_t ucOnline;
} Coffee3ServerStatus_t;
```

### [Application/UserAPP/Coffee3CloseApp/IO_State/coffee3_io.c](../../../../Application/UserAPP/Coffee3CloseApp/IO_State/coffee3_io.c)

SHA-256：`B37C0485C247D8EDBA73BFFA171951219BD15C50A6E220F7488A0A974975029B`。以下行号对应 2026-09-08 工作区快照。

| 定义行 | 函数 | 下行调用（词法检索，非运行时跟踪） |
| --- | --- | --- |
| 102 | `vCoffee3IoInitialize` | memset, taskENTER_CRITICAL, taskEXIT_CRITICAL, vCoffee3IoRefreshLocal |
| 114 | `vCoffee3IoRefreshLocal` | HAL_GPIO_ReadPin, memcpy, prvLogIoChanges, taskENTER_CRITICAL, taskEXIT_CRITICAL, xTaskGetTickCount |
| 156 | `ucCoffee3IoSetLocalOutput` | HAL_GPIO_ReadPin, HAL_GPIO_WritePin, taskENTER_CRITICAL, taskEXIT_CRITICAL, xCoffee3LogPrintfOrder, xTaskGetTickCount |
| 197 | `ucCoffee3IoApplyLocalDebugMask` | HAL_GPIO_WritePin, vCoffee3IoRefreshLocal |
| 212 | `vCoffee3IoCommitModbusInput` | memcpy, prvLogIoChanges, taskENTER_CRITICAL, taskEXIT_CRITICAL, xTaskGetTickCount |
| 240 | `vCoffee3IoCommitModbusOutputImage` | memcpy, prvLogIoChanges, taskENTER_CRITICAL, taskEXIT_CRITICAL, xTaskGetTickCount |
| 268 | `prvLogIoChanges` | xCoffee3LogPrintfOrder |
| 286 | `vCoffee3IoGetSnapshot` | taskENTER_CRITICAL, taskEXIT_CRITICAL |
### [Application/UserAPP/Coffee3CloseApp/IO_State/coffee3_io.h](../../../../Application/UserAPP/Coffee3CloseApp/IO_State/coffee3_io.h)

SHA-256：`AD31D6FF86790DCDB67280B7F74713BF710C3DCA3A14588B9A1A1A34F0DEEF62`。以下行号对应 2026-09-08 工作区快照。

类型与常量原文按头文件顺序列出；正文先解释用途，这里保留全部字段/枚举值，方便逐项对照。API 原型请沿文件链接阅读。

L9
```c
#define COFFEE3_IO_H
```


L18
```c
#define COFFEE3_LOCAL_IO_COUNT               8U
```


L20
```c
#define COFFEE3_MODBUS_IO_COUNT              16U
```


L22
```c
typedef enum {
	COFFEE3_LOCAL_DI_DOOR_UPPER = 0,
	COFFEE3_LOCAL_DI_DOOR_LOWER = 1,
	COFFEE3_LOCAL_DI_COFFEE_WATER_HIGH = 2,
	COFFEE3_LOCAL_DI_COFFEE_WATER_LOW = 3,
	COFFEE3_LOCAL_DI_HOT_WATER_HIGH = 4,
	COFFEE3_LOCAL_DI_HOT_WATER_LOW = 5
} Coffee3LocalInputPoint_e;
```


L31
```c
typedef enum {
	COFFEE3_LOCAL_DO_DOOR_UP = 0,
	COFFEE3_LOCAL_DO_DOOR_DOWN = 1,
	COFFEE3_LOCAL_DO_HOT_WATER_SUPPLY_VALVE = 2,
	COFFEE3_LOCAL_DO_COFFEE_WATER_PUMP = 3
} Coffee3LocalOutputPoint_e;
```


L38
```c
typedef enum {
	COFFEE3_EXTERNAL_DI_OUTPUT_FRONT_CUP = 0,
	COFFEE3_EXTERNAL_DI_OUTPUT_REAR_CUP = 1,
	COFFEE3_EXTERNAL_DI_OUTLET_CUP = 2,
	COFFEE3_EXTERNAL_DI_PURE_WATER_LOW = 3,
	COFFEE3_EXTERNAL_DI_WASTE_BIN_PRESENT = 4,
	COFFEE3_EXTERNAL_DI_MILK_LOW = 10,
	COFFEE3_EXTERNAL_DI_FRUIT_MILK_A_LOW = 11,
	COFFEE3_EXTERNAL_DI_FRUIT_MILK_B_LOW = 12
} Coffee3ExternalInputPoint_e;
```


L49
```c
typedef enum {
	COFFEE3_EXTERNAL_DO_WATER_HEATER_RELAY = 0,
	COFFEE3_EXTERNAL_DO_MILK_VALVE = 3,
	COFFEE3_EXTERNAL_DO_FRUIT_MILK_A_VALVE = 4,
	COFFEE3_EXTERNAL_DO_FRUIT_MILK_B_VALVE = 5,
	COFFEE3_EXTERNAL_DO_FRUIT_MILK_A_PUMP = 9,
	COFFEE3_EXTERNAL_DO_FRUIT_MILK_B_PUMP = 10
} Coffee3ExternalOutputPoint_e;
```


L59
```c
typedef struct {
	uint8_t aucXPin[COFFEE3_LOCAL_IO_COUNT];
	uint8_t aucMB1XPin[COFFEE3_MODBUS_IO_COUNT];
	uint8_t aucMB2XPin[COFFEE3_MODBUS_IO_COUNT];
} Coffee3InputIo_t;
```


L66
```c
typedef struct {
	uint8_t aucYPin[COFFEE3_LOCAL_IO_COUNT];
	uint8_t aucMB1YPin[COFFEE3_MODBUS_IO_COUNT];
	uint8_t aucMB2YPin[COFFEE3_MODBUS_IO_COUNT];
} Coffee3OutputIo_t;
```


L73
```c
typedef struct {
	Coffee3InputIo_t xInput;
	Coffee3OutputIo_t xOutput;
	uint32_t ulVersion;
	uint32_t ulLocalUpdateTick;
	uint32_t aulModbusUpdateTick[2];
	uint8_t aucModbusValid[2];
} Coffee3IoState_t;
```

### [Application/UserAPP/Coffee3CloseApp/Ota/coffee3_ota.c](../../../../Application/UserAPP/Coffee3CloseApp/Ota/coffee3_ota.c)

SHA-256：`A19C3D5819E11E2951A3A91E4B1D168ADA3A859439C2FEDDE6BB643D6872471F`。以下行号对应 2026-09-08 工作区快照。

| 定义行 | 函数 | 下行调用（词法检索，非运行时跟踪） |
| --- | --- | --- |
| 15 | `prvReportLwipFailure` | vCoffee3LogLwipResourceFailure |
| 28 | `xCoffee3OtaInitialize` | xAppOtaInitialize |
| 33 | `xCoffee3OtaBegin` | xAppOtaFlashBegin, xCoffee3OtaInitialize |
| 39 | `xCoffee3OtaWrite` | xAppOtaFlashWrite, xCoffee3OtaInitialize |
| 45 | `xCoffee3OtaFinish` | xAppOtaFlashFinish, xCoffee3OtaInitialize |
| 51 | `vCoffee3OtaAbort` | vAppOtaFlashAbort, xCoffee3OtaInitialize |
| 57 | `ucCoffee3OtaIsActive` | ucAppOtaFlashIsActive |
| 62 | `xCoffee3OtaHttpInitialize` | xAppOtaHttpInitialize, xCoffee3OtaInitialize |
| 68 | `ucCoffee3OtaHttpIsActive` | ucAppOtaHttpIsActive |
### [Application/UserAPP/Coffee3CloseApp/Ota/coffee3_ota.h](../../../../Application/UserAPP/Coffee3CloseApp/Ota/coffee3_ota.h)

SHA-256：`63A387D2524567A746C00F26DBBFA4672688FA05C8B0F59E7F305D032C584E01`。以下行号对应 2026-09-08 工作区快照。

类型与常量原文按头文件顺序列出；正文先解释用途，这里保留全部字段/枚举值，方便逐项对照。API 原型请沿文件链接阅读。

L7
```c
#define COFFEE3_OTA_H
```


L17
```c
#define COFFEE3_OTA_METADATA_ADDRESS       0x08004000UL
```


L18
```c
#define COFFEE3_OTA_STAGING_ADDRESS        0x08060000UL
```


L19
```c
#define COFFEE3_OTA_STAGING_END            0x080C0000UL
```


L20
```c
#define COFFEE3_OTA_APPLICATION_ADDRESS    0x0800C000UL
```


L21
```c
#define COFFEE3_OTA_APPLICATION_END        0x08060000UL
```


L22
```c
#define COFFEE3_OTA_MAX_IMAGE_SIZE \
```


L24
```c
#define COFFEE3_OTA_METADATA_MAGIC         0xDEADBEEFUL
```
