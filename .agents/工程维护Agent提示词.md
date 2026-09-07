# Project_Base 工程维护 Agent 提示词

> 适用对象：维护 `D:\Project_Items\Project_Base` 中 Coffee2 固件的 Codex Agent。
> 本文件只记录项目事实、保护边界和验收触发条件。模型选择与子代理路由由全局
> Codex `AGENTS.md` 管理，不在这里重复。

## 范围与事实源

- 当前正式产品范围是 Coffee2；MilkTea 仅作 `OUT_OF_SCOPE` 历史证据。
- CubeMX 维护输入只包括：
  - `CubeMX_Base/CubeMX_Genarate/CMAKE/F407Base_CMAKE.ioc`
  - `CubeMX_Base/CubeMX_Genarate/MDK/F407Base_MDK.ioc`
- `CubeMX_Base/F407Base.ioc` 已废弃且当前不存在。不得读取、恢复、编辑或把它
  作为当前事实源。
- 架构边界以 `AGENTS.md`、
  `资料文档/00_README/当前工程架构与公共私有边界.md` 和源码/工程配置为准。
- 快照与维护触发项见 `资料文档/00_README/工程基础缓存.md`；详细证据见
  `资料文档/全局审查.md`；每次文件变更追加 `CHANGES.md`。

## 不变量

- 公共源码保持一套正式来源。Keil 与 GCC 只拥有各自的启动、链接、构建描述和
  编译器适配，不复制公共业务、HAL、FreeRTOS 或 lwIP 源码。
- `Application/UserAPP/Coffee2App` 拥有 Coffee2 业务流程、设备/协议选型、静态
  实例、总线绑定、任务编排、状态投影、日志 source 和 Server 寄存器语义。
- `Application/Common`、`Transport`、`ProtocolStack`、`DeviceLibrary` 和
  `New_Party` 保持 target-neutral。除启动组合适配器 `CommonTargets.h` 外，公共
  层不得引用 UserAPP。
- Target 通过编译期源文件和私有静态配置组合公共模块，不引入运行时插件、动态
  注册、无证据的新任务/队列或堆分配。
- 保持 ARM Compiler V5.06、C90/C99 和 GCC 兼容。保留所有 USER CODE 标记。
- ISR 不阻塞、不延时、不调用非 FromISR API。DMA 缓冲只能位于 DMA 可访问 SRAM；
  CCM 堆和任务栈不得直接交给 DMA。

## 操作边界

- 未经用户明确授权，不编辑 `.ioc`、不运行 CubeMX、不覆盖生成物、不烧录。
- 不批量删除、清理或回退无关用户改动；先检查工作树并缩小改动范围。
- 历史日志必须标注 `HISTORICAL`，不能作为当前构建结论。
- 真实 BOM、硬件行为、Bootloader 搬运和实机时序在缺少证据时标为 `UNKNOWN`。
- 发现公共接口、架构、依赖、权限、安全边界或目标产品需要改变时，由根 Agent
  决策后再实施。

## 工作方式

1. 阅读根 `AGENTS.md`、本文件、工程基础缓存、`CHANGES.md` 近期相关记录及任务
   涉及的源码/配置。
2. 检查工作树并记录相关文件的当前路径、关键宏和必要哈希；保留无关改动。
3. 从当前源码、CMake、Keil 工程和两份正式维护 IOC 建立证据，不从旧报告推断
   当前状态。
4. 先确定最小改动和影响范围，再编辑；新增事实给出文件、符号或宏和行号。
5. 按下表选择验证，不为文档或 Codex 配置变更运行固件全量构建。
6. 更新 `CHANGES.md`；触发架构/配置快照时同步更新工程基础缓存和全局审查。

## 按影响范围验证

| 变更 | 必需验证 |
| --- | --- |
| 文档、Codex/Agent/Skill 配置 | Markdown/TOML/Frontmatter、链接、UTF-8、末尾换行、冲突短语检查 |
| Coffee2 私有 C/H | 受影响 Coffee2 Target 的编译/构建和聚焦静态检查 |
| 公共源码或公共接口 | 每个受影响 Target 与工具链；记录兼容性说明 |
| CMake/Keil 构建图、启动、链接 | GCC Coffee2 Debug/Release 与实际受影响的 Keil Target；跨产品共享变化才扩展到其他 Target |
| CCM、DMA、FreeRTOS、lwIP、链接描述 | 构建、map/地址、水位、生命周期和 DMA 可达性核对 |
| CubeMX 生成迁移 | 仅在用户授权的专门流程中，对两份维护 IOC 和生成差异执行检查 |
| 烧录或实机 | 仅在明确授权并确认产品、固件和硬件后执行 |

只有当改动实际影响 GCC 四个 Preset 与 Keil 两个 Target，或用户明确要求全矩阵
验收时，才执行原有六构建矩阵。验证失败时保留证据，不通过删除源文件或关闭模块
制造“通过”。

## 交付

先说明结果和实际验证，再列出改动文件、原因、未执行项目及其理由、剩余
`UNKNOWN/CONFLICT`。明确说明是否构建、是否烧录。输出保持简洁，不要求固定称呼
或模板，也不要求把本文件全文复制进每个任务。
