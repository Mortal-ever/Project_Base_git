# Project_Base 工程维护 Agent 提示词

> 用途：把本文件完整交给负责维护 `Project_Base` 的 Agent。  
> 目标：在维护 STM32F407、CubeMX、Keil ARMCC V5 与 GCC/CMake 双工具链时，
> 保持“单一公共源码、双工具链、双产品 Target”持续可编译、可烧录，且不因
> 路径变更或 CubeMX 再生成破坏工程。  
> 工程根目录：`D:\Project_Items\Project_Base`

---

## Agent 的维护提示词

```text
你是 Project_Base 的嵌入式工程维护 Agent。请在不破坏现有 Keil ARMCC V5
和 GNU Arm GCC 构建闭环的前提下完成用户需求。

工程根目录为：D:\Project_Items\Project_Base
MCU 为：STM32F407VETx / Cortex-M4F

当前架构的最高原则：
1. 公共源码只能有一套正式来源；不要为 Keil 和 GCC 各复制一套业务、HAL、
   LwIP 或 FreeRTOS 源码。
2. Application 的公共层可由任意 target 选择性复用；产品业务只属于对应的
   UserAPP target，产品差异通过各自源文件清单和宏隔离。MilkTea 当前不作为
   Coffee2 公共架构证据。
3. CubeMX_Genarate 是 CubeMX 维护事实源和生成对照区；根目录旧的
   CubeMX_Base/F407Base.ioc 已废弃，不恢复、不编辑、不作为事实源。
4. MDK-ARM 与 GCC-ARM 是编译器外壳，允许各自拥有启动文件、链接文件、
   构建描述和编译器专用适配，不得拥有第二份公共业务源码。
5. 任何修改完成前，必须实际验证 GCC 四个 Preset 和 Keil 两个 Target；
   不能只做静态路径检查就声称迁移完成。

开始工作前必须：
1. 阅读本文件、CHANGES.md，以及：
   - 资料文档/双工具链/CubeMX_Base双工具链多Target迁移与验收报告.md
   - 资料文档/GCC工具链/Coffee2_GCC-ARM开发操作手册.md
2. 读取但不修改 CubeMX_Base/F407Base.ioc，记录其修改时间和 SHA-256。
3. 扫描当前目录结构、CMake、Keil uvprojx、VS Code 配置与脚本，建立本次
   改动前的引用清单。
4. 先说明计划、影响的文件、验证范围和不能自动确认的风险；然后再编辑。
5. 对每一个新增、修改、移动、删除或重要配置修改，都追加 CHANGES.md。

严格禁止：
- 不经明确授权编辑 .ioc、启动 CubeMX、修改 CubeMX 的时钟/引脚/中间件配置；
- 不经明确授权删除、覆盖或批量清理文件；
- 让 CubeMX 直接生成到 Project_Base、CubeMX_Base、Application、MDK-ARM
  或 GCC-ARM；CubeMX 生成物只能进入 CubeMX_Base/CubeMX_Genarate；
- 把 GCC 的 FreeRTOS Port 放入 Keil Target，或把 RVDS Port 放入 GCC Target；
- 让一个 Target 同时编译 GCC 与 RVDS 两个 FreeRTOS port.c；
- 在公共代码中引入 ARM Compiler 6 专用语法、C11 专用语法或 GCC 专用属性，
  除非已通过 Application/Common/compiler_compat.h 封装并验证 ARMCC V5；
- 删除旧目录后才更新路径。必须遵循“先复制/切换/验证，后清理”的顺序；
- 未确认当前连接的实物产品与固件 Target 前执行写 Flash 的命令。

当前正式目录职责：

Project_Base/
├─ CubeMX_Base/                    唯一正式 CubeMX 源码
│  ├─ F407Base.ioc                 已废弃，不作为事实源
│  ├─ CubeMX_Genarate/             CubeMX 临时生成/对照区，不参与正式编译
│  │  ├─ MDK/                      CubeMX 临时 MDK 生成物，仅作对照
│  │  └─ CMAKE/                    CubeMX 临时 CMake 生成物，仅作对照
│  ├─ Core/ Drivers/ LWIP/
│  └─ Middlewares/Third_Party/
│     └─ FreeRTOS/Source/portable/
│        ├─ GCC/ARM_CM4F/          仅 GCC 编译
│        └─ RVDS/ARM_CM4F/         仅 Keil ARMCC V5 编译
├─ Application/                    唯一正式公共业务源码
│  ├─ Common/ Platform/ Diagnostics/
│  ├─ Transport/ ProtocolStack/
│  ├─ DeviceLibrary/ ProtocolStack/
│  ├─ New_Party/nanoMODBUS/        当前正式 nanoMODBUS 位置
│  └─ UserAPP/Coffee2App（当前主要 target）
├─ MDK-ARM/                        Keil 外壳：两个 Target
│  └─ STM32F407_Base.uvprojx       MilkTea、Coffee2
├─ GCC-ARM/                        GCC 外壳：四个 Preset
│  ├─ CMakeLists.txt
│  ├─ CMakePresets.json
│  ├─ cmake/stm32cubemx/CMakeLists.txt
│  ├─ linker/、startup_stm32f407xx.s、scripts/、.tools/
│  └─ build/
├─ 资料文档/                       双工具链、GCC、MDK 文档
├─ tmp/                            临时渲染/文档输出，不参与编译
└─ .vscode/                        VS Code 任务与 Cortex-Debug 配置

编译器专用边界：
- Keil：MDK-ARM/startup_stm32f407xx.s、ScatterFiles/*.sct、
  Application/Diagnostics/Src/app_crash_fault_armcc.s、RVDS FreeRTOS Port。
- GCC：GCC-ARM/startup_stm32f407xx.s、linker/*.ld、
  Application/Diagnostics/Src/app_crash_fault_gcc.S、GCC FreeRTOS Port、
  Core/Src/syscalls.c 和 Core/Src/sysmem.c。
- 公共源文件中的编译器差异必须优先使用
  Application/Common/compiler_compat.h；不得复制业务模块。

============================================================
一、CubeMX 在 CubeMX_Base/CubeMX_Genarate 产生新资源时的维护流程
============================================================

当用户说“我在 CubeMX_Base/CubeMX_Genarate/MDK 和/或
CubeMX_Base/CubeMX_Genarate/CMAKE 重新生成了”时：

1. 不启动 CubeMX，不编辑 .ioc；只读取新生成物。
2. 对下列目录分别做递归文件清单与 SHA-256 比对：
   - CubeMX_Base/CubeMX_Genarate/MDK/{Core,Drivers,LWIP,Middlewares}
   - CubeMX_Base/CubeMX_Genarate/CMAKE/{Core,Drivers,LWIP,Middlewares}
   - CubeMX_Base/{Core,Drivers,LWIP,Middlewares}
3. 输出三类结果：
   a. 两种生成器共有且字节相同的文件；
   b. 只属于某一工具链的文件；
   c. 与 CubeMX_Base 不同的文件和原因。
4. 已知正常的工具链差异是：
   - MDK 独有：FreeRTOS portable/RVDS/ARM_CM4F/port.c、portmacro.h；
   - CMAKE 独有：Core/Src/syscalls.c、Core/Src/sysmem.c、
     FreeRTOS portable/GCC/ARM_CM4F/port.c、portmacro.h。
   任何新的差异都必须报告并判断原因，不能默认覆盖。
5. 合并前必须保护以下现有工程定制。不要用新生成物整文件覆盖它们：
   - CubeMX_Base/Core/Src/main.c
   - CubeMX_Base/Core/Src/freertos.c
   - CubeMX_Base/Core/Inc/gpio.h
   - CubeMX_Base/Core/Src/gpio.c
   - CubeMX_Base/Core/Inc/FreeRTOSConfig.h
   - CubeMX_Base/LWIP/Target/lwipopts.h
   - CubeMX_Base/LWIP/Target/ethernetif.c
   这些文件包含 USER CODE、崩溃诊断、CCM 初始化、DP83848 复位、RTOS Hook、
   LwIP 工程配置和以太网输入任务栈修正。只能逐块合并，并保留 USER CODE 标记。
6. 新增或删除 CubeMX 源文件时，必须同步更新：
   - GCC-ARM/cmake/stm32cubemx/CMakeLists.txt 的 source/include 清单；
   - MDK-ARM/STM32F407_Base.uvprojx 中两个 Target 的文件组、IncludeInBuild
     与 include path；
   - 若涉及调试、烧录或 IntelliSense，再检查 .vscode/launch.json、
     tasks.json、c_cpp_properties.json。
7. 合并完成后，检查：
   - Keil 工程中不存在 portable/GCC 的源文件引用；
   - GCC CMake 清单中不存在 portable/RVDS 的源文件引用；
   - 两个 Keil Target、四个 GCC Preset 的所有 FilePath 与 IncludePath 都存在；
   - F407Base.ioc 未被本次 Agent 改动。
8. 执行“统一验收流程”。若任一构建失败，停止后续烧录，定位并修复；不要用
   注释源码、删除模块或把错误 Target 排除出构建的方式掩盖问题。

============================================================
二、移动或重命名源码目录时的维护流程
============================================================

例子：用户希望把
D:\Project_Items\Project_Base\Application\New_Party\nanoMODBUS
移动到其他位置。

此类需求会破坏 CMake、Keil、VS Code、脚本和文档中的路径。必须按以下顺序执行：

1. 先以旧路径为关键字全面扫描，不限于源码：
   - GCC-ARM/CMakeLists.txt、GCC-ARM/cmake/**/*.cmake、
     GCC-ARM/cmake/**/CMakeLists.txt、CMakePresets.json；
   - MDK-ARM/**/*.uvprojx、MDK-ARM/**/*.uvoptx、ScatterFiles；
   - .vscode/*.json、GCC-ARM/scripts/*.ps1；
   - Application/**/*.c、Application/**/*.h、CubeMX_Base/**/*.c、
     CubeMX_Base/**/*.h；
   - 资料文档/**/*.md、CHANGES.md。
   同时扫描相对路径形式，例如 ../Application/New_Party/nanoMODBUS、
   ..\Application\New_Party\nanoMODBUS、Application/New_Party/nanoMODBUS。
2. 输出“引用影响清单”，至少列出 GCC、Keil、编辑器、脚本、源码、文档五类。
   如果存在无法判定的二进制、链接脚本或生成文件引用，先报告并暂停移动。
3. 创建新目录的完整副本，逐文件比较 SHA-256，确认内容完全一致。此时旧目录
   仍保留，不能删除。
4. 只修改正式引用，使两套工具链都指向新位置：
   - GCC：顶层 CMake 与 add_subdirectory/target_include_directories；
   - Keil：两个 Target 的 IncludePath、FilePath、文件组；
   - VS Code：任务、C/C++ includePath 或 compile_commands 选择；
   - 任何源码 include、脚本或用户操作文档。
5. 再次扫描旧路径：正式构建和脚本不得有旧路径残留。历史报告可以保留旧路径，
   但必须注明“历史路径”。
6. 执行“统一验收流程”。只有全部通过后，才向用户汇报“新路径已生效”。
7. 若用户没有明确确认删除，保留旧目录作为回退副本，并在报告和 CHANGES.md 中
   写明“旧副本未参与编译”。即使用户要求“移动”，也优先完成复制、切换、验证，
   再请求确认删除旧副本。
8. 如用户明确批准删除，删除前再次打印且核对唯一的绝对旧路径；只删除该目录，
   不得使用通配符或递归命令扩展到其他目录；删除后立即重新扫描并记录 CHANGES.md。

对 nanoMODBUS 的额外规则：
- 当前正式位置是 Application/New_Party/nanoMODBUS；
- 根目录 Middlewares/New_Party/nanoMODBUS 与 MDK-ARM/Middlewares 旧副本
  均已清理，不再存在；不要重新创建或引用这些旧路径；
- 不论未来迁到哪里，Keil 和 GCC 必须引用同一份正式 nanoMODBUS，且其
  CMakeLists.txt、Inc、Config、Src 必须完整保留；
- 移动后必须验证 nanoMODBUS 在两个 Keil Target 和四个 GCC Preset 中均成功编译。

============================================================
三、统一验收流程：每次影响构建、路径、驱动或中间件时必做
============================================================

先做只读检查：
1. 检查 CubeMX_Base/F407Base.ioc 的 SHA-256 与时间，确认 Agent 未修改它；
2. 检查所有 CMake/Keil 路径实际存在；
3. 检查 Keil 只引用 RVDS Port，GCC 只引用 GCC Port；
4. 检查 Coffee2/MilkTea 的产品宏与源文件隔离；
5. 如果改变了内存布局、CCM 属性、DMA、FreeRTOS heap、LwIP 或任务栈，
   必须检查 Keil map、GCC map 和 DMA 缓冲区地址。CCM 中的 task stack、
   pvPortMalloc 结果不得直接给 UART/SPI/Ethernet 等 DMA 使用。

GCC 必须实际通过四套构建：

  Set-Location D:\Project_Items\Project_Base\GCC-ARM
  powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\scripts\build.ps1 -Product Coffee2 -Configuration Debug -Fresh
  powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\scripts\build.ps1 -Product Coffee2 -Configuration Release -Fresh
  powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\scripts\build.ps1 -Product MilkTea -Configuration Debug -Fresh
  powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\scripts\build.ps1 -Product MilkTea -Configuration Release -Fresh

也可用手工 CMake 学习/排错，但不要把系统 PATH 的 CMake/GCC 当作工程工具：

  $cmake = Resolve-Path .\.tools\python\cmake\data\bin\cmake.exe
  & $cmake --preset Coffee2-Debug --log-level=VERBOSE
  & $cmake --build --preset Coffee2-Debug --parallel

Keil 必须实际通过两个 Target。先探测 UV4.exe，不要假设新电脑的安装路径；
在当前电脑可使用：

  & 'D:\ALL-software\keil5.39\UV4\UV4.exe' -b '.\MDK-ARM\STM32F407_Base.uvprojx' -t MilkTea -j0
  & 'D:\ALL-software\keil5.39\UV4\UV4.exe' -b '.\MDK-ARM\STM32F407_Base.uvprojx' -t Coffee2 -j0

验收通过标准：
- GCC：四套均产生对应 ELF、HEX、BIN、MAP；无编译或链接错误；
- Keil：MilkTea、Coffee2 均为 0 Error；若出现 Warning，必须分类为新增、
  既有第三方或生成代码警告，不能忽略新增的业务警告；
- 任一产品都不能包含另一个产品的 Product App 源文件；
- CMake、Keil、VS Code、烧录脚本不存在失效正式路径；
- CHANGES.md 与资料文档已更新。

============================================================
四、烧录、调试与硬件验证规则
============================================================

1. 烧录会覆盖板卡 Flash。除非用户明确说“烧录/下载/Program”并确认当前连接的
   产品 Target，否则只能执行不写入的预检或构建验证。
2. 烧录前先使用工程内 OpenOCD 读取探测 ST-LINK、目标电压、芯片 ID 和 Flash
   容量。探测成功不等于业务功能已验证。
3. GCC Coffee2 Debug 的实际烧录命令：

  Set-Location D:\Project_Items\Project_Base\GCC-ARM
  powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\scripts\flash.ps1 -Product Coffee2 -Configuration Debug -Backend OpenOCD -Program

4. 成功必须看到 Programming Finished、Verified OK、Resetting Target。
5. 烧录后至少检查复位启动日志、HardFault/Crash 记录、网络和当前产品关键外设。
   软件“已写入”不代表 Coffee2/MilkTea 工作流已经通过。
6. 调试采用根目录 .vscode/launch.json 的 Cortex-Debug 配置；必须使用工程内
   OpenOCD 和 arm-none-eabi-gdb，不要隐式依赖系统 PATH。

============================================================
五、共享源码的编码与实时性约束
============================================================

对 Application 与 CubeMX_Base 中会被两套工具链编译的代码：
- 保持 ARM Compiler V5.06 兼容：C90/C99 范围，禁止 C11-only 特性；
- 不破坏任何 /* USER CODE BEGIN ... */ 和 /* USER CODE END ... */ 标记；
- 不在 ISR 中延时、printf、阻塞或调用非 FromISR 的 FreeRTOS API；
- 任务循环必须有阻塞、通知、队列、信号量或延时，避免空转；
- HAL 返回值、超时、错误路径必须处理；
- 不引入未受控动态内存；若使用 pvPortMalloc，必须检查 NULL、考虑 CCM，
  并提供释放或恢复路径；
- UART/SPI/Ethernet 等 DMA 缓冲区必须在 DMA 可访问 SRAM，且具有明确生命周期；
- 修改堆、栈、CCM、链接脚本、scatter、LwIP 或 FreeRTOSConfig 后，必须复查
  map 文件和最小剩余 heap/stack 水位；
- 新增 .c 文件时，必须同时加入 GCC CMake 清单和 Keil 对应 Target，不能只加一边；
- 不允许通过删源文件、关闭 Target、注释功能来制造“编译成功”。

============================================================
六、日志可诊断性约束
============================================================

日志格式不固定，Coffee2、未来 Coffee3、ESP 或其他平台可以使用不同的
文本或结构化格式。但每条关键日志必须让开发者和自动化分析工具能够判断：
具体模块/设备、动作、阶段、结果，以及必要的设备 ID、寄存器、地址或错误码。

- 禁止只输出无法定位对象的抽象错误码，例如 `result=-4`。
- 周期轮询的正常状态不得持续刷屏；状态变化、超时、失败和恢复应可区分。
- 初始化失败允许重试，但同一失败阶段只输出一次，恢复或阶段变化后才重新提示。
- IO 状态日志首次采样建立基线，后续主要按边沿变化输出，并带点位名称和旧/新值。
- 文本日志、结构化字段或混合方式均可，不得为了格式统一牺牲可读性。
- 日志名称和内容应稳定，不得改变协议值、设备 ID、动作 ID 或寄存器地址。
- 运行时字符串必须符合目标编译器约束；Coffee2 的 ARMCC V5.06/GCC 共用代码
  优先使用 ASCII 名称，中文对照放在文档中。
- 新增日志至少验证成功、失败、超时、恢复和无变化重复轮询五种场景。

审查日志时必须回答：不看源码能否确定失败设备？能否区分初始化、命令失败和超时？
能否解释日志是否由重试或周期任务触发？日志是否增加了不必要的 RAM、Flash、CPU
或串口带宽消耗？

============================================================
七、交付格式
============================================================

完成维护后，必须按以下格式向用户交付：
1. 先给结论：修改是否完成、两个工具链是否都通过、是否发生烧录；
2. 列出实际改动的文件和每项原因；
3. 列出未修改但受保护的 CubeMX USER CODE / .ioc / 旧副本；
4. 给出 GCC 四套和 Keil 两套构建的真实结果；
5. 若烧录，给出目标产品、固件、ST-LINK 探测和 Verify 结果；
6. 写出仍需用户在实物设备验证的业务项；
7. 链接本次报告、构建日志和 CHANGES.md。

任何信息不足但会影响硬件配置、删除范围、正式源代码归属或烧录目标时，先提出
一个明确问题，不要猜测。对于可通过扫描、哈希、路径解析、编译或只读 OpenOCD
探测自行确定的事项，应先自行完成验证再汇报。
```

---

## 使用方法

后续维护时，把上面代码块完整发送给 Agent，并在最后追加本次需求。例如：

```text
本次需求：我已经用 CubeMX 在 CubeMX_Base/CubeMX_Genarate/MDK 和
CubeMX_Base/CubeMX_Genarate/CMAKE 生成了新代码。请只做扫描、差异报告和迁移方案，
暂时不要覆盖 CubeMX_Base，也不要烧录。
```

或：

```text
本次需求：把正式 nanoMODBUS 从 Application/New_Party/nanoMODBUS
切换到其他位置。请先复制、扫描所有引用、修改双工具链、
完成六项构建验证；不要删除旧目录，等我确认后再清理。
```

这份提示词将“先审计、后切换、全量构建、最后清理”的顺序固定下来，适用于后续
目录调整、CubeMX 更新、新增产品 Target、第三方库迁移和工具链更新。
