# Coffee2 工程锐评与 Coffee3 落地建议

> 审查日期：2026-09-07  
> 审查对象：Project_Base 当前 Coffee2 工程；Coffee1 `coffee_close_v2.8.29_IOPage` 作为已落地业务基线；Coffee3Close V5 作为后续目标需求。  
> 审查范围：业务流程、公共/私有边界、任务与总线所有权、协议状态、异常恢复、双工具链工程配置、可测试性。  
> 结论性质：锐评和实施建议，不等同于已完成的固件改造。  
> 状态标签：`CONFIRMED` 为源码/文件已核对；`HISTORICAL` 为 Coffee1 追溯证据；`PROPOSED` 为建议；`UNKNOWN` 为缺少硬件或实机证据。

## 1. 结论先行

Coffee2 已经完成了一次有价值的结构性整理：公共 `DeviceLibrary`、`Transport`、
`ProtocolStack`、日志和 OTA 可以被 target 选择性链接，Coffee2 的业务、协议选择、
总线绑定和寄存器语义集中在 `Application/UserAPP/Coffee2App`。这条边界方向是对的，
不应该再为 Coffee3 重建一套公共业务框架。

但当前工程的主要问题已经从“目录混乱”转为“业务闭环证据不足”。代码可以编译，
也有命令队列、设备状态和日志，但很多链路只证明了“协议事务返回”或“步骤函数返回”，
没有统一证明“物理后置条件满足、寄存器状态正确、失败可恢复、重复写不会重复动作”。
Coffee1 的现场价值恰恰在这些边界处理，而不是在它的旧目录结构。

如果由我继续推进，目标不是再加抽象层，而是保留现有公共/私有分界，围绕一个
Coffee2 业务 owner 补齐可验证的订单事务、设备后置条件、出餐状态、维护互斥和异常
恢复；Coffee3 直接复用公共能力并重新实现自己的业务状态机和寄存器语义。

## 2. 证据基线

### 2.1 Coffee2 当前结构（CONFIRMED）

`Application/CMakeLists.txt` 已把以下目标独立声明为公共能力：

- `app_transport`、`app_modbus_port`、`app_log`、`app_diagnostics`、`app_ota`、
  `app_lwip_alert`、`app_tcp_client_session`；
- `device_dobot`、`device_f200`、`device_coffee_o/x/m50`、`device_cup_lid`、
  `device_syrup`、`device_ice`、`device_scale`、`device_power_meter`、`device_io`。

Coffee2 私有目标包含：

`Comm_Log`、`Device`、`IO_State`、`Modbus_Rtu_Bus`、`Modbus_Tcp_Server`、`Ota`、
`Robot_Tcp`、`Task_Manager`、`WorkFlow` 以及 `Config`。这是正确的产品私有组合方式。

### 2.2 Coffee2 业务基线（CONFIRMED）

权威流程文档《Coffee2OpenV3.0.0_上位机协议与完整业务流程.md》明确：

- Coffee2 是开放式机型，两个出餐口；没有储杯位、出餐门和升降机构；
- 上位机只通过 `0x000A` 选择出餐口，`0x0009/0x000C` 必须为 0；
- Workflow 是唯一业务状态机；Server 只接收/校验/冻结订单并投递；
- Bus/Robot owner 只执行原子协议事务；设备协议成功不能替代物理后置条件；
- `0x1008` 表示制作状态，`0x100B/0x100C` 表示出餐口状态；
- F200 使用自有协议，不应伪装成 Modbus RTU。

### 2.3 Coffee1 业务证据（HISTORICAL）

Coffee1 `ControlFlow/control_flow.c` 仍然把以下现场逻辑落在真实代码里：

- `order_make_finish()` 负责制作事务收尾；
- `order_take_finish()` 负责客户取餐事务收尾；
- `production_status` 与 `take_food_stat1/2` 分离；
- 出餐状态使用进行中、成功、失败、超时、放杯完成等多个阶段；
- 取消、设备失败、取餐超时和清洗请求会进入不同处理路径，而不是统一成一个失败值。

这些是业务语义证据，不能直接复制 Coffee1 的架构、全局变量或旧机型分支。

## 3. 锐评：当前 Coffee2 的主要不足

严重度：`P0` 会造成业务错误或安全状态错误；`P1` 会造成现场无法闭环或难以恢复；
`P2` 主要影响扩展、诊断和维护成本。

### P0-1：协议事务完成与业务完成仍存在混淆风险

Coffee2 文档已经明确区分“设备协议事务完成”和“物理后置条件完成”，但当前实现
的核心执行入口仍集中在 `coffee2_workflow.c` 的 `prvRunStep()`（约 636 行起），
并通过 `xCoffee2DeviceWaitCommand()` 等待设备结果。该机制能确认 commandId、
orderEpoch 和设备结果，却不能天然保证每个步骤都执行了对应的传感器、重量、杯位或
出餐状态确认。

例如：

- F200 返回制作完成，不等于杯已离开咖啡机；
- 杯机返回落杯成功，不等于杯位传感器确认有杯；
- 机器人返回动作完成，不等于目标出餐口已检测到杯；
- 糖浆机返回状态完成，不等于本次通道确实属于当前订单，而不是旧状态残留。

建议把每个业务步骤拆成明确的三段记录：`command_sent`、`device_transaction_done`、
`physical_postcondition_verified`。这不是增加任务，而是补齐同一 Workflow 中的步骤
状态和验收条件。

### P0-2：出餐状态必须以 Coffee1 的“双事务”语义为基线

Coffee1 已经证明制作收尾和客户取餐收尾是两件事。Coffee2 当前文档有这套设计，
但审查时不能只看 `0x1008=2` 或 `0x100B/0x100C=5` 是否写入，而要验证：

1. 制作结果是否在正确的业务终点发布；
2. 放杯失败是否保留清晰的出餐口失败状态；
3. 客户取餐确认是否只消费对应出餐口事件；
4. 另一订单是否会覆盖尚未被上位机消费的出餐口状态；
5. `order_make_finish()` 和 `order_take_finish()` 的所有权是否在 Coffee2 中明确。

当前 `coffee2_server.c` 同时负责寄存器投影、命令解释和部分状态收尾，容易让
Server 变成隐藏业务 owner。最终应由 Workflow 持有业务状态，Server 只读/写快照和
发布投影。

审批:通过.按照建议执行

### P1-1：初始化/残杯检查是文档基线，源码闭环需要逐项验收

Coffee2 文档已经定义依赖等待、出餐口基线、夹爪、咖啡机、压盖位、杯盖位置和 Home
复查，但当前 `coffee2_workflow.c` 的初始化逻辑仍使用统一的设备步骤等待和重试路径。
不能仅以“所有设备在线”宣称残杯检查完成，必须逐项验证：

- 出餐口初始状态有效且连续稳定；
- 每个无传感器源都执行一次“假设带杯→投放到可验证位置→检查结果”；
- 发现残杯后锁定接单，人工处理后从初始化起点重新开始；
- 初始化失败不会被周期任务无限刷屏；
- 机器人异常、杯位未知和传感器失联不会被解释成“空位”。

这些规则对 Coffee3 更重要，因为 Coffee3 只有一个出餐口，资源占用更容易被覆盖。

审批: 残杯检查就是进行检查 如果完成残杯检查流程 系统就可以启动 且开始接取订单 只不过发现残杯后需要输出日志 上位机会去读对应的传感器进行拦截 但是我们仍然需要二次拦截 接下来我需要你优化以下流程: 1.残杯检查出餐口不再纳入检查的杯位上,也就是说最多两个存储位用于残杯检查 如果出现第三杯没位置放了就需要进行机器报警告知上位机初始化失败 这时我们得知失败会进行人工处理,并重启下位机 2.订单二次拦截: 上位机会对线上单进行特殊处理,检查存储位是否空余再下发订单,但是为了防止上位机下发错误,我们对上位机的线上订单需要额外的二次检查,首先检查是线上订单->是否存储位空余->空余:可执行订单制作/存储位满:报告制作失败地址0x1008:值0x0003制作失败 3.出餐口不再作为放杯检查的原因就是防止都满了无法制作线下单

### P1-2：取消和失败后的安全收尾还需要形成显式事务

当前 Workflow 有 `xCoffee2CommandSubmitUrgent()`、取消标志、设备 stop/ack 等机制，
但代码规模已经达到约 2158 行，失败路径分散在订单流程、维护流程、急停和设备等待
函数中。风险包括：

- 取消发生在设备命令已发送但结果尚未回写时，业务是否等待安全停止确认；
- 设备状态未知时，是否禁止机器人继续取杯/放杯；
- 清理失败后，是否保留“杯位未知”而不是重新开放接单；
- 同一订单取消后，迟到的设备完成事件是否会被旧 commandId/orderEpoch 拒绝；
- 热水、果乳泵、糖浆阀等危险输出是否都经过同一 STOP_ALL 语义。

建议维护一个私有 `OrderTransaction` 上下文，至少保存 orderId、orderEpoch、当前
阶段、持杯位置、占用资源、取消原因和安全收尾状态；它可以是 Workflow 的静态结构体，
不需要新任务或动态内存。

审批:通过.按照建议执行

### P1-3：周期刷新、工作流动作和 Server 调试命令的所有权必须更硬

Coffee2 的 RTU 总线已经按 `COFFEE2_RTU_BUS_COUNT=4` 建立独占队列和任务，方向正确。
但 Server 仍会根据寄存器直接提交手动命令，Workflow 也会提交设备命令。若没有统一
资源仲裁，手动 IO/机器人动作可能和订单动作同时访问同一物理资源。

必须明确：

- Bus owner 只接受带 owner/事务身份的原子命令；

- Workflow 自动动作拥有默认优先级；

- 手动调试只能在维护模式或资源空闲时执行；

- 输出调试命令必须返回一次请求结果，不能悄悄改变自动流程；

- 周期 `REFRESH` 只能更新镜像，不能被当作业务动作完成。

审批:通过.按照建议执行

### P1-4：协议和文档冲突仍是实现风险

当前 Coffee2 文档已列出协议中的多个冲突，包括 `0x0007` 清零所有权、`0x1008` 完成
示例、`0x0208` 位图范围、`0x100B/0x100C` 值域和部分设备寄存器语义。只在文档中
标记 `CONFLICT` 还不够，源码必须有一份 target 私有“协议事实表”，明确每个地址的：

- 写入方和读取方；
- 边沿触发还是电平触发；
- 允许值和非法值；
- 状态所有权；
- 清除条件；
- 与 Coffee1 的兼容依据；
- 当前是否已实机验证。

未解决的协议冲突不能通过增加抽象层掩盖，也不能靠“兼容多个解释”让状态机继续跑。

审批:1.写入功能已在协议中声明:0x0000起是**功能码：6H/10H **- 上位机写入

读取功能也声明:**功能码：03H **- **上位机读取** 这些都是上位机的行为 可与Coffee1形成事实验证 

2.根据审批完成验证 不再进入Conflict状态

### P1-5：Coffee2 仍有较强的集中式复杂度

`coffee2_workflow.c` 约 2158 行，`coffee2_server.c` 约 1805 行，
`coffee2_robot_tcp.c` 约 1959 行。文件很大本身不是错误，但当前每个文件混合了：

- 业务状态推进；
- 设备动作映射；
- 协议寄存器投影；
- 日志；
- 失败和取消；
- 维护任务。

继续在这些文件里复制分支，会让 Coffee3 更难复用，也容易把 Coffee2 的设备数量、
出餐口和机型条件带到新 target。应按职责把 Workflow 内部拆成静态步骤表/少量私有
函数，保持一个 owner，不要新增 Router、Manager、Factory 等空抽象。

审批:通过.按照建议执行

### P2-1：设备绑定已经私有化，但配置事实还需要更集中

`coffee2_device.c` 的 `s_axBindings` 和 `coffee2_rtu_bus.c` 的 Bus 配置表属于正确的
target 私有配置；问题是设备 ID、Unit、串口、功能动作和日志名仍分散在多个文件。
后续 target 容易复制后漏改一处。

建议保留设备库的协议 API，在每个 target 的 `Config` 建立一份静态配置表，包含：
`device_id / bus_id / unit_id / driver_kind / capability / display_name / enabled`。
Workflow 只引用设备 ID 和能力，不直接写 Unit 或寄存器。

审批:通过.按照建议执行

### P2-2：双工具链闭合度必须作为交付条件，而不是最后补救

当前 CMake 已明确公共 target 和 Coffee2 私有 target，Keil 也已整理为单一 Coffee2
target。风险在于新增或移动文件时容易只更新其中一个工程，尤其是 source 表、设备库
驱动和 Config 头文件。

每次业务修改都应执行同一组静态检查：

- CMake source/include 闭合；
- Keil XML 中路径存在且 Include in Target Build 状态正确；
- 公共层无 UserAPP 反向 include；
- ARMCC V5.06 与 GCC 都能接受相同的 C90/C99/ASCII 代码；
- map 中 RAM/Flash/CCM/DMA 地址满足约束。
- 审批:通过.按照建议执行

## 4. 公共层与私有层的最终边界

### 4.1 公共层必须保留

公共层只放“多个 target 能直接复用且不携带 Coffee2 业务语义”的能力：

| 公共目录 | 允许内容 | 禁止内容 |
| --- | --- | --- |
| `Common/Log` | 有界日志 ring、Transport 适配契约、source 描述结构 | Coffee2/Coffee3 source、订单枚举、寄存器地址 |
| `Common/Diagnostics` | 崩溃上下文接口、弱钩子、通用异常输出 | 某 target 的订单和设备状态 |
| `Transport` | UART/TCP 通道、发送/打开/超时结果 | 业务重试和订单状态 |
| `ProtocolStack/ModbusPort` | Modbus RTU/TCP 帧和事务校验 | 设备型号业务、Coffee2 action |
| `DeviceLibrary` | F200、M50、O/X、机器人、杯盖、糖浆、制冰、称重、电表、IO 的协议能力 | 订单顺序、出餐策略、维护调度、Coffee2/Coffee3 寄存器 |
| `Common/Ota` | Flash/HTTP OTA 能力与安全状态 | Coffee2 订单准入和产品页面语义 |
| `New_Party` | 第三方协议库 | target 业务状态 |

公共设备 API 应接收设备上下文和明确参数，返回原子事务结果及必要的状态镜像；它不
决定“下一步做什么”。

审批:通过.按照建议执行

### 4.2 Coffee2/Coffee3 私有层必须拥有

每个 target 私有层负责：

- 设备型号选择和静态绑定；
- Bus/Robot owner 与任务创建；
- 订单快照、事务 epoch、阶段和资源预留；
- 协议寄存器语义和状态投影；
- 清洗、取消、继续、失败和客户取餐业务；
- IO 名称、极性、点位语义和边沿日志；
- target source 表和业务日志。

Coffee3 可以复制 Coffee2 的业务代码作为起点，但必须重新选择设备绑定、寄存器语义、
出餐状态和资源规则；不能把 `COFFEE2_*` 名称或两个出餐口假设直接作为公共层。

审批:通过.按照建议执行

## 5. 我会怎样优化 Coffee2

### 第一阶段：只补闭环，不改目录

1. 建立 `coffee2_protocol_fact.h/.c` 或等价私有事实表，冻结地址、写入方、清除方、
   值域和验证状态；
2. 建立一个 Workflow 私有 `Coffee2OrderContext_t`，收拢订单身份、当前阶段、资源、
   持杯位置和失败/取消原因；
3. 为每个步骤记录 command sent、device done、postcondition verified 三个结果；
4. 将出餐口状态和制作状态分开维护，明确两个收尾接口；
5. 给所有失败路径补安全停止、迟到事件拒绝和“位置未知”状态；
6. 保留现有任务和队列，先不新增线程、Router 或运行时插件。
7. 审批:通过.按照建议执行

### 第二阶段：拆分大文件内部职责

在已有 Coffee2 私有目录内拆成少量语义文件：

```text
Coffee2App/
  Config/                 target 绑定、协议事实、IO 名称
  Workflow/
    coffee2_workflow.c    唯一业务 owner
    coffee2_order.c       订单快照、幂等、资源预留
    coffee2_recovery.c    取消、失败、安全收尾
    coffee2_maintenance.c 清洗/热水维护
  Modbus_Tcp_Server/      协议回调和寄存器投影
  Modbus_Rtu_Bus/         总线 owner 与设备事务派发
  Robot_Tcp/              机器人 owner
  Device/                 target 设备绑定和状态镜像
```

这不是强制增加抽象层。只有当一个文件内的职责已经能独立测试、且拆分减少重复或
错误路径时才移动；公共 DeviceLibrary 不因文件变大而继续拆层。

### 第三阶段：Coffee1 对照验收

按 Coffee1 已落地行为建立场景矩阵：

- 上电残杯：空位、发现残杯、设备离线、人工清理后复位；
- 订单：热饮、冷饮、果乳 A/B、四路糖浆、咖啡/奶、落盖、两个出餐口；
- 设备后置条件：协议成功但传感器失败、重量不足、F200 完成但杯未取走；
- 取消：每个步骤、迟到回复、设备无法停止、杯位未知；
- 出餐：放杯失败、客户取走、取餐超时、重复 ACK、另一订单覆盖保护；
- 维护：热水、咖啡清洗、糖浆清洗、果乳清洗与订单互斥；
- 通信：RTU 离线恢复、Robot TCP 断线重连、Server 断线重连；
- 负载：日志 ring 满、任务栈水位、DMA 缓冲地址、RAM/Flash 水位。

## 6. Coffee3 应采用的形态

Coffee3 不应成为 Coffee2 的“改名副本”，也不应复制 Coffee1 的大中心状态机。
推荐形态是：

```text
公共 Transport/Modbus/DeviceLibrary/Log/Diagnostics
                 ↓
Coffee3 target adapter（设备选择、总线绑定、IO 映射）
                 ↓
Coffee3 Workflow（订单、制作、储位/出餐口、客户取餐、维护）
                 ↓
Coffee3 Server projection（只负责寄存器投影和输入校验）
```

Coffee3 业务状态最少需要区分：订单制作、内容完成、最终放杯/入储位、出餐口等待、
客户取餐完成、ACK 消费、失败/取消和初始化锁定。每个状态必须有进入条件、动作、
后置条件、超时、失败出口和上位机投影。

Coffee3 的物理差异必须只在私有层体现：一个出餐口、出餐门升降、X3 有杯到无杯并
连续无杯 30 秒、X4 缺水时本单正常收尾但禁止下一单、线上储位与线下出餐口分支。
公共设备库不应知道这些规则。

## 7. 必须保留、最小修改、后置能力、删除项

### 必须保留

- Coffee1 已验证的制作/取餐分离语义；
- 当前 Coffee2 公共 DeviceLibrary 和单总线 owner 模式；
- `orderId + orderEpoch + commandId` 的迟到回复保护；
- 静态内存、DMA 可访问缓冲、ARMCC V5.06/GCC 双工具链兼容；
- 当前日志 source 表和可读正文规则；
- Coffee2 现有设备协议事实，除非实机或协议文件证明确实错误。

### 最小修改

- 补协议事实表和状态所有权；
- 补物理后置条件确认；
- 补取消/失败/位置未知安全收尾；
- 拆分 Workflow 内部可独立验收的职责；
- 增加 Coffee1 对照测试矩阵和双工具链静态检查。

### 后置能力

- 多订单并行制作；
- 运行时设备插件；
- 通用策略/工厂/事件总线；
- Flash 持久化业务事件；
- 跨 target 动态配置。

在没有现场证据前，这些能力都不应提前进入公共层。

### 应删除或禁止继续扩大的内容

- 公共层中的 target 枚举、寄存器、设备 Unit 和业务命令；
- Coffee1 旧机型分支、旧 IO 编号和与 Coffee2 物理结构无关的代码；
- 只转发、不增加验证价值的 Protocol 薄层；
- 为单个调用方建立的 Router/Manager/Factory；
- 只为未来预留但没有测试场景的 API、任务和队列；
- 用“通信成功”直接推进业务的快捷路径。

## 8. 审查结论与执行顺序

当前 Coffee2 可以作为继续开发的基础，但不应把“目录已整理、CMake/Keil 可配置、
命令队列可运行”当作业务已完成。下一步应按以下顺序推进：

1. 先冻结 Coffee2 协议事实和状态所有权；
2. 再补订单事务与设备后置条件；
3. 再补失败、取消、迟到回复和出餐口保护；
4. 用 Coffee1 场景矩阵逐项验收 Coffee2；
5. 最后从公共层组合 Coffee3，重新实现 Coffee3 私有 Workflow；
6. 每个阶段都完成 GCC/Keil 静态闭合检查和资源报告。

本次审查没有修改固件、Keil/CMake、IOC 或协议 DOCX，也没有执行构建和烧录。Coffee1
源工程只作为业务证据读取，不能直接作为 Coffee2/Coffee3 的架构依赖。

## 9. 证据索引

| 结论 | 证据 |
| --- | --- |
| Coffee2 公共/私有目标划分 | `Application/CMakeLists.txt` 的 `APP_PUBLIC_TARGETS` 与 `coffee2_app` |
| Workflow 是业务 owner | `资料文档/03_技术实现核心文档/Coffee2Open技术资料/Coffee2OpenV3.0.0_上位机协议与完整业务流程.md` 第 2、3、6 节；`coffee2_workflow.c` |
| Coffee2 当前步骤编排 | `Application/UserAPP/Coffee2App/WorkFlow/coffee2_workflow.c:636-1068` |
| 设备命令/状态与 commandId/epoch | `Application/UserAPP/Coffee2App/Device/coffee2_device.c/.h:232-664` |
| 四条 RTU 总线及独占队列 | `Application/UserAPP/Coffee2App/Modbus_Rtu_Bus/coffee2_rtu_bus.c:160-340` |
| Server 寄存器/命令投影 | `Application/UserAPP/Coffee2App/Modbus_Tcp_Server/coffee2_server.c` |
| Coffee2 目标配置 | `Application/UserAPP/Coffee2App/Config/coffee2_app_config.h` |
| Coffee1 制作/取餐分离和收尾函数 | `D:/Project_Items/Coffee1/coffee_close_v2.8.29_IOPage/ControlFlow/control_flow.c:470-540, 3000-3130, 5520-5750` |
| Coffee1 业务分析基线 | `资料文档/99_其他资料/Coffee1_coffee_close_v2.7.23_工程分析报告.md` |
| Coffee3 目标需求 | `资料文档/03_技术实现核心文档/Coffee3Close技术资料/Coffee3Close第五版详细业务流程设计.md` |

