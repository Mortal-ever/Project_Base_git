# Coffee2 / MilkTea GCC-ARM 开发操作手册

> 适用工程：`C:\Users\13193\Desktop\Project_Base`  
> GCC 工程根目录：`C:\Users\13193\Desktop\Project_Base\GCC-ARM`  
> MCU：STM32F407VETx（Cortex-M4F）  
> 更新日期：2026-08-05

本文是日常使用手册。目标是让你能独立完成：检查环境、理解 CMake 命令、编译 Coffee2/MilkTea、烧录 ST-LINK、在 VS Code 中断点调试，以及遇到问题时知道先查哪里。

## 1. 先建立整体认识

工程只有一套公共源码，两个编译器外壳：

```text
Project_Base/
├─ Core/ Drivers/ Middlewares/ LWIP/ Application/   公共源码
├─ MDK-ARM/                                         Keil ARMCC V5 外壳
└─ GCC-ARM/                                         GNU Arm GCC 外壳（本手册使用）
```

GCC 外壳不会复制业务代码。它通过 CMake 选择要编译的产品源码：

| 产品 | CMake Preset | 输出固件 |
| --- | --- | --- |
| Coffee2 调试版 | `Coffee2-Debug` | `build/Coffee2-Debug/Coffee2Target.elf` |
| Coffee2 发布版 | `Coffee2-Release` | `build/Coffee2-Release/Coffee2Target.elf` |
| MilkTea 调试版 | `MilkTea-Debug` | `build/MilkTea-Debug/MilkTeaTarget.elf` |
| MilkTea 发布版 | `MilkTea-Release` | `build/MilkTea-Release/MilkTeaTarget.elf` |

每个配置都有独立的 `build/` 目录。不要把 Coffee2 的 HEX 烧到 MilkTea 板卡，也不要在不知道目标产品时直接执行烧录命令。

## 2. 工具链在哪里，分别做什么

本工程优先使用工程内工具，不依赖系统 PATH：

```text
GCC-ARM/.tools/
├─ python/cmake/data/bin/cmake.exe       读取 CMakeLists、生成构建规则
├─ python/bin/ninja.exe                  依规则执行增量编译
├─ arm-gnu-toolchain/bin/arm-none-eabi-gcc.exe  交叉编译和链接
├─ arm-gnu-toolchain/bin/arm-none-eabi-gdb.exe  调试客户端
└─ openocd/bin/openocd.exe               ST-LINK 烧录器和 GDB Server
```

工具关系如下：

```text
CMake Preset → 生成 Ninja 构建图 → Ninja 调用 arm-none-eabi-gcc → ELF/HEX/BIN
                                                              ↓
VS Code Cortex-Debug → GDB → OpenOCD → ST-LINK → STM32 Flash
```

这套方式不等于“必须放弃 Keil”。Keil 和 GCC 都编译同一套公共源码；区别是 Keil 使用 `.uvprojx + ARMCC`，GCC 使用 `CMake + Ninja + GNU Arm GCC`。

## 3. 首次检查环境

打开 PowerShell，执行：

```powershell
Set-Location C:\Users\13193\Desktop\Project_Base\GCC-ARM

$cmake = Resolve-Path .\.tools\python\cmake\data\bin\cmake.exe
& $cmake --version

& .\.tools\python\bin\ninja.exe --version
& .\.tools\arm-gnu-toolchain\bin\arm-none-eabi-gcc.exe --version
& .\.tools\openocd\bin\openocd.exe --version
```

应能看到 CMake、Ninja、GNU Arm GCC 和 OpenOCD 的版本信息。如果 CMake/Ninja/GCC 缺失，运行：

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass `
    -File .\scripts\setup_toolchain.ps1
```

该脚本只补齐不存在的 CMake/Ninja/GCC，不会重复下载已有文件。OpenOCD 由工程的 `.tools/openocd` 目录提供；如果它缺失，应从本工程工具备份恢复这个目录。

## 4. 最重要的两个 CMake 命令

先定义本次 PowerShell 会话的 CMake 路径：

```powershell
Set-Location C:\Users\13193\Desktop\Project_Base\GCC-ARM
$cmake = Resolve-Path .\.tools\python\cmake\data\bin\cmake.exe
```

### 4.1 查看可用产品与配置

```powershell
& $cmake --list-presets=all
```

此命令只读取 `CMakePresets.json`，不会修改源码、不会编译、不会连接板卡。

### 4.2 Configure：生成构建规则

```powershell
& $cmake --preset Coffee2-Debug --log-level=VERBOSE
```

Configure 会：

1. 选择 `Coffee2-Debug`；
2. 选择工程内 GNU Arm GCC 和 Ninja；
3. 设置 `USE_COFFEE2=1`、Debug 编译选项和 GCC 链接脚本；
4. 读取公共源码与 Coffee2 源码清单；
5. 在 `build/Coffee2-Debug/` 生成 Ninja 规则和 `compile_commands.json`。

看到以下输出表示成功：

```text
-- Configuring done
-- Generating done
-- Build files have been written to: .../build/Coffee2-Debug
```

Configure 成功不代表已生成固件；下一步还要 Build。

### 4.3 Build：真正编译和链接

```powershell
& $cmake --build --preset Coffee2-Debug --parallel
```

Build 会调用 Ninja，编译改动过的 `.c/.s`，链接为 ELF，并自动生成：

```text
Coffee2Target.elf    调试、GDB、OpenOCD 主要使用的文件
Coffee2Target.hex    Intel HEX 格式，兼容多种烧录器
Coffee2Target.bin    原始二进制镜像
Coffee2Target.map    链接映射，用于分析 Flash/RAM/CCM 占用
```

如果看到：

```text
ninja: no work to do.
```

不是报错，而是源码没有变化，现有固件已经是最新的。

### 4.4 强制重新配置

切换编译器、修改 CMakeLists、发生缓存异常时使用：

```powershell
& $cmake --fresh --preset Coffee2-Debug
& $cmake --build --preset Coffee2-Debug --parallel
```

`--fresh` 只重建当前产品配置的 CMake Cache；不会删除 `Core/`、`Application/` 等公共源码。

## 5. 编译 Coffee2、MilkTea 的完整命令

### Coffee2 Debug：日常开发首选

```powershell
Set-Location C:\Users\13193\Desktop\Project_Base\GCC-ARM
$cmake = Resolve-Path .\.tools\python\cmake\data\bin\cmake.exe
& $cmake --preset Coffee2-Debug
& $cmake --build --preset Coffee2-Debug --parallel
```

### Coffee2 Release：性能/容量验证

```powershell
& $cmake --preset Coffee2-Release
& $cmake --build --preset Coffee2-Release --parallel
```

Release 优化更高，变量可能显示为 `<optimized out>`，不适合逐行调试；优先用 Debug 定位问题。

### MilkTea Debug / Release

```powershell
& $cmake --preset MilkTea-Debug
& $cmake --build --preset MilkTea-Debug --parallel

& $cmake --preset MilkTea-Release
& $cmake --build --preset MilkTea-Release --parallel
```

## 6. 编译脚本：它只是命令的快捷方式

手工理解 CMake 命令后，可以使用：

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass `
    -File .\scripts\build.ps1 `
    -Product Coffee2 `
    -Configuration Debug
```

等价于：

```powershell
& $cmake --preset Coffee2-Debug
& $cmake --build --preset Coffee2-Debug --parallel
```

完整重新配置：

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass `
    -File .\scripts\build.ps1 `
    -Product Coffee2 `
    -Configuration Debug `
    -Fresh
```

参数含义：

| 参数 | 含义 |
| --- | --- |
| `-Product Coffee2` | 选择 Coffee2 源码和 `USE_COFFEE2` 宏 |
| `-Product MilkTea` | 选择 MilkTea 源码和 `USE_MILKTEA` 宏 |
| `-Configuration Debug` | 无优化或低优化，包含完整调试信息 |
| `-Configuration Release` | 面向发布，优化更高 |
| `-Fresh` | 在编译前重新生成当前配置的 CMake Cache |

`-ExecutionPolicy Bypass` 只作用于这一次启动的 PowerShell 子进程，不会修改 Windows 全局策略。

## 7. 烧录前的硬件检查

烧录失败时，先检查硬件，不要先修改业务代码。

1. 板卡供电正常，3.3 V 和 GND 已确认；
2. `BOOT0` 为低电平，确保从主 Flash 启动；
3. ST-LINK 与板卡至少连接 `SWDIO`、`SWCLK`、`GND`；推荐连 `NRST`；
4. ST-LINK 已被 Windows 识别；
5. 关闭 Keil、STM32CubeProgrammer、其他 OpenOCD/GDB Server，避免占用 ST-LINK；
6. 确认即将烧录的产品与板卡一致。

## 8. 使用工程内 OpenOCD 烧录

### 8.1 为什么默认使用 OpenOCD

OpenOCD 是一个通用的调试服务器和烧录器。它通过工程内的：

```text
interface/stlink.cfg
target/stm32f4x.cfg
```

识别 ST-LINK 和 STM32F4，然后执行“写入 → 校验 → 复位”。因此不要求额外安装旧版 ST-LINK Utility CLI。

### 8.2 第一步：安全预检（不会写板）

先编译 Coffee2 Debug，然后执行：

```powershell
Set-Location C:\Users\13193\Desktop\Project_Base\GCC-ARM

powershell.exe -NoProfile -ExecutionPolicy Bypass `
    -File .\scripts\flash.ps1 `
    -Product Coffee2 `
    -Configuration Debug `
    -Backend OpenOCD
```

不带 `-Program` 时，脚本只检查 ELF、OpenOCD 和 OpenOCD scripts 是否存在，并打印准备烧录的固件路径。它不会连接板卡、擦除或写 Flash。

### 8.3 第二步：真正写入 Flash

确认硬件正确后再执行：

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass `
    -File .\scripts\flash.ps1 `
    -Product Coffee2 `
    -Configuration Debug `
    -Backend OpenOCD `
    -Program
```

此命令会：

1. 连接 ST-LINK；
2. 写入 `Coffee2Target.elf`；
3. 校验写入内容；
4. 复位芯片；
5. 退出 OpenOCD。

烧录 MilkTea：

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass `
    -File .\scripts\flash.ps1 `
    -Product MilkTea `
    -Configuration Debug `
    -Backend OpenOCD `
    -Program
```

若需要烧录 Release，只把 `Debug` 替换为 `Release`。

### 8.4 手工 OpenOCD 命令

脚本底层等价命令如下，通常无需手动输入，但理解它有助于排障：

```powershell
$openocd = Resolve-Path .\.tools\openocd\bin\openocd.exe
$scripts = Resolve-Path .\.tools\openocd\openocd\scripts
$elf = (Resolve-Path .\build\Coffee2-Debug\Coffee2Target.elf).Path.Replace('\', '/')

& $openocd `
    -s $scripts `
    -f interface/stlink.cfg `
    -f target/stm32f4x.cfg `
    -c "adapter speed 4000" `
    -c "program {$elf} verify reset exit"
```

`program` 是写入命令，`verify` 是校验，`reset` 是复位，`exit` 是完成后退出。没有硬件确认时，不要执行这一段。

### 8.5 ST-LINK Utility CLI 兼容后端

如果电脑已安装旧版 ST-LINK Utility，仍可使用：

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass `
    -File .\scripts\flash.ps1 `
    -Product Coffee2 `
    -Configuration Debug `
    -Backend STLink `
    -Program
```

这个后端要求存在：

```text
C:\Program Files (x86)\STMicroelectronics\STM32 ST-LINK Utility\ST-LINK Utility\ST-LINK_CLI.exe
```

默认 OpenOCD 后端不依赖它。

## 9. VS Code 编译与调试

### 9.1 推荐扩展

| 扩展 | 是否必需 | 用途 |
| --- | --- | --- |
| `C/C++`（Microsoft） | 推荐 | 代码补全、跳转、错误提示、读取 compile_commands |
| `Cortex-Debug`（marus25） | F5 调试必需 | 启动 GDB/OpenOCD、断点、变量、调用栈 |
| `CMake Tools`（Microsoft） | 可选 | 图形化查看/选择 CMake preset；不装也能用任务和命令行 |

插件不是编译器。真正执行编译的是工程内 CMake、Ninja 和 GCC；插件只提供编辑和操作界面。

### 9.2 VS Code 编译

1. 用 VS Code 打开 `C:\Users\13193\Desktop\Project_Base`；
2. 按 `Ctrl+Shift+B`，默认构建 Coffee2 Debug；
3. 或按 `Ctrl+Shift+P`，选择 `Tasks: Run Task`；
4. 选择 `GCC: Build Coffee2 Debug`、`GCC: Build MilkTea Debug` 等任务。

任务定义在 `.vscode/tasks.json`，它们调用的仍是第 6 节 `build.ps1`。

### 9.3 VS Code 烧录

在命令面板运行以下任务之一：

- `GCC: Program Coffee2 Debug (OpenOCD)`；
- `GCC: Program MilkTea Debug (OpenOCD)`。

这两个任务会先构建对应 Debug 固件，再带 `-Program` 执行烧录。它们会写板，执行前必须确认目标板卡。

### 9.4 VS Code 断点调试

前置条件：安装 Cortex-Debug、连接 ST-LINK、关闭其他占用 ST-LINK 的软件。

1. 在左侧“运行和调试”选择：`GCC Coffee2 Debug (OpenOCD)` 或 `GCC MilkTea Debug (OpenOCD)`；
2. 在所需代码行点击行号左侧设置断点；
3. 按 F5；
4. VS Code 会先执行 Debug 构建，然后启动 OpenOCD 与 GDB，下载 ELF 并运行到 `main`；
5. 使用 F10 单步跳过、F11 单步进入、Shift+F11 跳出、F5 继续运行。

F5 会下载并写入芯片 Flash。若只想检查构建，使用 Build Task，不要按 F5。

调试配置文件为 `.vscode/launch.json`。GDB 和 OpenOCD 均指向 `GCC-ARM/.tools`，换电脑后只要保留工具目录，不需要重设用户路径。

## 10. 调试时重点看什么

### 10.1 启动就停在 HardFault

本工程已链接 GCC HardFault 入口和 `vApplicationStackOverflowHook`。优先检查：

1. Call Stack（调用栈）最先进入的业务函数；
2. `app_crash_diag.c` 中保存的异常寄存器；
3. 任务栈是否溢出；
4. 是否把 DMA/以太网访问的缓冲区放入了 CCM；
5. 近期是否修改了 CubeMX 时钟、以太网、FreeRTOS 配置。

### 10.2 程序能下载但无串口日志

按以下顺序排查：

1. 确认烧录的是正确产品和 Debug/Release 文件；
2. 检查板卡供电、复位和 BOOT0；
3. 检查串口号、波特率和接线；
4. 在 `main()`、日志初始化、UART 初始化处设断点；
5. 在示波器或逻辑分析仪上确认 MCU TX 引脚是否有波形；
6. 再检查日志任务、队列和网络业务逻辑。

### 10.3 OpenOCD 无法连接

| OpenOCD 现象 | 首先检查 |
| --- | --- |
| `unable to find a matching CMSIS-DAP device` 或无探针 | ST-LINK 驱动、USB 线、设备管理器 |
| `target not halted` / 无法识别芯片 | SWDIO/SWCLK/GND/NRST 接线、板卡供电、降低 `adapter speed` |
| `device is busy` | 关闭 Keil、CubeProgrammer、其他 OpenOCD 进程 |
| 可连接但写入失败 | BOOT0、读保护、供电稳定性、目标芯片型号 |

必要时把 `flash.ps1` 中的 `adapter speed 4000` 临时改为 `adapter speed 1000` 后重试；确认稳定后再恢复。该修改只影响 SWD 通信速度，不影响固件时钟。

### 10.4 Debug 与 Release 表现不同

Debug 和 Release 不能只看“是否能编译”：

- Debug 保留符号、优化低，适合断点和变量观察；
- Release 优化高，时序、栈、Flash 占用可能变化；
- Release 的局部变量可能被优化掉，显示 `<optimized out>` 属正常现象；
- 功能验收应至少覆盖一次 Debug 和一次 Release。

## 11. 查看固件大小和链接布局

构建结束后会自动打印 `text/data/bss`。手工查看：

```powershell
& .\.tools\arm-gnu-toolchain\bin\arm-none-eabi-size.exe `
    .\build\Coffee2-Debug\Coffee2Target.elf
```

查看符号：

```powershell
& .\.tools\arm-gnu-toolchain\bin\arm-none-eabi-nm.exe `
    -g .\build\Coffee2-Debug\Coffee2Target.elf `
    | Select-String 'AppHardFault_Handler|vApplicationStackOverflowHook|ucHeap'
```

更详细的段、符号和地址请查看同目录的 `Coffee2Target.map`。关注三个容量：

- Flash：代码和常量；
- SRAM：全局数据、协议缓冲、任务栈等；
- CCMRAM：CPU 专用数据和 FreeRTOS 堆，不能用于 DMA/以太网外设访问。

## 12. CubeMX 再生成时的规则

本手册不要求通过 CubeMX 工作，但你后续可使用 CubeMX 修改时钟、外设、FreeRTOS 或 LwIP。

重新生成前后应遵守：

1. 先提交或备份；
2. 只在 CubeMX 中修改 `F407Base.ioc` 并生成；
3. 保留 `USER CODE BEGIN/END` 内的业务初始化；
4. 不要用 CubeMX 生成的单 Target CMake 文件覆盖 `GCC-ARM/CMakeLists.txt`、`Application/CMakeLists.txt`、`CMakePresets.json`、`scripts/`、链接脚本；
5. 若 HAL/LwIP 源文件清单变化，人工更新 `GCC-ARM/cmake/stm32cubemx/CMakeLists.txt`；
6. 重新构建 Coffee2/MilkTea 的 Debug/Release；
7. 重新在 Keil 编译两个 Target，保持双工具链一致。

## 13. 换电脑后的最短恢复流程

最稳妥方式是复制整个 `Project_Base`，特别是：

```text
GCC-ARM/.tools/
GCC-ARM/CMakePresets.json
GCC-ARM/cmake/
GCC-ARM/linker/
GCC-ARM/scripts/
.vscode/
```

`.tools` 被 `.gitignore` 排除，单纯 Git clone 不会包含它。团队应把 `.tools` 作为独立工具包保留。复制完成后执行：

```powershell
Set-Location <新路径>\Project_Base\GCC-ARM
& .\.tools\python\cmake\data\bin\cmake.exe --list-presets=all

powershell.exe -NoProfile -ExecutionPolicy Bypass `
    -File .\scripts\build.ps1 `
    -Product Coffee2 `
    -Configuration Debug `
    -Fresh
```

通过后执行第 8.2 节预检；确认 OpenOCD 和固件均存在，再连接板卡烧录。

## 14. 常用命令速查

```powershell
# 进入 GCC 工程
Set-Location C:\Users\13193\Desktop\Project_Base\GCC-ARM

# 查看全部配置
& .\.tools\python\cmake\data\bin\cmake.exe --list-presets=all

# Coffee2 Debug：配置和编译
& .\.tools\python\cmake\data\bin\cmake.exe --preset Coffee2-Debug
& .\.tools\python\cmake\data\bin\cmake.exe --build --preset Coffee2-Debug --parallel

# Coffee2 Debug：安全预检（不写板）
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\scripts\flash.ps1 -Product Coffee2 -Configuration Debug

# Coffee2 Debug：真正烧录（写板）
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\scripts\flash.ps1 -Product Coffee2 -Configuration Debug -Program

# MilkTea Debug：编译并烧录
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\scripts\build.ps1 -Product MilkTea -Configuration Debug
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\scripts\flash.ps1 -Product MilkTea -Configuration Debug -Program
```

## 15. 相关文件

- `GCC-ARM/CMakePresets.json`：产品/配置入口；
- `GCC-ARM/CMakeLists.txt`：最终固件 Target；
- `Application/CMakeLists.txt`：公共和产品源码清单；
- `GCC-ARM/scripts/build.ps1`：构建快捷入口；
- `GCC-ARM/scripts/flash.ps1`：带保护的烧录入口；
- `.vscode/tasks.json`：VS Code 构建/烧录任务；
- `.vscode/launch.json`：Cortex-Debug/OpenOCD 调试配置；
- `资料文档/Project_Base_GCC-ARM多Target迁移实施与验收报告.md`：迁移设计、兼容性和验证记录。
