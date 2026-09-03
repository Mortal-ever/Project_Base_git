# Coffee2 GCC-ARM 外壳实施方案

> 日期：2026-08-04  
> 核心思路：**以旧 Keil 工程（`Project_Base`）为基底**，先用 CubeMX 生成一套 CMake 工具链，再手动收拢 GCC 外壳到 `GCC-ARM/` 文件夹。  
> 这不是"合并两个工程"，而是在旧工程上**扩展出一套 GCC 构建外壳**，核心源码保持一份。

---

## 一、方案定位

### 1.1 一句话理解

```
旧工程 Project_Base（Keil 基底）
    │
    ├── 核心源码（Core/Drivers/Middlewares/LWIP/Application）← 保持一份，不动
    ├── MDK-ARM/（Keil 外壳，已有）
    │
    └── GCC-ARM/（新外壳，本次要建）
        ├── CMakeLists.txt / CMakePresets.json
        ├── cmake/（工具链配置）
        ├── linker/（.ld 链接脚本）
        ├── scripts/（build.ps1 等）
        └── startup_stm32f407xx.s（GCC 版启动文件）
```

### 1.2 与"合并"的区别

| 维度 | 合并 | 本方案（扩展外壳） |
|------|------|--------------------|
| 目标 | 把两套工程并成一套 | 在旧工程上加一套 GCC 外壳 |
| 源码 | 需要迁移、合并差异 | 核心源码保持一份，不迁移 |
| 风险 | 高（可能破坏 keil 工作流） | 低（Keil 外壳原样保留） |
| 结果 | 一个根目录两套壳 | 根目录核心源码 + MDK-ARM + GCC-ARM 两壳 |

---

## 二、为什么在旧工程上扩展（而不是合并）

1. **旧工程 `Project_Base` 是 Keil 的完整基底**：MDK-ARM 外壳、两个 Target（MilkTea/Coffee2）、ScatterFiles 都在。
2. **核心源码已经在旧工程里**：Core/Drivers/Middlewares/LWIP/Application 都是同一份（当前 GCC 工程是从它迁移过去的）。
3. **CubeMX 生成 CMake 外壳更自然**：CubeMX 直接针对 `F407Base.ioc` 生成 CMakeLists.txt，不用手动从零写。
4. **风险可控**：Keil 外壳不动，即使 GCC 外壳有问题，Keil 工作流不受影响。

> 注意：当前 `Project_Base_cmake`（本目录）已经是"旧工程基础上迁移的 GCC 工程"。本方案的最终目标，是让**旧工程 `Project_Base` 自己**也拥有一个 `GCC-ARM/` 外壳，替代/对齐当前的 `Project_Base_cmake`。

---

## 三、准备阶段：前置条件确认

在开始前，确认以下内容：

| 前置项 | 说明 |
|--------|------|
| 旧工程路径 | `C:\Users\13193\Desktop\Project_Base` |
| CubeMX 版本 | 6.16.1（能生成 CMake 工具链） |
| 工具链包 | `arm-gnu-toolchain-15.2.rel1` 已下载 |
| CMake/Ninja | 通过 pip 安装到 `.tools/python` |
| 目标芯片 | STM32F407VETx |

---

## 四、实施步骤（阶段一：CubeMX 生成 CMake 外壳）

### 4.1 用 CubeMX 生成 CMake 工程

1. 打开 `C:\Users\13193\Desktop\Project_Base\F407Base.ioc`
2. 菜单 → **Project** → **Settings**
3. **Toolchain / IDE** 选择：**CMake**
4. **Project Name**：保持 `F407Base`（或设为 `Project_Base`）
5. **Project Location**：建议设为 `C:\Users\13193\Desktop\Project_Base`
6. 点击 **GENERATE CODE**

> ⚠️ **重要**：CubeMX 生成时，可能会覆盖 `Core/`、`Drivers/` 等已修改的源码。  
> 生成前**务必先备份** `Core/Src/main.c`、`Core/Src/freertos.c`、`Core/Inc/FreeRTOSConfig.h` 等被用户修改过的文件。

### 4.2 CubeMX 生成后会产生的文件（预期）

```
Project_Base/
├── CMakeLists.txt              ← CubeMX 生成的根构建文件
├── cmake/
│   ├── gcc-arm-none-eabi.cmake  ← CubeMX 生成的工具链配置
│   └── stm32cubemx/
│       └── CMakeLists.txt       ← CubeMX 生成的驱动/中间件构建
├── Core/                        ← 可能被重新生成（注意备份）
├── Drivers/
├── Middlewares/
├── LWIP/
└── startup_stm32f407xx.s       ← CubeMX 生成的 GCC 版启动文件（可能在 Core/ 或根目录）
```

---

## 五、实施步骤（阶段二：手动收拢 GCC 外壳到 GCC-ARM/）

### 5.1 创建 `GCC-ARM/` 文件夹

在 `Project_Base/` 下创建 `GCC-ARM/` 目录，与 `MDK-ARM/` 对称。

### 5.2 收拢需要移动的文件

把 CubeMX 生成的 GCC 外壳文件和工具链，移动到 `GCC-ARM/`：

```
GCC-ARM/
├── CMakeLists.txt              ← 从根目录移入（CubeMX 生成的）
├── CMakePresets.json           ← 手动创建或从当前 Project_Base_cmake 复制
├── cmake/                      ← 从根目录移入
│   ├── gcc-arm-none-eabi.cmake ← 需修改路径
│   └── stm32cubemx/
│       └── CMakeLists.txt      ← 需修改路径（原来引用 ../../Core）
├── linker/                     ← 从当前 Project_Base_cmake 复制
│   └── STM32F407XX_FLASH.ld    ← GCC 版链接脚本
├── scripts/                    ← 从当前 Project_Base_cmake 复制
│   ├── build.ps1               ← 需修改工程根路径
│   └── setup_toolchain.ps1     ← 需修改工具路径
├── startup_stm32f407xx.s       ← GCC 版启动文件（CubeMX 生成，移入）
└── .tools/                     ← 工具链（可留本机，不随工程）
```

### 5.3 各文件路径修改清单

#### (1) `GCC-ARM/CMakeLists.txt`

CubeMX 生成的根 CMakeLists 会引用 `${CMAKE_SOURCE_DIR}/...` 指向核心源码。移到 `GCC-ARM/` 后，`CMAKE_SOURCE_DIR` = `GCC-ARM/`，核心源码在上级，需要：

```cmake
# 在文件开头定义核心源码根目录
set(CORE_SOURCE_DIR "${CMAKE_SOURCE_DIR}/..")

# 原来：
# add_subdirectory(Application)
# 改成：
add_subdirectory(${CORE_SOURCE_DIR}/Application)

# 原来引用 ${CMAKE_SOURCE_DIR}/... 的路径，改成 ${CORE_SOURCE_DIR}/...
```

#### (2) `GCC-ARM/cmake/stm32cubemx/CMakeLists.txt`

此文件现在在 `GCC-ARM/cmake/stm32cubemx/`，原来引用 `${CMAKE_CURRENT_SOURCE_DIR}/../../Core`（即 `cmake/stm32cubemx/` 上行两级到根）。移到 `GCC-ARM/` 后，Core 在更上层：

```cmake
# 原来：${CMAKE_CURRENT_SOURCE_DIR}/../../Core/Inc
# 现在：${CMAKE_CURRENT_SOURCE_DIR}/../../../Core/Inc
# 或者统一用 ${CORE_SOURCE_DIR}/Core/Inc
```

#### (3) `GCC-ARM/cmake/gcc-arm-none-eabi.cmake`

```cmake
# 原来工具链路径：${CMAKE_CURRENT_LIST_DIR}/../.tools/arm-gnu-toolchain
# 现在 .tools 在 GCC-ARM 下：${CMAKE_CURRENT_LIST_DIR}/../.tools/arm-gnu-toolchain
# 若 .tools 放本机，可改为 ${CMAKE_SOURCE_DIR}/.tools 或绝对路径
```

#### (4) `GCC-ARM/scripts/build.ps1`

```powershell
# 原来：$ProjectRoot = Split-Path -Parent $PSScriptRoot  （脚本在 scripts/，其父即项目根）
# 现在脚本在 GCC-ARM/scripts/，项目根是上上级：
$ProjectRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)

# 工具路径也要重新定位，例如：
$CMake = Join-Path $ProjectRoot "GCC-ARM\.tools\python\cmake\data\bin\cmake.exe"
```

#### (5) `GCC-ARM/scripts/setup_toolchain.ps1`

同样调整 `$ToolsRoot`、`$PythonTools` 等路径，指向 `GCC-ARM/.tools` 或其他位置。

---

## 六、实施步骤（阶段三：CMake 目标与配置）

### 6.1 CMakePresets.json（多 Target）

旧工程有两个 Keil Target（MilkTea/Coffee2），GCC 外壳也要对应支持：

```json
{
    "version": 3,
    "configurePresets": [
        {
            "name": "default",
            "hidden": true,
            "generator": "Ninja",
            "binaryDir": "${sourceDir}/../build/${presetName}",
            "toolchainFile": "${sourceDir}/cmake/gcc-arm-none-eabi.cmake",
            "cacheVariables": {
                "CMAKE_MAKE_PROGRAM": "${sourceDir}/.tools/python/bin/ninja.exe"
            }
        },
        { "name": "Coffee2-Debug",   "inherits": "default",
          "cacheVariables": { "CMAKE_BUILD_TYPE": "Debug",   "TARGET_NAME": "Coffee2" } },
        { "name": "Coffee2-Release", "inherits": "default",
          "cacheVariables": { "CMAKE_BUILD_TYPE": "Release", "TARGET_NAME": "Coffee2" } },
        { "name": "MilkTea-Debug",   "inherits": "default",
          "cacheVariables": { "CMAKE_BUILD_TYPE": "Debug",   "TARGET_NAME": "MilkTea" } },
        { "name": "MilkTea-Release", "inherits": "default",
          "cacheVariables": { "CMAKE_BUILD_TYPE": "Release", "TARGET_NAME": "MilkTea" } }
    ],
    "buildPresets": [
        { "name": "Coffee2-Debug",   "configurePreset": "Coffee2-Debug" },
        { "name": "Coffee2-Release", "configurePreset": "Coffee2-Release" },
        { "name": "MilkTea-Debug",   "configurePreset": "MilkTea-Debug" },
        { "name": "MilkTea-Release", "configurePreset": "MilkTea-Release" }
    ]
}
```

### 6.2 Application/CMakeLists.txt（业务层）

`Application/` 下的 CMakeLists 需要支持根据 `TARGET_NAME` 选择对应应用层：

```cmake
if(NOT DEFINED TARGET_NAME)
    set(TARGET_NAME "Coffee2")
endif()

if(TARGET_NAME STREQUAL "Coffee2")
    set(APP_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/UserAPP/Coffee2App)
    set(APP_DEFINE USE_COFFEE2=1)
    # ... Coffee2 的源文件列表
elseif(TARGET_NAME STREQUAL "MilkTea")
    set(APP_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/UserAPP/MilkTeaApp)
    set(APP_DEFINE USE_MILKTEA=1)
    # ... MilkTea 的源文件列表
endif()
```

### 6.3 启动文件和 FreeRTOS 移植层

- GCC 版启动文件：`GCC-ARM/startup_stm32f407xx.s`（GNU 语法）
- FreeRTOS 移植层：`Middlewares/.../portable/GCC/ARM_CM4F/port.c`（已存在，共用）
- Keil 版启动文件：`MDK-ARM/startup_stm32f407xx.s`（ARMCC 语法，不动）

---

## 七、关键差异处理（双编译器兼容）

### 7.1 main.c 半主机处理

旧工程 `Core/Src/main.c` 有 ARMCC 专有的半主机禁用代码：

```c
#pragma import(__use_no_semihosting)   // ARMCC 专有，GCC 会报错
```

需要加条件编译：

```c
#if defined(__CC_ARM)
#pragma import(__use_no_semihosting)
struct __FILE { int handle; };
FILE __stdout;
void _sys_exit(int x) { (void)x; }
#elif defined(__GNUC__)
/* GCC 版由 syscalls.c + sysmem.c 处理标准库 */
#endif
```

### 7.2 新增编译兼容头文件（CompilerCompat.h）

建议在 `Application/Common/` 新增统一宏头文件，供业务代码引用：

```c
#ifndef COMPILER_COMPAT_H
#define COMPILER_COMPAT_H

#if defined(__GNUC__)
    #define COMPILER_GCC   1
    #define COMPILER_ARMCC 0
#elif defined(__CC_ARM)
    #define COMPILER_GCC   0
    #define COMPILER_ARMCC 1
#endif

/* CCM 数据放置 */
#if COMPILER_GCC
    #define CCM_DATA_SECTION __attribute__((section(".ccm_bss"), aligned(8)))
#elif COMPILER_ARMCC
    #define CCM_DATA_SECTION __attribute__((section("CCM_APP"), zero_init, aligned(8)))
#endif

#endif
```

---

## 八、实施顺序建议

```
第 1 步：备份旧工程被 CubeMX 可能覆盖的源码（main.c、freertos.c、FreeRTOSConfig.h 等）
第 2 步：用 CubeMX 生成 CMake 外壳（Toolchain = CMake）
第 3 步：手动创建 GCC-ARM/ 文件夹并移动外壳文件
第 4 步：修改所有 CMake/脚本路径（CORE_SOURCE_DIR、工具链路径、build.ps1 工程根）
第 5 步：创建 CMakePresets.json（Coffee2/MilkTea 多 Target）
第 6 步：改造 Application/CMakeLists.txt 支持 TARGET_NAME 选择
第 7 步：给 main.c 加半主机条件编译
第 8 步：创建 CompilerCompat.h 统一宏
第 9 步：用 build.ps1 验证 Coffee2 和 MilkTea 都能编译
第 10 步：验证 Keil 外壳（MDK-ARM）仍然正常编译
```

---

## 九、风险与注意事项

| 风险 | 应对 |
|------|------|
| CubeMX 生成覆盖源码 | 生成前先备份，生成后 diff 对比再合并 |
| 路径改动导致构建失败 | 逐个文件对照修改，用小步验证 |
| GCC 与 ARMCC 行为差异 | 双工具链各编译验证一次 |
| MilkTea 在 GCC 下未验证 | 先只验证 Coffee2，MilkTea 后续补充 |
| Keil 外壳被破坏 | GCC-ARM 与 MDK-ARM 完全隔离，互不影响 |

---

## 十、结论

本方案以**旧工程 `Project_Base` 为基底**，通过：
1. **CubeMX 生成 CMake 外壳**
2. **手动收拢到 `GCC-ARM/` 文件夹**
3. **多 Target + 双编译器兼容处理**

最终实现：**核心源码一份，Keil 外壳（MDK-ARM）与 GCC 外壳（GCC-ARM）各自独立，两套工具链都能编译出 Coffee2 和 MilkTea 固件。**

这正好符合双工具链架构理念——**同一份业务源码，双工具链可复现构建；工具链只是生产固件的手段，不是产品的一部分。**