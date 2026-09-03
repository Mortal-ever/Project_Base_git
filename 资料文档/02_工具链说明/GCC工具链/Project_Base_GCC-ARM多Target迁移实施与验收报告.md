# Project_Base GCC-ARM 多 Target 迁移实施与验收报告

> 日期：2026-08-04  
> 适用工程：`C:\Users\13193\Desktop\Project_Base`  
> MCU：STM32F407VETx / Cortex-M4F  
> GCC 工程入口：`Project_Base\GCC-ARM`  
> 结论：Coffee2、MilkTea 的 Debug/Release 四套 GCC 构建均已通过；未操作 CubeMX，未烧录实体板卡。

## 1. 本次完成范围

本次工作的目标不是复制第二套业务源码，而是在同一个 `Project_Base` 根目录内建立独立的 GCC-ARM 工程外壳，使 Keil ARMCC V5 与 GNU Arm GCC 根据各自工程配置编译同一套 `Core/`、`Drivers/`、`Middlewares/`、`LWIP/`、`Application/` 源码。

已完成以下事项：

1. 修复 CubeMX 生成但尚不完整的 `GCC-ARM` CMake 外壳，使其能够引用上一级公共源码。
2. 建立 Coffee2、MilkTea 两个产品 Target，以及 Debug、Release 两种构建配置。
3. 把 Coffee2 修正版日志、HardFault/FreeRTOS 崩溃诊断、工作流取消保护、RTU 协议修正同步到当前工程。
4. 增加 ARMCC V5/GCC 编译器兼容层，使 CCM 数据声明根据编译器自动切换。
5. 为 GCC 增加独立启动文件、链接脚本、CCM 清零逻辑和 GCC HardFault 汇编入口。
6. 增加工程内 CMake、Ninja、GNU Arm Toolchain、GDB、OpenOCD 使用闭环。
7. 增加 VS Code 编译任务、Cortex-Debug/OpenOCD 调试配置和 OpenOCD 烧录任务。
8. 实际编译并检查四套 GCC 产物。

本次明确没有执行的操作：

- 没有启动 STM32CubeMX；
- 没有修改 `F407Base.ioc`；
- 没有修改 Keil 的 `.uvprojx`、`.uvoptx`、ARMCC 启动文件或 scatter 文件；
- 没有连接或烧录实体板卡；
- 没有重复从网络下载参考工程中已经存在的工具，而是把已有 `.tools` 收拢到当前工程。

## 2. 迁移后的目录职责

```text
Project_Base/
├─ Core/ Drivers/ Middlewares/ LWIP/ Application/  公共源码，Keil/GCC 共用
├─ MDK-ARM/                                      Keil ARMCC V5 工程外壳
│  ├─ STM32F407_Base.uvprojx
│  └─ ScatterFiles/
├─ GCC-ARM/                                      GNU Arm GCC 工程外壳
│  ├─ CMakeLists.txt                             产品选择与最终固件 Target
│  ├─ CMakePresets.json                          四个可复现配置预设
│  ├─ cmake/
│  │  ├─ gcc-arm-none-eabi.cmake                 交叉编译器与编译/链接选项
│  │  └─ stm32cubemx/CMakeLists.txt              CubeMX 驱动、中间件源码清单
│  ├─ linker/STM32F407XX_FLASH.ld                GCC 专用内存布局
│  ├─ startup_stm32f407xx.s                      GCC 专用启动文件
│  ├─ scripts/                                   环境、编译、烧录入口
│  ├─ .tools/                                    工程内固定版本工具
│  └─ build/                                     四套独立构建目录
├─ .vscode/                                      编辑、任务、下载与调试入口
└─ 资料文档/                                     工程报告和使用说明
```

这里的核心原则是“公共源码只有一份，编译器外壳分开”。CubeMX、Keil、GCC 可以拥有各自的工程描述文件、启动文件和链接布局，但不能各复制一套业务源码，否则后续修复会产生分叉。

## 3. 多 Target 设计

### 3.1 四个 CMake Preset

| Preset | 产品宏 | 最终 ELF | 构建目录 |
| --- | --- | --- | --- |
| `Coffee2-Debug` | `USE_COFFEE2=1` | `Coffee2Target.elf` | `GCC-ARM/build/Coffee2-Debug` |
| `Coffee2-Release` | `USE_COFFEE2=1` | `Coffee2Target.elf` | `GCC-ARM/build/Coffee2-Release` |
| `MilkTea-Debug` | `USE_MILKTEA=1` | `MilkTeaTarget.elf` | `GCC-ARM/build/MilkTea-Debug` |
| `MilkTea-Release` | `USE_MILKTEA=1` | `MilkTeaTarget.elf` | `GCC-ARM/build/MilkTea-Release` |

各预设拥有独立的 CMake Cache、对象文件和固件名称，因此切换产品时不会错误复用另一个产品的增量编译结果。

### 3.2 源码选择方式

`Application/CMakeLists.txt` 先定义公共对象库：

- `app_diagnostics`：崩溃诊断、GCC HardFault 入口、GCC CCM 初始化；
- `app_transport`：公共 TCP/UART Transport；
- `app_modbus_port`：公共 nanoMODBUS 端口层。

然后根据 `PRODUCT_NAME` 只加入当前产品源码：

- Coffee2：日志、崩溃日志端口、设备层、IO、RTU 总线、TCP Server、Robot TCP、工作流、任务管理器、Coffee2 协议；
- MilkTea：日志、网络监测、调试、RTU、TCP Client、工作流、任务管理器以及 MilkTea/Robot/IO 协议。

Coffee2 的 `coffee2_retarget.c` 没有加入 GCC Target，因为 GCC 已使用 CubeMX 的 `syscalls.c`，同时加入两者会造成 `_write`/stdio 重定向职责重叠。Keil 工程仍按原配置使用自己的 retarget 实现。

### 3.3 后续增加第三个产品

增加第三个产品时，应按以下顺序扩展：

1. 在 `GCC-ARM/CMakeLists.txt` 增加 `PRODUCT_NAME` 分支、产品宏和固件名称；
2. 在 `Application/CMakeLists.txt` 增加该产品的 include 目录、精确源码清单与对象库；
3. 在 `GCC-ARM/CMakePresets.json` 增加 Debug/Release 预设；
4. 在 `.vscode/tasks.json` 和 `.vscode/launch.json` 增加相应入口；
5. 不复制公共源码，也不使用递归 glob 自动吞入所有 `.c`，避免把其他产品源码误编译进来。

## 4. ARMCC V5 与 GCC 共用源码的兼容设计

### 4.1 自动识别编译器

公共头文件 `Application/Common/compiler_compat.h` 使用编译器预定义宏判断当前工具链：

- ARMCC V5：`__CC_ARM`；
- GNU Arm GCC：`__GNUC__`。

`__CC_ARM` 的判断放在 `__GNUC__` 之前，避免兼容宏造成误判。

### 4.2 编译标准与 RTOS Port

| 项目 | Keil/ARMCC V5 | GNU Arm GCC |
| --- | --- | --- |
| 工程入口 | `MDK-ARM/STM32F407_Base.uvprojx` | `GCC-ARM/CMakeLists.txt` |
| C 语言模式 | Keil 当前 C99 兼容设置 | GNU C11（开启 GNU 扩展） |
| FreeRTOS Port | `portable/RVDS/ARM_CM4F` | `portable/GCC/ARM_CM4F` |
| 启动文件 | Keil/ARMCC 启动文件 | `GCC-ARM/startup_stm32f407xx.s` |
| 内存描述 | Keil `.sct` | GCC `.ld` |
| 产品宏 | Keil Target 配置 | CMake Preset 自动注入 |

业务源文件继续按 ARMCC V5 可接受的 C 语法编写；GCC 工程可以使用更现代的构建系统，但不应把 GCC 独有语法直接散落到公共业务代码中。

### 4.3 CCM 数据兼容

统一使用：

```c
APP_CCM_DATA
APP_CCM_HEAP
```

ARMCC V5 会映射到 `CCM_APP` / `CCM_HEAP` scatter 区域；GCC 会映射到 `.ccm_bss`。GCC 的启动文件不会自动处理项目自定义 `.ccm_bss`，因此 `main.c` 只在 GCC 下调用 `vAppCcmInit()` 完成清零。ARMCC 仍由 scatter-load 机制处理，不会重复清零。

不要把 DMA 描述符、以太网缓冲或需要外设访问的内存放进 CCM；STM32F407 的 CCM 只能由 CPU 访问。

### 4.4 当前兼容性验证边界

四套 GCC 已实际编译。当前电脑未找到 `UV4.exe`，因此本轮没有重新执行 Keil 命令行构建；Keil 工程文件也没有被修改。公共源码保留 ARMCC V5 条件分支和原 scatter 区名，且同步内容来自已经过 Keil 修正版验证的参考工程。正式合入前仍建议在安装 Keil V5.06u7 的电脑上各重编一次 Coffee2 和 MilkTea，作为双工具链最终验收。

## 5. 日志、崩溃诊断和工作流修正

### 5.1 日志链路

检查发现当前工程的 `coffee2_log.c` 和 `app_crash_diag.c` 与参考修正版主体一致，原问题主要是 GCC 外壳没有把完整诊断链路纳入构建。本次 CMake 已明确加入：

- `coffee2_log.c`；
- `coffee2_crash_log_port.c`；
- `app_crash_diag.c`；
- `app_crash_fault_gcc.S`；
- FreeRTOS stack overflow hook。

这样 Coffee2 在 GCC 下不会只编译普通串口日志，而遗漏 HardFault/栈溢出持久化诊断。

### 5.2 崩溃闭环

GCC ELF 的符号检查已确认：

- `AppHardFault_Handler` 已链接；
- `vApplicationStackOverflowHook` 已链接；
- `__ccm_bss_start__` / `__ccm_bss_end__` 已导出；
- `ucHeap` 位于 CCM。

GCC 使用独立汇编入口捕获异常现场，C 层继续复用公共 `app_crash_diag.c`。`main.c` 在 RTOS 启动前执行崩溃诊断初始化。

### 5.3 Coffee2 工作流和通信修正

本次从 `Project_Base_cmake` 的修正版同步了 Coffee2 工作流取消/过期事件保护、设备状态处理、RTU 总线、Robot TCP、任务管理器和协议配置等相关文件，并同步 nanoMODBUS 端口配置。目的包括：

- 避免订单取消后旧事件继续推进工作流；
- 强化设备失败、超时、队列和 epoch/订单边界；
- 统一 RTU 协议和 nanoMODBUS 配置；
- 保留修正版连接与日志行为。

以太网接口任务栈同步到修正版的 1024 words，替换原先偏小的 350 words 配置，以降低网络任务栈溢出风险。该变化会增加 SRAM 占用，已反映在后面的内存统计中。

## 6. 工程内工具链

所有工具位于：

```text
Project_Base\GCC-ARM\.tools\
├─ python\cmake\data\bin\cmake.exe
├─ python\bin\ninja.exe
├─ arm-gnu-toolchain\bin\arm-none-eabi-gcc.exe
├─ arm-gnu-toolchain\bin\arm-none-eabi-gdb.exe
├─ openocd\bin\openocd.exe
├─ openocd\openocd\scripts\
└─ downloads\arm-gnu-toolchain-15.2.rel1-...zip
```

当前实测版本：

| 工具 | 版本 | 职责 |
| --- | --- | --- |
| CMake | 4.3.4 | 读取 CMakeLists，生成 Ninja 构建图 |
| Ninja | 1.13.0 | 按构建图执行增量编译 |
| GNU Arm GCC | 15.2.1 / 15.2.Rel1 | C/ASM 交叉编译与链接 |
| GNU Arm GDB | 16.3.90 | 源码级调试客户端 |
| xPack OpenOCD | 0.12.0+dev | ST-LINK 的 GDB Server 与烧录器 |

这套工具链与 ST 官方 VS Code 插件、EIDE 的关系如下：它们都可以在底层调用编译器、构建器、GDB Server 和烧录器；本工程把这些底层工具和命令显式保存在仓库外壳中，不依赖某个 IDE 扩展替你生成工程。VS Code 插件只是交互界面，不是编译器本身。

## 7. 手工学习 CMake 构建流程

建议先在 PowerShell 中逐条执行以下命令，理解每一层，再使用封装脚本。

### 7.1 进入 GCC 工程并定义 CMake 路径

```powershell
Set-Location C:\Users\13193\Desktop\Project_Base\GCC-ARM
$cmake = Resolve-Path .\.tools\python\cmake\data\bin\cmake.exe
```

`$cmake` 只是 PowerShell 变量，保存工程内 `cmake.exe` 的绝对路径。前面的 `&` 是 PowerShell 的调用运算符，用于执行变量中保存的程序路径。

### 7.2 查看所有配置

```powershell
& $cmake --list-presets=all
```

它读取 `CMakePresets.json`，只列出可用配置，不编译源码。

### 7.3 Configure：生成构建系统

```powershell
& $cmake --preset Coffee2-Debug --log-level=VERBOSE
```

这一步完成：选择交叉编译器、选择 Coffee2 源码、检查依赖、生成 `build/Coffee2-Debug/build.ninja` 和 `compile_commands.json`。它通常不会生成最终固件。

如果输出 `Configuring done`、`Generating done`，说明生成成功。

### 7.4 Build：执行编译和链接

```powershell
& $cmake --build --preset Coffee2-Debug --parallel
```

这一步由 CMake 调用 Ninja。Ninja 根据依赖关系只重编发生变化的文件，然后链接 ELF，并由 post-build 命令生成 HEX/BIN 和 size 摘要。

输出 `ninja: no work to do.` 不是错误，表示源码和配置自上次成功构建后没有变化，当前产物已经是最新的。

### 7.5 强制重新配置

```powershell
& $cmake --fresh --preset Coffee2-Debug
& $cmake --build --preset Coffee2-Debug --parallel
```

`--fresh` 会重新创建 CMake Cache，但不会删除公共源码。遇到切换工具链或缓存配置异常时使用，不必每次构建都使用。

### 7.6 Release 和 MilkTea

只需替换 preset：

```powershell
& $cmake --preset Coffee2-Release
& $cmake --build --preset Coffee2-Release --parallel

& $cmake --preset MilkTea-Debug
& $cmake --build --preset MilkTea-Debug --parallel

& $cmake --preset MilkTea-Release
& $cmake --build --preset MilkTea-Release --parallel
```

CMake 官方命令帮助可直接离线查看：

```powershell
& $cmake --help
& $cmake --help-command add_executable
& $cmake --help-command target_link_libraries
& $cmake --help-manual cmake-presets
& $cmake --help-manual cmake-toolchains
```

Ninja 和 GCC 也有本机帮助：

```powershell
& .\.tools\python\bin\ninja.exe --help
& .\.tools\arm-gnu-toolchain\bin\arm-none-eabi-gcc.exe --help
& .\.tools\arm-gnu-toolchain\bin\arm-none-eabi-size.exe --help
```

## 8. 脚本与 VS Code 快捷入口

理解手工命令后，可以使用：

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\scripts\build.ps1 `
    -Product Coffee2 -Configuration Debug
```

完整重新配置：

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\scripts\build.ps1 `
    -Product Coffee2 -Configuration Debug -Fresh
```

`build.ps1` 没有隐藏另一套编译逻辑，它只是按参数选择 preset，然后依次执行本报告第 7.3、7.4 节的两个 CMake 命令。`-ExecutionPolicy Bypass` 只影响本次 PowerShell 子进程，不修改电脑的全局执行策略。

VS Code 中可按 `Ctrl+Shift+B` 构建默认的 Coffee2 Debug，或通过 `Tasks: Run Task` 选择六个构建任务。

## 9. 下载到板卡

### 9.1 接线与前置条件

1. 板卡供电正常，BOOT0 为低；
2. ST-LINK 使用 SWDIO、SWCLK、GND，必要时连接 NRST；
3. 关闭可能占用 ST-LINK 的 Keil、CubeProgrammer 或其他 GDB Server；
4. 先确认选择的是 Coffee2 还是 MilkTea 固件。

### 9.2 先执行安全预检

```powershell
Set-Location C:\Users\13193\Desktop\Project_Base\GCC-ARM
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\scripts\flash.ps1 `
    -Product Coffee2 -Configuration Debug -Backend OpenOCD
```

没有 `-Program` 时脚本只检查 ELF 和 OpenOCD 是否存在，并显示准备烧录的文件，不连接、不擦除、不写芯片。

### 9.3 确认板卡后真正烧录

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\scripts\flash.ps1 `
    -Product Coffee2 -Configuration Debug -Backend OpenOCD -Program
```

底层等价操作为：OpenOCD 使用工程内脚本加载 `interface/stlink.cfg` 和 `target/stm32f4x.cfg`，以 SWD 连接 STM32F4，执行 `program <ELF> verify reset exit`。它会写入、校验、复位并退出。

MilkTea 只需把 `Coffee2` 改为 `MilkTea`。Release 同理把 `Debug` 改为 `Release`。

如果必须使用旧版 ST-LINK Utility CLI，可指定：

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\scripts\flash.ps1 `
    -Product Coffee2 -Configuration Debug -Backend STLink -Program
```

该兼容后端需要电脑安装 `ST-LINK_CLI.exe`；默认 OpenOCD 后端不需要它。

VS Code 中也可以运行：

- `GCC: Program Coffee2 Debug (OpenOCD)`；
- `GCC: Program MilkTea Debug (OpenOCD)`。

烧录任务会先依赖对应构建任务，再执行真正的 `-Program`，因此选择任务前应确认已连接正确板卡。

## 10. VS Code 在线调试

需要安装的 VS Code 扩展：

- `C/C++`：代码跳转和 IntelliSense；
- `Cortex-Debug`：调用 GDB 和 OpenOCD 进行源码级调试；
- `CMake Tools`：可选。当前工程有 preset、task 和脚本，不安装也能编译。

`.vscode/launch.json` 已提供：

- `GCC Coffee2 Debug (OpenOCD)`；
- `GCC MilkTea Debug (OpenOCD)`。

操作流程：

1. 连接 ST-LINK；
2. 在 VS Code 的“运行和调试”中选择对应产品；
3. 按 F5；
4. VS Code 先执行 Debug 构建，再启动工程内 OpenOCD 和 GDB；
5. 调试器下载 ELF，复位后运行到 `main`。

F5 调试属于会写入目标 Flash 的操作。当前配置没有提交 SVD 文件，因此可以正常断点和查看变量，但外设寄存器位域视图需要后续加入 STM32F407 SVD 后再配置 `svdFile`。

## 11. CubeMX 再生成的影响与正确边界

CubeMX 的确可以让 Keil 与 CMake 指向同一个工程根目录并共享 `Core/`、`Drivers/` 等源码，但生成器仍可能重写工程描述文件和生成区之外的内容，不能把“通常共用源码”理解为“无条件覆盖绝对安全”。

本工程采用两层方式：

- `F407Base.ioc` 和 CubeMX 生成源码属于硬件配置基线；
- `GCC-ARM` 下的多 Target 外壳属于人工维护的产品构建层。

后续你使用 CubeMX 重新生成时：

1. 先备份或提交版本；
2. 只由你本人打开 `F407Base.ioc` 并生成；
3. 重点审查 `Core/Src/main.c`、`Core/Src/freertos.c`、中断文件、驱动初始化和 `LWIP/Target` 的差异；
4. 保证自定义代码处于 `USER CODE BEGIN/END`；
5. 不要用 CubeMX 新生成的单 Target `CMakeLists.txt` 直接覆盖本次 `GCC-ARM/CMakeLists.txt`、`Application/CMakeLists.txt`、presets、链接脚本和脚本目录；
6. 如果 CubeMX 改变了 HAL/LwIP 源文件清单，把差异人工同步进 `GCC-ARM/cmake/stm32cubemx/CMakeLists.txt`；
7. 重新执行四个 preset，并在 Keil 下重新编译两个 Target。

本轮结束时，`F407Base.ioc` 的最后写入时间仍为 `2026-08-04 15:36:51`，说明本次工作没有触碰 CubeMX 配置文件。

## 12. 换电脑恢复环境

### 12.1 最稳妥方式：连同 `.tools` 一起复制

当前 `.tools` 已完整包含编译、调试和烧录所需工具。换电脑时把整个 `Project_Base` 复制到新电脑，确保以下文件仍存在：

```text
GCC-ARM\.tools\python\cmake\data\bin\cmake.exe
GCC-ARM\.tools\python\bin\ninja.exe
GCC-ARM\.tools\arm-gnu-toolchain\bin\arm-none-eabi-gcc.exe
GCC-ARM\.tools\arm-gnu-toolchain\bin\arm-none-eabi-gdb.exe
GCC-ARM\.tools\openocd\bin\openocd.exe
```

`.tools` 大约 1.5 GiB，并已写入 `.gitignore`，所以仅执行 Git clone 不会得到它。团队应把 `.tools` 单独保存为内部工具包，或者直接使用移动硬盘/压缩包随工程迁移。

### 12.2 仅缺 CMake/Ninja/GCC 时

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\GCC-ARM\scripts\setup_toolchain.ps1
```

脚本只补齐缺失项：存在的文件不会重复下载。GCC 下载包会进行 SHA-256 校验；当前缓存包在 `.tools/downloads`，保留它即可离线重新解压 GCC。

当前 `setup_toolchain.ps1` 不从网络自动安装 OpenOCD。若 `.tools/openocd` 缺失，请从本工程备份恢复同一目录；构建仍可工作，但烧录和 Cortex-Debug 暂不可用。

### 12.3 新电脑验收

```powershell
Set-Location <新路径>\Project_Base\GCC-ARM
& .\.tools\python\cmake\data\bin\cmake.exe --list-presets=all
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\scripts\build.ps1 `
    -Product Coffee2 -Configuration Debug -Fresh
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\scripts\flash.ps1 `
    -Product Coffee2 -Configuration Debug -Backend OpenOCD
```

工程没有硬编码当前用户名路径。CMake、脚本和 VS Code 配置都从 `${workspaceFolder}`、当前脚本目录或 CMake 源目录计算路径，因此换到其他盘符和用户名后无需改源码。

## 13. 实际构建结果

四套构建均使用工程内 GNU Arm GCC 15.2.1，成功生成 `.elf`、`.hex`、`.bin`、`.map`。

| 配置 | text | data | bss | 总计（dec） | 结果 |
| --- | ---: | ---: | ---: | ---: | --- |
| Coffee2 Debug | 207772 | 140 | 137260 | 345172 | 通过 |
| Coffee2 Release | 171912 | 136 | 137280 | 309328 | 通过 |
| MilkTea Debug | 167672 | 140 | 136996 | 304808 | 通过 |
| MilkTea Release | 145916 | 136 | 137024 | 283076 | 通过 |

链接器输出的代表性内存占用：

- Coffee2 Debug：RAM 98,312 B / 128 KiB（75.01%），CCMRAM 39,088 B / 64 KiB（59.64%），Flash 207,920 B / 512 KiB（39.66%）；
- MilkTea Debug：RAM 104,368 B / 128 KiB（79.63%），CCMRAM 32 KiB / 64 KiB（50.00%），Flash 167,820 B / 512 KiB（32.01%）；
- MilkTea Release：RAM 104,416 B / 128 KiB（79.66%），CCMRAM 32 KiB / 64 KiB（50.00%），Flash 146,060 B / 512 KiB（27.86%）。

构建中存在的警告来自 CubeMX/HAL/LwIP 第三方或生成代码中的未使用形参，例如 `stm32f4xx_hal_flash_ex.c`、`ethernetif.c`、`dp83848.c`；没有业务源码编译错误，也没有链接失败。本轮没有为了追求零警告而直接修改厂商源码接口。

## 14. 当前验收状态与下一步

软件侧已完成：

- 四个 CMake preset 配置、编译、链接、固件格式转换；
- 双产品源码隔离；
- 日志与崩溃链路纳入 GCC；
- GCC CCM 初始化和符号检查；
- 工程内 OpenOCD 烧录预检；
- VS Code 配置 JSON 校验。

需要你在硬件侧完成：

1. 先烧录 `Coffee2-Debug`，确认启动日志、网口、TCP Server、RTU、设备流程；
2. 人为触发一个可控断言或栈溢出测试前，先保留正常固件，验证重启后的 crash log；
3. 验证订单取消时旧设备事件不会推进新订单；
4. 再烧录 `MilkTea-Debug`，验证产品 Target 没有串用 Coffee2 任务；
5. 在 Keil V5.06u7 下分别 Rebuild Coffee2、MilkTea，记录 0 error 结果；
6. Debug 验证通过后，再把 Release 用于性能与容量对比。

硬件验证若出现 OpenOCD 连接错误，请保存完整终端输出以及 ST-LINK 型号/固件版本；若程序已运行但业务异常，请同时提供 USART 启动到首次异常的完整日志，便于区分烧录链路、时钟/外设初始化和业务状态机问题。
