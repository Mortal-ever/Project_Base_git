# 17 Coffee2Open Workflow、Server、IO 与 OTA 业务闭环

适用2026-09-08源码。先打开本篇附录的workflow.h和server.h，再读本文函数分解。本文区分协议回包、命令终态、设备物理完成、订单完成与客户取走五种不同结果。

## 一、先认识业务对象

`Coffee2Order_t.ausRegister[32]` 是接受订单时的寄存器快照。Workflow队列按值复制它，后续上位机修改命令寄存器不应改变已接订单。

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

### Coffee2：初始化以残杯阻止启动

`prvRunInitialization` 先关闭产品输出，再刷新IO输入、Cup、Lid。检查前后成品位；为空时依次探测机械手、咖啡位、压盖位残杯。探测以回原点、必要取杯、放出餐位1、500ms后刷新输入完成。有杯记occupied point并返回INIT_OCCUPIED=-1009；还检查Cup/Lid诊断线圈的指定位置。全部通过才机器人回原点。不能把Coffee3“有杯转存后继续”套在这里。

### Coffee2：订单步骤按源码顺序

`prvRunOrder` 从冰量判冷热、从ONLINE_OUTPUT取出口，从咖啡类型0xFFFF判是否跳过咖啡站。先刷新需要的设备、检查目标出口空，再回原点→取热/冷杯→落杯→等待杯条件。按需制冰、到果乳糖浆位、出果乳/四路糖浆。糖浆每路动作之后都等待状态SUCCESS或FAILED。

咖啡阶段70/75到咖啡位，80发送F200 MAKE，85通过 `prvWaitDeviceReportedComplete` 等F200应用状态COMPLETE/FAILED，然后90取咖啡。开盖参数非零时执行140..170压盖链，155单独等盖条件。放杯前再次检查目标空：发布出口2→180放杯→185检查有杯，成功发布5、失败发布3；190回原点。外层任务根据返回结果发布制作完成/失败。步骤编号非严格递增，不可按数字排序重新排列。

### Coffee2：客户取餐另行推进

`prvServicePickup`、`prvPublishOutput` 持有各出口状态和订单归属；ACK与传感器检查共同决定释放。它不包含Coffee3出餐门升降服务。已完成订单和出口事务不是同一个生命周期。

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

`prvCommitIoDebugWrite` 先检查初始化完成；再检查高8位全0，否则异常03；调用 `ucCoffee2IoApplyLocalDebugMask`。该函数循环8个点，bit为1输出SET，bit为0输出RESET，最后刷新本机镜像。因此写5是DO1和DO3开，其余关，不是“追加打开两点”。

0x0209则投递 IO_WRITE_MASK 到Bus5，执行结果和读回由设备owner随后产生。Modbus成功应答与外部模块物理完成的时刻不同。多个寄存器写入顺序执行，不提供跨本机GPIO/RTU的硬件原子提交。

## 五、IO_State函数体和极性

`vCoffee2IoInitialize` 清镜像、清baseline，立即采样。`vCoffee2IoRefreshLocal` 在临界区外采GPIO，再在临界区复制旧值、新值、tick、version，退出临界区后打印变化。第一次只建立基线，不把全体点位当作变化。

```c
/* 输入逻辑值：高为0，低为1。 */
(HAL_GPIO_ReadPin(port, pin) == GPIO_PIN_SET) ? 0U : 1U;
```

输出读ODR并用SET表示1，不能把输入低有效规则无条件应用到输出。输出镜像证明软件输出电平，不证明继电器触点或机构已动作。外部图像只有成功读取后才commit，更新时间和valid可以区分有效零与尚无样本。

`Coffee2IoState_t` 的xInput/xOutput分别含本机8点和两个16点数组；ulVersion每次提交增加；ulLocalUpdateTick标本机采样时间；aulModbusUpdateTick[2]/aucModbusValid[2]标模块有效性。`GetSnapshot`临界区复制，不返回内部可写指针。

Coffee2的点位含义来自自己的Config名称表；本机输出调试同样是整幅写，不借用Coffee3出餐门逻辑。

## 六、维护、制冰与取消要怎样读

`prvDispenseIce` 先tare、等待、取稳定秤值，按目标0.1g计算阀脉冲；开阀后计时并检查取消，随后始终尝试关阀，再复测和有限次数修正。关阀失败单独处理，不能因为达到重量就忽略关闭错误。

`prvRunFruit` 用通道选阀和泵，数量换算运行时间，关闭输出和失败返回由本函数维护。`prvRunMaintenance` 依据维护类型串行执行糖浆/咖啡/果乳操作。热水服务以状态推进方式穿插在Workflow等待中，并非新增的独立任务。

`prvAbortDevices` 对活跃设备发安全停止。取消是协作式检查，不能保证已发出的动作从未生效。读ucPositionUncertain和ucRecoveryRequired判断是否需要恢复，不要根据取消标志立即宣布物理安全。

## 七、OTA私有接入

`coffee2_ota.c` 保存 `static const AppOtaConfig_t`：metadata/staging/application起止地址、SRAM范围、magic、Flash扇区、HTTP配置、CRC句柄与日志回调。地址属于产品组合，擦写和传输实现位于公共Common/Ota。

Initialize把配置交给公共服务；Begin/Write/Finish每次先确认初始化，再调用对应公共函数。初始化失败被门面转换为BUSY，排查时还要看公共初始错误。Abort调用公共终止。WorkflowAcquireOta是业务准入，必须从Server/升级入口一起追，不能只看这个短门面认为任意时刻可擦Flash。

## 八、日志与崩溃如何对接

Comm_Log维护本产品source与串口参数，调用公共Text/Printf/Field接口。正文既有EVENT，也有可读字符串，教材照实际代码解释。IO日志是首次baseline后逐点比较，仅变化时打印；不能把“无变化日志”理解成没有刷新。

`coffee2_crash_log_port.c` 提供公共诊断弱接口的产品输出实现，retarget承接工具链运行库输出。崩溃时不应假设调度器和普通日志消费仍工作；具体同步输出路径见第04册。两个产品只选择自己的强实现。

## 九、读懂后的检查题

1. 为什么FC03返回全0不能单独证明模块离线？找valid/tick。
2. 为什么一个设备BUSY不等于整机只能处理这一件事情？找到等待循环推进的服务。
3. 何处区分订单完成、放杯完成和客户取走？逐个定位寄存器写入。
4. 0x0208写入成功后为何其他位被关掉？读mask循环。
5. 哪个返回值是原始设备result，哪个是Workflow归一化错误？查terminal键。
## 源码导航与版本证据

### [Application/UserAPP/Coffee2OpenApp/WorkFlow/coffee2_workflow.c](../../../../Application/UserAPP/Coffee2OpenApp/WorkFlow/coffee2_workflow.c)

SHA-256：`1F3C413735A781A6B4B4AABE949CB210239E92F09F2184BAC8C8879C11B63003`。以下行号对应 2026-09-08 工作区快照。

| 定义行 | 函数 | 下行调用（词法检索，非运行时跟踪） |
| --- | --- | --- |
| 202 | `xCoffee2WorkflowAcquireManual` | taskENTER_CRITICAL, taskEXIT_CRITICAL |
| 228 | `vCoffee2WorkflowReleaseManual` | taskENTER_CRITICAL, taskEXIT_CRITICAL |
| 238 | `vCoffee2WorkflowConfirmPickup` | taskENTER_CRITICAL, taskEXIT_CRITICAL |
| 251 | `prvServicePickup` | taskENTER_CRITICAL, taskEXIT_CRITICAL |
| 269 | `prvPublishOutput` | taskENTER_CRITICAL, taskEXIT_CRITICAL, vCoffee2ServerPublishOutput |
| 282 | `prvCheckOutputEmpty` | prvBusinessConditionActive, prvRefreshDeviceQuiet, prvServicePickup |
| 301 | `xCoffee2WorkflowInitialize` | memset, xQueueCreateStatic |
| 329 | `ucCoffee2WorkflowInitializationComplete` |  |
| 335 | `xCoffee2WorkflowSubmitMaintenance` | taskENTER_CRITICAL, taskEXIT_CRITICAL |
| 362 | `xCoffee2WorkflowSetHotWater` | memset, taskENTER_CRITICAL, taskEXIT_CRITICAL, xCoffee2LogWriteFieldOrder |
| 397 | `vCoffee2WorkflowAcknowledgeAlarm` | taskENTER_CRITICAL, taskEXIT_CRITICAL |
| 415 | `xCoffee2WorkflowSubmitOrder` | taskENTER_CRITICAL, taskEXIT_CRITICAL, vCoffee2OrderCancelRequest, xCoffee2LogWriteFieldOrder, xQueueSend |
| 474 | `xCoffee2WorkflowSubmitManualIce` | taskENTER_CRITICAL, taskEXIT_CRITICAL, uxQueueMessagesWaiting |
| 497 | `vCoffee2WorkflowRequestCancel` | taskENTER_CRITICAL, taskEXIT_CRITICAL, vCoffee2OrderCancelRequest, xCoffee2LogWriteFieldOrder |
| 514 | `vCoffee2WorkflowTask` | memset, pdMS_TO_TICKS, prvAbortDevices, prvDelayWithServices, prvDispenseIce, prvPublish, prvQueuePendingOrder, prvRunInitialization, prvRunMaintenance, prvRunOrder, prvServiceHotWater, prvServiceIoRefresh, taskENTER_CRITICAL, taskEXIT_CRITICAL, vCoffee2ServerPublishOrder, vTaskDelay, xCoffee2LogPrintfOrder, xCoffee2LogWrite, xCoffee2LogWriteFieldOrder, xCoffee2LogWriteOrder, xQueueReceive |
| 776 | `prvRobotPositionAction` |  |
| 784 | `prvQueuePendingOrder` | taskENTER_CRITICAL, taskEXIT_CRITICAL, xCoffee2LogWriteFieldOrder, xQueueSendToFront |
| 819 | `prvRunStep` | lCoffee2DeviceGetTerminalResult, memset, pdMS_TO_TICKS, prvPublish, prvRobotPositionAction, prvServiceHotWater, prvServiceIoRefresh, pxCoffee2DeviceGetBinding, xCoffee2CommandSubmit, xCoffee2DeviceWaitCommand, xCoffee2LogPrintfOrder, xCoffee2LogWriteFieldOrder, xTaskGetTickCount |
| 1043 | `prvRunOrder` | prvCheckOutputEmpty, prvDispenseIce, prvOrderValid, prvPublishOutput, prvRunFruit, prvRunStep, prvWaitBusinessCondition, prvWaitDeviceReportedComplete, xCoffee2LogWriteFieldOrder |
| 1298 | `prvDispenseIce` | pdMS_TO_TICKS, prvCalculateIcePulseMs, prvReadStableScale, prvRunStep, vTaskDelay, xCoffee2LogWriteField |
| 1390 | `prvCalculateIcePulseMs` |  |
| 1407 | `prvReadStableScale` | pdMS_TO_TICKS, prvRunStep, vTaskDelay |
| 1447 | `prvRunIoOutput` | lCoffee2DeviceGetTerminalResult, memset, pdMS_TO_TICKS, prvServiceHotWater, xCoffee2CommandSubmit, xCoffee2DeviceWaitCommand, xTaskGetTickCount |
| 1500 | `prvSetProductOutputsOff` | prvRunIoOutput, ucCoffee2IoSetLocalOutput |
| 1528 | `prvRunInitialization` | prvProbeResidualCup, prvRunStep, prvSetProductOutputsOff, vCoffee2IoGetSnapshot, xCoffee2LogWriteFieldOrder |
| 1611 | `prvProbeResidualCup` | pdMS_TO_TICKS, prvRunStep, vCoffee2IoGetSnapshot, vTaskDelay, xCoffee2LogWriteFieldOrder |
| 1653 | `prvRunFruit` | prvDelayWithServices, prvRunIoOutput, prvRunStep, vCoffee2IoGetSnapshot, xCoffee2LogWriteFieldOrder |
| 1728 | `prvRunMaintenance` | prvRunFruit, prvRunStep, prvWaitDeviceReportedComplete |
| 1776 | `prvOrderValid` |  |
| 1813 | `prvRefreshDeviceQuiet` | lCoffee2DeviceGetTerminalResult, memset, pdMS_TO_TICKS, prvServiceHotWater, xCoffee2CommandSubmit, xCoffee2DeviceWaitCommand, xTaskGetTickCount |
| 1876 | `prvBusinessConditionActive` | vCoffee2IoGetSnapshot |
| 1902 | `prvWaitBusinessCondition` | pdMS_TO_TICKS, prvBusinessConditionActive, prvPublish, prvRefreshDeviceQuiet, prvServiceHotWater, vTaskDelay, xCoffee2LogWriteFieldOrder |
| 1969 | `prvWaitDeviceReportedComplete` | pdMS_TO_TICKS, prvPublish, prvRefreshDeviceQuiet, prvServiceHotWater, vTaskDelay, xCoffee2LogWriteFieldOrder |
| 2045 | `prvDelayWithServices` | pdMS_TO_TICKS, prvServiceHotWater, prvServiceIoRefresh, vTaskDelay, xTaskGetTickCount |
| 2058 | `prvSubmitHotWaterIo` | memset, xCoffee2CommandSubmit |
| 2083 | `prvPollHotWaterIo` | lCoffee2DeviceGetTerminalResult |
| 2108 | `prvSetHotWaterPublicState` | xCoffee2LogWriteFieldOrder |
| 2128 | `prvServiceHotWater` | pdMS_TO_TICKS, prvPollHotWaterIo, prvSetHotWaterPublicState, prvSubmitHotWaterIo, ucCoffee2IoSetLocalOutput, vCoffee2IoGetSnapshot, vCoffee2IoRefreshLocal, xCoffee2LogWriteFieldOrder, xTaskGetTickCount |
| 2281 | `prvAbortDevices` | lCoffee2DeviceGetTerminalResult, memset, pdMS_TO_TICKS, prvPublish, prvSetProductOutputsOff, ucCoffee2IoSetLocalOutput, vCoffee2OrderCancelRequest, xCoffee2CommandSubmitUrgent, xCoffee2DeviceWaitCommand, xCoffee2LogPrintfOrder, xCoffee2LogWriteFieldOrder, xCoffee2LogWriteOrder |
| 2390 | `prvServiceIoRefresh` | memset, pdMS_TO_TICKS, prvServicePickup, vCoffee2IoRefreshLocal, xCoffee2CommandSubmit, xTaskGetTickCount |
| 2441 | `prvPublish` | taskENTER_CRITICAL, taskEXIT_CRITICAL, vCoffee2ServerPublishWorkflow |
### [Application/UserAPP/Coffee2OpenApp/WorkFlow/coffee2_workflow.h](../../../../Application/UserAPP/Coffee2OpenApp/WorkFlow/coffee2_workflow.h)

SHA-256：`72912F33B8879AE5CCCD6252C0C046C2FCDEB7343816B69CA973C03F2CF77AC8`。以下行号对应 2026-09-08 工作区快照。

类型与常量原文按头文件顺序列出；正文先解释用途，这里保留全部字段/枚举值，方便逐项对照。API 原型请沿文件链接阅读。

L9
```c
#define COFFEE2_WORKFLOW_H
```


L21
```c
#define COFFEE2_ORDER_REGISTER_COUNT          32U
```


L24
```c
typedef struct {
	uint16_t ausRegister[COFFEE2_ORDER_REGISTER_COUNT];
} Coffee2Order_t;
```


L29
```c
typedef enum {
	COFFEE2_WORKFLOW_IDLE = 0,
	COFFEE2_WORKFLOW_RUNNING = 1,
	COFFEE2_WORKFLOW_COMPLETED = 2,
	COFFEE2_WORKFLOW_FAILED = 3,
	COFFEE2_WORKFLOW_CANCELING = 4
} Coffee2WorkflowState_e;
```


L38
```c
typedef enum {
	COFFEE2_MACHINE_DEFAULT = 0,
	COFFEE2_MACHINE_IDLE = 1,
	COFFEE2_MACHINE_INITIALIZING = 2,
	COFFEE2_MACHINE_BUSY = 3,
	COFFEE2_MACHINE_ALARM = 4
} Coffee2MachineState_e;
```


L47
```c
typedef enum {
	COFFEE2_MAINTENANCE_NONE = 0,
	COFFEE2_MAINTENANCE_SYRUP_CLEAN = 1,
	COFFEE2_MAINTENANCE_COFFEE_CLEAN = 2,
	COFFEE2_MAINTENANCE_FRUIT_DISPENSE = 3,
	COFFEE2_MAINTENANCE_FRUIT_CLEAN = 4
} Coffee2MaintenanceType_e;
```


L56
```c
typedef enum {
	COFFEE2_MAINTENANCE_IDLE = 0,
	COFFEE2_MAINTENANCE_RUNNING = 1,
	COFFEE2_MAINTENANCE_COMPLETED = 2,
	COFFEE2_MAINTENANCE_FAILED = 3,
	COFFEE2_MAINTENANCE_ALARM = 4
} Coffee2MaintenanceState_e;
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
	Coffee2WorkflowState_e xState;
	Coffee2MachineState_e xMachineState;
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
} Coffee2WorkflowStatus_t;
```

### [Application/UserAPP/Coffee2OpenApp/Modbus_Tcp_Server/coffee2_server.c](../../../../Application/UserAPP/Coffee2OpenApp/Modbus_Tcp_Server/coffee2_server.c)

SHA-256：`5FE9D65490319EC1596F6D22A7BC23AC20BAA57F4EF974F8C62381D0D8CEEAB7`。以下行号对应 2026-09-08 工作区快照。

| 定义行 | 函数 | 下行调用（词法检索，非运行时跟踪） |
| --- | --- | --- |
| 210 | `xCoffee2ServerInitialize` | memset, nmbs_callbacks_create |
| 235 | `prvLogClientConnected` | memcpy, ucTransportTcpFormatIpv4Endpoint, xCoffee2LogWriteField |
| 263 | `vCoffee2ServerTask` | FD_ISSET, FD_SET, FD_ZERO, lwip_accept, lwip_close, lwip_fcntl, lwip_select, memset, ntohl, ntohs, NVIC_SystemReset, pdMS_TO_TICKS, prvCloseSlot, prvCreateListener, prvLogClientConnected, prvUpdateActiveClientCount, ucAppTaskManagerIsNetworkReady, uxTaskGetStackHighWaterMark, vAppTaskManagerWaitNetworkStackReady, vCoffee2LogLwipResourceFailure, vTaskDelay, xCoffee2LogWrite, xCoffee2LogWriteField, xModbusPortServerInit, xModbusPortServerPoll, xTaskGetTickCount, xTransportTcpSocketAttach, xTransportTcpSocketCreate |
| 549 | `vCoffee2ServerPublishOrder` | taskENTER_CRITICAL, taskEXIT_CRITICAL |
| 585 | `vCoffee2ServerPublishWorkflow` | taskENTER_CRITICAL, taskEXIT_CRITICAL |
| 601 | `vCoffee2ServerPublishOutput` | taskENTER_CRITICAL, taskEXIT_CRITICAL |
| 614 | `usCoffee2ServerGetCommandRegister` | taskENTER_CRITICAL, taskEXIT_CRITICAL |
| 628 | `prvReadHolding` | memcpy, prvBuildDebugRegisters, prvReadIoDebugValue, prvRefreshStatusRegisters, taskENTER_CRITICAL, taskEXIT_CRITICAL |
| 694 | `prvCommitIoDebugWrite` | prvSubmitManual, ucCoffee2IoApplyLocalDebugMask, ucCoffee2WorkflowInitializationComplete, xCoffee2LogPrintfOrder, xCoffee2LogWrite |
| 725 | `prvReadIoDebugValue` | vCoffee2IoGetSnapshot, vCoffee2IoRefreshLocal |
| 757 | `prvCommitIoDebugWriteRange` | prvCommitIoDebugWrite, ucCoffee2WorkflowInitializationComplete, xCoffee2LogWrite |
| 791 | `prvWriteSingle` | prvCommitWrite |
| 800 | `prvWriteMultiple` | prvCommitWrite |
| 810 | `prvCommitWrite` | memcpy, prvCommitIoDebugWriteRange, prvCommitUpgradeWrite, prvEvaluateManualCommands, prvEvaluateOrder, taskENTER_CRITICAL, taskEXIT_CRITICAL, ucCoffee2OtaHttpIsActive, xCoffee2LogWrite |
| 859 | `prvCommitUpgradeWrite` | memcpy, xCoffee2LogWrite, xCoffee2OtaHttpInitialize, xTaskGetTickCount |
| 909 | `prvEvaluateOrder` | taskENTER_CRITICAL, taskEXIT_CRITICAL, xCoffee2LogWriteFieldOrder, xCoffee2WorkflowSubmitOrder |
| 962 | `prvEvaluateManualCommands` | prvSubmitManual, prvSubmitRobotPosition, vCoffee2WorkflowAcknowledgeAlarm, vCoffee2WorkflowConfirmPickup, vCoffee2WorkflowRequestCancel, xCoffee2LogWriteFieldOrder, xCoffee2WorkflowSetHotWater, xCoffee2WorkflowSubmitMaintenance, xCoffee2WorkflowSubmitManualIce |
| 1220 | `prvSubmitRobotPosition` | prvSubmitManual, xCoffee2LogWriteFieldOrder |
| 1287 | `prvSubmitManual` | memset, ucCoffee2WorkflowInitializationComplete, vCoffee2WorkflowRequestCancel, xCoffee2CommandSubmit, xCoffee2CommandSubmitUrgent, xCoffee2LogWriteFieldOrder |
| 1347 | `prvBuildDebugRegisters` | memset, vAppTaskManagerGetStatus, vCoffee2LogGetStatus, xCoffee2DeviceGetEvents, xPortGetFreeHeapSize, xPortGetMinimumEverFreeHeapSize |
| 1486 | `prvRefreshStatusRegisters` | taskENTER_CRITICAL, taskEXIT_CRITICAL, vCoffee2IoGetSnapshot, vCoffee2IoRefreshLocal, xCoffee2DeviceGetEvents |
| 1791 | `prvCreateListener` | htons, lwip_bind, lwip_close, lwip_fcntl, lwip_listen, lwip_setsockopt, lwip_socket, memset, PP_HTONL |
| 1822 | `prvCloseSlot` | xCoffee2LogWriteField, xTransportClose |
| 1848 | `prvUpdateActiveClientCount` | xCoffee2LogWriteField |
### [Application/UserAPP/Coffee2OpenApp/Modbus_Tcp_Server/coffee2_server.h](../../../../Application/UserAPP/Coffee2OpenApp/Modbus_Tcp_Server/coffee2_server.h)

SHA-256：`2DEB066DA1FC7FE969D4C06BC635C5F516956B2A246846620F8A46A96A0EFDDC`。以下行号对应 2026-09-08 工作区快照。

类型与常量原文按头文件顺序列出；正文先解释用途，这里保留全部字段/枚举值，方便逐项对照。API 原型请沿文件链接阅读。

L9
```c
#define COFFEE2_SERVER_H
```


L22
```c
#define COFFEE2_SERVER_COMMAND_COUNT          0x00B0U
```


L24
```c
#define COFFEE2_SERVER_STATUS_COUNT           0x0100U
```


L26
```c
#define COFFEE2_SERVER_DEBUG_COUNT            0x0080U
```


L29
```c
#define COFFEE2_REG_ORDER_NUMBER              0x0000U
```


L30
```c
#define COFFEE2_REG_COFFEE_TYPE               0x0001U
```


L31
```c
#define COFFEE2_REG_LID_ENABLE                0x0002U
```


L32
```c
#define COFFEE2_REG_SYRUP_1                   0x0003U
```


L33
```c
#define COFFEE2_REG_SYRUP_2                   0x0004U
```


L34
```c
#define COFFEE2_REG_ICE_AMOUNT                0x0005U
```


L35
```c
#define COFFEE2_REG_RESERVED_0006             0x0006U
```


L36
```c
#define COFFEE2_REG_ORDER_PRESENT             0x0007U
```


L37
```c
#define COFFEE2_REG_ORDER_VERIFIED            0x0008U
```


L38
```c
#define COFFEE2_REG_RESERVED_0009             0x0009U
```


L39
```c
#define COFFEE2_REG_ONLINE_OUTPUT             0x000AU
```


L40
```c
#define COFFEE2_REG_PICKUP_CONFIRM            0x000BU
```


L41
```c
#define COFFEE2_REG_RESERVED_000C             0x000CU
```


L42
```c
#define COFFEE2_REG_FRUIT_MILK_A              0x000DU
```


L43
```c
#define COFFEE2_REG_FRUIT_MILK_B              0x000EU
```


L44
```c
#define COFFEE2_REG_SYRUP_3                   0x0013U
```


L45
```c
#define COFFEE2_REG_SYRUP_4                   0x0014U
```


L46
```c
#define COFFEE2_REG_CLEAR_ALARM               0x0021U
```


L47
```c
#define COFFEE2_REG_CANCEL_ORDER              0x0022U
```


L48
```c
#define COFFEE2_REG_RESERVED_002A             0x002AU
```


L49
```c
#define COFFEE2_REG_HOT_WATER_START            0x0080U
```


L50
```c
#define COFFEE2_REG_HOT_WATER_MINUTES          0x0081U
```


L51
```c
#define COFFEE2_REG_SYRUP_CLEAN                0x0082U
```


L52
```c
#define COFFEE2_REG_COFFEE_PIPE_CLEAN          0x0083U
```


L53
```c
#define COFFEE2_REG_MANUAL_FRUIT_TYPE          0x00A1U
```


L54
```c
#define COFFEE2_REG_MANUAL_FRUIT_AMOUNT        0x00A2U
```


L55
```c
#define COFFEE2_REG_FRUIT_A_CLEAN              0x00A3U
```


L56
```c
#define COFFEE2_REG_FRUIT_B_CLEAN              0x00A4U
```


L58
```c
#define COFFEE2_REG_STATUS_BASE               0x1000U
```


L59
```c
#define COFFEE2_REG_PRODUCTION_STATUS         0x1008U
```


L60
```c
#define COFFEE2_REG_WORKFLOW_STEP             0x1018U
```


L61
```c
#define COFFEE2_REG_WORKFLOW_ERROR            0x1029U
```


L62
```c
#define COFFEE2_REG_MACHINE_STATUS             0x1020U
```


L63
```c
#define COFFEE2_REG_HOT_WATER_STATUS           0x1082U
```


L64
```c
#define COFFEE2_REG_COFFEE_PIPE_STATUS         0x1083U
```


L65
```c
#define COFFEE2_REG_FRUIT_STATUS               0x10A0U
```


L66
```c
#define COFFEE2_REG_FRUIT_A_LOW                0x10A1U
```


L67
```c
#define COFFEE2_REG_FRUIT_B_LOW                0x10A2U
```


L68
```c
#define COFFEE2_REG_FRUIT_A_STATUS             0x10A8U
```


L69
```c
#define COFFEE2_REG_FRUIT_B_STATUS             0x10A9U
```


L71
```c
#define COFFEE2_REG_LOCAL_INPUT_LOW           0x10F0U
```


L72
```c
#define COFFEE2_REG_LOCAL_INPUT_HIGH          0x10F1U
```


L73
```c
#define COFFEE2_REG_EXTERNAL_INPUT_1_LOW      0x10F2U
```


L74
```c
#define COFFEE2_REG_EXTERNAL_INPUT_1_HIGH     0x10F3U
```


L75
```c
#define COFFEE2_REG_EXTERNAL_INPUT_2_LOW      0x10F4U
```


L76
```c
#define COFFEE2_REG_EXTERNAL_INPUT_2_HIGH     0x10F5U
```


L77
```c
#define COFFEE2_REG_EXTERNAL_INPUT_3_LOW      0x10F6U
```


L78
```c
#define COFFEE2_REG_EXTERNAL_INPUT_3_HIGH     0x10F7U
```


L79
```c
#define COFFEE2_REG_LOCAL_OUTPUT_LOW          0x10F8U
```


L80
```c
#define COFFEE2_REG_LOCAL_OUTPUT_HIGH         0x10F9U
```


L81
```c
#define COFFEE2_REG_EXTERNAL_OUTPUT_1_LOW     0x10FAU
```


L82
```c
#define COFFEE2_REG_EXTERNAL_OUTPUT_1_HIGH    0x10FBU
```


L83
```c
#define COFFEE2_REG_EXTERNAL_OUTPUT_2_LOW     0x10FCU
```


L84
```c
#define COFFEE2_REG_EXTERNAL_OUTPUT_2_HIGH    0x10FDU
```


L85
```c
#define COFFEE2_REG_EXTERNAL_OUTPUT_3_LOW     0x10FEU
```


L86
```c
#define COFFEE2_REG_EXTERNAL_OUTPUT_3_HIGH    0x10FFU
```


L88
```c
#define COFFEE2_REG_LOCAL_IO_DEBUG             0x0208U
```


L89
```c
#define COFFEE2_REG_EXTERNAL_IO_DEBUG          0x0209U
```


L92
```c
#define COFFEE2_PRODUCTION_IDLE               0U
```


L93
```c
#define COFFEE2_PRODUCTION_RUNNING            1U
```


L94
```c
#define COFFEE2_PRODUCTION_COMPLETED          2U
```


L95
```c
#define COFFEE2_PRODUCTION_FAILED             3U
```


L101
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
} Coffee2ServerClientStatus_t;
```


L113
```c
typedef struct {
	Coffee2ServerClientStatus_t axClient[COFFEE2_SERVER_MAX_CLIENTS];
	uint32_t ulAcceptedCount;
	uint32_t ulRejectedCount;
	uint32_t ulListenerErrorCount;
	uint32_t ulOnlineTransitionCount;
	uint16_t usListenPort;
	uint8_t ucActiveClients;
	uint8_t ucListening;
	uint8_t ucOnline;
} Coffee2ServerStatus_t;
```

### [Application/UserAPP/Coffee2OpenApp/IO_State/coffee2_io.c](../../../../Application/UserAPP/Coffee2OpenApp/IO_State/coffee2_io.c)

SHA-256：`44B50E37BE6D4E610272F34C4C119569113341490D384129377937EA37BE871C`。以下行号对应 2026-09-08 工作区快照。

| 定义行 | 函数 | 下行调用（词法检索，非运行时跟踪） |
| --- | --- | --- |
| 102 | `vCoffee2IoInitialize` | memset, taskENTER_CRITICAL, taskEXIT_CRITICAL, vCoffee2IoRefreshLocal |
| 114 | `vCoffee2IoRefreshLocal` | HAL_GPIO_ReadPin, memcpy, prvLogIoChanges, taskENTER_CRITICAL, taskEXIT_CRITICAL, xTaskGetTickCount |
| 156 | `ucCoffee2IoSetLocalOutput` | HAL_GPIO_WritePin, taskENTER_CRITICAL, taskEXIT_CRITICAL, xCoffee2LogPrintfOrder, xTaskGetTickCount |
| 191 | `ucCoffee2IoApplyLocalDebugMask` | HAL_GPIO_WritePin, vCoffee2IoRefreshLocal |
| 206 | `vCoffee2IoCommitModbusInput` | memcpy, prvLogIoChanges, taskENTER_CRITICAL, taskEXIT_CRITICAL, xTaskGetTickCount |
| 234 | `vCoffee2IoCommitModbusOutputImage` | memcpy, prvLogIoChanges, taskENTER_CRITICAL, taskEXIT_CRITICAL, xTaskGetTickCount |
| 262 | `prvLogIoChanges` | xCoffee2LogPrintfOrder |
| 280 | `vCoffee2IoGetSnapshot` | taskENTER_CRITICAL, taskEXIT_CRITICAL |
### [Application/UserAPP/Coffee2OpenApp/IO_State/coffee2_io.h](../../../../Application/UserAPP/Coffee2OpenApp/IO_State/coffee2_io.h)

SHA-256：`170DF450286BFD29EFDAD0A0999E9C49C20C2891F6E5DA8FFDC9B8A17E8D2B22`。以下行号对应 2026-09-08 工作区快照。

类型与常量原文按头文件顺序列出；正文先解释用途，这里保留全部字段/枚举值，方便逐项对照。API 原型请沿文件链接阅读。

L9
```c
#define COFFEE2_IO_H
```


L18
```c
#define COFFEE2_LOCAL_IO_COUNT               8U
```


L20
```c
#define COFFEE2_MODBUS_IO_COUNT              16U
```


L22
```c
typedef enum {
	COFFEE2_LOCAL_DI_HOT_WATER_HIGH = 4,
	COFFEE2_LOCAL_DI_HOT_WATER_LOW = 5
} Coffee2LocalInputPoint_e;
```


L27
```c
typedef enum {
	COFFEE2_LOCAL_DO_HOT_WATER_SUPPLY_VALVE = 2
} Coffee2LocalOutputPoint_e;
```


L31
```c
typedef enum {
	COFFEE2_EXTERNAL_DI_OUTPUT_FRONT_CUP = 0,
	COFFEE2_EXTERNAL_DI_OUTPUT_REAR_CUP = 1,
	COFFEE2_EXTERNAL_DI_PURE_WATER_LOW = 3,
	COFFEE2_EXTERNAL_DI_WASTE_BIN_PRESENT = 4,
	COFFEE2_EXTERNAL_DI_MILK_LOW = 10,
	COFFEE2_EXTERNAL_DI_FRUIT_MILK_A_LOW = 11,
	COFFEE2_EXTERNAL_DI_FRUIT_MILK_B_LOW = 12
} Coffee2ExternalInputPoint_e;
```


L41
```c
typedef enum {
	COFFEE2_EXTERNAL_DO_WATER_HEATER_RELAY = 0,
	COFFEE2_EXTERNAL_DO_MILK_VALVE = 3,
	COFFEE2_EXTERNAL_DO_FRUIT_MILK_A_VALVE = 4,
	COFFEE2_EXTERNAL_DO_FRUIT_MILK_B_VALVE = 5,
	COFFEE2_EXTERNAL_DO_FRUIT_MILK_A_PUMP = 9,
	COFFEE2_EXTERNAL_DO_FRUIT_MILK_B_PUMP = 10,
	COFFEE2_EXTERNAL_DO_BOOSTER_PUMP = 11
} Coffee2ExternalOutputPoint_e;
```


L52
```c
typedef struct {
	uint8_t aucXPin[COFFEE2_LOCAL_IO_COUNT];
	uint8_t aucMB1XPin[COFFEE2_MODBUS_IO_COUNT];
	uint8_t aucMB2XPin[COFFEE2_MODBUS_IO_COUNT];
} Coffee2InputIo_t;
```


L59
```c
typedef struct {
	uint8_t aucYPin[COFFEE2_LOCAL_IO_COUNT];
	uint8_t aucMB1YPin[COFFEE2_MODBUS_IO_COUNT];
	uint8_t aucMB2YPin[COFFEE2_MODBUS_IO_COUNT];
} Coffee2OutputIo_t;
```


L66
```c
typedef struct {
	Coffee2InputIo_t xInput;
	Coffee2OutputIo_t xOutput;
	uint32_t ulVersion;
	uint32_t ulLocalUpdateTick;
	uint32_t aulModbusUpdateTick[2];
	uint8_t aucModbusValid[2];
} Coffee2IoState_t;
```

### [Application/UserAPP/Coffee2OpenApp/Ota/coffee2_ota.c](../../../../Application/UserAPP/Coffee2OpenApp/Ota/coffee2_ota.c)

SHA-256：`BD900A5F484CA00F881B71321556B0D47535064E2C16BF11AA8C73368C425D4F`。以下行号对应 2026-09-08 工作区快照。

| 定义行 | 函数 | 下行调用（词法检索，非运行时跟踪） |
| --- | --- | --- |
| 15 | `prvReportLwipFailure` | vCoffee2LogLwipResourceFailure |
| 28 | `xCoffee2OtaInitialize` | xAppOtaInitialize |
| 33 | `xCoffee2OtaBegin` | xAppOtaFlashBegin, xCoffee2OtaInitialize |
| 39 | `xCoffee2OtaWrite` | xAppOtaFlashWrite, xCoffee2OtaInitialize |
| 45 | `xCoffee2OtaFinish` | xAppOtaFlashFinish, xCoffee2OtaInitialize |
| 51 | `vCoffee2OtaAbort` | vAppOtaFlashAbort, xCoffee2OtaInitialize |
| 57 | `ucCoffee2OtaIsActive` | ucAppOtaFlashIsActive |
| 62 | `xCoffee2OtaHttpInitialize` | xAppOtaHttpInitialize, xCoffee2OtaInitialize |
| 68 | `ucCoffee2OtaHttpIsActive` | ucAppOtaHttpIsActive |
### [Application/UserAPP/Coffee2OpenApp/Ota/coffee2_ota.h](../../../../Application/UserAPP/Coffee2OpenApp/Ota/coffee2_ota.h)

SHA-256：`9DBA4A839838BAAA00DE6639B8C925CD3C1F899074F6470FFC849A59A041AADB`。以下行号对应 2026-09-08 工作区快照。

类型与常量原文按头文件顺序列出；正文先解释用途，这里保留全部字段/枚举值，方便逐项对照。API 原型请沿文件链接阅读。

L7
```c
#define COFFEE2_OTA_H
```


L17
```c
#define COFFEE2_OTA_METADATA_ADDRESS       0x08004000UL
```


L18
```c
#define COFFEE2_OTA_STAGING_ADDRESS        0x08060000UL
```


L19
```c
#define COFFEE2_OTA_STAGING_END            0x080C0000UL
```


L20
```c
#define COFFEE2_OTA_APPLICATION_ADDRESS    0x0800C000UL
```


L21
```c
#define COFFEE2_OTA_APPLICATION_END        0x08060000UL
```


L22
```c
#define COFFEE2_OTA_MAX_IMAGE_SIZE \
```


L24
```c
#define COFFEE2_OTA_METADATA_MAGIC         0xDEADBEEFUL
```
