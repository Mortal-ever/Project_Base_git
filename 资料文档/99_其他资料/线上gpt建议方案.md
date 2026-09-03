# STM32F407 通用设备库与双协议后端重构参考说明

## 一、任务定位

请先完整分析工程，再决定最终重构方式。本文是基于部分源码和需求讨论形成的候选架构，不是要求无条件照搬的实施指令。

你需要结合完整工程中的：

* 实际目录结构
* `AGENTS.md`或其他工程约束
* Keil ARMCC5工程配置
* GCC/CMake工程配置
* Coffee2与MilkTea两个产品Target
* FreeRTOS任务和队列结构
* UART、RS-485、TCP物理路分配
* nanoMODBUS实例创建位置
* `Coffee2Command_t`的定义和调用关系
* Flash配置及OTA内存布局
* SRAM、CCM和DMA缓冲分配
* 本地GPIO与协议IO的现有接口
* 各设备实际品牌、型号和协议文档

对本文建议进行验证、修正和收敛。

如果完整工程已经存在等价或更成熟的抽象，应优先复用现有结构，不要重复建设。

---

# 二、已知工程背景

以下信息来自当前提供的部分源码，但仍需在完整工程中复核。

## 2.1 Transport层

现有文件包括：

```text
transport.c/.h
transport_uart.c/.h
transport_tcp.c/.h
```

当前Transport已经实现了典型的C语言面向对象结构：

* `TransportOps_t`作为操作函数表。
* `TransportChannel_t`作为Transport实例。
* `pvContext`保存后端私有上下文。
* UART和TCP实现统一的Open、Close、Send、Receive和Control接口。
* UART后端包含DMA发送、接收StreamBuffer、互斥锁和HAL回调管理。
* Transport支持按名称注册和查找通道。

初步判断：Transport层边界合理，应优先保留，不要重新实现第二套串口或TCP抽象。

## 2.2 Modbus层

现有文件包括：

```text
modbus_port.c/.h
modbus_port_config.h
nanomodbus.c/.h
nanomodbus_config.h
```

`modbus_port`已经完成：

* nanoMODBUS与Transport的绑定
* RTU和TCP模式
* Client和Server角色
* Modbus功能码封装
* RTU帧间静默
* 请求总超时和字节超时
* Transport错误映射
* Modbus异常映射
* 收发报文跟踪
* 原始PDU请求
* 一个`ModbusPort_t`一次只能由一个事务所有者使用

初步判断：现有`modbus_port + nanoMODBUS`应继续作为唯一公共Modbus后端，不建议另建Modbus协议栈。

## 2.3 当前设备协议集合

现有：

```text
coffee2_rtu_protocol.c/.h
```

该模块当前同时包含：

* 咖啡机
* 落杯机
* 落盖机
* 糖浆机
* 制冰机
* 称重模块
* 电能表
* Modbus IO模块

因此它更像“Coffee2产品使用的多个Modbus设备寄存器驱动集合”，而不是单一设备协议。

当前可见耦合包括：

* 公共头文件直接依赖`modbus_port.h`。
* 所有设备接口返回`ModbusPortResult_e`。
* 多个设备状态使用全局镜像。
* IO驱动中包含Coffee2产品日志和状态提交逻辑。
* 部分设备执行函数会同步轮询并调用`vTaskDelay()`。
* 同一个文件内包含多个互不相同的设备寄存器表。

初步判断：未来扩展品牌和自有协议时，应优先拆分这一层；但拆分前必须先检查全部调用者、任务所有权和状态消费者。

## 2.4 IO状态层

现有：

```text
io_state.c/.h
```

当前作用是将原始协议IO状态映射为：

```text
takecapPins
presscapPins
milkshakePins
```

初步判断：这属于产品级逻辑状态映射，不属于Modbus协议栈。

需要检查：

* STM32本地GPIO当前在哪里采集。
* `io_protocol.h`及其全局状态由哪个任务更新。
* `vIOStateUpdate()`的调用者。
* 是否存在并发读取不一致。
* 压盖和其他机构是否已经拥有独立状态机。

---

# 三、目标需求

系统后续需要支持以下设备品类：

* 咖啡机
* 奶茶机
* 制冰机
* 称重模块
* 机器手
* 蒸汽机
* IO模块
* 压盖模组
* 现有工程中仍在使用的落杯机、落盖机、糖浆机和电能表等设备

同一设备品类未来可能存在多个品牌和协议，例如：

```text
咖啡机
├── O系列：Modbus
└── 咖博士F200：自有26字节协议
```

期望达到：

1. 同一设备品类的所有品牌集中管理。
2. 每个实现模块按`.c/.h`成对组织，便于查看和维护。
3. Modbus设备继续复用nanoMODBUS。
4. 自有协议设备使用各自的协议Port和品牌驱动。
5. 产品Workflow不依赖品牌、Modbus寄存器、UART或GPIO编号。
6. 品牌和设备启用情况由配置决定。
7. 同一设备公共接口可以在不修改Workflow的情况下切换品牌。
8. Coffee2和MilkTea可以复用设备库，但保留不同业务流程。
9. 保证Keil ARMCC5和GCC/CMake双工具链同步编译。

---

# 四、候选总体架构

建议验证以下结构是否适合完整工程：

```text
Product Workflow
        ↓
设备品类公共API
        ↓
DeviceManager / DeviceRegistry
        ↓
具体品牌驱动
   ┌────┴────┐
Modbus      Custom
驱动         驱动
   ↓           ↓
modbus_port  专用Protocol Port
   ↓           ↓
nanoMODBUS   Transport或IoService
   └────┬──────┘
      Transport
```

这里不是把全部设备简单分成两个“大设备类”，而是将协议执行层分为两个后端：

```text
Modbus Backend
Custom Backend
```

设备品类仍然独立存在。

需要同时保留以下正交维度：

| 维度      | 作用       | 示例               |
| ------- | -------- | ---------------- |
| 设备品类    | 决定设备能做什么 | 咖啡机、制冰机、称重       |
| 品牌/型号驱动 | 决定具体实现   | O系列、咖博士F200      |
| 协议家族    | 决定底层后端   | Modbus、Custom    |
| 设备角色    | 决定具体实例   | 主咖啡机、Robot2      |
| Route   | 决定物理通信路  | UART2、MBRTU1、TCP |
| IO来源    | 决定逻辑IO来源 | 本地GPIO、Modbus IO |

---

# 五、源码组织原则

可以把同一设备的全部品牌放在同一个设备品类目录中，但不建议把所有品牌和协议实现写进同一个`.c`文件。

建议参考：

```text
Devices/
└── CoffeeMachine/
    ├── CoffeeMachine.c/.h
    └── Drivers/
        ├── CoffeeMachine_O_Modbus.c/.h
        ├── CoffeeMachine_DoctorF200.c/.h
        └── DoctorF200_Port.c/.h
```

含义如下：

* `CoffeeMachine.c/.h`：稳定的咖啡机公共能力。
* `CoffeeMachine_O_Modbus.c/.h`：O系列品牌寄存器协议。
* `CoffeeMachine_DoctorF200.c/.h`：咖博士设备语义和状态机。
* `DoctorF200_Port.c/.h`：26字节组帧、收帧和校验。
* Workflow只包含`CoffeeMachine.h`。
* Workflow不包含品牌驱动和协议Port头文件。

其他设备沿用相同模式。

文件名和目录名可以根据完整工程现有命名规范调整，不要求机械采用上述名称。

不要为了形式给纯类型头文件创建空的`.c`文件。

---

# 六、品牌选择机制

建议由稳定的`DriverId`选择具体品牌驱动，而不是让配置任意组合：

```text
Category
Brand
Protocol
```

因为独立组合可能产生非法配置，例如“咖博士F200 + Modbus”。

候选驱动编号：

```text
COFFEE_O_MODBUS
COFFEE_DOCTOR_F200_CUSTOM
ICE_X_MODBUS
SCALE_X_MODBUS
ROBOT_Y_MODBUS
STEAM_X_MODBUS
IO_X_MODBUS
CAPPING_IO_SEQUENCE_CUSTOM
```

每个驱动描述符应至少包含：

* DriverId
* 设备品类
* 品牌和型号
* 协议家族
* 支持的Transport类型
* 操作函数表
* 所需上下文信息
* 可选能力标志

配置中的一个设备槽位至少需要表达：

* 是否启用
* 设备角色
* DriverId
* RouteId
* Modbus Unit ID或自有协议参数
* 组合设备依赖的其他设备角色

上电时：

```text
读取配置
→ 按DriverId查注册表
→ 验证品类和Route兼容性
→ 根据协议家族绑定后端
→ 创建设备实例
→ 建立Role到实例的映射
```

运行期间不应在每次设备操作中进行大量品牌`switch`。

品牌应在上电时绑定一次，运行时通过驱动函数表完成分发。

---

# 七、C语言面向对象方式

当前Transport已经使用函数表和上下文实现C语言OOP。

建议评估是否将同一模式扩展到设备层：

| OOP概念  | C语言对应实现                          |
| ------ | -------------------------------- |
| 类描述    | 驱动描述符                            |
| 对象     | 设备实例                             |
| 私有成员   | 品牌驱动上下文                          |
| 虚函数表   | DriverOps                        |
| 构造/初始化 | Init                             |
| 多态     | 不同品牌实现同一接口                       |
| 依赖注入   | 注入ModbusPort、Transport或IoService |

注意：

* 驱动描述符应为只读全局信息。
* 运行上下文应属于设备实例。
* 不要把唯一`pvContext`放入全局驱动描述符，否则无法创建多个相同品牌实例。
* 优先使用静态实例或启动阶段静态内存池，不要在运行期间频繁动态分配。
* 不要求改用C++。

如果完整工程已有设备函数表、实例池或注册机制，应直接复用或扩展，不要重复创建新框架。

---

# 八、Modbus后端要求

所有Modbus设备应继续通过现有：

```text
设备品牌Modbus驱动
→ modbus_port
→ nanoMODBUS
→ Transport
```

设备Modbus驱动只负责：

* 寄存器地址
* 功能码选择
* 数值和单位转换
* 命令到寄存器的映射
* 状态寄存器解析
* 品牌特殊故障解释

不要在每个设备驱动中重复实现：

* Modbus CRC16
* RTU组帧
* RTU收包
* 帧间静默
* Transport错误处理
* 总线互斥

同一条Modbus RTU总线原则上应共享：

* 一个Transport通道
* 一个ModbusPort
* 一个通信所有者或Route任务

多个从站通过Unit ID区分。

必须检查完整工程当前是否已经实现了总线级队列或串行化。如果已经存在，应复用；如果不存在，需要防止多个设备任务并发操作同一个`ModbusPort_t`。

---

# 九、自有协议后端要求

自有协议不经过nanoMODBUS。

建议采用：

```text
设备品牌驱动
→ 该协议专用Port
→ Transport
```

每种自有协议可以拥有自己的Port，不必强行抽象成一个万能自有协议栈。

例如咖博士：

```text
CoffeeMachine_DoctorF200
→ DoctorF200_Port
→ UART Transport
```

职责建议：

## DoctorF200品牌驱动

负责：

* 饮品制作
* 取消应用
* 200ms状态查询
* 机器状态转换
* 应用状态转换
* 警告和故障快照
* 超时、重试和在线状态

## DoctorF200 Port

负责：

* 固定26字节组帧
* 包头和包尾
* 接收帧同步
* 帧长度检查
* 校验和
* Transport发送和接收
* 收发错误映射

咖博士协议校验和已经通过协议示例核对：

```text
Checksum = sum(frame[1..23]) & 0xFF
```

规则：

* `frame[0]`包头不参与。
* `frame[24]`校验和本身不参与。
* `frame[25]`包尾不参与。
* 不取反。
* 不求补码。
* 不使用CRC。
* 不使用STM32硬件CRC替代。
* 警告和故障字段按照实际发送的原始字节累加。

正式实现还应处理串口噪声和包头重新同步，不能只假设每次接收都从正确的26字节边界开始。

---

# 十、STM32本地IO与Modbus IO

本地GPIO和协议IO同时存在不会天然污染架构，但必须明确它们不是同一概念。

建议分类：

```text
STM32 GPIO
→ Local IO Backend

Modbus IO模块
→ IoModule Modbus Driver
→ modbus_port

两者共同提供
→ IoService或统一逻辑IO能力
```

STM32本地GPIO不是Custom协议，不需要为了形式强行归入自有协议。

原始状态建议区分：

```text
LocalIoState
ModbusIoState
MachineIoState
```

其中：

* `LocalIoState`保存本地GPIO原始状态。
* `ModbusIoState`保存协议IO原始状态、在线状态和更新时间。
* `MachineIoState`保存取盖、压盖、奶昔等产品逻辑状态。

需要检查现有结构体是否已经完成这一划分。如果已有清晰结构，应保留，不要为了统一而重新合并。

还要检查：

* 本地IO和Modbus IO是否可能绑定到同一个逻辑输出。
* 是否存在多个写入所有者。
* Modbus IO离线后是否继续使用旧状态。
* 状态镜像更新是否具有一致性。
* Workflow是否直接访问HAL GPIO或Modbus线圈地址。

理想边界是：

```text
Workflow和设备动作状态机
只使用逻辑IO编号
不直接使用GPIO端口、引脚、Unit ID和线圈地址
```

---

# 十一、压盖模组定位

压盖模组可以作为设备库中的独立设备品类。

建议候选定位：

```text
Category       = CappingModule
ProtocolFamily = Custom
CustomKind     = IoSequence
```

这里的Custom表示设备级动作协议，不代表它一定使用串口自有帧。

压盖模组对上提供：

* 取盖
* 压盖
* 完整压盖循环
* 回零
* 停止
* 查询状态

内部通过IoService控制：

* STM32本地GPIO
* Modbus IO模块
* 电机输出
* 原点和终点传感器
* 安全互锁

调用关系应类似：

```text
CappingModule
→ Capping IoSequence Driver
→ Capping Port
→ IoService
├── Local GPIO
└── Modbus IO模块
```

压盖模组不应直接依赖：

* HAL GPIO端口和引脚
* `xModbusPortWriteCoil()`
* 某个固定Unit ID
* 产品Workflow内部变量

但如果完整工程现有IO抽象已经提供等价能力，应直接使用现有接口，不要重复增加IoService。

压盖模组包括两个主要动作：

1. 取盖：从杯盖槽取出盖子并移动到落盖位置。
2. 压盖：将杯盖和杯子压合。

这两个动作建议可以独立调用，同时允许上层组合成完整压盖循环。

---

# 十二、产品Workflow边界

Coffee2与MilkTea应分别维护产品业务流程。

Workflow负责：

* 设备调用顺序
* 多设备协同
* 工艺超时
* 产品级异常恢复
* Robot2和Robot3之间的协调
* 取杯、制备、包装、贴标等流程

品牌驱动负责单设备能力，不负责跨设备工艺。

例如：

```text
Robot2举杯并保持
→ 确认Robot2进入保持状态
→ Robot3贴标签
→ 确认Robot3完成
→ Robot2释放或转移杯子
```

这属于Workflow，不应写入Robot2或Robot3品牌驱动。

同样，设备库不应包含Coffee2或MilkTea的完整制作流程。

---

# 十三、任务与并发模型

候选建议是“按物理Route或总线创建通信所有者”，而不是简单地为每个设备槽位创建一个任务。

原因：

* 同一Modbus RTU总线必须串行访问。
* `ModbusPort_t`要求一次只有一个事务所有者。
* 每设备一个任务会增加FreeRTOS栈占用。
* 多任务访问同一UART会产生竞争。
* Route级所有权便于统一管理超时、重连和报文跟踪。

但在实施前必须先检查完整工程现有：

* 任务创建位置
* 命令队列
* 设备轮询机制
* UART所有权
* ModbusPort生命周期
* Workflow是否直接调用阻塞协议函数

现有`coffee2_rtu_protocol.c`包含同步轮询和`vTaskDelay()`。如果保留这些行为，应确保它们只运行在允许阻塞的通信工作任务中，而不是阻塞产品主流程或共享调度任务。

如果完整工程已有可靠的设备任务模型，可以保留现状，只增加必要的总线互斥和驱动分发，不要为追求架构形式进行大范围任务重写。

---

# 十四、内存约束

工程使用STM32F407，必须继续遵循：

```text
DMA可访问缓冲 → SRAM
CPU专用状态数据 → 可考虑CCM
```

必须核实：

* `TransportUartContext_t`
* UART DMA发送暂存区
* UART接收缓冲区
* 咖博士收发帧
* 任何直接交给HAL DMA的缓冲区

均位于DMA可访问SRAM。

设备状态镜像、Workflow状态和不参与DMA的数据可以根据完整内存布局考虑放入CCM。

不要仅因为添加了CCM段，就自动把所有设备上下文移动到CCM。

---

# 十五、Flash配置约束

配置驱动是后续目标，但实施前必须检查：

* Coffee2 OTA布局
* MilkTea完整Flash布局
* Bootloader占用
* 应用占用
* 已有参数区
* 擦除扇区大小
* 掉电恢复要求

候选配置字段包括：

```text
Magic
Schema Version
Payload Length
Sequence
CRC32
Commit Marker
Route配置
设备槽位配置
```

如果配置掉电可靠性重要，建议评估A/B双副本。

需要严格区分：

```text
咖博士通信帧 → 8位累加和
Flash配置     → CRC32
```

仅在主机明确下发“保存配置”时擦写Flash；上电只读取并校验，无效时回退编译期默认配置。

不要在未核实Flash布局前写死配置地址。

---

# 十六、建议Codex先执行的工程调查

在修改代码前，请完成以下检查并给出证据。

## 16.1 构建目标

确认：

* Keil工程中的全部Target
* GCC/CMake Target
* Coffee2和MilkTea公共源码边界
* 产品特有宏
* 新增文件需要加入哪些工程清单

## 16.2 调用关系

搜索并整理：

```text
xCoffee2CoffeeMachineExecute
xCoffee2CupMachineExecute
xCoffee2LidMachineExecute
xCoffee2SyrupMachineExecute
xCoffee2IceMachineExecute
xCoffee2ScaleExecute
xCoffee2PowerMeterExecute
xCoffee2IoModuleExecute
```

同时搜索：

* 全部状态全局变量的读写者
* `Coffee2Command_t`
* `Coffee2Action_e`
* `ucCoffee2CommandIsCanceled`
* ModbusPort创建和绑定位置
* Transport通道注册位置
* `io_protocol`
* `vIOStateUpdate`
* 本地GPIO访问
* 设备任务创建
* Route和Unit ID配置
* CCM段宏和链接配置

## 16.3 设备实例

确认：

* 每个设备品类当前有几个实例。
* 是否可能存在多个相同品牌实例。
* 落杯机和落盖机是否是同一物理控制器。
* 多个设备是否共享同一ModbusPort。
* 机器手是否通过Modbus、自有协议或其他控制接口。
* 压盖模组当前是否已经拥有动作状态机。

## 16.4 并发所有权

确认：

* 谁拥有UART接收。
* 谁调用ModbusPort。
* 是否存在多个任务访问同一ModbusPort。
* 状态镜像是否存在并发读写。
* Workflow调用协议函数时是否会被长时间阻塞。

---

# 十七、建议的迁移策略

最终顺序可以根据完整工程调整，但优先采用小步迁移，不建议一次性重写。

## 阶段0：工程事实报告

先输出：

* 当前架构图
* 设备调用关系
* Route与任务对应关系
* 内存布局
* 现有配置来源
* 已发现风险
* 建议保留和建议拆分的模块

## 阶段1：机械拆分设备Modbus驱动

将`coffee2_rtu_protocol.c`按设备拆分。

第一阶段尽量保持：

* 原函数行为
* 原寄存器地址
* 原超时
* 原轮询逻辑
* 原状态更新
* 原返回结果

只改变文件归属，减少行为风险。

## 阶段2：增加设备描述符和注册机制

引入：

* DriverId
* 驱动描述符
* 设备实例
* Role到Instance映射
* Modbus与Custom后端标记

先使用适配器调用旧接口，避免大爆炸式重构。

## 阶段3：接入两种咖啡机驱动

至少同时验证：

* 一个现有Modbus咖啡机驱动
* 咖博士F200自有协议驱动

确认同一个咖啡机公共API可以通过配置切换品牌。

## 阶段4：统一IO访问边界

在确认现有IO结构后，决定是：

* 直接复用现有IO抽象；
* 还是补充Local IO与Modbus IO统一能力。

不要重复创建同功能模块。

## 阶段5：压盖模组设备化

将取盖和压盖动作收敛为独立设备能力，增加：

* 状态机
* 限位判断
* 超时
* 互锁
* 停止
* 回零
* 依赖IO模块离线处理

## 阶段6：配置驱动

最后再接入Flash持久化配置、Route生成、设备启用和品牌选择。

---

# 十八、禁止事项

在完整工程分析完成前，不要：

1. 删除或重写现有Transport层。
2. 新建第二套Modbus栈。
3. 将所有品牌协议继续追加进单个大`.c`文件。
4. 将STM32本地GPIO错误归类为自有通信协议。
5. 让Workflow直接访问Modbus寄存器或GPIO引脚。
6. 让多个任务无保护地调用同一个ModbusPort。
7. 在未查调用者前删除现有全局状态镜像。
8. 在未检查内存布局前移动DMA缓冲到CCM。
9. 在未确认Flash布局前写死配置地址。
10. 只修改GCC或只修改Keil工程。
11. 为了实现OOP而引入不必要的动态内存。
12. 在没有兼容适配层的情况下同时重写全部设备驱动和业务流程。

---

# 十九、验收目标

最终实现至少应满足：

1. 现有Modbus设备行为和寄存器访问不变。
2. 只有Modbus Route创建nanoMODBUS上下文。
3. 咖博士Route不创建ModbusPort。
4. 同一个咖啡机业务接口可以通过配置切换Modbus和咖博士。
5. Workflow不需要感知品牌和协议。
6. 同一Modbus总线事务严格串行。
7. STM32本地IO与Modbus IO可以同时存在。
8. 压盖模组可以通过统一IO能力完成取盖和压盖。
9. 咖博士校验和通过协议示例测试。
10. DMA缓冲全部位于SRAM。
11. Keil ARMCC5与GCC/CMake全部编译通过。
12. Coffee2和MilkTea两个Target均无回归。
13. 无效配置可以安全回退。
14. 新增品牌只需要增加品牌驱动并注册，不需要修改Workflow。

---

# 二十、要求Codex输出的结果

请先给出：

1. 基于完整工程的当前架构分析。
2. 本文哪些判断与工程一致。
3. 本文哪些判断需要修正。
4. 最终推荐目录和模块边界。
5. 旧文件到新文件的精确迁移表。
6. 品牌选择和设备实例化流程。
7. Route、任务和ModbusPort所有权设计。
8. 本地IO与协议IO的实际处理方式。
9. 内存和Flash风险。
10. 分阶段实施顺序。
11. 每阶段的编译与测试方法。

如果当前任务授权包含代码修改，请在完成上述调查后按照最小风险顺序实施；如果当前任务只是评估，请不要直接修改源码。

最终目标不是机械实现本文结构，而是：

> 在完整工程真实约束下，形成“设备品类清晰、品牌可配置、Modbus复用、自有协议独立、产品业务解耦、双工具链稳定”的可持续设备库架构。
