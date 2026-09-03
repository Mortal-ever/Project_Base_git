# Coffee2 完整程序编排就绪度与嵌入式实施交接书

> **2026-08-26 状态更新：** 本文原为“实施前评审基线”。Coffee2OpenV3.0.0 已完成本轮软件实现；权威的协议边界、业务链和现状见同目录《Coffee2OpenV3.0.0_上位机协议与完整业务流程.md》，完成证据见 `04_执行报告与审计/Coffee2OpenV3.0.0_实现完成报告_2026-08-26.md`。本文中的“待编码”表述仅作为历史评审记录，不再代表当前源码状态。

> 文档状态：嵌入式实施前评审基线  
> 日期：2026-08-24  
> 范围：Coffee2；MilkTea 不在本轮范围  
> 本轮动作：仅分析、建模和文档交接，未修改固件源码

## 1. 结论

当前工程已经具备完整程序的架构骨架，可以开始按模块编码，但还不具备“资料零补充、一次性写完并直接量产”的条件。

已经具备的基石：

- 上位机 Modbus TCP 寄存器镜像、真实订单号和调试订单号 F123；
- 单 Workflow Owner、Robot TCP Owner、每物理 Bus 一个 Owner；
- 公共 DeviceLibrary 和静态 Target Binding；
- Robot 严格接单/完成闭环、断线重连和事务恢复；
- F200、杯盖、糖浆、制冰、称重、电表、数字 IO 协议驱动；
- 静态日志环、订单关联日志和 LwIP 资源诊断；
- GCC/Keil 双工具链工程和 OTA 分区。

不能直接进入整机最终编码的 P0 缺口：

1. Coffee2 当前选中越疆协议1，但 Robot Target 仍把放出餐解析为闭式机 3138；必须改为开放式 Coffee2 两出餐口 Profile，由上位机选择 3131/3132；
2. `TAKE_LID` 名称与业务语义不一致；协议1的 3134/3114 应明确命名为“取压盖位”，不新增寄存器；
3. X01/X02 和 Bus3/Unit1/FC01 四个杯盖出口传感器的实机极性尚未验收；
4. 0x1020 整机状态当前实现与正式上位机协议语义不一致；
5. 新果乳、四路糖浆、F200 奶/咖啡个性化业务仍处于设计而非完整代码状态；
6. 第二外部输出模块 Unit、F200 配方号、热水控制边界等硬件事实仍未冻结。

因此，本交接建议先完成“上电初始化最小闭环”，再重写订单步骤；不要同时引入新任务、脚本引擎、运行时 Profile 或通用流程 DSL。

就绪度图例：🟢 可直接复用；🟡 可编码但需联调；🔴 必须先冻结事实。

| 子系统 | 状态 | 判断 |
|---|---|---|
| 任务/Owner/队列骨架 | 🟢 | 不需增加任务或队列 |
| 公共设备协议 | 🟢 | 当前已选设备均有公共 Driver，剩余是 Target 语义接线 |
| 上位机订单号与日志 | 🟢 | 真实订单/F123/0000 边界已具备 |
| 0x1008/0x1020 状态投影 | 🔴 | 当前存在确定的语义错位，必须先修 |
| 上电残杯状态机 | 🟡 | 流程已冻结，Robot 三项动作语义待确认 |
| 新 Coffee2 完整订单流程 | 🟡 | 可按步骤实现，配方和部分硬件参数待确认 |
| 机器人轨迹与碰撞 | 🔴 | 必须由机器人/机械工程师实机签字 |

## 2. 资料依据与适用边界

本结论综合：

- Coffee2 当前源码；
- 《咖啡售卖机下位机-通信协议》；
- 《店中店开放式咖啡机图纸0820V0.2》；
- Coffee2 3D 结构图；
- 《晟枢 - 落杯落盖分体 - 通讯协议v1.4》；
- Coffee1 `coffee_close_v2.7.23_ccram` 的 `food_pickup_window_init()` 和相关 Robot 回收动作；
- 已完成的公共设备库、订单上下文和 Coffee2 业务设计文档。

Coffee1 仅用于继承已经验证过的“初始化是业务状态机、动作后检查真实条件、异常后人工处理”的经验。Coffee2 没有出餐门、升降台和出餐电机，不能复制 Coffee1 的旧机构状态。

3D 图可用于工位关系和动作顺序建模，不能替代机器人轨迹、夹具开合、杯高、碰撞和安全区验收。

## 3. 当前架构和初始化调用链

~~~mermaid
sequenceDiagram
    participant M as Manager
    participant W as C2Workflow
    participant R as C2Robot Owner
    participant B3 as C2Bus3 Owner
    participant B5 as C2Bus5 Owner
    participant S as C2Server

    M->>W: 创建已有 Workflow Task
    W->>W: OrderId=0000, AdmissionClosed
    W->>R: Refresh/Ready
    W->>B3: Refresh Cup/Lid
    W->>B5: Refresh external DI
    B3-->>W: 0x1008/100D/1012/1017
    B5-->>W: X01/X02 snapshot
    W->>R: Home / residual-cup atomic actions
    R-->>W: Command accepted + completed
    W->>B5: Refresh X01/X02
    B5-->>W: physical postcondition
    alt residual found
        W->>S: machine=ALARM, admission closed
    else all empty
        W->>R: Final Home
        W->>S: machine=STANDBY, admission open
    end
~~~

所有动作继续走现有 Command/Queue/EventGroup 链路。Workflow 不能直接调用 Robot Driver 或 ModbusPort；Server 不能直接执行初始化动作。

## 4. 整机状态和订单状态必须分离

正式上位机协议已经给出两个不同维度：

| 地址 | 语义 | 值 |
|---:|---|---|
| 0x1008 | 制作状态 | 0未完成、1制作中、2完成、3失败 |
| 0x1020 | 整机状态 | 0默认、1待机、2初始化、3忙碌、4报警 |

当前 `prvRefreshStatusRegisters()` 直接把 `g_xCoffee2WorkflowStatus.xState` 写到 0x1020，这是错误映射。例如 Workflow RUNNING=1 会被上位机解释为待机，FAILED=3 会被解释为忙碌。

实施时必须增加显式映射函数，不必新增寄存器：

| 实际阶段 | 0x1008 | 0x1020 |
|---|---:|---:|
| 模块启动未完成 | 0 | 0 |
| 上电残杯检查 | 0 | 2 |
| 初始化报警/等待人工 | 0 | 4 |
| 可接单 | 0 | 1 |
| 订单或维护执行中 | 1或0 | 3 |
| 订单完成 | 2 | 1 |
| 订单失败但整机仍可用 | 3 | 1 |
| 整机故障 | 3或0 | 4 |

## 5. 上电残杯检查实施规格

### 5.1 启动门槛

初始化前必须同时满足：

- Robot TCP 在线、使能、Ready，且无报警；碰撞、空夹爪和机械安全策略由越疆 PLC 程序管理，STM32 不增加业务互锁；
- F200 在线且未制作、未清洗，允许机器人进入咖啡机；
- Bus3 杯盖设备在线，原始线圈镜像有效；
- Bus5 输入设备在线，X01/X02 镜像有效且未过期；
- 无订单、无维护任务、无手动机器人动作在途。

未知状态一律不是“空”。依赖未就绪时保持 0x1020=2，并按状态变化或节流周期记录等待日志。

### 5.2 检查顺序

| 序号 | 位置 | 检查方式 | 成功条件 |
|---:|---|---|---|
| 1 | 出餐口1/2 | 直接读取 X01/X02 | 两者均为无杯且连续三次一致 |
| 2 | 机械手夹爪 | Home→把当前夹持物放出餐口1→刷新X01 | X01仍无杯 |
| 3 | 咖啡机内部 | Home→取咖啡机杯→放出餐口1→刷新X01 | X01仍无杯 |
| 4 | 压盖位 | Home→取压盖位杯→放出餐口1→刷新X01 | X01仍无杯 |
| 5 | 落杯1 | Bus3/Unit1/FC01 0x1008 | 0 |
| 6 | 落杯2 | Bus3/Unit1/FC01 0x100D | 0 |
| 7 | 落盖3 | Bus3/Unit1/FC01 0x1012 | 0 |
| 8 | 落盖4 | Bus3/Unit1/FC01 0x1017 | 0 |
| 9 | 最终姿态 | Robot Home并复查全部输入 | 全部为空、镜像有效 |

前三个无传感器位置必须通过“搬运到有传感器的位置”形成后置条件。Robot 命令完成只证明脚本完成，不证明真的有杯；X01 的变化才是残杯结论。

### 5.3 报警和恢复

发现任一残杯后：

1. 记录 `INIT_RESIDUAL_CUP_FOUND` 和位置码；
2. 停止后续初始化动作；
3. 0x1020=4，订单准入保持关闭；
4. 人工移除杯/盖；
5. 上位机写现有 0x0021=1；
6. Workflow 重新读取传感器并从步骤 1 完整复查；
7. 全部通过后 0x1020=1，才允许订单进入。

不能检测到杯后继续扫描其他隐藏位置，因为唯一检测槽已被占用，会产生叠杯和误判。四个杯盖传感器可以一次读取并在一条位图日志中报告全部异常位置。

## 6. 最小代码改动工作包

### WP1：修正 Coffee2 协议1 Target 语义

责任人：机器人程序工程师 + STM32 工程师。

必须实施：

- Coffee2 保持 `DOBOT_PROTOCOL_1`，不切换奶茶机协议2/3；
- Coffee2 只接受上位机 `0x000A=1/2` 选择出餐口；`0x000C` 为封闭式 v2 字段，在 Coffee2 订单中必须为 0；Target 分别解析到两个开放式出餐动作；
- 3134/3114 的 Target 语义命名为 `TAKE_PRESS_POSITION`，保留线上地址；
- 机器人 PLC 程序允许空夹爪执行；STM32 不增加空杯抓取互锁；
- 仍遵循命令位置 1→机器人接收清 0→结果位置 1→主控清结果的闭环。

可能涉及：

- `Application/UserAPP/Coffee2App/Device/coffee2_device.h`
- `Application/UserAPP/Coffee2App/Robot_Tcp/coffee2_robot_tcp.c`
- `Application/DeviceLibrary/Robot/Dobot/dobot_robot_device.c/.h`
- `Application/UserAPP/Coffee2App/Config/coffee2_app_config.h`

### WP2：语义化传感器

给现有镜像增加只读语义函数，不复制镜像、不增加任务：

- `ucCoffee2IoOutputCupPresent(lane)`；
- `ucCoffee2CupPositionOccupied(slot)`；
- `ucCoffee2LidPositionOccupied(slot)`；
- `ucCoffee2IoSnapshotFresh()`；
- `ucCoffee2CupLidSnapshotFresh()`。

可能涉及：

- `Application/UserAPP/Coffee2App/IO_State/coffee2_io.c/.h`
- `Application/DeviceProtocol/Coffee2Protocol/coffee2_rtu_protocol.c/.h`
- `Application/DeviceLibrary/CupLidController/ShengShu/cup_lid_shengshu.c/.h`

公共 Driver 继续只保存协议事实；“出餐口1”“落盖位置4”等产品语义留在 Coffee2 Target。

### WP3：Workflow 初始化子状态机

在现有 Workflow Task 中增加静态 `Coffee2InitContext_t`，不创建 InitTask：

~~~text
state
step
location
sensorBaseline
lastSensorBitmap
commandId
phase
retryRequested
lastLogTick
~~~

建议状态小于 32～40 字节。复用现有 `prvRunStep`、Robot 命令闭环、IO Refresh 和日志 API。

涉及：

- `Application/UserAPP/Coffee2App/WorkFlow/coffee2_workflow.c/.h`
- `Application/UserAPP/Coffee2App/Config/coffee2_app_config.h`

### WP4：Server 状态、准入和人工确认

实施：

- 启动时订单准入关闭；
- 0x1020 使用独立机器状态映射；
- 初始化未完成时订单明确拒绝并带订单号日志；
- 0x0021 清报警同时通知 Workflow 复查初始化；
- 0x1008 保持制作状态语义，不用它承载初始化。

涉及：

- `Application/UserAPP/Coffee2App/Modbus_Tcp_Server/coffee2_server.c/.h`
- `Application/UserAPP/Coffee2App/WorkFlow/coffee2_workflow.c/.h`

### WP5：整机业务状态机

初始化闭环通过后，再按《Coffee2 完整业务流程设计》落地：

1. 冻结订单；
2. 校验配方和设备条件；
3. 热/冷落杯，Workflow 检查杯位；
4. 冷饮制冰并用称重闭环；
5. 果乳独立工位；
6. 四路糖浆串行；
7. F200 咖啡/奶；
8. 跳过保留的打印字段（Coffee2 无打印机）；
9. 落盖/取盖/压盖；
10. Robot 放到指定出餐口，X01/X02 验证；
11. 完成、继续或取消。

## 7. 日志交接

初始化日志全部使用系统订单 `0000`：

| 事件 | 含义 | 关键字段 |
|---|---|---|
| INIT_BEGIN | 进入业务初始化 | step |
| INIT_WAIT_DEPENDENCY | 某依赖未就绪 | dependency/mask |
| INIT_OUTPUT_BASELINE | 出餐口基线 | bitmap |
| INIT_LOCATION_CHECK_BEGIN | 开始检查位置 | location/output |
| INIT_LOCATION_EMPTY | 未发现杯 | location |
| INIT_RESIDUAL_CUP_FOUND | 找到杯/盖 | location/bitmap |
| INIT_ACTION_FAILED | Robot 原子动作失败 | location/action/result |
| INIT_SENSOR_INVALID | 镜像无效或过期 | device/age_ms |
| INIT_BLOCKED | 等待人工处理 | location |
| INIT_RETRY_ACCEPTED | 收到 0x0021 | command |
| INIT_COMPLETED | 进入待机 | step=899 |

禁止在 100ms 轮询中逐条打印同一状态。日志输出条件是：状态进入、状态变化、失败、完成，或长等待的节流心跳。

## 8. 资源评估

按奥卡姆剃刀方案：

| 资源 | 预计增量 |
|---|---:|
| 新任务 | 0 |
| 新队列/EventGroup | 0 |
| 动态内存 | 0 |
| 初始化静态上下文 | 约 24～40 B RAM |
| 新 Robot 点位表项 | const Flash，约几十字节 |
| 日志字符串和状态机代码 | 数 KB 以内 Flash，需构建实测 |
| Workflow 栈 | 原则上不调整；用高水位验证后再决定 |

最大风险不是 RAM，而是机器人原子动作语义和传感器实机真实性。

## 9. 单步与整机验收矩阵

### 9.1 P0 单步验收

| 用例 | 预期 |
|---|---|
| X01/X02 空/有杯逐点触发 | 日志和状态位与实物一致 |
| Bus3/Unit1/FC01 0x1008/100D/1012/1017 逐点放杯/盖 | 四位置无串位、极性正确，不影响 Host TCP 同数值寄存器 |
| 空夹爪放出餐1/2 | Robot 按 PLC 程序正常完成，传感器仍空；STM32 日志记录完整握手 |
| 夹爪夹杯放出餐1/2 | 对应传感器从0变1 |
| 咖啡机有杯/无杯取杯 | 两种情况都不碰撞；有杯可搬出，无杯可正常结束 |
| 压盖位有杯/无杯取杯 | 同上 |
| Robot/Bus3/Bus5 断线 | 初始化不误判为空，不开放订单 |

### 9.2 上电场景验收

1. 全部为空：依次检查，最终 Home，0x1020 从 2 变 1；
2. 出餐口已有杯：不动机器人，立即报警；
3. 夹爪有杯：放到出餐口，报警并停止；
4. 咖啡机有杯：搬出后报警；
5. 压盖位有杯：搬出后报警；
6. 四杯盖位置任一有物：日志指出准确位置；
7. 人工清理并写 0x0021：完整复查后待机；
8. 初始化中下订单：拒绝，不进入队列；
9. 日志串口未接：初始化和业务仍正常运行；
10. 重启/掉线恢复：不重复执行仍在途的 Robot 动作。

## 10. Definition of Done

只有同时满足以下条件，才能称为“Coffee2 完整程序编排已具备”：

- P0 机器人动作表签字并完成实机逐点测试；
- 图纸 IO 与现场线号、Unit、极性逐点通过；
- 初始化全部场景通过，且不会因日志、Server 客户端或设备离线而崩溃；
- 0x1008/0x1020 与上位机显示一致；
- 冷/热及果乳/糖浆/咖啡/奶组合订单矩阵通过；
- 落杯、落盖、制冰、出餐都有 Workflow 后置条件；
- 继续、取消、通信重试和 Robot 断线恢复通过；
- GCC Debug/Release 和 Keil ARMCC V5.06 Rebuild 通过；
- 记录最终 RAM/CCM/Flash、任务栈高水位和硬件日志。

## 11. 禁止的捷径

- 不把 Manager 模块初始化成功当成整机业务初始化成功；
- 不用固定延时推断有杯/无杯；
- 不把 Robot 动作完成位当成杯传感器；
- 不再把协议1的 3134/3114 误读为“取落下的盖”，它对应取压盖位；
- 不继续使用当前忽略出餐口参数的单一 3138 映射；
- 不在输入镜像无效时按 0 处理；
- 不复制 Coffee1 的门、升降、电机状态；
- 不为初始化新增任务、队列、动态分配或脚本框架；
- 不在机器人轨迹未验证前执行整套自动初始化。

## 12. 工程师开工前签字项

| 项目 | 负责人 | 结果 |
|---|---|---|
| Coffee2 协议1开放式 Profile | 嵌入式 | 已由 Coffee1 标准业务源码确认，待接线实现 |
| 出餐口1/2 映射 3131/3132 | 嵌入式/Robot程序 | 源码事实已确认，待实机逐点验收 |
| 3134/3114 取压盖位 | 嵌入式/Robot程序 | 源码事实已确认，待实机逐点验收 |
| X01/X02 对应和极性 | 电气/嵌入式 | 未签字 |
| 四杯盖出口位和极性 | 电气/嵌入式 | 未签字 |
| 0x0021 初始化复查交互 | 上位机/嵌入式 | 未签字 |
| Robot 指令握手与完成位 | 嵌入式/Robot程序 | 待实机逐点验收 |

签字项是硬件和协议事实，不是软件架构问题。事实冻结后，现有架构可以用最小改动完成实现。
