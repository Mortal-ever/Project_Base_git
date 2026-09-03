# 00_Coffee2 工程架构分析

> 文档类型：只读人工 Review 主文档（与四份分层源码带读配套）
> 审查日期：2026-09-02
> 仓库根：本次以实际工作区 `C:\Users\13193\Desktop\Project_Base` 为准
>（`AGENTS.md:9` 记录的 `D:\Project_Items\Project_Base` 在当前环境中不存在，记为
> `CONFLICT`，不影响源码与构建解读；详见 §1.3）
> 适用范围：仅 Coffee2。MilkTea 一律 `OUT_OF_SCOPE`，不作为公共架构或兼容性证据。

## 目录

- [1. 工程适用范围](#1-工程适用范围)
- [2. 工程技术栈](#2-工程技术栈)
- [3. 四层架构](#3-四层架构)
- [4. 为什么是四层（为什么没有第五层）](#4-为什么是四层为什么没有第五层)
- [5. GCC 工具链](#5-gcc-工具链)
- [6. Keil 工具链](#6-keil-工具链)
- [7. 双工具链对照](#7-双工具链对照)
- [8. 当前设计哲学](#8-当前设计哲学)
- [9. 系统关系图](#9-系统关系图)
- [10. 人工 Review 推荐顺序](#10-人工-review-推荐顺序)
- [11. 行号快照与状态标签说明](#11-行号快照与状态标签说明)
- [12. 相关文档](#12-相关文档)

---

## 1. 工程适用范围

### 1.1 产品范围

- 本架构分析只覆盖当前 Coffee2 固件（产品宏 `USE_COFFEE2`）。
- 仓库中残留的 `Application/UserAPP/MilkTeaApp/*` 属于待后续独立重写的遗留资料：
  不进入 GCC preset、Keil target 或公共层验证（`当前工程架构与公共私有边界.md:7-8`），
  也不反向定义本架构。

### 1.2 审查日期

2026-09-02。所有“当前行号”均为当日读取快照。

### 1.3 Git 状态与根路径说明

- Git 状态：`UNKNOWN`。仓库存在 `.git` 目录（2026-07-30 创建），但只有 `info`
  子目录，无 `HEAD`/`objects`/`refs`；`git status`/`git log` 均返回
  `fatal: not a git repository`。无法用 Git 核对提交/未提交差异，审查以文件系统实测为准。
- 根路径：环境实际工作区为 `C:\Users\13193\Desktop\Project_Base`；
  `D:\Project_Items\Project_Base` 不存在。`AGENTS.md:9`、`工程基础缓存.md:11`、
  `全局审查.md:25` 中写死的 D 盘根路径在本环境不成立，记录为 `CONFLICT`（文档路径陈旧），
  本轮所有相对路径均以 C 盘工作区为准。
- `全局审查.md:25` 声称“桌面路径不存在、以 D 盘为准”，与当前实测相反，同样归为陈旧
  环境证据（`HISTORICAL`/`CONFLICT`）。

### 1.4 未提交修改说明

- 无法从 Git 判定未提交差异。为控制风险，本次静态验证以 2026-09-02 16:39 的 GCC
  Coffee2-Debug 完整产物（`GCC-ARM/build/Coffee2-Debug/Coffee2Target.{elf,map,hex,bin}`）
  为“构建图与源码一致”的主证据，未重新构建。
- 本任务产生的修改仅为：本目录 5 份解读文档 + 根 `CHANGES.md` +
  `资料文档/00_README/工程基础缓存.md` + `资料文档/全局审查.md` 的文档索引更新。
- 确认未修改任何 `.c/.h/.s/.ioc/.uvprojx/.uvoptx/CMakeLists/链接脚本`。

---

## 2. 工程技术栈

以下条目均由当前源码/工程配置证实：

| 项 | 事实 | 证据（当前文件与行号） |
| --- | --- | --- |
| MCU | STM32F407 系列 Cortex-M4F，168 MHz | `CubeMX_Base/Core/Src/main.c:203-229`（PLLM4/PLLN168/PLLP2）；编译宏 `STM32F407xx`（`GCC-ARM/cmake/stm32cubemx/CMakeLists.txt:7`） |
| MCU 型号冲突 | Keil `Device=STM32F407VGTx`；两份维护 IOC `ProjectManager.DeviceId=STM32F407VETx` | `MDK-ARM/STM32F407_Base.uvprojx:26`；`CubeMX_Base/CubeMX_Genarate/CMAKE/F407Base_CMAKE.ioc:441`、`CubeMX_Base/CubeMX_Genarate/MDK/F407Base_MDK.ioc:441`（详见 §7） |
| HAL/LL | STM32F4xx HAL（LL 用于崩溃输出端口） | `CubeMX_Base/Drivers/STM32F4xx_HAL_Driver`；`coffee2_crash_log_port.c:14` |
| FreeRTOS | V10.3.1 + CMSIS-RTOS V2 | `CubeMX_Base/Core/Inc/FreeRTOSConfig.h:3,21` |
| lwIP | 2.1.2（CubeMX 版本注释） | `CubeMX_Base/LWIP/Target/lwipopts.h:28` |
| nanoMODBUS | v1.23.0（revision 91d6782）+ 5 条 Coffee2 本地补丁 | `Application/New_Party/nanoMODBUS/UPSTREAM_VERSION.md:1-15` |
| Modbus RTU | Bus3/4/5 从站设备经 nanoMODBUS+ModbusPort | `coffee2_app_config.h:66-76`；`coffee2_rtu_bus.c:241-258` |
| Modbus TCP | Server（端口 6001，Unit 1）与 Robot Client（192.168.5.1:502，Unit 1） | `coffee2_app_config.h:36-48` |
| Robot TCP | Dobot 命令/结果线圈点表，Protocol 1 活动 | `coffee2_app_config.h:60-63`；`dobot_robot_device.h:35-75`；`coffee2_robot_tcp.c:950-968` |
| OTA | 公共 Flash 分段写 + HTTP 上传；Coffee2 分区绑定 | `Application/Common/Ota/app_ota_flash.c/.h`、`app_ota_http.c/.h`；`coffee2_ota.c:20-26` |
| Bootloader 协作 | 元数据 `0x08004000`、应用 `0x0800C000`、staging `0x08060000-0x080C0000`；Bootloader 本体无仓库源码 | `coffee2_ota.h:17-24`；Bootloader 行为 `UNKNOWN` |
| ARM Compiler | ARM Compiler V5.06 update 7（build 960） | `MDK-ARM/STM32F407_Base.uvprojx:13-15`（pArmCC 5060960） |
| GCC ARM | arm-none-eabi-gcc（本地 .tools 工具链 15.2.1，map 可见） | `GCC-ARM/cmake/gcc-arm-none-eabi.cmake:8-12`；`Coffee2Target.map` |
| 公共日志 | 32 条 overwrite ring + USART1 输出 | `Application/Common/Log/app_log.c:24-37`；`coffee2_log.c:25-45` |
| 崩溃诊断 | 汇编入口 + 调度器无关输出；弱写接口由 Coffee2 强实现 | `app_crash_diag.c:53-59`；`coffee2_crash_log_port.c:63-66` |

---

## 3. 四层架构

依赖方向（`当前工程架构与公共私有边界.md:44-49`）：

```text
Target App(L4)  ->  DeviceLibrary(L3)  ->  ModbusPort(L2)  ->  Transport(L2)  ->  L1 平台
Target App(L4)  ->  Common(L2)（日志/OTA/TCP 会话/诊断/LwipAlert）
```

公共层禁止反向依赖任何 `UserAPP/<Target>`；唯一受控例外是启动组合根
`Application/Common/CommonTargets.h`（§3.5）。

### 3.1 L1 平台/生成层

| 项 | 内容 |
| --- | --- |
| 物理目录 | `CubeMX_Base/Core`、`Drivers`、`Middlewares`、`LWIP`；GCC `GCC-ARM/startup_stm32f407xx.s`，Keil `MDK-ARM/startup_stm32f407xx.s`；GCC `GCC-ARM/linker/STM32F407XX_COFFEE2_OTA_FLASH.ld`；Keil `MDK-ARM/ScatterFiles/Coffee2_CCM.sct` |
| 逻辑职责 | MCU 启动（startup→main）、时钟、中断向量与 `stm32f4xx_it.c`、HAL 外设初始化（GPIO/UART/SPI/ETH/RTC/CRC/DMA/TIM6 时基）、FreeRTOS（V10.3.1+CMSIS-RTOS V2）、lwIP（2.1.2 含 `ethernetif.c` 与 `sys_arch.c`）、平台启动入口 `freertos.c` |
| 公共/私有 | CubeMX 生成物；维护输入 `CubeMX_Base/CubeMX_Genarate/{CMAKE,MDK}/*.ioc` 只读 |
| 对外接口 | `main.h`（引脚宏）、`usart.h`（`huart1..6`）、HAL/LL API、`lwip.h`、FreeRTOS/cmsis_os |
| 允许依赖 | 无（最底层） |
| 禁止依赖 | 不得 include `Application/UserAPP/**`；只有 `freertos.c` 允许 include `CommonTargets.h`（`freertos.c:28`） |
| 状态所有者 | `main.c` 启动顺序、复位原因、CubeMX 句柄 |
| 任务所有者 | `defaultTask`（`StartDefaultTask`，栈 1024 B）、lwIP `EthLink` 由生成代码创建；业务任务全部由 L4 创建 |
| 硬件/总线所有者 | MCU 与全部片上外设的 HAL 所有权 |

详细带读：`01_L1平台与生成层源码带读.md`。

### 3.2 L2 公共基础层

| 项 | 内容 |
| --- | --- |
| 物理目录 | `Application/Common`（Log、Diagnostics、LwipAlert、Ota、TcpClientSession、`CommonTargets.h`、`compiler_compat.h`）、`Application/Transport`、`Application/ProtocolStack/ModbusPort`、`Application/New_Party/nanoMODBUS` |
| 逻辑职责 | 异步日志核心、崩溃诊断核心、lwIP 资源告警、公共 OTA Flash/HTTP、TCP 客户端会话状态机、UART/TCP 字节传输、Modbus 端口适配、nanoMODBUS 协议栈 |
| 公共/私有 | 全部公共（target-neutral）；CMake 目标在产品分支前声明（`Application/CMakeLists.txt:30-148`）；Keil 公共组只出现一次 |
| 对外接口 | `Log/app_log.h`、`Diagnostics/Inc/app_crash_diag.h`、`LwipAlert/app_lwip_alert.h`、`Ota/app_ota_*.h`、`TcpClientSession/tcp_client_session.h`、`transport*.h`、`modbus_port.h`、`nanomodbus.h` |
| 允许依赖 | 相互依赖 + L1（HAL/FreeRTOS/lwIP） |
| 禁止依赖 | 不得 include `Application/UserAPP/**`；不得出现产品枚举/寄存器/物理总线绑定（`AGENTS.md:45-47`） |
| 状态所有者 | `g_xAppLogStatus`（`app_log.c:55`）、崩溃任务登记表与寄存器快照（`app_crash_diag.c:42-64`）、Transport 注册表、调用者持有的 ModbusPort 实例 |
| 任务所有者 | 无；日志输出任务由产品适配创建（`vCoffee2LogTask`→`vAppLogTask`） |
| 硬件/总线所有者 | 无；UART/TCP 端点由 owner 传入 |

详细带读：`02_L2公共基础层源码带读.md`。

### 3.3 L3 公共设备库层

| 项 | 内容 |
| --- | --- |
| 物理目录 | `Application/DeviceLibrary`：`Robot/Dobot`、`CoffeeMachine/{f200,O,X,m50}`、`CupLidController/ShengShu`、`SyrupMachine/CurrentModbus`、`IceMachine/CurrentModbus`、`Scale/BSQ_DG_V2`、`PowerMeter/DDSU666`、`IoModule/ModbusDigitalIo`、`Inc/device_library.h` |
| 逻辑职责 | 设备原生请求构造、响应解析、寄存器/协议点表、设备上下文/镜像、设备级状态、设备级错误返回 |
| 公共/私有 | 公共；`device_*` CMake 目标在产品分支前（`Application/CMakeLists.txt:74-127`）；Keil 公共 `DeviceLibrary` 组一次 |
| 对外接口 | `device_library.h` 身份枚举 + 各设备头；驱动只接收上下文/传输参数并返回设备结果（`AGENTS.md:46-47`） |
| 允许依赖 | `modbus_port.h`/`transport.h`/`device_library.h` + L1 |
| 禁止依赖 | 禁止 include Coffee2 状态/命令/image 头文件（`coffee2_device_image.h` 是 Coffee2 私有） |
| 状态所有者 | 无产品状态；镜像由调用方持有（如 `g_xCoffee2SyrupImage`）或函数出参 |
| 任务所有者 | 无；由 owner 任务串行调用 |
| 硬件/总线所有者 | 无；由调用方传入 `ModbusPort_t*`/`TransportChannel_t*` |

> F200 使用自有串口帧协议、不是 Modbus，但“公共”由无 target 语义决定而非协议类型
> （`AGENTS.md:33-34`）。

详细带读：`03_L3公共设备库层源码带读.md`。

### 3.4 L4 Coffee2 Target 私有应用层

| 项 | 内容 |
| --- | --- |
| 物理目录 | `Application/UserAPP/Coffee2App`：Config、Task_Manager、WorkFlow、Modbus_Rtu_Bus、Robot_Tcp、Modbus_Tcp_Server、Device、IO_State、Ota、Comm_Log |
| 逻辑职责 | Coffee2 业务/Workflow、Coffee2 命令体系、任务与队列、设备型号选择、UART/从站/IP 绑定、物理总线 owner、Robot TCP owner、Modbus TCP Server 寄存器语义、`coffee2_device_image`、状态投影、OTA 编排、日志 source 与崩溃日志桥接 |
| 公共/私有 | Coffee2 私有；GCC `coffee2_app` 目标（`Application/CMakeLists.txt:153-183`）；Keil `Application/Coffee2App` 组 |
| 对外接口 | 对上位机为 Modbus TCP 寄存器；对启动根为 `xAppTaskManagerCreateTasks/vAppTaskManagerRunDefaultTask`（`coffee2_manager.h:68-74`，经 CommonTargets 选择） |
| 允许依赖 | 公共层 + L1 |
| 禁止依赖 | 公共层不得反向 include 本层任何头文件 |
| 状态所有者 | `g_axCoffee2DeviceStatus`、`g_xCoffee2WorkflowStatus`、`g_xCoffee2ServerStatus`、`g_xCoffee2RobotTcpStatus`、`g_xCoffee2Io`、各 `g_xCoffee2*Image` |
| 任务所有者 | C2Server/C2Robot/C2Bus2-5/C2Workflow/C2Log（`coffee2_manager.c` 任务创建区） |
| 硬件/总线所有者 | Bus2=UART2/F200、Bus3=UART3/RTU、Bus4=UART4/RTU、Bus5=UART5/RTU；Robot=TCP:502；Server=TCP:6001；日志=USART1 |

详细带读：`04_L4_Coffee2私有应用层源码带读.md`。

### 3.5 CommonTargets 受控例外

- `Application/Common/CommonTargets.h` 物理在 Common，语义不是可复用公共 API
  （`AGENTS.md:41-44`）；按 `USE_COFFEE2` include `coffee2_manager.h`
  （`CommonTargets.h:26-28`）。
- 只有生成的 `freertos.c` include 它（`freertos.c:28`）；GCC 将该组合根 include 限定为
  产品 executable PRIVATE（`GCC-ARM/CMakeLists.txt:62-64`）；Keil 顶层 IncludePath 同时
  列出公共与 Coffee2 私有目录是 target 级属性，不改变源码边界。

---

## 4. 为什么是四层（为什么没有第五层）

### 4.1 DeviceProtocol 已删除，不属于当前架构

- 目录 `Application/DeviceProtocol` 与 `Application/Common/Command`（旧 `app_command.h`）
  在当前仓库都不存在（实测确认）。
- `当前工程架构与公共私有边界.md:20-22`：“原目录中的文件只是把产品命令转成设备库调用的
  薄转发层，且夹带 Coffee2 的动作枚举、状态镜像或站点点表……已删除”。
- Keil uvprojx 活动 Groups（`uvprojx:384-1486`）无 DeviceProtocol 组；GCC
  `Application/CMakeLists.txt` 无对应目标。
- `CHANGES.md` 中的 `Application/DeviceProtocol/*` 路径只表示当时构建记录
  （`AGENTS.md:34-37`），不得据此判断当前仍存在该层。

### 4.2 旧 DeviceProtocol 没有独立复用价值

旧 DeviceProtocol 是“Coffee2 命令 → DeviceLibrary API”的薄转发：输入是产品私有命令枚举
（`Coffee2Action_e`/`Coffee2Command_t`），输出是设备库调用。它无独立状态、无独立协议
语义，等于在 L4 与 L3 之间复制调用关系，其他 target 不能无修改复用，不满足层级成立条件
（独立契约、所有权、复用边界）。

### 4.3 设备原生协议归属 DeviceLibrary

设备原生协议（F200 26 字节帧、Dobot 线圈点表、各 Modbus 寄存器/地址/校验与解析）已内聚
在各设备目录（`coffee_machine_f200.c`、`dobot_robot_device.h`、`*_modbus.h` 等），
不带产品命令语义，无需再设一层协议目录。

### 4.4 Modbus 协议栈与传输适配属于 L2

nanoMODBUS（帧编解码）、ModbusPort（把协议栈接到 Transport）、Transport（UART/TCP 字节
收发）三者属于同一水平职责——“协议与传输基础设施”，只是纵向调用深度大：
`L3 -> ModbusPort -> Transport -> L1`。**调用深度不等于层级数量**。

### 4.5 产品命令归属 L4

命令是业务输入：枚举/参数布局、队列、超时/重试/取消、order/epoch 关联、命令→设备库 API
映射、结果→状态/寄存器投影都由 Coffee2 私有定义（`coffee2_device.h:56-126`、
`coffee2_device.c` 绑定表）。不形成公共层。

### 4.6 没有职责区满足“第五层”成立条件

- 把 DeviceProtocol 当作“第五层”：输入输出都依赖 Coffee2 命令，无法公共化。
- 把“Modbus 家族”（nanoMODBUS+ModbusPort+Transport）拆出：与 Common/日志/OTA 无水平
  边界，只是 L2 内部深度。
- 把 Robot TCP/Dobot 点表拆出：其动作枚举与 owner 状态机在 `coffee2_robot_tcp.c`
  （Coffee2 私有），属于 L4。

### 4.7 目录数量与协议种类不等于层级数量

11 个 Coffee2 子目录、10+ 设备目录、4 条 UART 总线 + 2 个 TCP 端点只是目录/任务维度的
组织；架构层级回答的是“可复用契约与 owner”，当前所有职责都归入 L1-L4。

### 4.8 当前依赖关系证明四层足够

- 编译期：公共目标在产品分支前声明，`coffee2_app` 只 include 私有目录
  （`Application/CMakeLists.txt:30-183`）。
- 源码期：公共层头文件不 include 任何 `UserAPP` 头文件；Coffee2 私有头只被 Coffee2App
  与生成启动根引用（本 review 逐一确认）。
- Keil：Coffee2 源只在 `Application/Coffee2App` 组出现。
- 结论：四层 + 受控启动组合例外已闭合并覆盖当前全部真实调用关系，不重建第五层。

---

## 5. GCC 工具链

### 5.1 Preset 与 CMake 入口

- `GCC-ARM/CMakePresets.json`：version 3；隐藏 `base`（generator=Ninja、
  binaryDir=`build/${presetName}`、toolchain=`cmake/gcc-arm-none-eabi.cmake`）
  （`CMakePresets.json:4-13`）。只暴露 `Coffee2-Debug`/`Coffee2-Release` 两个 preset，
  均设 `PRODUCT_NAME=Coffee2`（`:15-29`）。
- `GCC-ARM/CMakeLists.txt`：`PRODUCT_NAME` 决定 `FIRMWARE_TARGET=Coffee2Target` 与
  `PRODUCT_DEFINE=USE_COFFEE2=1`（`:12-20`）；随后加入 `Platform/app_ccm.c`、
  `add_subdirectory(cmake/stm32cubemx)`、nanoMODBUS、`Application`（`:37-72`）。
- `gcc-arm-none-eabi.cmake`：Cortex-M4F（`-mfpu=fpv4-sp-d16 -mfloat-abi=hard`）；
  Coffee2 Debug `-Og -g3`、Release `-Os -g0`（`:19-38`）。

### 5.2 Application 目标

- `Application/CMakeLists.txt`：`product_interfaces` INTERFACE 只导出公共 include
  （`:21-28`）；公共 `app_*`/`device_*` OBJECT 目标在产品分支前（`:30-148`）；
  `coffee2_app` 仅在 `PRODUCT_NAME STREQUAL "Coffee2"` 分支（`:150-189`）。
- `PRODUCT_LINK_TARGETS = ${APP_PUBLIC_TARGETS} coffee2_app`（`:185-189`），
  由顶层 `target_link_libraries(${FIRMWARE_TARGET} stm32cubemx nanomodbus
  ${PRODUCT_LINK_TARGETS})`（`GCC-ARM/CMakeLists.txt:88-92`）组合。
- 公共目标共 18 个：`app_diagnostics/app_transport/app_modbus_port/app_log/
  app_lwip_alert/app_ota/app_tcp_client_session/device_dobot/device_f200/
  device_coffee_o/device_coffee_x/device_coffee_m50/device_cup_lid/device_syrup/
  device_ice/device_scale/device_power_meter/device_io`（`:129-148`）。

> 注意：`device_coffee_o/x/m50` 出现在 CMake 构建图（对象目录存在），但当前 GCC
> 链接产物中这些对象的代码段为 0、被 `--gc-sections` 丢弃（map 仅剩 debug 段），
> 净效果与 Keil `IncludeInBuild=0` 一致；但“GCC 完全不编译 O/X/M50”的旧表述与
> 当前 CMake 图不完全一致（P2 CONFLICT，见 §7 与 03 文档问题清单）。

### 5.3 PUBLIC/PRIVATE include 与编译宏

- `product_interfaces` 导出：Common、Transport、ModbusPort、nanoMODBUS、DeviceLibrary
  全部公共路径（`Application/CMakeLists.txt:1-19`）。
- 平台层：顶层给 stm32cubemx INTERFACE 加 `Application/Common`、
  `Common/Diagnostics/{Inc,Config}`、`GCC-ARM/Platform`（`GCC-ARM/CMakeLists.txt:51-56`）。
- `coffee2_app` PRIVATE include 指向 Coffee2App 10 个子目录（`:168-179`），不传播。
- 组合根 include：`.../Coffee2App/Task_Manager` 作为 executable PRIVATE
  （`GCC-ARM/CMakeLists.txt:62-64`）。
- 编译宏：`USE_HAL_DRIVER STM32F407xx $<$<CONFIG:Debug>:DEBUG>`
  （`cmake/stm32cubemx/CMakeLists.txt:5-9`）+ `USE_COFFEE2=1` + Coffee2 专属
  `USER_VECT_TAB_ADDRESS VECT_TAB_OFFSET=0x0000C000U`（`GCC-ARM/CMakeLists.txt:75-87`）。

### 5.4 链接脚本、OTA 布局与产物

- Coffee2 链接脚本：`GCC-ARM/linker/STM32F407XX_COFFEE2_OTA_FLASH.ld`
  （选用于 `GCC-ARM/CMakeLists.txt:105-114`）。FLASH `0x0800C000` 长 `0x54000`；
  RAM `0x20000000` 128K；CCM `0x10000000` 64K（`:56-61`）。`.ccm_bss` 段在 CCM
  （`:245-253`），由 `Platform/app_ccm.c` `vAppCcmInit()` 在 main 中清零。
- 中断向量偏移 `VECT_TAB_OFFSET=0x0000C000U` 与 OTA 应用地址 `0x0800C000` 一致。
- 产物：`build/<Preset>/Coffee2Target.{elf,map,hex,bin}`，POST_BUILD objcopy+size
  （`GCC-ARM/CMakeLists.txt:116-125`）。
- 实测 Debug 产物（2026-09-02 16:39）：`text 174960 / data 168 / bss 150656`
  （arm-none-eabi-size）；BIN 175,136 B；map 显示 `vCoffee2RtuBusTask @ 0x08024c10`、
  nanomodbus 与 `coffee2_*` 对象存在；O/X/M50 仅剩 0 尺寸调试段。
  Release 目录未发现同日完整产物（`UNKNOWN`）。
- 脚本：`GCC-ARM/scripts/build.ps1`（配置+构建）、`flash.ps1`（OpenOCD/ST-LINK，
  默认只预检，需 `-Program` 才烧录）、`setup_toolchain.ps1`、`clean_intermediates.ps1`。
- 本轮未执行完整重新构建，也未烧录。

---

## 6. Keil 工具链

### 6.1 Coffee2 Target 与编译器

- `MDK-ARM/STM32F407_Base.uvprojx` 只有一个 Target：`Coffee2`（`uvprojx:10`）；
  `uAC6=0`；`pArmCC=5060960::V5.06 update 7 (build 960)::.\ARM_Compiler_5.06u7`
  （`uvprojx:13-15`）；`Device=STM32F407VGTx`（`uvprojx:26`）。
- 输出：`.\Objects\Coffee2\`、`OutputName=Coffee2`、`CreateHexFile=1`
  （`uvprojx:87-91`）。

### 6.2 编译选项（target 级）

- `MiscControls=--preinclude ..\Application\UserAPP\Coffee2App\Config\coffee2_build_config.h`
  （`uvprojx:341`）——每个 TU 先获得 nanoMODBUS 配置
  （`coffee2_build_config.h:16-22`）。
- `Define=USE_HAL_DRIVER,STM32F407xx,USE_COFFEE2,USER_VECT_TAB_ADDRESS,VECT_TAB_OFFSET=0x0000C000U`
  （`uvprojx:342`）。
- IncludePath 为 target 级：公共目录 + Coffee2 私有 10 子目录 + CubeMX 全部目录
  （`uvprojx:344`）。
- target Cads：`Optim=1`（-O1）、`uC99=1`、`uGnu=1`（`uvprojx:318,330-331`）。

### 6.3 Keil Group 与 IncludeInBuild

- 组（`uvprojx`）：`Application/MDK-ARM`（startup，386）、`Application/User/Core`
  （396：main/gpio/freertos/crc/dma/rtc/spi/usart/stm32f4xx_it/hal_msp/timebase）、
  `Application/User/LWIP/Target`（507：ethernetif.c）、`Application/User/Diagnostics`
  （517：app_crash_diag.c + app_crash_fault_armcc.s）、`Application/Common`
  （532：app_log/lwip_alert/ota_flash/ota_http/tcp_client_session）、
  `Application/Coffee2App`（562：Coffee2 12 源，含 `coffee2_device_image.c`、
  `coffee2_ota.c`）、`Application/DeviceLibrary`（627：公共设备源；
  `coffee_machine_O.c:650`、`coffee_machine_X.c:706`、`coffee_machine_m50.c:762`
  为 `IncludeInBuild=0`）、`Application/Transport`（840）、
  `Application/ProtocolStack/ModbusPort`（860）、`Application/New_Party/nanoMODBUS`
  （870）、`Application/User/LWIP/App`（880：lwip.c）、`Drivers/BSP/Components`
  （890：dp83848.c）、`Drivers/STM32F4xx_HAL_Driver`（900）、`Drivers/CMSIS`（1010）、
  `Middlewares/FreeRTOS`（1020）、`Middlewares/LwIP`（1075）、`::CMSIS`（RTE，1485）。
- 无 DeviceProtocol 组、无 MilkTeaApp 组；MilkTea 只在 `Objects/` 遗留目录与旧
  `uvguix`/`ScatterFiles/MilkTea_CCM.sct` 中作为历史残留存在。

### 6.4 Scatter、CCM、RTE 与输出

- ScatterFile：`.\ScatterFiles\Coffee2_CCM.sct`（`uvprojx:375`）：
  `LR_IROM1 0x0800C000 0x54000`；
  `RW_CCM 0x10000000 0x10000`（`coffee2_manager.o(CCM_HEAP)` + `*.o(CCM_APP)`）；
  `RW_IRAM2 0x2001C000 UNINIT 0x4000`（startup STACK）；
  `RW_IRAM1 0x20000000 0x1C000`（`.ANY +RW +ZI`）（`Coffee2_CCM.sct:5-24`）。
- ARMCC 段属性来源：`compiler_compat.h:14-18`（`CCM_APP`/`CCM_HEAP`）与
  `coffee2_manager.c:32-35`（CCM 堆 `ucHeap`）。
- RTE：`<targetInfo name="Coffee2"/>`（`uvprojx:1497`）。
- Debug：`SARMCM3.DLL` + `UL2CM3.DLL`（`uvprojx:133-148`）；`uvoptx` 只保存窗口/断点，
  不作为构建事实。
- 历史 ARMCC 构建证据（`HISTORICAL`）：`MDK-ARM/Objects/Coffee2/Coffee2_cache_audit_20260829.log`
  （Code 191044 / RO 5824 / RW 532 / ZI 150436，0E/0W）与 `Coffee2_P3_final.log`
  （Code 191032 / RO 5772 / RW 532 / ZI 150436，0E/0W）。本轮未执行 ARMCC 构建。

---

## 7. 双工具链对照

| 对照项 | GCC（Coffee2Target） | Keil（Coffee2） | 状态 |
| --- | --- | --- | --- |
| MCU 型号与宏 | `STM32F407xx` + `USE_HAL_DRIVER` + `USE_COFFEE2`（`stm32cubemx/CMakeLists.txt:5-9`、顶层 `:17`） | `Device=STM32F407VGTx`，Define 同上 + `USER_VECT_TAB_ADDRESS/VECT_TAB_OFFSET=0x0000C000U`（`uvprojx:26,342`） | CONFIRMED（宏一致）；型号 CONFLICT（VGT vs IOC VET） |
| startup | `GCC-ARM/startup_stm32f407xx.s`（GCC 语法，Reset_Handler→main，`startup:57-101`） | `MDK-ARM/startup_stm32f407xx.s`（ARMCC 语法，组 386） | CONFIRMED（文件不同、语义相同） |
| HAL | `STM32_Drivers` OBJECT（`cmake/stm32cubemx/CMakeLists.txt:204-207`） | `Drivers/STM32F4xx_HAL_Driver` 组（`uvprojx:900`） | CONFIRMED |
| FreeRTOS | `FreeRTOS` OBJECT（`cmake/stm32cubemx/CMakeLists.txt:210-213`） | `Middlewares/FreeRTOS` 组（`uvprojx:1020`） | CONFIRMED |
| lwIP | `LwIP` OBJECT（`cmake/stm32cubemx/CMakeLists.txt:215-218`） | `Middlewares/LwIP` 组（`uvprojx:1075`） | CONFIRMED |
| CubeMX 生成源 | `MX_Application_Src` 全部加入 executable（`cmake/stm32cubemx/CMakeLists.txt:41-58,220`） | `Application/User/*` 组（`uvprojx:396-514,880-888`） | CONFIRMED（同一 `CubeMX_Base`） |
| Coffee2 私有源 | `coffee2_app` 12 源（`Application/CMakeLists.txt:153-166`） | `Application/Coffee2App` 组 12 源（`uvprojx:562-624`） | CONFIRMED（一一对应） |
| DeviceLibrary 源 | 全部 `device_*` 编译，O/X/M50 链接期被 gc-sections 丢弃 | 公共组含 O/X/M50 但 `IncludeInBuild=0` | CONFLICT（“是否编译”表述不一致，链接净效果一致） |
| include 泄漏 Coffee2 私有路径 | `coffee2_app` PRIVATE、组合根 PRIVATE；公共目标不得见（`Application/CMakeLists.txt:168-179`、`GCC-ARM/CMakeLists.txt:62-64`） | target 级 IncludePath 列出 Coffee2 私有目录（`uvprojx:344`） | CONFIRMED（机制不同，源码边界一致） |
| 优化等级 | Debug `-Og` / Release `-Os`（`gcc-arm-none-eabi.cmake:30-38`） | `Optim=1`（-O1）（`uvprojx:318`） | CONFIRMED（不同） |
| 链接地址 | FLASH `0x0800C000`/`0x54000`（OTA ld `:60`） | `LR_IROM1 0x0800C000 0x54000`（`Coffee2_CCM.sct:5`） | CONFIRMED（一致） |
| CCM | `.ccm_bss` 64K（ld `:245-253`），`app_ccm.c` 清零 | `RW_CCM` 64K（`Coffee2_CCM.sct:13`），startup UNINIT | CONFIRMED（一致） |
| 主栈 | ld `_estack` RAM 顶部（OTA ld `:64`） | `RW_IRAM2 0x2001C000` startup STACK（`Coffee2_CCM.sct:18-20`） | CONFIRMED（一致，0x20020000-8 顶部栈） |
| OTA 布局 | 元数据 0x08004000/staging 0x08060000-0x080C0000/应用 0x0800C000-0x08060000（`coffee2_ota.h:17-24`） | 同（由同一 `coffee2_ota.h` 与 Coffee2 scatter/ld 保证） | CONFIRMED |
| 构建产物 | `Coffee2Target.{elf,hex,bin,map}`（Debug 174960 text/2026-09-02 实测） | `Objects/Coffee2/Coffee2.{axf,hex}`（历史 Code≈191K） | 当前 Debug CONFIRMED；Keil HISTORICAL |
| 崩溃汇编 | `app_crash_fault_gcc.S`（`Application/CMakeLists.txt:32`） | `app_crash_fault_armcc.s`（`uvprojx:525-527`） | CONFIRMED |

### §7 记录的状态标签说明

- `CONFIRMED`：以上表格里源码/配置直接证实的行。
- `CONFLICT`：① MCU 型号（uvprojx VGTx vs 维护 IOC VETx，`F407Base_*.ioc:441`）；
  ② GCC 构建图“编译 O/X/M50”与 CHANGES“完整排除 Kalerm O/X”表述不一致；
  ③ 根路径文档值 D: 与实际工作区 C: 不符。
- `UNKNOWN`：真实 BOM/丝印、Release 当日产物、Bootloader 本体行为。
- `HISTORICAL`：2026-08-29/08-28 的 Keil 与 GCC 尺寸日志。
- `OUT_OF_SCOPE`：MilkTea、legacy `ProtocolStack/Modbus`（`README_LEGACY.md`）、
  `DeviceModel/IO_State`。

---

## 8. 当前设计哲学

结合当前源码逐条说明：

1. **公共/私有边界**：公共 = 无产品名称/寄存器/订单语义/物理总线绑定的代码；Coffee2
   业务只在 `UserAPP/Coffee2App`（`当前工程架构与公共私有边界.md:56-62`）。公共目标在
   CMake 产品分支之前定义（`Application/CMakeLists.txt:30-148`），Keil 公共组唯一。
2. **公共设备库可供所有 Target 使用**：`DeviceLibrary` 的驱动不引用 Coffee2 头文件，
   只接收“设备上下文 + 传输 + 结果”三元组（`AGENTS.md:46-47`）。
3. **Target 在编译期选择设备**：Coffee2 用静态 `s_axBusConfigs`/绑定表选择 F200/杯盖/
   糖浆等（`coffee2_rtu_bus.c:46-56`、`coffee2_device.c:136-176`），Keil 用
   `IncludeInBuild`（O/X/M50=0），GCC 用 `--gc-sections` 丢弃未引用驱动——同一
   “产品组合 = 源集合 × 链接”思路，无运行时插件。
4. **每条物理总线只有一个 owner**：`vCoffee2RtuBusTask`（Bus2-5 各一任务）+ Robot
   owner（`vCoffee2RobotTcpTask`）+ Server 多槽轮询；同一条 UART 不被两个任务抢
   （`coffee2_rtu_bus.c:194-360`）。
5. **业务命令由 Target 私有管理**：命令枚举/队列/超时/取消全在 Coffee2
   （`coffee2_device.h`、`coffee2_device.c`），没有公共 CommandManager。
6. **DeviceLibrary 不解释业务**：驱动函数完成“动作→帧→等待→解析→镜像”，业务结果
   归一化由 owner 完成（`coffee2_rtu_bus.c:330-359`、`coffee2_device.c`）。
7. **不保留 DeviceProtocol 薄转发**（见 §4）。
8. **静态对象优先**：任务栈/队列/EventGroup/信号量/环缓冲均为静态或 CCM 静态
   （`StaticQueue_t`/`StaticEventGroup_t`，如 `coffee2_rtu_bus.c:30-40`、
   `coffee2_device.c:49-74`）；FreeRTOS 堆 32K 在 CCM（`coffee2_manager.c:32-35`）。
9. **避免动态插件与无意义抽象**：只有 `CommonTargets.h` 一个按宏组合的适配点。
10. **状态投影与设备驱动分离**：驱动写镜像结构（`.h` 纯数据），Workflow/Server 将
    设备镜像投影为产品状态/寄存器（`coffee2_workflow.c`、`coffee2_server.c:1474-1866`）。
11. **Keil 与 GCC 表达同一个产品组合**：同一源码 + 同一分区/CCM/向量偏移常量。
12. **日志必须帮助定位设备、动作与错误阶段**：所有日志按 `source + 动作事件 + result +
    field` 结构提交（`coffee2_log.h`、`app_log.c`），事件文本为 ASCII。

---

## 9. 系统关系图

### 9.1 四层依赖图

```text
        L4 Coffee2App（私有：业务/命令/任务/绑定/寄存器语义）
         │ 依赖（向下）
         ▼
        L3 DeviceLibrary（公共设备驱动）
         │ 依赖
         ▼
        L2 Common/Transport/ModbusPort/nanoMODBUS（公共基础）
         │ 依赖
         ▼
        L1 CubeMX_Base 平台（HAL/FreeRTOS/lwIP/startup）
```

受控例外：`L1 freertos.c -> Common/CommonTargets.h -> L4 coffee2_manager.h`。

### 9.2 启动图

```text
startup (Reset_Handler) -> SystemInit -> main
main: 关中断清NVIC -> HAL_Init -> SystemClock_Config(168MHz)
      -> PeriphCommonClock_Config -> MX_GPIO/DMA/SPI1/UART1-6/RTC/CRC_Init
      -> [GCC] vAppCcmInit -> vAppCrashDiagInit -> dp83848_hw_reset -> HAL_Delay(5000)
      -> osKernelInitialize -> MX_FREERTOS_Init -> osKernelStart
MX_FREERTOS_Init: osThreadNew(StartDefaultTask) + xAppTaskManagerCreateTasks()
StartDefaultTask: MX_LWIP_Init() + vAppTaskManagerRunDefaultTask()
xAppTaskManagerCreateTasks: 静态基设 -> C2Server/C2Robot/C2Bus2-5/C2Workflow/C2Log
```

### 9.3 任务关系图

```text
defaultTask(普通) ── MX_LWIP_Init 后常驻网络监控/等待
C2Server(tskIDLE+3)      ── 监听 6001、轮询槽位、调用 nanoMODBUS server
C2Robot(tskIDLE+2)       ── Robot TCP client + 命令队列(4) + TcpClientSession
C2Bus2..5(tskIDLE+2)     ── 每路静态命令队列(4)，owner 串行执行设备命令
C2Workflow(tskIDLE+2)    ── 订单队列(2)、热水/IO 维护子状态机
C2Log(tskIDLE+2)         ── 32 条日志 ring，独占 USART1
EthLink(lwIP,BelowNormal)── PHY 链接线程
```

### 9.4 总线所有权图

```text
USART1  -> C2Log（日志）
UART2   -> C2Bus2（F200 咖啡机；无 ModbusPort）
UART3   -> C2Bus3（RTU：杯/盖=晟枢、糖浆、电能表）
UART4   -> C2Bus4（RTU：冰机、秤）
UART5   -> C2Bus5（RTU：IO 输入 Unit1/FC02、IO 输出 Unit2/FC01+FC05）
ETH TCP -> C2Server（6001 上位机）/ C2Robot（502 Dobot client）
```

### 9.5 命令下行图

```text
Server(寄存器写)或Workflow(步骤)
 -> Coffee2Command_t(xCoffee2CommandSubmit)
 -> 设备绑定(route->queue)（coffee2_device.c）
 -> owner 任务(C2Busx / C2Robot)
 -> DeviceLibrary(驱动) -> ModbusPort/Transport -> UART/lwIP -> 设备
```

### 9.6 状态上行图

```text
设备响应 -> DeviceLibrary 解析(镜像/Status) -> owner 归一化
 -> vCoffee2DeviceCommandCompleted/commit image -> Coffee2 私有状态
 -> Workflow 推进 / Server 状态区(0x1000..0x10FF) -> 上位机读 -> 日志
```













---

## 10. 人工 Review 推荐顺序

| 顺序 | 文件 | 阅读时必须回答的问题 |
| --- | --- | --- |
| 1 | `GCC-ARM/CMakePresets.json`、`GCC-ARM/CMakeLists.txt`、`Application/CMakeLists.txt` | 当前产品只组合哪些目标；include 是否泄漏私有路径 |
| 2 | `MDK-ARM/STM32F407_Base.uvprojx`、`ScatterFiles/Coffee2_CCM.sct`、`GCC-ARM/linker/*.ld` | Keil/GCC 是否同一源码同一布局；哪些源 IncludeInBuild=0 |
| 3 | `GCC-ARM/startup_stm32f407xx.s`、`CubeMX_Base/Core/Src/main.c`、`freertos.c` | 启动顺序、时基、5 秒延时、RTOS 启动点 |
| 4 | `Application/Common/CommonTargets.h`、`coffee2_manager.h/.c` | manager 如何被选中；创建哪些任务/队列/同步对象；谁拥有谁 |
| 5 | `coffee2_workflow.h/.c` | 业务流程如何表示；Step 是什么；失败如何影响状态 |
| 6 | `coffee2_device.h/.c` | 命令结构、route/绑定、事件组语义、结果如何关联原命令 |
| 7 | `coffee2_rtu_bus.h/.c` | 每条总线 owner、重试/超时/取消、设备结果如何提交 |
| 8 | `coffee2_robot_tcp.h/.c`、`dobot_robot_device.h` | Robot 动作如何映射线圈；握手/恢复/超时策略 |
| 9 | `coffee2_server.h/.c` | 寄存器区划分；FC03/06/10 语义；命令如何投递到 owner |
| 10 | `coffee2_device_image.h/.c`、`coffee2_io.h/.c` | 状态镜像如何刷新/投影；IO 状态如何进入产品状态 |
| 11 | `coffee2_ota.h/.c`、`Common/Ota/*` | OTA 分区、Flash 写回读、元数据、Bootloader 约定 |
| 12 | `Common/Log`、`Common/Diagnostics`、`coffee2_log.c`、`coffee2_crash_log_port.c` | 日志与崩溃输出介质、弱接口与强实现 |
| 13 | `transport*.c`、`modbus_port.c`、`nanomodbus` | 字节生命周期、超时传播、错误归一化 |
| 14 | `DeviceLibrary/**` | 每类设备协议归属、状态结构、Coffee2 是否选用 |

各层内推荐断点与检查表见对应分层文档。

---

## 11. 行号快照与状态标签说明

- 本文所有“文件:行号”为 2026-09-02 只读快照；后续源码变更后需重新核对。
- 状态标签含义：`CONFIRMED` 源码/配置直接证实；`CONFLICT` 当前证据互相矛盾；
  `UNKNOWN` 需构建/硬件/缺失资料确认；`HISTORICAL` 历史记录不是当前构建；
  `OUT_OF_SCOPE` 本轮/本架构不处理。

## 12. 相关文档

- 架构定稿：`资料文档/00_README/当前工程架构与公共私有边界.md`
- 工程缓存：`资料文档/00_README/工程基础缓存.md`
- 详细汇总：`资料文档/全局审查.md`
- 分层带读：`01_L1平台与生成层源码带读.md`、`02_L2公共基础层源码带读.md`、
  `03_L3公共设备库层源码带读.md`、`04_L4_Coffee2私有应用层源码带读.md`（同目录）
- 维护规则：根 `AGENTS.md`；变更记录：根 `CHANGES.md`

