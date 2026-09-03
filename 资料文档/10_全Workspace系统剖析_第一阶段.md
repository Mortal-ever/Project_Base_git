# Project_Base 全 Workspace 系统剖析——第一阶段

> 文档性质：只读分析、系统建模与问题定位  
> 分析日期：2026-08-20  
> 分析范围：`C:\Users\13193\Desktop\Project_Base` 整个 Workspace  
> 本阶段未修改任何 `.c/.h/.s/.ld/.sct/.uvprojx/.cmake` 源码或工程配置，也未重新构建工程。  
> 本文描述的是“当前 Workspace 中能够由源码和现有构建产物证明的状态”，不是目标架构说明。

## 0. 标签与证据规则

本文采用图标和文字双重标记，避免 Markdown 渲染器不支持颜色时丢失语义：

| 标记 | 含义 |
|---|---|
| 🔵【事实】 | 已由当前源码、工程文件或现有构建产物直接证明 |
| 🟣【推断】 | 由多处事实交叉推出，但仍需要运行时或硬件证据最终确认 |
| 🟠【问题】 | 当前实现、配置或文档之间存在明确不一致或维护缺口 |
| 🔴【风险】 | 在特定条件下可能导致功能、容量、可靠性或扩展问题 |
| 🟢【建议】 | 后续阶段可考虑的最小核查或改进方向，本阶段不实施 |
| ⚪【待确认】 | 当前 Workspace 没有足够证据，不能写成确定结论 |

风险等级：

- `P0`：已确认的功能错误、数据错误、崩溃或安全问题。
- `P1`：高风险工程问题，进入生产或继续扩展前应优先核查。
- `P2`：可维护性、可诊断性或局部资源问题。
- `P3`：长期优化项，不应为了“架构漂亮”立即重构。

---

# 第一部分：《工程初步扫描结果》

## 1. 识别到的 MCU

| 项目 | 当前证据结论 | 证据 |
|---|---|---|
| MCU | STM32F407VET6，LQFP100 | `CubeMX_Base/F407Base.ioc` |
| CPU | ARM Cortex-M4F | `.ioc`、GCC/Keil CPU 参数 |
| 主频 | 168 MHz | `SystemClock_Config()`、`.ioc` |
| 外部时钟 | HSE 8 MHz | `.ioc` |
| Flash | 512 KiB | GCC linker、Keil scatter、芯片型号 |
| 普通 SRAM | 128 KiB，`0x20000000` | `GCC-ARM/linker/*.ld` |
| CCMRAM | 64 KiB，`0x10000000` | linker/scatter、`app_ccm.c` |
| 外部 RAM | ⚪未发现已配置证据 | `.ioc`、链接脚本和当前活动源码未发现外部 RAM 区 |
| 外部 Flash | ⚪未发现已配置证据 | 当前活动工程未发现 QSPI/FSMC 外部 Flash 映射 |
| RTOS | FreeRTOS，经 CMSIS-RTOS V1 启动 | `freertos.c`、`FreeRTOSConfig.h` |
| 网络 | STM32 ETH RMII + LwIP 2.1.2 | `.ioc`、`LWIP/`、`ethernetif.c` |
| 库 | STM32Cube HAL，Cube FW F4 1.28.3 | `.ioc`、`Drivers/` |

🔵【事实】当前启用外设包括 GPIO、DMA、CRC、ETH、RTC、SPI1、UART4、UART5、USART1/2/3/6、FreeRTOS 和 LwIP。`IWDG`、`WWDG` HAL 模块当前没有启用。

## 2. 工具链

| 工具链 | 当前配置 | 关键文件 |
|---|---|---|
| GNU Arm Embedded | GCC 15.2.1，C11/gnu11，Cortex-M4，hard-float | `GCC-ARM/CMakeLists.txt`、`GCC-ARM/cmake/gcc-arm-none-eabi.cmake` |
| CMake/Ninja | 四个 preset：Coffee2/MilkTea × Debug/Release | `GCC-ARM/CMakePresets.json` |
| Keil MDK | ARM Compiler V5.06 update 7；历史构建日志显示 Keil 5.39 | `MDK-ARM/Project_Base.uvprojx`、现有构建日志 |
| Linker | GNU `.ld` + Keil `.sct` 双链 | `GCC-ARM/linker/`、`MDK-ARM/ScatterFiles/` |
| 启动文件 | GCC 与 ARMCC 各自 startup/异常汇编 | `CubeMX_Base/Core/Startup/`、`Application/Diagnostics/Src/` |

🟠【问题 P1】Coffee2/MilkTea Release 的实际 `compile_commands.json` 同时出现 `-Os -g0 -O3 -DNDEBUG`。`gcc-arm-none-eabi.cmake` 显式设置 `-Os`，CMake 默认 Release 参数又追加 `-O3`，后出现的 `-O3` 实际生效。这不是单纯显示问题，会直接影响 Flash 体积、时序与后续容量判断。

## 3. Target

| Target | 产品宏 | 向量表/Flash | 当前主要能力 |
|---|---|---|---|
| Coffee2-Debug | `USE_COFFEE2` | App 从 `0x0800C000` 启动，App 区 208 KiB | 完整 Coffee2 应用、OTA、Server、Robot、RTU Bus、Workflow、DeviceLibrary |
| Coffee2-Release | `USE_COFFEE2` | 同上 | 生产优化配置，但当前实际为 `-O3` |
| MilkTea-Debug | `USE_MILKTEA` | 从 `0x08000000` 启动，512 KiB | MilkTea TCP/RTU/IO 框架，业务仍是骨架 |
| MilkTea-Release | `USE_MILKTEA` | 同上 | 同上 |
| Keil Coffee2 | `USE_COFFEE2` + VTOR offset `0xC000` | `Coffee2_CCM.sct` | 对应 Coffee2 产品组 |
| Keil MilkTea | `USE_MILKTEA` | `MilkTea_CCM.sct` | 对应 MilkTea 产品组 |

🔵【事实】MDK 工程通过 `IncludeInBuild` 排除另一产品组，不是把两个产品的全部应用源码同时链接进同一个 Target。

🟠【问题 P1】当前可见 Keil 产物时间早于 2026-08-20 的 DeviceLibrary/协议源码更新。因此只能证明“Keil 工程已经配置这些组”，不能证明“当前最新 Workspace 已由 Keil V5 完整重建通过”。

## 4. 目录结构——按逻辑职责重建

Workspace 约 2957 个文件，其中大量为 HAL/LwIP/FreeRTOS、Keil 中间产物和 CubeMX 镜像。不能把“文件存在”直接等同为“当前 Target 正在使用”。

```text
Project_Base
├─ Application
│  ├─ Common                  产品选择和公共入口
│  ├─ Diagnostics             HardFault/栈溢出/断言/崩溃输出
│  ├─ Transport               UART/TCP统一传输接口及backend
│  ├─ ProtocolStack
│  │  ├─ ModbusPort           当前活动Modbus端口抽象
│  │  └─ Modbus               旧实现，当前GCC Target未编译
│  ├─ New_Party/nanoMODBUS    当前Modbus协议核心
│  ├─ DeviceLibrary           公共设备身份、协议配置及部分设备驱动
│  ├─ DeviceModel             公共IO模型候选，当前Target未启用
│  ├─ DeviceProtocol          Coffee2/MilkTea产品协议适配
│  └─ UserAPP
│     ├─ Coffee2App           Coffee2业务、设备、Bus、Server、OTA、Robot
│     └─ MilkTeaApp           MilkTea任务/TCP/RTU/IO骨架
├─ CubeMX_Base
│  ├─ Core                    当前活动BSP、main、startup和中断
│  ├─ Drivers                 STM32 HAL/CMSIS第三方代码
│  ├─ Middlewares             FreeRTOS/LwIP第三方代码
│  ├─ LWIP                    当前网络配置与ethernetif
│  └─ CubeMX_Genarate         多份生成镜像，不是当前主构建源
├─ GCC-ARM                    CMake、preset、linker、现有构建产物
├─ MDK-ARM                    Keil工程、scatter、历史产物
├─ 资料文档                   协议、架构、调试、项目输入资料
├─ 交付包 / tmp               交付和临时副本，不参与固件编译
└─ CHANGES.md                 人工维护的变更记录
```

🔴【风险 P2】`CubeMX_Base/CubeMX_Genarate` 中存在 Core/LWIP/MDK/CMAKE 的重复镜像。当前活动构建使用的是 `CubeMX_Base/Core`、`CubeMX_Base/LWIP` 等主目录；如果维护者只按文件名搜索，很容易修改到不参与构建的副本。

## 5. 核心源码

现有 `compile_commands.json` 交叉验证结果：Coffee2 约 153 个活动编译单元，MilkTea 约 150 个，二者共享约 138 个；大部分共享单元属于 HAL、FreeRTOS、LwIP 和公共底层。

| 模块/文件组 | 所属层 | 真实职责 | 主要上游 | 主要下游 | 重要度 |
|---|---|---|---|---|---|
| `Core/Src/main.c`、startup | 平台/启动 | CPU复位后硬件与RTOS启动 | Reset_Handler | HAL、FreeRTOS | 核心 |
| `Core/Src/freertos.c` | 系统入口 | 创建默认任务，初始化LwIP | `main()` | 产品Task Manager | 核心 |
| `Coffee2App/Task_Manager` | 产品系统管理 | 初始化模块、创建业务任务、发布网络事件 | 默认任务 | Server/Robot/Bus/Workflow/Log | 核心 |
| `Coffee2App/Modbus_Tcp_Server` | 上位机入口 | 6001监听、寄存器镜像、订单/维护指令 | LwIP socket | Workflow/Device/OTA | 核心 |
| `Coffee2App/WorkFlow` | 业务编排 | 订单步骤、取消、手动落冰、设备协作 | Server | Device命令队列 | 核心 |
| `Coffee2App/Device` | 命令/状态中枢 | 设备绑定、命令ID、队列路由、事件和终态历史 | Workflow/Server | Robot或RTU Bus | 核心 |
| `Coffee2App/Robot_Tcp` | Robot owner | Robot TCP、启动、动作握手、重连恢复 | Device队列 | ModbusPort/TCP Transport | 核心 |
| `Coffee2App/Modbus_Rtu_Bus` | 串口Bus owner | 每路串行调度、串口配置、设备协议分派 | Device队列 | Protocol adapter/ModbusPort | 核心 |
| `DeviceProtocol/Coffee2Protocol` | 产品协议适配 | 设备动作转寄存器/线圈/帧 | RTU Bus | DeviceLibrary/ModbusPort | 核心 |
| `DeviceLibrary` | 公共设备库 | 身份、协议/驱动配置、部分公共驱动 | 产品适配 | ModbusPort/Transport | 重要 |
| `ProtocolStack/ModbusPort` | 协议端口 | nanoMODBUS封装、错误归一、总预算、帧追踪 | Robot/RTU adapter | nanoMODBUS/Transport | 核心 |
| `Transport` | 通信抽象 | open/send/receive/close函数表；UART/TCP backend | ModbusPort/Log | HAL UART DMA/LwIP socket | 核心 |
| `Diagnostics` | 故障诊断 | HardFault、RTOS hooks、崩溃上下文输出 | 异常向量/RTOS | 直接诊断通道 | 重要 |
| `Coffee2App/Ota` | 升级 | HTTP80流式接收、Flash staging、CRC、metadata、复位 | Server触发/LwIP raw API | HAL Flash/CRC | 核心 |
| `MilkTeaApp/*` | 产品应用 | 双TCP、RTU任务、IO刷新、日志/调试 | 产品Task Manager | Transport/Protocol | 核心但未完成 |

明确未参与当前 GCC 构建的代表项：

- `Application/ProtocolStack/Modbus/Src/*` 旧 Modbus 实现；
- `Application/DeviceModel/IO_State/Src/io_state.c`；
- `Coffee2App/Comm_Log/coffee2_retarget.c`；
- 与工具链不匹配的另一份 crash assembly；
- `CubeMX_Base/CubeMX_Genarate` 下的镜像副本。

## 6. 通信模块

| 通道 | 当前用途 | 物理/软件owner | 接收模式 |
|---|---|---|---|
| USART1 | 日志输出，115200 8N1 | C2Log/MilkTea Log | 主要为TX |
| USART2 / Bus2 | Coffee2咖啡机，19200 | `C2Bus2` | UART字节中断 + StreamBuffer，TX DMA |
| USART3 / Bus3 | 杯机/糖浆/盖机，9600 | `C2Bus3` | 同上 |
| UART4 / Bus4 | 制冰机/称重/电源表，统一19200 | `C2Bus4` | 同上 |
| UART5 / Bus5 | 外部IO输入/输出，38400 | `C2Bus5` | 同上 |
| USART6 | 当前Coffee2未作为主Bus使用 | 平台已初始化 | ⚪需结合目标板继续确认用途 |
| TCP 6001 | 上位机Modbus TCP Server，双客户端槽 | `C2Server` | LwIP socket |
| TCP 192.168.5.1:502 | Coffee2 Robot Modbus TCP Client | `C2Robot` | LwIP socket |
| TCP 80 | Coffee2 OTA HTTP | raw LwIP callback | 单上传会话 |
| MilkTea TCP | Robot `.100:502`、MilkTea `.100:1502` | 各自任务 | LwIP socket |

🔵【事实】当前 Coffee2 的设计原则是“一条物理 Bus 一个 owner task”。Bus task 不承载“常冰/少冰”一类业务语义；它接收统一命令，执行串口所有权、超时/重试/间隔和设备协议分派。

## 7. 协议模块

```text
业务动作 / 维护动作
        ↓
Coffee2Command_t
        ↓
产品协议适配（Coffee2Protocol / Robot语义表）
        ↓
公共设备驱动或 ModbusPort
        ↓
nanoMODBUS
        ↓
Transport UART/TCP
```

当前活动协议包括：

- Modbus TCP Server：FC03/FC06/FC16；
- Robot Modbus TCP Client：FC01/FC02/FC05/FC15 等；
- Modbus RTU：FC01/02/03/04/05/06/0F/10 等，由设备适配选择；
- 咖啡机公共驱动：Kalerm O/X Modbus 与 Dr.Coffee F200 UART；
- Robot 公共配置：Dobot legacy 3139、extended 3150，JAKA 目前仅保留身份；
- OTA：HTTP multipart，不属于 ModbusPort/Transport 体系，直接使用 LwIP raw API。

## 8. 设备模块

Coffee2 当前设备绑定表覆盖：Robot、Coffee、Cup、Syrup、Lid、Ice、Scale、EnergyMeter、IoInput、IoOutput。每个实例包含 route、unit、baud、category、role、driver、protocol 等信息。

🔵【事实】公共 DeviceLibrary 已具备“品类/角色/协议/驱动/传输”身份模型，Dobot 协议配置可描述 legacy 3139 与 extended 3150；Coffee2 当前只实例化一台 Robot1，且配置仍选择 legacy 3139。

🔵【事实】Robot 的“同一地址在不同产品中的业务语义”没有被硬塞进公共协议库，而是由 Coffee2 Robot 点位表完成产品语义映射。这符合“公共库管协议能力、产品层管业务语义”的边界。

⚪【待确认】当前没有 Robot2 的第二套 owner task、命令队列、TCP channel 和状态实例。因此“配置中能表示 Robot2”不等于“Coffee2 运行时已经支持第二台机器人”。

## 9. 业务模块

Coffee2 Workflow 当前源码已经实现一个明确的步骤驱动流程：

```text
刷新设备
  → Robot Home
  → 取杯/落杯
  → 咖啡机前/后位置
  → 制作咖啡
  → 称重去皮/落冰闭环
  → 糖浆
  → 打印/落盖/取盖/压盖
  → 出餐
```

还包含：订单替换 pending、取消 epoch、手动落冰、失败时设备 abort、Robot 断线恢复和命令终态等待。

🟣【推断】代码层面“订单状态机已经存在”，但产品方案和实机验收尚未完全确定，因此应表述为“已实现、待产品/硬件验收”，不能表述为“订单业务已经投产完成”。

MilkTea 当前业务层主要是 20 ms 的 IO 状态更新；四路 RTU 配置均为 disabled，RTU poll hook 和完整产品流程仍是骨架。

## 10. 当前准备重点追踪的调用链

第一阶段已经建立入口和owner关系；下一阶段最值得逐函数闭环的链路是：

1. 上位机 FC06/FC16 → Server寄存器 → 手动原子命令 → Device → Robot/RTU → 设备反馈；
2. 订单提交/替换/取消 → Workflow步骤 → 多设备命令 → 完成/超时/恢复；
3. Robot命令 3121~3150 → 接单清零 → 运动60s → 结果清零 → 断线重连恢复；
4. Bus2~5 UART RX ISR → StreamBuffer → ModbusPort → 协议适配 → Device状态/Workflow；
5. IO写入 → FC05 → FC02/FC01快照 → 软件镜像与上位机状态区；
6. OTA触发0x0200 → HTTP会话 → Flash staging → CRC/metadata → Bootloader；
7. HardFault/栈溢出/malloc失败 → 上下文采集 → 日志输出/系统停机。

---

# 第二部分：当前工程系统模型

## 11. 当前真实分层

```text
┌──────────────────────────────────────────────────────────┐
│ 产品入口：Coffee2 Server / MilkTea TCP Client            │
├──────────────────────────────────────────────────────────┤
│ 业务编排：Workflow、订单/取消/维护动作                    │
├──────────────────────────────────────────────────────────┤
│ 命令与状态：Device binding、Command、Event、History       │
├──────────────────────────────────────────────────────────┤
│ 产品语义适配：Coffee2Protocol、Robot point table          │
├──────────────────────────────────────────────────────────┤
│ 公共设备能力：DeviceLibrary driver/config                 │
├──────────────────────────────────────────────────────────┤
│ 协议端口：ModbusPort → nanoMODBUS                         │
├──────────────────────────────────────────────────────────┤
│ Transport：UART backend / TCP backend                     │
├──────────────────────────────────────────────────────────┤
│ HAL/BSP：UART DMA、ETH、GPIO、CRC、Flash                  │
└──────────────────────────────────────────────────────────┘
```

这不是完全纯粹的教科书分层，当前至少有两个例外：

1. OTA 直接使用 LwIP raw API 和 HAL Flash/CRC，是独立系统通道；
2. `DeviceLibrary/CoffeeMachine/coffee_machine_modbus.c` 直接依赖 FreeRTOS `vTaskDelay()`，公共设备驱动仍带有 RTOS/阻塞策略。

🟠【问题 P2】第二点会降低设备库在裸机、其他RTOS或不同调度模型中的复用度。它目前不构成功能错误，但与“跨产品公共设备库”的目标存在边界泄漏。

## 12. 产品、设备、协议、传输边界

| 概念 | 当前归属 | 评价 |
|---|---|---|
| 产品流程 | `UserAPP/*/WorkFlow` | 正确，产品差异应停留在这里 |
| 产品动作语义 | Coffee2 Robot点位表、Coffee2Protocol | 基本正确 |
| 设备身份/协议能力 | `DeviceLibrary` | 已开始公共化 |
| 实例route/unit/baud | Coffee2 Device binding | 合理，属于产品布线配置 |
| Modbus帧与错误归一 | `ModbusPort` | 边界清楚 |
| UART/TCP实际收发 | `Transport` backend | 边界清楚 |
| OTA网络与Flash | Coffee2 OTA独立通道 | 可接受，不必强行塞进Modbus Transport |

🔵【事实】当前架构不是“每个设备各自创建一套串口任务”，而是“物理Bus owner + 设备协议adapter”。这对共享串口、时序控制和后续接入自有协议是良性基础。

## 13. 系统启动调用链

```text
Reset_Handler
  → SystemInit
  → 初始化.data/.bss与C运行库
  → main()
     → 中断状态清理
     → HAL_Init()
     → SystemClock_Config()
     → GPIO / DMA / CRC / SPI / UART / RTC 初始化
     → GCC: vAppCcmInit() 清零.ccm_bss
     → Crash Diagnostics 初始化
     → PHY reset
     → HAL_Delay(5000)
     → osKernelInitialize()
     → MX_FREERTOS_Init()
        → xAppTaskManagerCreateTasks()
        → 创建defaultTask
     → osKernelStart()
        → defaultTask
           → MX_LWIP_Init()
           → 产品DefaultTask进入网络状态维护
```

🔵【事实】`main.c:163` 的 5 秒延时发生在调度器启动前，注释目的是避免上电时串口输出干扰。它不是“必须接日志串口才能启动”的等待条件，但会造成固定的 5 秒启动延迟。

🟠【问题 P2】固定延时无法证明UART、PHY或外设真的就绪，只是无条件等待；它会掩盖真实初始化时序，也增加掉电重启恢复时间。

🟠【问题 P1】`freertos.c:111` 忽略 `xAppTaskManagerCreateTasks()` 返回值。Coffee2 Manager 又是逐个创建业务任务，后续创建失败时没有删除已经创建的任务。因此低堆或任务创建失败时，系统可能进入“部分任务已经运行、部分任务缺失”的状态，而不是统一拒绝启动。

## 14. 主运行模型

当前是“FreeRTOS任务 + 中断/DMA + LwIP线程/raw callback + owner状态机”的混合模型，不是超级循环。

### Coffee2任务模型

| 任务 | 优先级关系 | owner资源 | 主要职责 |
|---|---|---|---|
| defaultTask | CMSIS normal | LwIP初始化、网络ready事件 | 网络栈和产品默认循环 |
| C2Server | idle+3 | TCP 6001监听及两个client slot | 上位机寄存器/订单/维护入口 |
| C2Robot | idle+2 | Robot TCP channel/queue/transaction | Robot连接、握手、恢复 |
| C2Bus2~5 | idle+2 | 对应UART/RTU队列 | 串行执行各物理Bus命令 |
| C2Workflow | idle+2 | 订单队列/业务状态机 | 多设备编排 |
| C2Log | idle+2，可选 | UART1日志输出 | 环形日志发送 |

### MilkTea任务模型

MilkTea 会创建 Log、Debug、RobotTcp、MilkTeaTcp、四个RtuBus和Workflow任务。当前四路 RTU 的 `ENABLE` 均为0，但任务仍被创建，随后每秒休眠检查。

🟠【问题 P2】禁用功能仍占用四个任务的栈和TCB。当前不是内存崩溃点，但在 MilkTea 后续重构前应决定“禁用时不创建”还是“统一Bus任务常驻”，避免配置语义和资源行为不一致。

## 15. 命令与状态模型

核心命令对象 `Coffee2Command_t` 携带：commandId、orderId、epoch、timeout、step、action、参数、device、source、retry、flags。

```text
Server / Workflow
       ↓ xCoffee2CommandSubmit()
分配单调递增commandId
       ↓ route选择
Robot queue 或 Bus queue
       ↓ owner执行
vCoffee2DeviceCommandStarted()
       ↓
协议/Transport/硬件
       ↓
vCoffee2DeviceCommandCompleted()
       ↓
DeviceStatus + EventGroup + terminal history
       ↓
Workflow / Server日志与状态投影
```

设备状态、每设备 EventGroup 和 6 项终态历史共同解决“新命令覆盖旧单槽结果”的问题。

🟣【推断 P2】终态 EventGroup bit 在下一条命令开始前可能保留。`xCoffee2DeviceWaitCommand()` 已用 commandId/history 二次核验，不会直接把旧结果认作新结果；但在旧bit已置位、owner尚未清理的短窗口内，等待方可能立即唤醒并重复检查。是否造成可测CPU抖动需要运行时trace确认。

## 16. Coffee2关键TX/RX链路

### 16.1 上位机维护命令 TX

```text
PC Modbus TCP FC06/FC16
 → C2Server socket接收
 → ModbusPort server解析
 → 命令寄存器镜像提交
 → prvEvaluateManual()/prvSubmitManual()
 → Coffee2Command_t
 → Device route
 → Robot owner 或 RTU Bus owner
 → 产品协议adapter
 → ModbusPort client
 → UART/TCP Transport
 → 外部设备
```

### 16.2 RTU RX

```text
UART字节到达
 → HAL_UART_RxCpltCallback（ISR）
 → UART backend静态StreamBuffer
 → ModbusPort等待完整帧
 → nanoMODBUS解析/CRC验证
 → 产品adapter提交设备快照/结果
 → DeviceStatus/EventGroup
 → Workflow决定下一步
```

### 16.3 Robot动作闭环

```text
动作命令
 → 清3121..3139动作区
 → 清对应结果位
 → 目标命令位置1
 → 每100ms读取命令位
 → 命令位回0：判定机器人接单
 → 从此刻开始60s动作计时
 → 轮询对应310x/311x结果位
 → 结果位置1
 → 主控写0并确认回0
 → 命令完成
```

断线时 Robot owner 保存活动事务；重连后根据命令位/结果位进行 reconcile，正常程序生命周期内不直接把活动订单动作丢弃。

### 16.4 IO写入闭环

```text
IO维护写命令
 → Bus5 / IO_OUTPUT
 → FC05写单线圈
 → FC02读48路输入
 → FC01读48路输出
 → 提交完整IO镜像
 → 比较目标输出coil与期望值
 → 结果/日志/上位机状态区
```

⚠️ 此处确认的是IO模块的Modbus coil镜像，不是独立物理反馈触点；不能把它描述成对实际负载通断的电气闭环。

## 17. Server、订单与OTA入口

Server地址空间：

- 命令区：`0x0000~0x00AF`；
- 状态区：`0x1000~0x10AF`；
- 监控区：`0x1100~0x117F`；
- 升级触发：`0x0200~0x0202`；
- IO调试：命令 `0x0084~0x0086`，状态 `0x1084~0x108F`。

OTA不是 Manager 上电立即常驻启动。上位机向 `0x0200` 写1后，Server触发 HTTP 80 服务。HTTP仅接受 `POST /upload.cgi` 的 multipart 数据，part header 必须含 `filename=` 和小写 `.bin`。

🟠【问题 P2】`coffee2_ota_flash.h` 仍声明“Workflow或设备busy会返回 REJECTED”，但当前 `xCoffee2OtaFlashBegin()` 只保留重入保护，已不存在设备忙门槛。该注释/枚举是历史语义残留。

🟠【问题 P2】HTTP错误 `-6` 可覆盖缺少filename、不是`.bin`、part header异常或FlashBegin失败等多类原因。当前已打印有限错误输入，能定位大多数现场问题，但结构化错误码仍不够细。

## 18. 设备与Bus配置

| Bus | 设备 | 串口参数 | Unit/说明 |
|---|---|---|---|
| Bus2 | Coffee | 19200 8N1 | 产品binding定义 |
| Bus3 | Cup/Syrup/Lid | 9600 8N1 | 同一Bus共享统一波特率 |
| Bus4 | Ice/Scale/EnergyMeter | 19200 8N1 | 同一Bus已统一到最大目标波特率 |
| Bus5 | IoInput/IoOutput | 38400 8N1 | 外部IO模块 |

Bus任务只负责通用资源所有权和分派；“按重量落冰”不是Bus原子操作，而是Workflow依次调度称重去皮、阀开、等待、阀关、重新称重和补偿。

## 19. 内存布局与现有产物

### 19.1 Coffee2 Flash布局

```text
0x08000000 ─ Bootloader区域
0x08004000 ─ OTA metadata/config所在sector
0x0800C000 ─ Coffee2 App向量表与代码起点
0x08040000 ─ OTA staging起点
0x08080000 ─ 512KiB Flash结束
```

Coffee2 App可用区：`0x0800C000~0x0803FFFF`，共 `0x34000 = 208 KiB`。

### 19.2 现有GCC产物静态占用

| Target | text | data | bss | 说明 |
|---|---:|---:|---:|---|
| Coffee2-Debug | 164120 | 168 | 142384 | 现有ELF静态值 |
| Coffee2-Release | 197176 | 168 | 142408 | Release体积反而显著更大 |
| MilkTea-Debug | 168920 | 140 | 137476 | 全512KiB Flash布局 |
| MilkTea-Release | 147252 | 136 | 137504 | 正常小于Debug |

Coffee2 Release 的 `text+data≈197344 B`，相对208 KiB App区只剩约15648 B，Flash利用率约92.7%。

🔴【风险 P1】Coffee2当前最紧张的是App Flash，不是RAM。继续加入新设备、Robot2、节卡协议或诊断功能前，必须先解决Release优化参数冲突并用最终双工具链产物重新计量，否则容量决策会建立在错误基线上。

### 19.3 SRAM/CCM

Coffee2 Release 从section看：

- 普通SRAM已链接跨度约101728/131072 B（约77.6%）；
- CCM `.ccm_bss` 约40872/65536 B（约62.4%）；
- 两区合计静态提交约142.6 KiB/192 KiB（约72.5%）。

主要大对象：

| 对象 | 约占用 | 区域/说明 |
|---|---:|---|
| FreeRTOS `ucHeap` | 32768 B | CCM，`configAPPLICATION_ALLOCATED_HEAP=1` |
| LwIP heap | 24595 B | 普通SRAM |
| ETH RX pool | 约18819 B | 普通SRAM，DMA可访问 |
| LwIP PBUF pool | 约18627 B | 普通SRAM |
| Coffee2 Bus contexts | 约6960 B | 静态对象 |
| Crash text/context | 4096 B级 | 诊断缓冲 |
| Log ring | 约3712 B | 32条覆盖式ring |
| OTA HTTP session | 约2836 B | 单会话静态缓冲 |

🔵【事实】CCMRAM仅CPU可访问，UART/ETH DMA缓冲必须留在普通SRAM。当前链接布局遵守了这一限制，不能为了“省普通RAM”把网络或DMA缓冲整体迁入CCM。

## 20. 中断、DMA与Timer

| 中断/回调 | 作用 | 共享对象/后续路径 |
|---|---|---|
| UART RX callback | 每字节接收 | 写入静态StreamBuffer，任务侧组帧 |
| UART TX DMA IRQ | 完成发送 | 释放backend同步状态/信号量 |
| ETH IRQ/DMA | 以太网帧 | ethernetif/LwIP |
| TIM6 | HAL tick | `HAL_TIM_PeriodElapsedCallback` |
| SysTick | FreeRTOS tick | 任务调度 |
| RTC Wakeup | 平台已配置 | ⚪产品业务用途需继续确认 |

UART/DMA/RTC相关IRQ优先级使用5，与 `configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY=5` 的边界一致，允许调用相应 `FromISR` API，但后续新增ISR必须继续遵守该阈值。

第一阶段未发现ISR内直接执行阻塞Modbus、printf或业务状态机的证据；主要工作被转移到owner任务。

## 21. 错误处理、HardFault与Watchdog

当前已实现：

- HardFault/MemManage/BusFault/UsageFault汇编入口；
- CPU/SCB/任务/栈上下文采集；
- FreeRTOS stack overflow、malloc failed、assert hook；
- 致命日志重复输出；
- Robot TCP永久退避重连、活动事务协调；
- RTU命令超时/重试/错误归一；
- 日志串口不可用时业务继续运行，日志ring覆盖最早记录。

当前未实现或未启用：

- IWDG/WWDG运行时监督；
- HardFault上下文跨复位持久保存；
- `Error_Handler()`接入统一Crash Diagnostics；
- 任务管理器失败后的统一回滚/安全停机。

🔴【风险 P1】当前致命异常路径会停在循环中，无运行Watchdog把系统自动拉回；`Error_Handler()`也只是关中断后停机。对需要无人值守长期运行的售卖机，这是可靠性策略缺口，但是否启用IWDG及喂狗责任必须在产品级确定，不能直接在本阶段修改。

## 22. LwIP资源模型

`lwipopts.h` 当前关键值：

- `MEM_SIZE=24576`；
- `MEMP_NUM_TCP_PCB=8`；
- `MEMP_NUM_TCP_PCB_LISTEN=3`；
- `MEMP_NUM_NETCONN=8`；
- `PBUF_POOL_SIZE=12`，每项1536 B；
- `LWIP_STATS=0`。

Coffee2同时可能存在：Server监听PCB、两个上位机连接、Robot连接、OTA监听和上传会话，以及断连过程中的暂态PCB。

🔴【风险 P1】当前单Robot/双Server槽/单OTA组合尚可，但若直接扩展Robot2/Robot3或更多TCP设备，`TCP_PCB=8`必须重新做最坏情况核算。不能只增加任务和实例表，而不调整LwIP池；反复重连时还要考虑TIME_WAIT/暂态占用。

🟠【问题 P2】`LWIP_STATS=0` 使现场无法直接观察memp/pbuf/pcb耗尽，是网络资源类问题定位的盲点。

## 23. 多产品与多实例现状

### 公共能力

- Transport和ModbusPort已支持多个静态channel/port；
- Coffee2每条物理Bus有独立context/queue；
- Device binding以实例记录route/unit/baud/driver/protocol；
- DeviceLibrary中的配置表主要为`const`，RAM增量很小；
- Robot协议范围已能描述3139和3150两种Dobot profile。

### 尚未完成的能力

- Coffee2 Robot task仍是单例，固定一个endpoint、queue、transaction和状态；
- Robot2需要第二个owner实例或参数化同一个任务函数，并需要LwIP/heap/stack核算；
- MilkTea当前没有接入公共DeviceLibrary构建；
- JAKA仅有保留身份，没有可执行协议；
- “同类型设备不同业务语义”仍应由产品点位映射解决，公共库只提供协议地址能力。

🟣【结论】当前架构为“已具备多实例所需的部分底座，但产品运行时仍主要是静态单例”。它适合渐进扩展，不适合宣称已经支持任意数量Robot。

---

# 第三部分：首轮问题定位

## 24. 问题清单

### P1-01 Release优化参数冲突

- 🔵【证据】`gcc-arm-none-eabi.cmake:37` 设置 `-Os -g0`；实际 Release compile command 后面又出现 `-O3`。
- 🟠【问题】最终生效优化与工程作者的显式目标不一致。
- 🔴【影响】Coffee2 Release接近App区上限；不同优化也可能改变栈深、时序和调试复现。
- 🟢【最小建议】下一阶段先只统一Release优化为单一选项，再比较size/map和功能回归；不进行架构重构。

### P1-02 当前Keil链缺少“最新源码重建”证据

- 🔵【证据】Keil工程已列入Coffee2 DeviceLibrary组，但现有Keil产物时间早于最新公共设备库源码。
- 🟠【问题】双工具链配置存在，不等于最新版本双工具链均验证。
- 🔴【影响】ARMCC V5的C语法、section、scatter和链接差异可能到另一台电脑才暴露。
- 🟢【最小建议】获得UV4/ARMCC环境后执行Coffee2/MilkTea Rebuild All并保存map/bin日志。

### P1-03 任务创建失败可能留下部分运行系统

- 🔵【证据】Manager按顺序创建任务；`freertos.c`对返回值使用`(void)`忽略；未见失败时删除已创建任务。
- 🔴【影响】堆不足或创建失败时，Server可能已经启动而Workflow/Bus缺失，形成外部可连接但业务不完整的危险状态。
- 🟢【最小建议】后续先设计明确的“全部业务任务成功才发布SYSTEM_READY”门槛；不必立即引入复杂supervisor。

### P1-04 无运行Watchdog和统一致命错误恢复策略

- 🔵【证据】HAL IWDG/WWDG均禁用；Crash配置不刷新IWDG；fatal和Error_Handler最终循环。
- 🔴【影响】死锁、任务饥饿、总线永久阻塞或HardFault后不会自动恢复。
- 🟢【最小建议】先定义“谁有资格喂狗”和健康条件，再考虑开启IWDG；禁止在多个任务无条件喂狗。

### P1-05 网络资源对多Robot扩展偏紧

- 🔵【证据】TCP PCB仅8；当前已有Server双槽、Robot、OTA等消费者。
- 🔴【影响】新增Robot2/3或TCP设备可能出现偶发connect/accept失败，而RAM表面仍有余量。
- 🟢【最小建议】新增实例前先画最坏PCB/NETCONN/PBUF预算表，再决定池大小。

### P2-01 重复生成目录容易改错文件

- 🔵【证据】`CubeMX_Base/CubeMX_Genarate`存在多套Core/LWIP镜像，实际CMake/Keil使用主目录。
- 🟠【影响】同名文件修改后构建无变化，导致误判。
- 🟢【建议】维护指南明确“活动源码根”，长期再考虑隔离归档；本阶段不删除。

### P2-02 公共咖啡机驱动依赖FreeRTOS

- 🔵【证据】`coffee_machine_modbus.c:12-13`包含FreeRTOS/task，`:96`调用`vTaskDelay()`。
- 🟠【影响】驱动的重试/等待策略与RTOS绑定，降低跨项目复用度，也可能占用Bus owner执行时间。
- 🟢【建议】后续评估把“单次协议动作”与“轮询/延时策略”分开；当前功能稳定前不大改。

### P2-03 MilkTea禁用RTU仍创建任务

- 🔵【证据】四个`APP_MODBUS_RTU_BUSx_ENABLE=0`；Task Manager仍创建四个Bus任务；任务禁用时每秒delay。
- 🟠【影响】无效栈/TCB开销，配置含义不直观。
- 🟢【建议】MilkTea重构时再统一处理，不污染当前Coffee2基线。

### P2-04 MilkTea目前仍是架构骨架

- 🔵【证据】Workflow主要只做`vIOStateUpdate()`；RTU weak poll未形成完整设备业务。
- 🟠【影响】不能用Coffee2的成熟度评价MilkTea Target，也不能假定公共DeviceLibrary已被MilkTea消费。
- 🟢【建议】后续从Coffee2 owner/command/device模型迁移，而不是继续扩展现有weak poll。

### P2-05 OTA头文件语义与实现漂移

- 🔵【证据】头文件仍说明设备busy会REJECTED；实现只检查上传重入。
- 🟠【影响】维护者会根据错误注释判断现场`-3/-6`。
- 🟢【建议】后续文档/注释同步即可，不影响当前OTA主流程。

### P2-06 固定5秒启动延迟

- 🔵【证据】`main.c:163 HAL_Delay(5000)`。
- 🟠【影响】每次复位固定增加恢复时间，且不是实际ready判定。
- 🟢【建议】先验证去除或替换为明确硬件条件的影响，再修改。

### P2-07 IO输出“读回”不是独立物理反馈

- 🔵【证据】写后读取的是IO模块FC01 coil镜像。
- 🔴【影响】上位机看到输出状态正确，不等价于继电器、线缆和负载真实动作。
- 🟢【建议】使用说明中明确边界；需要电气闭环时另接输入反馈，不应在软件中虚构。

## 25. 第一阶段未发现的P0

本轮没有找到可以仅凭静态证据直接定性的P0问题。这里不代表工程“没有Bug”，只代表：

- 未运行硬件；
- 未做动态race、长时间网络、栈高水位和故障注入；
- 未把每个设备协议文档与全部寄存器逐项复核；
- 未对全部第三方代码做审计。

因此，任何现场异常仍必须沿真实日志、任务状态、commandId、Modbus fault和总线帧继续验证。

---

# 第四部分：修改影响与扩展判断

## 26. 修改影响矩阵

| 修改目标 | 主要入口 | 必查下游 | 风险 |
|---|---|---|---|
| 修改上位机寄存器 | `coffee2_server.h/.c` | PC协议、寄存器镜像、Workflow/Device动作 | 高 |
| 修改订单步骤 | `coffee2_workflow.c` | Device命令、取消、超时、安全关闭、Server状态 | 高 |
| 增加RTU设备 | Device binding + Coffee2Protocol | Bus route、Unit/baud、ModbusPort、状态投影 | 中 |
| 增加自有串口协议 | Bus protocol kind + adapter | 同Bus排他owner、UART backend、帧解析 | 中高 |
| 增加机器人语义 | 产品Robot点位表 | 公共protocol profile、Server action、日志 | 中 |
| 增加Robot2 | Robot实例/owner | LwIP PCB、队列、栈、DeviceId、状态区、Workflow | 高 |
| 增加Robot品牌 | DeviceLibrary identity/protocol + 产品adapter | Transport与产品动作语义 | 中高 |
| 修改UART/DMA | Transport UART backend/BSP | 所有RTU设备、日志、ISR优先级、CCM放置 | 高 |
| 修改OTA分区 | linker/scatter/Bootloader ABI | VTOR、bin大小、metadata、两工具链 | 极高 |
| 增加新产品Target | CMake/uvprojx/AppConfig | source set、宏、linker、manager、memory | 高 |

## 27. 当前扩展能力判断

### 增加新RTU设备

当前路径已经比较清楚：Device identity/config → Coffee2 binding → 产品protocol adapter → 现有Bus owner。无需为每个设备再创建任务。

### 增加同Bus自有协议设备

应保留同一个Bus owner，由绑定配置选择“Modbus RTU或自有协议adapter”。不能同时让一个新的自有协议任务直接操作同一UART，否则破坏owner规则。

### 增加同类型不同语义Robot

公共DeviceLibrary提供地址范围和协议能力；Coffee2/MilkTea各自在产品点位表中把“去制冰机/去奶茶机”映射到相应地址。不要创建“越疆协议1/2”只为表达产品业务名称差异。

### 增加第二台Robot

架构方向可用，但当前不能只复制配置：还需要真正的第二实例context、queue/owner、endpoint、DeviceId/Role、状态投影和Workflow选择，并重新核算LwIP/heap/stack。静态配置表本身只增加很少RAM，真正的资源开销来自任务栈、TCP PCB、队列和事务状态。

---

# 第五部分：当前架构评价

## 28. 分项评分（第一阶段）

| 维度 | 评分/10 | 依据 |
|---|---:|---|
| 架构清晰度 | 7 | Coffee2 owner/Device/Protocol/Transport边界较清楚，但重复目录和历史代码增加认知成本 |
| 模块化 | 7 | Bus/Robot/Server/Workflow拆分合理，OTA独立；部分公共driver仍含RTOS策略 |
| 低耦合 | 6 | Transport/ModbusPort良好；产品协议与公共设备库仍在迁移期 |
| 可读性 | 6 | 命名和日志较完整，但状态机大、同名镜像多 |
| 扩展性 | 6 | 配置与profile底座存在，真实多Robot/多产品消费尚未完成 |
| 可测试性 | 5 | 日志/仿真变量丰富，但缺Host单测、LwIP stats和系统性故障注入 |
| 多实例能力 | 5 | RTU Bus多实例已成立；Robot和产品状态仍单例 |
| 设备抽象能力 | 6 | DeviceLibrary方向正确，当前覆盖和边界仍不完整 |
| 协议抽象能力 | 7 | ModbusPort+nanoMODBUS+adapter分层清晰 |
| 通信抽象能力 | 7 | UART/TCP backend统一；OTA为有理由的旁路 |
| 内存安全 | 7 | 以静态对象为主、CCM/DMA区分正确；Flash余量紧张 |
| 实时性 | 7 | owner任务和ISR轻量；部分阻塞driver/固定延时仍需测量 |
| 可靠性 | 5 | 重试/重连/Crash较强，但无Watchdog、部分启动风险未闭环 |
| 异常恢复能力 | 6 | Robot恢复设计较完整；fatal和系统级任务缺失不能自动恢复 |
| 多产品维护能力 | 5 | 双Target存在，但Coffee2成熟、MilkTea骨架，公共库尚未贯通 |

## 29. 当前设计中不应随意修改的部分

1. 一条物理Bus一个owner任务；
2. Workflow负责组合动作，Bus只负责传输和协议分派；
3. Device命令ID、EventGroup和终态历史组成的异步完成模型；
4. Robot命令位接单、结果位完成、清结果确认的闭环；
5. Protocol与Transport分离，ModbusPort统一错误和预算；
6. DMA缓冲留在普通SRAM、CPU-only状态放CCM；
7. OTA流式写Flash，不把完整固件放入RAM；
8. 产品业务语义留在产品层，公共设备库只描述设备/协议能力。

## 30. 当前最优先的3~5个问题

按“最小改动、先修证据基础”的顺序：

1. 统一Release优化参数并重新量化Coffee2 208KiB App余量；
2. 用当前源码完成Keil V5双Target重建，恢复双工具链一致性证据；
3. 明确任务创建失败时的系统ready门槛，避免部分启动；
4. 为Watchdog建立产品级健康条件和唯一喂狗策略；
5. 在扩展Robot2/更多TCP实例前完成LwIP最坏资源预算。

---

# 第六部分：下一阶段只读分析计划

## 31. 深挖顺序

后续仍不修改源码时，建议按以下真实链路逐一闭环：

1. Server FC06/FC16收包、寄存器写提交和命令重复触发规则；
2. `Coffee2Command_t`从提交、抢占、队列、started、terminal history到wait的全部竞争窗口；
3. Robot action FSM每个phase、断线重连、手动抢占与订单动作的状态转换表；
4. Workflow订单每一步的前置、命令、等待、超时、取消和安全收尾；
5. Bus2~5每个设备adapter的TX/RX寄存器与协议文档逐项对照；
6. 所有全局/静态状态的读写者、ISR共享关系和临界区；
7. 每个任务栈高水位、FreeRTOS heap峰值、LwIP pool峰值和长期运行证据；
8. Keil/GCC map逐section、逐大符号和目标差异；
9. Error_Handler、Fault、Watchdog和掉电/复位恢复边界；
10. 新设备库对Coffee2的实际消费点与MilkTea未来迁移边界。

## 32. 第一阶段完成度

- [x] 全 Workspace 文件类型与目录扫描
- [x] 真实活动源与历史/镜像源区分
- [x] MCU、外设、RTOS、网络与双工具链识别
- [x] Coffee2/MilkTea四Target识别
- [x] 启动链与主任务模型建立
- [x] Device/Protocol/Transport/Bus/Workflow边界建立
- [x] 关键TX/RX/Robot/IO/OTA链路建立
- [x] Flash/SRAM/CCM与主要大对象初步核算
- [x] IRQ/DMA/Timer/Crash/Watchdog初步分析
- [x] 多实例和公共设备库现状判断
- [x] 首轮P1/P2问题定位和修改影响矩阵
- [ ] 全部设备寄存器逐项对照协议文档
- [ ] 全部状态机逐状态转换表
- [ ] 全局变量逐对象读写矩阵
- [ ] 硬件运行时栈/堆/LwIP峰值测量
- [ ] 双工具链当前源码重建证据
- [ ] 实机异常注入与恢复验证

## 33. 第一阶段结论

当前工程属于：

> **以FreeRTOS owner任务为运行骨架、以Device命令/状态为异步协作中枢、以产品Workflow组织业务、以产品Protocol adapter连接公共DeviceLibrary与ModbusPort/Transport、同时支持Coffee2和MilkTea两个静态Target的嵌入式多产品架构。**

Coffee2已经形成较完整的运行闭环，MilkTea仍是待迁移的产品骨架。工程最值得保留的是Bus所有权、命令终态、Robot握手恢复、Transport/ModbusPort边界和OTA流式设计；最需要优先处理的是Release构建一致性、Keil验证缺口、部分启动、Watchdog策略和多TCP实例资源预算。

本阶段没有执行重构，也没有把“理论上更优雅”当成必须修改项。后续所有结论仍应继续沿真实函数、真实数据对象、真实Target和真实运行证据验证。
