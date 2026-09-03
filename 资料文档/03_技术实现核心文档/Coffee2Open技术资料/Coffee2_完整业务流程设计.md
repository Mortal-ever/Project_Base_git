# Coffee2 完整业务流程设计

> **2026-08-26 实现校正：** Coffee2OpenV3.0.0 已完成协议与主流程收敛。Coffee2 只使用 `0x000A` 选择出餐口，`0x0009/0x000C` 必须为 0；没有打印、储杯、出餐门或升降机构。当前权威实现说明见同目录《Coffee2OpenV3.0.0_上位机协议与完整业务流程.md》。本文保留较完整的产品推导过程；与权威说明冲突的旧评审描述不再适用。

> 文档状态：业务模型评审版  
> 日期：2026-08-24  
> 本轮范围：整理并评估用户描述的 Coffee2 新流程，不修改源码。  
> 机器人范围：只使用已确认的越疆资料；节卡协议未确定，不进入流程。

## 1. 本文解决什么问题

本文把用户提供的零散描述整理成一条可编程、可诊断、可继续、可取消的 Coffee2 业务流程，回答：

- 上位机订单如何进入下位机；
- 错误订单如何失败并清理；
- 冷杯和热杯如何分支；
- 果乳、咖啡、奶如何按个性化配方组合；
- 四路糖浆口味如何按订单逐路执行、反馈和恢复；
- 为什么 Coffee2 必须先接果乳再接咖啡/奶；
- 机器人、Bus 设备、IO 和 Workflow 各自负责什么；
- 热水、咖啡机清洗、果乳清洗如何作为维护任务执行；
- 每一步卡住后日志和继续指令如何定位与恢复。

## 2. 业务原则

### 2.1 协议外包，业务不外包

设备协议可以交给公共 DeviceLibrary，但业务必须由 Coffee2 Workflow 决定。

以落杯为例：

1. Workflow 判断是否已有杯；
2. 没有杯才向杯机投递落杯命令；
3. Bus/Driver 完成 Modbus 请求和回复；
4. Driver 返回命令成功；
5. Workflow 再检查杯到位传感器；
6. 有杯才完成“落杯步骤”；
7. 没有杯则停在该步骤，等待人工处理、继续或取消。

Bus 不负责判断“订单能否进入下一步”。

### 2.2 上位机允许写入错误订单，下位机负责业务拒绝

“允许下发”不等于“必须执行”。下位机必须：

1. 先形成不可变订单快照；
2. 校验字段和值域；
3. 校验本订单实际需要的设备和条件；
4. 不满足时输出明确失败原因；
5. 执行安全清理；
6. 把订单终态写为失败；
7. 清除订单触发/释放订单槽。

内部可复用 Cancel 清理路径，但对上位机的业务终态应是 FAILED_INVALID 或 FAILED_PRECONDITION，而不是模糊的 CANCELED。

### 2.3 不用“通信成功”冒充“业务成功”

| 事件 | 是否进入下一步 |
|---|---|
| Modbus 收到合法回复 | 否 |
| 设备接受原子命令 | 否 |
| 传感器/重量/机器人完成位满足 | 是 |
| 上位机取消 | 否，进入安全清理 |

### 2.4 一般设备没有统一动作超时

- 通信无回复：是通信超时，可按设备策略重试；
- 从站已回复、但杯传感器没到位：不是通信超时；
- 制冰命令已回复、但重量不足：不是通信超时；
- Robot 是例外：接单后有 60 秒动作时间；
- 阀、泵、加热器必须有安全最大开启时间，但该时间不代表业务动作完成。

## 3. 订单模型

### 3.1 订单必须携带的逻辑字段

当前上位机协议尚未完整定义全部新配方字段，因此这里先定义业务模型，后续再映射寄存器。

| 字段 | 含义 | 校验 |
|---|---|---|
| OrderId | 上位机订单号 | 不得为 0000 或 F123 |
| CupLane | 杯型/杯位 | 必须映射到落杯位 1/2 |
| LidEnable/LidLane | 是否需要盖及盖位 | 映射到落盖位 3/4 |
| IceAmountG | 目标落冰克重 | 单位 1 g；0 按现行上位机协议兼容规则表示热饮 |
| FruitAAmount | 果乳 A 量 | 0 表示不执行 |
| FruitBAmount | 果乳 B 量 | 0 表示不执行 |
| Syrup1Amount～Syrup4Amount | 四种糖浆用量 | 0 表示跳过；必须换算成 0.1 秒 |
| CoffeeRecipe | 咖啡配方 | 0 表示不执行 |
| MilkAmount/Recipe | 奶配方 | 0 表示不执行 |
| OutputLane | 出餐口 1/2 | 只能为 1 或 2 |
| Reserved0006 | 保留字段 | Coffee2 无打印机，写入值被忽略 |

现行上位机协议没有独立的 Temperature 字段，`0x0005` 是落冰克重，不得在下位机文档或代码语义中命名为 temperature。为与 Coffee1 和已开发上位机兼容，当前按协议明文继续使用 `0=热饮，>0=冷饮且数值为落冰克重`。若未来产品要支持“冷饮不加冰”，必须由上位机协议增加独立冷热字段，不能由下位机猜测。

### 3.2 订单校验分层

#### A. 结构校验

- OrderId 合法；
- 订单快照完整；
- 冷/热、杯位、盖位、出餐口枚举合法；
- 数量/配方在 Driver 支持范围；
- 不存在溢出或互斥字段。

#### B. 配方校验

- 咖啡配方是否由当前 F200 Driver 支持；
- 奶配方是否支持独立出奶或与咖啡组合；
- 果乳 A/B 数量是否有校准参数；
- 糖浆 1～4 是否映射到已确认口味，数量换算是否在安全上限内；
- 是否允许“空杯直接出餐”。

用户描述中“没有果乳和咖啡就直接出餐”没有排除“有奶”或“只有糖浆”的情况，因此必须独立判断果乳、四路糖浆、咖啡和奶。只有这些配方量全部为 0 时，才是空杯直出。产品是否允许空杯订单需最终确认。

#### C. 条件校验

只检查本订单需要的设备：

- 需要咖啡/奶才要求 F200 可用；
- 冷饮且需要冰才要求制冰机/称重可用；
- 有果乳才要求对应果乳液位与 IO 模块可用；
- 有任一路糖浆才要求 Bus3 Unit2 糖浆机可用；
- 需要盖才要求落盖设备可用；
- 对应出餐口不可处于冲突状态。

未使用设备离线不能无条件拒绝订单。

### 3.3 错误订单处理

~~~mermaid
flowchart LR
    A[上位机写订单] --> B[Server 形成 Snapshot]
    B --> C[Workflow VALIDATE]
    C -->|合法| D[ORDER_ACCEPTED]
    C -->|非法| E[ORDER_VALIDATION_FAILED]
    E --> F[停止危险输出/取消在途命令]
    F --> G[ORDER_FAILED]
    G --> H[清触发并释放订单槽]
~~~

建议日志：

~~~text
[1234INFO][C2Server:MBTcpServer] ORDER_RECEIVED result=0 order=4660
[1234ERROR][C2Workflow:Workflow] ORDER_VALIDATION_FAILED result=-1201 reason=7
[1234WARN][C2Workflow:Workflow] SAFE_STOP_BEGIN result=0 step=0
[1234ERROR][C2Workflow:Workflow] ORDER_FAILED result=-1201 step=0
~~~

reason 必须使用稳定枚举，例如 INVALID_TEMPERATURE、UNSUPPORTED_RECIPE、FRUIT_A_EMPTY、INVALID_OUTPUT。

## 4. 上电业务初始化与残杯检查

### 4.1 物理空间模型

根据 Coffee2 3D 结构图和电气图，软件应把机台理解为固定工位，而不是沿用 Coffee1 带门、升降和出餐电机的机构：

~~~mermaid
flowchart TB
    subgraph REAR[后侧设备区]
        C[左后：咖啡机\n内部取杯工位]
        I[中后：制冰机]
        CL[右后高台：落杯落盖机\n落杯1/2、落盖3/4]
    end
    subgraph WORK[台面作业区]
        FS[左前：2路果乳管道\n4路糖浆管道]
        R[中央：越疆机器人]
        P[前中：压盖位]
        O1[右前：出餐口1\n有杯传感器]
        O2[右前：出餐口2\n有杯传感器]
    end
    R --- C
    R --- I
    R --- CL
    R --- FS
    R --- P
    R --- O1
    R --- O2
~~~

当前产品没有 Coffee1 的出餐门、升降台和出餐电机。因此：

- 出餐完成只由机器人放杯动作和 X01/X02 有杯信号闭环；
- 上电回收不继承 `FOOD_DOOR_OPEN/CLOSE`、`FOOD_PICKUP_UP/DOWN` 等旧状态；
- Coffee1 的价值是证明“上电回收必须是独立业务状态机”，不是复制旧机构动作。

### 4.2 为什么必须在接单前检查

机械手夹爪、咖啡机内部和压盖位都没有杯传感器，不能把“没有信号”解释为空。唯一可验证的目标位置是出餐口 1/2。初始化必须利用机器人把潜在残杯搬到一个已确认空的出餐口，再用出餐口传感器判断是否真的带出了杯。

初始化属于系统动作，日志订单号固定为 `0000`。初始化完成前关闭订单准入；调试订单号 `F123` 也不能绕过正在运动的初始化状态机。

### 4.3 传感器事实

电气图确认：

| 来源 | 语义 | 用途 |
|---|---|---|
| Bus5 外部 DI X01 | 成品放杯位前有杯 | 出餐口基线与搬运后验证 |
| Bus5 外部 DI X02 | 成品放杯位后有杯 | 出餐口基线与搬运后验证 |

晟枢杯盖协议确认四个独立出口存在位。下表地址属于 **Bus3 Modbus RTU、Unit1、FC01 线圈命名空间**，不是上位机 TCP Server 寄存器：

| 位置 | 端点/功能码/地址 | 语义 |
|---|---|---|
| 落杯1 | Bus3 / Unit1 / FC01 / 0x1008 | 1=有杯检测 |
| 落杯2 | Bus3 / Unit1 / FC01 / 0x100D | 1=有杯检测 |
| 落盖3/落盖1 | Bus3 / Unit1 / FC01 / 0x1012 | 1=有盖检测 |
| 落盖4/落盖2 | Bus3 / Unit1 / FC01 / 0x1017 | 1=有盖检测 |

`Host TCP Server 0x1008/0x100D/0x1012/0x1017` 分别是制作状态、当前现场订单出餐口、果乳通道5数量和预留糖浆6，与上表没有语义关系。后续文档引用这些数字地址时必须同时写出端点、Unit 和功能码。

合体式设备四组地址仍由 Unit1 提供。当前 Driver 已读取这些原始位，但 Workflow 尚未使用索引 `cup[4]`、`cup[9]`、`lid[4]`、`lid[9]`。协议表中的 0x1005/0x100A/0x100F/0x1014 同时被写成缺料和成功感应，存在文档冲突；初始化只使用上表四个互不重复的“有杯/有盖检测”位，并必须实机确认极性。

### 4.4 初始化状态机

~~~mermaid
stateDiagram-v2
    [*] --> WAIT_DEPENDENCIES
    WAIT_DEPENDENCIES --> CHECK_OUTPUT_BASELINE: Robot/F200/Bus3/Bus5有效
    CHECK_OUTPUT_BASELINE --> BLOCKED: 任一出餐口已有杯
    CHECK_OUTPUT_BASELINE --> HOME_1: 两口均空
    HOME_1 --> CHECK_GRIPPER
    CHECK_GRIPPER --> VERIFY_OUTPUT_1
    VERIFY_OUTPUT_1 --> BLOCKED: 搬出杯
    VERIFY_OUTPUT_1 --> HOME_2: 未搬出杯
    HOME_2 --> CHECK_COFFEE
    CHECK_COFFEE --> VERIFY_OUTPUT_2
    VERIFY_OUTPUT_2 --> BLOCKED: 搬出杯
    VERIFY_OUTPUT_2 --> HOME_3: 未搬出杯
    HOME_3 --> CHECK_PRESS
    CHECK_PRESS --> VERIFY_OUTPUT_3
    VERIFY_OUTPUT_3 --> BLOCKED: 搬出杯
    VERIFY_OUTPUT_3 --> CHECK_CUP_LID: 未搬出杯
    CHECK_CUP_LID --> BLOCKED: 四位置任一有物
    CHECK_CUP_LID --> FINAL_HOME: 四位置均空
    FINAL_HOME --> READY
    BLOCKED --> WAIT_OPERATOR
    WAIT_OPERATOR --> WAIT_DEPENDENCIES: 人工清理并写0x0021
~~~

详细步骤：

1. `WAIT_DEPENDENCIES`：Robot 已连接、使能、Ready；F200 在线且未制作/清洗；Bus3 杯盖镜像有效；Bus5 输入镜像有效且新鲜。任一状态未知时只能等待，不能按“空”处理。
2. `CHECK_OUTPUT_BASELINE`：连续读取 X01/X02 至少三次，状态一致才接受。任一出餐口已有杯，立即报警并停止机器人初始化动作。
3. `CHECK_GRIPPER`：Home 后执行“把夹爪当前物放到出餐口1”的协议1开放式出餐口动作。机器人 PLC 程序允许空夹爪执行，STM32 只负责指令接单/完成握手和 X01 后置判断，不增加下位机机械安全互锁。动作完成后刷新 X01；0→1 表示夹爪原有杯，记录警告并阻塞；仍为 0 表示未发现杯。
4. `CHECK_COFFEE`：回 Home，执行“从咖啡机内部取杯”，再放到出餐口1；X01 变为 1 表示咖啡机有残杯，警告并阻塞。
5. `CHECK_PRESS`：回 Home，执行“从压盖位取杯”，再放到出餐口1；X01 变为 1 表示压盖位有残杯，警告并阻塞。
6. `CHECK_CUP_LID`：读取 Bus3/Unit1/FC01 的 0x1008、0x100D、0x1012、0x1017。任一为 1，输出具体位置警告并阻塞；无需机器人搬运。
7. `FINAL_HOME`：机器人回 Home，再复查 X01/X02 和四个杯盖位置均为空，整机状态从初始化中切换为待机，开放订单。

检查夹爪、咖啡机和压盖位时，每次都从 Home 开始，结束后也回 Home，避免把前一步姿态隐式传给下一步。Coffee2 有两个独立出餐口，正常订单的出餐口由上位机 `0x000A`/现场单 `0x000C` 指定。当前 Robot Target 将 `PUT_OUTPUT` 固定到 3138 且忽略参数，实施时必须改为 Coffee1 开放式协议1的两点语义：出餐口1 使用 3131/3111，出餐口2 使用 3132/3112。上电检测可固定使用已空的出餐口1，不改变正常订单的上位机选口规则。

### 4.5 找到杯后的处理

发现残杯不是“初始化成功但带警告”，而是“已把风险显性化但尚未达到可接单状态”：

- 0x1020=4（报警中），0x1008 保持 0；
- 记录唯一位置码和传感器位图；
- 保持订单准入关闭；
- 停止后续搬运动作，防止检测槽已有杯时继续叠杯；
- 人工移除残杯后写现有 0x0021=1；
- Workflow 重新读取所有传感器并从初始化起点复查，不从中间步骤盲续。

建议位置码：1出餐口1、2出餐口2、3夹爪、4咖啡机、5压盖位、6落杯1、7落杯2、8落盖3、9落盖4。位置码是静态枚举，只需 1 字节。

### 4.6 当前代码不能直接实现的三项

1. 当前 `COFFEE2_ACTION_ROBOT_PUT_OUTPUT` 仍是闭式机语义的 3138/3118，`ausParameter[0]` 未参与选择；应改为协议1开放式 Coffee2 Profile 的 3131/3111 和 3132/3112。
2. 当前枚举名 `COFFEE2_ACTION_ROBOT_TAKE_LID` 容易误解，但协议1的 3134/3114 在 Coffee1 标准业务中就是“取压盖位”；应在 Target 语义层重命名或加别名为 `TAKE_PRESS_POSITION`，不新增机器人寄存器。
3. 机器人 PLC 程序允许空夹爪执行。STM32 不增加“有杯才允许抓取”的机械安全判断，只保留通信、接单、动作完成与后置传感器日志。

这三项未确认前，只能实现状态机骨架和传感器直检，不能宣称残杯检查闭环完成。

### 4.7 初始化日志

日志必须能让现场区分“等待设备”“正在检查”“发现杯”“动作失败”和“初始化完成”：

~~~text
[0000INFO][C2Workflow:Init] INIT_BEGIN result=0 step=800
[0000INFO][C2Workflow:Init] INIT_DEPENDENCIES_READY result=0 mask=15
[0000INFO][C2Workflow:Init] INIT_OUTPUT_BASELINE result=0 bitmap=0
[0000INFO][C2Workflow:Init] INIT_GRIPPER_CHECK_BEGIN result=0 output=1
[0000INFO][C2Workflow:Init] INIT_LOCATION_EMPTY result=0 location=3
[0000WARN][C2Workflow:Init] INIT_RESIDUAL_CUP_FOUND result=1 location=4
[0000WARN][C2Workflow:Init] INIT_BLOCKED result=-1 sensor_bitmap=1
[0000INFO][C2Workflow:Init] INIT_RETRY_ACCEPTED result=0 command=33
[0000INFO][C2Workflow:Init] INIT_COMPLETED result=0 step=899
~~~

等待状态只在进入、依赖变化和节流周期输出，不允许每 100ms 刷屏。机器人每个原子动作仍由 C2Robot 输出 SENT/ACCEPTED/MOVING/COMPLETE，Workflow 只记录业务位置和传感器结论。

## 5. Coffee2 总体订单流程

### 5.1 主流程图

~~~mermaid
flowchart TD
    A[订单快照] --> B{校验通过?}
    B -->|否| X[失败日志 + 安全取消]
    B -->|是| C[机器人可用/Home]
    C --> D{冷饮?}
    D -->|冷| E[机器人到冷杯位<br>落冷杯<br>检查杯到位]
    E --> F[机器人到制冰位<br>按重量落冰]
    D -->|热| G[机器人到热杯位<br>落热杯<br>检查杯到位]
    F --> H
    G --> H{有果乳 A/B?}
    H -->|有| I[机器人到果乳位<br>依次出果乳]
    H -->|无| J
    I --> J{有糖浆1～4?}
    J -->|有| S[到糖浆接液工位<br>通道1→4串行出糖]
    J -->|无| K
    S --> K{有咖啡或奶?}
    K -->|有| Q[机器人到咖啡机前/内<br>执行咖啡/奶配方]
    K -->|无| L
    Q --> L{需要盖?}
    L -->|是| M[机器人到盖位3/4<br>落盖/取盖/压盖]
    L -->|否| N
    M --> N[机器人到出餐口1/2]
    N --> O[检查出餐条件]
    O --> P[订单完成并释放槽]
~~~

### 5.2 为什么果乳必须在咖啡/奶之前

Coffee1 的果乳出口接在咖啡机出口，历史状态机可以把果乳和咖啡视为同一工位链路。

Coffee2 已改变机械结构：

- 果乳出口独立；
- 机器人必须先去果乳工位接果乳；
- 再去咖啡机前/咖啡机内接咖啡或奶；
- 不能复制 Coffee1 的旧工位顺序。

这属于产品 Workflow 差异，不属于果乳 Driver 或 F200 Driver。

糖浆在 Coffee1 中于咖啡工位启动，旧版还允许与咖啡制作重叠。Coffee2 首版应优先采用可诊断的串行顺序：果乳→糖浆 1～4→咖啡/奶。若机械确认糖浆仍与咖啡共用接液工位，机器人可以保持同一工位，但 Driver 命令仍逐通道串行；只有完成硬件验证后才考虑并行优化。

## 6. 订单步骤详细设计

### Step 0：接收与冻结

| 项 | 内容 |
|---|---|
| Owner | Server |
| 输入 | 上位机寄存器 |
| 动作 | 复制为 Coffee2Order Snapshot |
| 完成 | 快照不可再被后续寄存器写覆盖 |
| 日志 | ORDER_RECEIVED |

### Step 10：订单校验

| 项 | 内容 |
|---|---|
| Owner | Workflow |
| 前置 | Snapshot 有效 |
| 动作 | 结构、配方、必要设备、液位、输出位校验 |
| 完成 | 生成可执行 RecipePlan |
| 失败 | ORDER_VALIDATION_FAILED→安全清理→ORDER_FAILED |

RecipePlan 是对订单的只读解释结果，可以只保存若干标志和数量，不需要动态列表。

### Step 20：机器人准备

| 项 | 内容 |
|---|---|
| Owner | Robot TCP Owner + Workflow |
| 前置 | 机器人 TCP 在线 |
| 动作 | 冷启动或 warm attach；必要时 Home |
| 接单 | 命令位置 1 后，每 100ms 读命令位；变 0 才是 ACCEPTED |
| 动作时间 | 从 ACCEPTED 开始 60 秒 |
| 断线 | 保留活动事务，重连后读取命令/结果继续闭环 |
| 完成 | Home 对应状态位有效并清完成位 |

如果命令位一直为 1，表示机器人尚未接单。流程保持 WAIT_ACCEPT，不启动 60 秒动作计时；每 5 秒打印一次仍在等待的日志，直到接单、取消或链路恢复。

### Step 30：取杯和落杯

冷饮：

1. Robot 到落杯位 1/2 中对应冷杯的位置；
2. Workflow 读取对应杯到位传感器；
3. 已有杯：跳过落杯命令；
4. 无杯：向杯机投递落杯命令；
5. Bus/Driver 完成设备协议；
6. Workflow 再读杯传感器；
7. 有杯：步骤完成；
8. 无杯：进入 WAIT_CUP_PRESENT。

热饮同样执行，只是选择热杯位置。

落杯位 1/2 与具体冷热杯型的映射尚需机械定义，不能在 Workflow 中用魔法数字。

### Step 40：冷饮制冰

仅 `IceAmountG > 0` 执行；该值是落冰克重，不是温度：

1. Robot 到制冰机位置；
2. 确认杯已位于称重台；
3. 称重去皮；
4. 读取稳定重量；
5. 下发制冰机落冰原子命令；
6. 通信完成后等待重量稳定；
7. 达到目标范围：步骤完成；
8. 未达到：保留 ICE_WEIGHT_NOT_REACHED 状态。

Workflow 可以按既有校准执行有限次自动补偿。自动补偿结束仍未达标时，不伪装成通信超时，等待人工处理、继续或取消。

继续时先重新称重：

- 重量已达标：进入下一步；
- 重量不足：重新下发一次落冰命令；
- 重量超出允许范围：失败并提示人工处理。

### Step 50：果乳

只有 FruitAAmount 或 FruitBAmount 非零才执行。

机器人先到果乳工位。每路按顺序执行，推荐 A 后 B，避免两个泵同时运行造成电源、流量和诊断复杂度。

正常出果乳 A：

1. 读取 A 低液位，缺料则订单失败；
2. 关闭 A 电磁阀；
3. 通过 Bus5 回读确认阀为关；
4. 打开 A 蠕动泵；
5. 按校准时间运行；
6. 关闭 A 泵；
7. 再次确保 A 阀关闭；
8. 完成该通道。

正常出果乳 B 同理。

果乳量到运行时间的换算必须来自静态校准参数；安全最大开启时间必须独立存在。

### Step 55：四路糖浆

只有 `Syrup1Amount～Syrup4Amount` 至少一路非零才执行。四个通道代表四种固定口味，订单字段先映射到物理通道，再按 1→2→3→4 串行处理；不得让四路同时运行。

协议事实：

| 项 | 内容 |
|---|---|
| 链路 | Bus3，9600，8N1，Unit2 |
| 时间设置 | FC06 写 0x0000，单位 0.1 秒 |
| 通道任务 | FC06 写 0x0001～0x0004=1 |
| 状态查询 | FC03 读同一通道寄存器 |
| 状态值 | 2=出糖中，3=出糖成功，4=出糖失败 |
| 剩余量 | 0x000B～0x000E，单位 0.1 秒 |

单通道流程：

1. Workflow 读取冻结订单中的该路配方量；
2. 该路为 0，记录跳过状态并检查下一路；
3. 使用该路静态校准参数换算为 `time_ds`，校验非零、无溢出且不超过安全上限；
4. 如需要独立接液工位，先完成机器人到位闭环；
5. Workflow 投递 `SYRUP_DISPENSE(channel, time_ds)`；
6. Bus3/Driver 写时间并触发对应通道；
7. Driver 每 100ms 读状态，只在状态边沿打印日志；
8. 状态 2 保持该通道执行中；状态 3 完成并进入下一路；状态 4 进入设备失败/人工处理；
9. 四路全部跳过或成功后，Step 55 完成。

Coffee1 v2.7.23_ccram 的 `single_syrup_started`、`multi_syrup_started` 和逐通道任务证明了“同一订单只启动一次、多个通道顺序执行”的现场经验。Coffee2 使用 OrderId、StepId、CommandId 实现同一语义，不移植 Coffee1 的全局 flag、六通道扩展、阻塞 delay 或 `mount / 8` 换算。

继续执行时先读取当前通道状态：

- 2：设备仍在执行，继续等待，不重发；
- 3：本次在途命令已完成，进入下一路；
- 4：记录设备失败，允许人工确认后重新触发当前路或取消订单；
- 空闲/其他：确认不是上次残留终态后，才重新投递当前路。

协议没有订单号或 CommandId，状态 3 可能是旧任务残留。新通道触发前必须建立本地事务边界，不能仅凭触发前读到 3 就判本次成功。

### Step 60：咖啡和奶

只有 CoffeeRecipe 或 MilkRecipe 非零才执行：

1. Robot 到咖啡机前；
2. 条件允许后进入咖啡机内；
3. Workflow 根据配方组合调用咖啡机语义动作；
4. 当前 Coffee2 由 F200 自有 UART Driver 完成，不在 Workflow 中写 Modbus 地址；
5. Driver 完成请求/应答；
6. Workflow 根据设备状态查询确认制作完成；
7. Robot 离开咖啡机工位。

配方组合：

| 果乳 | 咖啡 | 奶 | 工位顺序 |
|---:|---:|---:|---|
| 0 | 0 | 0 | 杯/冰后直接去盖或出餐 |
| 0 | 1 | 0 | 咖啡机 |
| 0 | 0 | 1 | 咖啡机出奶 |
| 0 | 1 | 1 | 咖啡机按咖啡+奶配方 |
| 1 | 0 | 0 | 果乳机 |
| 1 | 1 | 0 | 果乳机→咖啡机出咖啡 |
| 1 | 0 | 1 | 果乳机→咖啡机出奶 |
| 1 | 1 | 1 | 果乳机→咖啡机出咖啡+奶 |

FruitA 和 FruitB 可在“果乳=1”的分支中继续逐路判断。

糖浆与果乳、咖啡、奶同样是独立条件。组合数量从 8 种扩展后不应继续枚举所有排列，固定使用“果乳 A/B→糖浆 1/2/3/4→咖啡/奶”的条件跳过模型。

### Step 65：保留（Coffee2 无打印）

`0x0006` 只为兼容公共寄存器表保留。Coffee2OpenV3.0.0 不创建打印步骤、不等待打印状态，也不向 `0x102A` 投影打印请求。

### Step 70：落盖与压盖

若 LidEnable=1：

1. Robot 到落盖位 3/4；
2. 检查该位置是否已有盖；
3. 无盖才向合体杯盖机投递落盖命令；
4. Driver 返回协议完成；
5. Workflow 检查盖到位条件；
6. Robot 取盖；
7. Robot 压盖；
8. 结果位闭环完成。

本轮基础流程没有给出盖传感器的明确图纸点和“是否所有订单都需要盖”，因此该步骤必须保持可选并等待机械规则确认。

### Step 80：出餐

1. 根据 OutputLane 选择出餐口 1 或 2；
2. Robot 执行对应动作；
3. 命令位清零确认接单；
4. 结果状态位置位确认到达；
5. 检查 X01/X02 杯称/占用信号；
6. 条件满足后订单完成；
7. 写上位机订单完成状态；
8. 清订单触发并释放订单槽。

X01 是前杯称、X02 是后杯称，但与出餐口 1/2 的最终对应关系需机械确认。

## 7. 个性化状态机实现

不建议为每种饮料建立一个大 switch。使用固定顺序和条件跳过：

~~~text
VALIDATE
ROBOT_PREPARE
CUP
IF COLD AND ICE>0: ICE
IF FRUIT_A>0: FRUIT_A
IF FRUIT_B>0: FRUIT_B
IF SYRUP_1>0: SYRUP_1
IF SYRUP_2>0: SYRUP_2
IF SYRUP_3>0: SYRUP_3
IF SYRUP_4>0: SYRUP_4
IF COFFEE OR MILK: COFFEE_MACHINE
IF PRINT: PRINT
IF LID: LID
OUTPUT
COMPLETE
~~~

每一步都使用相同模板：

~~~mermaid
flowchart LR
    A[ENTER] --> B[CHECK_PRECONDITION]
    B -->|已满足| F[DONE]
    B -->|需动作| C[DISPATCH_COMMAND]
    C --> D[WAIT_PROTOCOL_RESULT]
    D -->|失败| X[FAILED]
    D -->|成功| E[CHECK_POSTCONDITION]
    E -->|满足| F
    E -->|不满足| G[WAIT_CONDITION]
    G -->|继续| B
    G -->|取消| Y[CANCELED]
~~~

这比复制 Coffee1 的 80 多个全局状态更清晰，同时保留其“前检、动作、后检”的业务经验。

## 8. 热水维护任务

### 8.1 定位

热水不是订单配方步骤，而是 F123 调试/维护任务。它仍由 Workflow 任务执行，避免 Server 直接控制阀和加热器。

### 8.2 图纸 IO

| 设备 | IO |
|---|---|
| 上液位 | 本地 DI5 |
| 下液位 | 本地 DI6 |
| 供水阀 | 本地 DO3 |
| 热水继电器 | 外部输出 N01 |

输入有效电平必须在 IO 语义层归一化，Workflow 只使用 HighReached/LowReached，不判断 0/1 原始极性。

### 8.3 推荐状态机

~~~mermaid
stateDiagram-v2
    [*] --> IDLE
    IDLE --> CHECK_LEVEL: 收到热水任务
    CHECK_LEVEL --> ALARM_HIGH: 上液位已到
    CHECK_LEVEL --> START_FILL: 下液位未到
    CHECK_LEVEL --> START_HEAT: 下液位已到且上液位未到
    START_FILL --> WAIT_LOW
    WAIT_LOW --> ALARM_HIGH: 上液位到
    WAIT_LOW --> STOP_FILL: 下液位到
    WAIT_LOW --> ALARM_FILL_TIMEOUT: 补水超过安全上限
    STOP_FILL --> START_HEAT
    START_HEAT --> HEATING
    HEATING --> STOP_ALL: 达到加热结束条件
    HEATING --> ALARM_HIGH: 上液位异常
    STOP_ALL --> DONE
    ALARM_HIGH --> STOP_ALL
    ALARM_FILL_TIMEOUT --> STOP_ALL
    DONE --> IDLE
~~~

### 8.4 安全规则

1. 下液位未到时禁止开启加热；
2. 上液位一旦有效，立即关闭供水阀和加热器并报警；
3. 补水超过安全时间仍不到下液位，关闭供水阀并报警；
4. 加热必须有独立最大开启时间；
5. 取消、任务失败、系统异常都必须进入 STOP_ALL；
6. 热水继电器在外部 Modbus IO 上，必须经 Bus5 写入和读回。

Coffee1 原型使用 120 秒补水上限、默认 30 分钟加热，但这些数值不能直接作为 Coffee2 定值。用户本轮没有确定补水时间、加热时间/温度终止条件，因此必须配置确认。

## 9. 咖啡机清洗维护任务

咖啡+牛奶管道清洗使用咖啡机 Driver：

1. 校验 Workflow 无订单占用或按产品规则先取消订单；
2. 校验咖啡机在线；
3. 投递 CLEAN_COFFEE_MILK；
4. 等待 F200 请求/应答；
5. 周期查询清洗状态；
6. 完成时输出 CLEAN_COFFEE_DONE；
7. 通信失败按 Driver 策略重试；
8. 取消时发送咖啡机取消并恢复安全状态。

清洗是 MaintenanceContext，不应伪装成普通 CoffeeMake 订单。

## 10. 糖浆机维护任务

糖浆维护继续复用 Workflow 和 F123，不新增任务：

### 10.1 管路清洗

1. 校验没有订单占用糖浆机；
2. 向 Bus3 Unit2 投递 `SYRUP_CLEAN`；
3. Driver 向 0x0005 写 1；
4. 每 100ms 读取 0x0005；
5. 状态 2 记录清洗中，3 表示完成，4 表示失败；
6. 取消或失败时输出明确终态，不用 Server 直接写寄存器。

正式协议只定义“清洗管路”一个动作，没有按通道清洗的寄存器，因此不得在 Workflow 中虚构四个清洗命令。

### 10.2 剩余时间维护

- 0x000A 写入剩余时间设置值，单位 0.1 秒；
- 0x000B～0x000E 分别读取四路剩余时间；
- 设置和读取都属于维护/状态功能，不进入正常订单步骤；
- 上位机侧的口味名称必须通过静态通道映射显示，不能只显示“设备 4”。

## 11. 果乳清洗维护任务

### 11.1 果乳 A

1. 打开 A 电磁阀；
2. 回读确认；
3. 打开 A 蠕动泵；
4. 按清洗时间运行；
5. 关闭泵；
6. 关闭阀；
7. 回读全部关闭；
8. 完成。

### 11.2 果乳 B

用户描述为“B 电磁阀关闭后启动 B 泵”，但 Coffee1 v2.7.23_ccram 的清洗原型对 A/B 都是“阀和泵同时打开”。两者冲突。

因此本轮只能形成两个可选的静态策略：

- CLEAN_WITH_VALVE_OPEN；
- CLEAN_WITH_VALVE_CLOSED。

产品配置必须在实现前明确选择 B 通道策略。不能因为 A/B 对称就擅自修改用户需求，也不能直接复制 Coffee1。

### 11.3 共用规则

- A/B 默认串行清洗；
- 清洗与正常出液互斥；
- 每个阀和泵写入后要读回；
- 取消时关闭所有果乳泵和阀；
- 清洗时长与安全最大开启时间分开；
- 第二输出模块站号未确认前，B 泵不可验收。

## 12. 越疆机器人业务流程

### 12.1 本体准备

越疆连接后读取：

- FC02 本体状态 0～11；
- FC01 当前协议的位置状态/命令区。

无活动事务且未准备好时执行已验证的 9 段本体启动流程；有活动事务时优先恢复动作闭环，不能先清命令位。

### 12.2 位置动作闭环

~~~mermaid
sequenceDiagram
    participant W as Workflow
    participant R as Robot Owner
    participant D as 越疆机器人
    W->>R: Coffee2Command(OrderId, StepId, Action)
    R->>D: 清普通命令区
    R->>D: 目标命令位置1
    loop 每100ms
        R->>D: 读取目标命令位
        D-->>R: 1=未接单 / 0=已接单
    end
    R-->>W: ACCEPTED/MOVING
    loop 接单后最多60s
        R->>D: 读取对应状态位
        D-->>R: 0=运动中 / 1=完成
    end
    R->>D: 清完成状态位
    R->>D: 读回0确认
    R-->>W: COMMAND_DONE
~~~

关键日志：

| 日志 | 含义 |
|---|---|
| ROBOT_ACTION_START | Owner 开始处理该动作 |
| ROBOT_ACTION_PREPARED | 普通命令区已清理 |
| ROBOT_ACTION_SENT | 目标命令位置 1 成功 |
| ROBOT_ACTION_ACCEPTED | 机器人已把命令位清 0，确认接单 |
| ROBOT_ACTION_MOVING | 60 秒动作计时开始 |
| ROBOT_ACTION_COMPLETE_SIGNAL | 对应状态位检测到 1 |
| ROBOT_ACTION_RESULT_CLEARED | 主控清完成位并读回 0 |
| ROBOT_ACTION_COMPLETE | 整个单步握手闭环完成 |

### 12.3 新点位

Coffee2 需要：

- Home；
- 落杯位 1、2；
- 落盖位 3、4；
- 果乳位；
- 制冰机位；
- 咖啡机前；
- 咖啡机内；
- 出餐口 1、2。

建议地址见《Coffee2 新业务与设备适配修改方案》。最终以机器人程序负责人确认表为准。

## 13. 继续订单

继续不是“无条件跳到下一步”，而是重新执行当前步骤的业务判断。

| 卡住步骤 | 继续时先检查 | 已满足 | 未满足 |
|---|---|---|---|
| 落杯 | 对应杯传感器 | 下一步 | 重发落杯 |
| 制冰 | 当前稳定重量 | 下一步 | 再落一次冰 |
| Robot | 命令位/完成位 | 恢复完成闭环 | 继续等待或恢复事务 |
| 果乳 | 阀泵状态和已运行量 | 完成/安全停止 | 重启当前通道 |
| 咖啡/奶 | F200 当前状态 | 下一步 | 查询后决定重发/失败 |
| 落盖 | 盖到位条件 | 下一步 | 重发落盖 |
| 出餐 | 出餐口杯称 | 完成订单 | 保持等待 |

每次重新投递生成新的 CommandId，但保持 OrderId、OrderEpoch、StepId，便于日志区分“同一步第几次尝试”。

## 14. 取消订单

取消必须以当前 OrderId 为对象：

1. Workflow 标记当前 OrderEpoch 取消；
2. 停止热水、果乳、制冰等危险输出；
3. 对咖啡机发送取消；
4. 对当前 Robot 事务执行可恢复的取消策略；
5. 不接受旧命令迟到结果推进新订单；
6. 输出 ORDER_CANCELED 或 ORDER_FAILED；
7. 清上位机订单槽。

如果错误订单在执行前被拒绝，使用同一安全清理函数，但终态仍记录失败原因。

## 15. 日志设计

统一格式：

~~~text
[%04XLEVEL][TASK:MODULE] EVENT result=... field=...
~~~

订单：

- 真实订单：使用上位机 OrderId；
- 调试/维护：F123；
- 系统启动/后台刷新：0000。

### 15.1 Workflow 日志

| 事件 | 触发 |
|---|---|
| ORDER_RECEIVED | Server 形成快照 |
| ORDER_VALIDATION_START | 开始校验 |
| ORDER_VALIDATION_FAILED | 明确原因码 |
| ORDER_ACCEPTED | 可执行 |
| WORKFLOW_STEP_ENTER | 进入步骤 |
| WORKFLOW_COMMAND_SENT | 已投递原子命令 |
| WORKFLOW_COMMAND_DONE | 设备协议完成 |
| WORKFLOW_WAIT_CONDITION | 后置条件未满足，只在状态进入时打印 |
| WORKFLOW_CONDITION_MET | 条件满足 |
| WORKFLOW_CONTINUE | 人工继续 |
| WORKFLOW_STEP_RECOVERED | 继续后通过条件或重发完成 |
| ORDER_COMPLETED | 出餐完成 |
| ORDER_FAILED | 失败终态 |
| ORDER_CANCELED | 取消终态 |

糖浆需要同时保留 Workflow 业务痕迹和 Bus3 协议痕迹：

~~~text
[1234INFO][C2Workflow:Workflow] SYRUP_STEP_ENTER result=0 channels=5
[1234INFO][C2Bus3:Syrup] SYRUP_CHANNEL_COMMAND_SENT result=0 channel=1
[1234INFO][C2Bus3:Syrup] SYRUP_CHANNEL_WORKING result=0 channel=1
[1234INFO][C2Bus3:Syrup] SYRUP_CHANNEL_SUCCESS result=0 channel=1
[1234INFO][C2Workflow:Workflow] SYRUP_CHANNEL_DONE result=0 channel=1
[1234ERROR][C2Bus3:Syrup] SYRUP_CHANNEL_FAILED result=-3 channel=2
~~~

`channels=5` 是通道位图（bit0=通道1、bit2=通道3），不是设备号。100ms 状态轮询不得逐次打印，只打印 0→2、2→3、2→4 等状态变化。

### 15.2 IO/维护日志

建议只在状态变化时打印：

~~~text
[F123INFO][C2Workflow:HotWater] HOT_WATER_FILL_START result=0 lower=0
[F123INFO][C2Workflow:HotWater] HOT_WATER_LOW_REACHED result=0 elapsed_ms=...
[F123ERROR][C2Workflow:HotWater] HOT_WATER_HIGH_ALARM result=-... upper=1
[1234INFO][C2Workflow:FruitMilk] FRUIT_A_VALVE_CLOSED result=0 point=5
[1234INFO][C2Workflow:FruitMilk] FRUIT_A_PUMP_STARTED result=0 point=10
~~~

WAIT 状态不得每 100ms 刷屏。可以进入时打印一次，随后按 5 秒或 10 秒节流打印“仍在等待”。

## 16. 当前流程与目标流程对比

| 项目 | 当前 Coffee2 | 目标 |
|---|---|---|
| 冷热判断 | `0x0005` 非零推断冷饮 | 保持上位机兼容规则；字段语义统一为落冰克重 |
| 落杯完成 | Driver/命令结果为主 | Workflow 再检查杯传感器 |
| 制作顺序 | 咖啡→冰→糖浆 | 冷杯先冰；果乳→糖浆1～4→咖啡/奶 |
| 四路糖浆 | Driver 和 Workflow 已有 1～4 路调用，但 3/4 使用魔法地址、文档无完整流程 | 固定口味映射；1→4 串行；状态边沿、恢复与清洗闭环 |
| 打印 | 旧源码曾保留等待函数 | Coffee2OpenV3.0.0 已从订单主线移除，`0x0006/0x002A/0x102A` 作为保留字段 |
| 果乳 | 未形成 Coffee2 独立流程 | 独立工位、A/B 阀泵状态机 |
| 奶 | 未形成独立业务条件 | 与咖啡分别判断，F200 Driver 执行 |
| 热水 | 无 Coffee2 完整维护状态机 | Workflow MaintenanceContext |
| 清洗 | 分散调试动作 | 咖啡/奶清洗与果乳清洗状态机 |
| Robot 地址 | 协议1旧表，但当前出餐语义套用了闭式机 | 继续使用协议1 3100～3139，选择开放式 Coffee2 两出餐口 Profile |
| Robot2/节卡 | 不适用 Coffee2 | 协议2/3只供后续 MilkTea Robot1/2；节卡 Robot3 待协议到位 |
| 上电协议日志 | 只有任务创建 | 逐设备输出 Driver/协议/Route/Unit/baud |
| 上电残杯检查 | 无 | Workflow 0000 初始化态；出餐口基线→夹爪→咖啡机→压盖位→杯盖四位置→Home |
| 整机状态 0x1020 | 直接投影 Workflow 枚举，语义错位 | 0默认/1待机/2初始化/3忙碌/4报警的独立映射 |
| 旧出餐机构 | 当前旧 Workflow 仍残留存储/旧业务概念 | Coffee2 不使用门、升降和出餐电机，只保留机器人放杯+X01/X02 |

## 17. 实施前开放问题

1. 落杯位 1/2 与冷热杯型的映射；
4. 落盖位 3/4 与盖型的映射；
5. 出餐口 1/2 与 X01/X02 的对应；
6. 果乳 B 清洗阀开/关；
7. 第二输出模块 Unit ID；
8. 热水 DI5/DI6 有效电平；
9. 热水补水安全上限；
10. 加热结束使用时间还是温度，Coffee1 的 30 分钟不能直接采用；
11. F200 咖啡/奶独立与组合配方编号；
12. 是否允许果乳、糖浆、咖啡、奶都为 0 的空杯订单；
13. 是否所有订单都需要落盖/压盖。
14. 糖浆 1～4 对应的四种口味及上位机字段；
15. 四路配方量到 0.1 秒的校准值和安全最大时间；
16. 糖浆状态 3/4 的复位方式，写 1 后是否保证先进入状态 2；
17. 糖浆出口与咖啡出口是否同一工位，首版串行顺序是否需要调整；
18. F200 现场 1～34 菜单槽位与逻辑配方目录的一致性验收。
19. X01/X02 的实机有效电平；本设计按 X01=前/出餐口1、X02=后/出餐口2 实施；
20. Bus3/Unit1/FC01 的 0x1008/0x100D/0x1012/0x1017 四个出口位的实机有效电平；
21. 0x0021清除报警后由上位机触发初始化复查的交互确认。

这些是业务或硬件事实缺口，不应通过增加抽象层来掩盖。确认后即可按本文状态机逐步实现和单设备验收。
