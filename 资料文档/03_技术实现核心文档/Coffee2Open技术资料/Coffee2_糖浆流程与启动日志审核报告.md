# Coffee2 糖浆流程与启动日志审核报告

> 日期：2026-08-24  
> 范围：审核四路糖浆业务、Coffee1 原型、Coffee2 当前实现、两份设计文档和启动协议日志。  
> 本轮交付：只修订 Markdown 文档，不修改源码。

## 1. 结论

本轮确认三个问题：

1. 原两份设计文档确实遗漏了四路糖浆的完整订单流程、清洗流程、继续边界和日志链路；
2. 当前 Coffee2 源码不是“完全没有糖浆”，而是已有四路 Driver 和旧 Workflow 顺序调用，但仍缺口味映射、正式字段、量纲校准、恢复防重复和完整日志；
3. 原建议把设备协议清单统一打印成 `C2Main:System` 不够准确。协议选择应由真正拥有链路的 Robot/Bus 任务在链路就绪后逐设备打印，现有日志源已经支持这一做法。

修正后的推荐顺序是：

~~~text
取杯/落杯
→ 冷饮按需制冰
→ 果乳 A/B
→ 糖浆 1/2/3/4（非零通道串行）
→ 咖啡/奶
→ 可选打印
→ 落盖/压盖
→ 出餐
~~~

若机械确认糖浆与咖啡共用接液工位，机器人可以保持同一工位；首版仍建议串行，不复制 Coffee1 的并行优化。

## 2. 依据

### 2.1 正式糖浆机协议

《糖浆机接口文件.docx》共两页，已逐页检查。确认：

| 项 | 正式定义 |
|---|---|
| 协议 | Modbus RTU |
| 串口 | 9600，8 位数据，1 位停止 |
| 站号 | 2 |
| 0x0000 | 出糖时间，单位 0.1 秒 |
| 0x0001～0x0004 | 糖浆 1～4；写 1 启动；读 2/3/4=执行中/成功/失败 |
| 0x0005 | 管路清洗；写 1 启动；读 2/3/4 |
| 0x000A | 剩余时间设置，单位 0.1 秒 |
| 0x000B～0x000E | 四路剩余时间，单位 0.1 秒 |

协议只定义四路。Coffee1 后期源码中的六路兼容不能扩大本次 Coffee2 的设备边界。

### 2.2 Coffee1 v2.7.23_ccram 可继承经验

Coffee1 的价值是业务经验，不是代码结构：

- `ControlFlow/control_flow.c` 使用订单号和 started 标志，避免同一订单重复启动糖浆；
- 多通道路径按通道顺序逐路执行，而不是同时启动；
- 当前通道结束后才进入下一路；
- 在咖啡工位触发糖浆，部分机型允许糖浆和咖啡制作重叠；
- `modbus/sugar.c` 负责 Unit2、时间设置、通道触发、清洗和剩余时间。

不应移植：

- 全局 flag 和集中式大状态机；
- 阻塞 `delay_ms`；
- 六通道扩展；
- `mount / 8` 的旧换算；
- 用固定 30 秒后强制跳到下一通道的旧容错。

当前正式协议的时间单位是 0.1 秒，Coffee2 必须按新配方校准重新定义，不能沿用 `/8`。

### 2.3 Coffee2 当前源码

当前已有能力：

- `Application/DeviceLibrary/SyrupMachine/CurrentModbus/syrup_machine_modbus.c` 已实现刷新、通道 1～4 出糖、清洗和剩余时间设置；
- Driver 写 0x0000 后触发 0x0001～0x0004，并每 100ms 读取状态；
- `Application/DeviceProtocol/Coffee2Protocol/coffee2_rtu_protocol.c` 已把 `SYRUP_DISPENSE/CLEAN/SET_REMAINING` 适配到公共 Driver；
- `Application/UserAPP/Coffee2App/Device/coffee2_device.c` 已绑定 Bus3、Unit2、四路糖浆 Driver；
- `Application/UserAPP/Coffee2App/WorkFlow/coffee2_workflow.c` 已按 1→2→3→4 调用糖浆；
- Server 的 F123 维护入口支持通道 1～4、时间设置、剩余时间设置和清洗；
- 状态投影能读取四路剩余时间。

当前缺口：

| 缺口 | 风险 |
|---|---|
| 订单通道 3/4 使用 0x0013/0x0014 魔法地址 | 上位机协议和源码难以同步 |
| 配方量直接乘固定系数 1 | 毫升、配方单位和 0.1 秒混淆 |
| Driver 将启动和等待终态绑成一次调用 | 重试时可能无法区分“已经执行”与“需要重发” |
| 状态 3/4 的复位语义未确认 | 可能把旧终态误认成本次结果 |
| 30 秒统一返回 TIMEOUT | 把状态不结束与单帧通信超时混在一起 |
| 缺少逐通道状态边沿日志 | 现场无法判断停在发送、执行中还是设备失败 |
| 旧 Workflow 顺序为咖啡→冰→糖浆 | 不符合新的 Coffee2 个性化流程 |

## 3. 四路糖浆目标流程

### 3.1 订单模型

订单使用四个逻辑字段：`Syrup1Amount～Syrup4Amount`。每个字段对应一个固定口味和一个物理通道；0 表示跳过。

通道与口味的映射必须是 Coffee2 Target 的静态 const 配置，不能由 Driver 猜，也不能让 Workflow 到处写 1、2、3、4 魔法数字。

### 3.2 执行边界

~~~mermaid
flowchart LR
    A[Workflow 冻结订单] --> B{该路用量>0?}
    B -->|否| N[下一路]
    B -->|是| C[配方量换算 time_ds]
    C --> D[投递 Bus3 Unit2 命令]
    D --> E[Driver 写0x0000时间]
    E --> F[Driver 写通道寄存器=1]
    F --> G{每100ms读状态}
    G -->|2 执行中| G
    G -->|3 成功| N
    G -->|4 失败| X[人工处理/继续/取消]
    N --> H{还有非零通道?}
    H -->|是| B
    H -->|否| I[糖浆步骤完成]
~~~

Bus3 只判断设备协议结果，Workflow 才判断本订单的四路是否全部完成。

### 3.3 继续执行

继续指令针对当前订单、当前 StepId 和当前通道：

1. 先读当前通道状态；
2. 状态 2：保持等待，不重发；
3. 状态 3：完成本通道，进入下一路；
4. 状态 4：保留失败痕迹，按人工确认决定重发或取消；
5. 空闲：确认不是旧终态后重新投递当前通道；
6. 已完成通道不得重放。

这比“继续就无条件再写 1”更安全，也能避免重复出糖。

### 3.4 清洗和剩余时间

- 管路清洗是 F123 维护任务，使用 0x0005，一个动作覆盖设备定义的管路；
- 正式协议没有四个独立清洗寄存器，不得虚构按通道清洗；
- 0x000A 用于设置剩余时间，0x000B～0x000E 用于显示四路剩余时间；
- 清洗和剩余时间维护不进入订单主流程。

## 4. 启动协议日志修正

### 4.1 原建议的问题

以下形式只能说明 Manager 读到了静态表，不能证明设备由哪个任务实际接管：

~~~text
[0000INFO][C2Main:System] DEVICE_PROTOCOL:F200 result=0 device=2
[0000INFO][C2Main:System] DEVICE_LINK:BUS2_115200 result=0 unit=0
~~~

一个 Bus 任务可挂多个设备，`device=2` 也需要反查枚举，现场可读性不足。

### 4.2 推荐 Owner

| 信息 | 打印者 | 原因 |
|---|---|---|
| TASK_CREATE | C2Main:System | Manager 确实负责创建任务 |
| RTU_READY/UART_READY | C2BusN:MBRtu 或链路 Owner | Owner 确实完成物理链路初始化 |
| 某设备所选 Driver/Unit | 对应设备日志源 | 能直接看出谁使用什么协议 |
| Robot 协议和端点 | C2Robot:MBTcpClient | Robot Task 实际持有 TCP 链路 |

现有 `coffee2_log.c` 已定义 `C2Bus2:Coffee`、`C2Bus3:Cup`、`C2Bus3:Syrup`、`C2Bus3:Lid`、`C2Bus4:Ice`、`C2Bus4:Weigh`、`C2Bus4:EnergyMeter`、`C2Bus5:IoInput` 和 `C2Bus5:IoOutput`，无需增加日志格式。但电能表 Route 已规划迁到 Bus3，静态来源描述也必须从 `C2Bus4:EnergyMeter` 改为 `C2Bus3:EnergyMeter`，否则日志仍会误导现场人员。

推荐示例：

~~~text
[0000INFO][C2Bus3:MBRtu] RTU_READY result=0 baud=9600
[0000INFO][C2Bus3:Cup] DEVICE_PROTOCOL:SHENGSHU_COMBINED result=0 unit=1
[0000INFO][C2Bus3:Syrup] DEVICE_PROTOCOL:SYRUP_4CH_MODBUS result=0 unit=2
[0000INFO][C2Bus3:EnergyMeter] DEVICE_PROTOCOL:DDSU666 result=0 unit=3
~~~

所以不是 `C2Main:lid`，而是现有标准大小写 `C2Bus3:Lid`。`prvCreateTaskLogged` 继续只做任务创建日志；设备协议由任务进入运行态并完成链路初始化后打印。

## 5. 糖浆订单日志

Workflow 和 Bus3 分别打印业务层与协议层：

~~~text
[1234INFO][C2Workflow:Workflow] SYRUP_STEP_ENTER result=0 channels=5
[1234INFO][C2Bus3:Syrup] SYRUP_CHANNEL_COMMAND_SENT result=0 channel=1
[1234INFO][C2Bus3:Syrup] SYRUP_CHANNEL_WORKING result=0 channel=1
[1234INFO][C2Bus3:Syrup] SYRUP_CHANNEL_SUCCESS result=0 channel=1
[1234INFO][C2Workflow:Workflow] SYRUP_CHANNEL_DONE result=0 channel=1
[1234ERROR][C2Bus3:Syrup] SYRUP_CHANNEL_FAILED result=-3 channel=2
~~~

日志规则：

- 100ms 轮询只在状态变化时打印；
- 每条日志带真实 OrderId，F123 仅用于调试/维护；
- `channels` 使用位图并在文档中固定定义；
- 通信超时、设备状态 4、状态长期不结束使用不同事件；
- 不把 Unit、DeviceId 或通道号混成同一个含糊字段。

## 6. 流程遗漏审计

| 流程 | 结果 | 本轮处理 |
|---|---|---|
| 错误订单校验/失败 | 已覆盖 | 保持 |
| Robot 准备、接单、完成、重连 | 已覆盖 | 保持 |
| 冷热杯与落杯后置检查 | 已覆盖但传感器映射开放 | 保持开放项 |
| 冷饮制冰与称重 | 已覆盖 | 保持 |
| 果乳 A/B 配方和清洗 | 已覆盖但 B 阀语义开放 | 保持开放项 |
| 四路糖浆订单 | 文档遗漏，源码部分存在 | 已补完整流程 |
| 糖浆清洗/剩余时间 | 文档遗漏，Driver 已存在 | 已补维护流程 |
| 咖啡/奶配方和清洗 | 已覆盖但 F200 配方号开放 | 保持开放项 |
| 可选打印 | 字段和源码存在，详细流程遗漏 | 已补 Step 65 边界 |
| 落盖/压盖 | 已覆盖但盖传感器开放 | 保持开放项 |
| 出餐口 1/2 | 已覆盖但 X01/X02 对应开放 | 保持开放项 |
| 热水维护 | 已覆盖但时间/有效电平开放 | 保持开放项 |
| 继续/取消 | 已覆盖 | 糖浆增加状态优先判断 |
| 旧版存杯/取存杯、取餐回收 | 新 Coffee2 产品规则未确认 | 不擅自加入主流程 |

除糖浆和可选打印外，没有发现已确认用户流程被整段漏掉。剩余项目属于硬件/配方事实尚未冻结，不应假装已经设计完成。

## 7. RAM 和复杂度

本轮只改文档，RAM/Flash 增量为 0。

未来按本文实施时：

- 不新增任务、队列或动态内存；
- 复用现有 15 寄存器糖浆镜像；
- Workflow 只增加当前通道、通道位图、阶段和状态等约 8～16 字节；
- 协议名和事件名用 const 字符串，主要增加 Flash；
- 复用现有设备级日志源，不增加运行时 Profile。

符合奥卡姆剃刀：补齐事实和状态边界，不新建糖浆任务、不引入脚本引擎、不复制 Driver。

## 8. 本轮修订文件

1. `Coffee2_新业务与设备适配修改方案.md`：新增糖浆协议/流程/恢复，纠正启动日志 Owner；
2. `Coffee2_完整业务流程设计.md`：新增四路糖浆订单、清洗、剩余时间、日志和可选打印步骤；
3. `Coffee2_糖浆流程与启动日志审核报告.md`：本审核报告。
