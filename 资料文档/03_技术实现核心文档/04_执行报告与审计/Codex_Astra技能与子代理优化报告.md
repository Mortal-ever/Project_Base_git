# Codex Astra 技能与子代理优化报告

## 1. 结论

2026-09-07 已完成当前 Codex 配置、全局 Agent 路由、嵌入式 Skills 和本工程维护
提示词的 Astra 适配。工具能力本身保持不变；本次消除了“配置选择 Astra，但全局
规则仍把 GPT-5.6 Sol 定义为根 Agent”的冲突，并将委派从强制流程改为按收益和风险
触发。

本次没有修改 Coffee2 固件源码、CMake/Keil 工程、链接文件、CubeMX IOC 或协议
文档，没有执行固件构建、CubeMX、烧录或实机操作。

## 2. 修改前问题

| 问题 | 修改前状态 | 风险 |
| --- | --- | --- |
| 根模型冲突 | `config.toml` 选择 `gpt-6-astra`，全局 `AGENTS.md` 强制 root Sol | 复杂任务可能从 Astra 降级给 GPT-5.6 |
| 推理档位 | Astra 默认 `low` | 固件架构、协议和并发任务的推理余量偏小 |
| 委派策略 | 几乎所有实质任务必须使用子代理 | 增加启动、等待、上下文传递和容量失败成本 |
| 专家角色 | `sol_high_specialist` 被定义为最高难题升级路径 | Astra 根模型存在时属于能力倒挂 |
| Skill 触发 | `embedded-dev` 覆盖过宽且包含通用模板；STM32 Skill 强制称呼和固定交付话术 | 增加无关上下文并干扰用户表达偏好 |
| 项目提示词 | 约 18.8 KB，重复全局路由，所有维护倾向六构建 | 小任务过度验证，重复指令增加注意力成本 |
| IOC 事实源 | 项目提示词仍要求读取已废弃 `CubeMX_Base/F407Base.ioc` | 与仓库 `AGENTS.md` 冲突 |
| 凭据卫生 | 全局配置保留一段被注释的密钥样式文本 | 注释内容仍可能进入备份或诊断输出 |

## 3. 已完成修改

### 3.1 全局模型配置

文件：`C:\Users\13193\.codex\config.toml`

- 保持根模型为 `gpt-6-astra`。
- 默认推理档位从 `low` 调整为 `medium`。
- 保持 `service_tier = "default"`，不引入额外费用策略变化。
- 删除被注释的密钥样式文本，保留环境变量登录示例。
- 保持现有插件、MCP、Windows sandbox 和项目可信设置不变。

### 3.2 全局 Agent 路由

文件：`C:\Users\13193\.codex\AGENTS.md`

- 根 Agent 改为“当前配置模型拥有最终决策权”；使用 Astra 时，架构、复杂诊断、
  权限、风险、集成和最终验收留在根 Astra。
- 默认推理分级为 `low / medium / high / xhigh或max`，明确 Astra 不使用 `none`。
- 删除 Terra 占比目标和“实质任务必须委派”规则。
- 委派只在并行收益、根上下文隔离、确定性批处理、普通实现或独立审查确有价值时
  启用。
- 子代理容量不足时，根 Agent 可继续安全范围内工作，不把可选委派失败视为阻塞。
- 保留完整 work order、单写入 Agent、独立审查和根验收约束。

### 3.3 子代理

文件：`C:\Users\13193\.codex\agents\sol-high-specialist.toml`

- 保留角色名，避免破坏已有调用方。
- 将其定义为兼容性 fallback：只有 Astra 不可用、根模型不是 Astra，或明确要求
  GPT-5.6 Sol 独立意见时使用。
- Terra/Luna 角色和模型保持不变：Terra 承担边界明确的普通实现，Luna 承担搜索、
  批处理和构建输出等确定性工作。

### 3.4 Skills

文件：

- `C:\Users\13193\.codex\skills\embedded-dev\SKILL.md`
- `C:\Users\13193\.codex\skills\stm32-keil-v5\SKILL.md`
- `C:\Users\13193\.codex\skills\stm32-keil-v5\references\style-and-naming.md`
- `C:\Users\13193\.codex\skills\stm32-keil-v5\references\project-safety.md`

`embedded-dev` 现在只在实质性嵌入式工程工作中触发，按任务加载对应 reference，删除
了可由 Astra 自行生成的通用代码模板和固定回答模板。保留资源预算、确定性、ISR、
DMA、静态所有权、最小架构和证据分级等真正改变工程决策的约束。

`stm32-keil-v5` 保留 ARM Compiler V5.06、C90/C99、USER CODE、HAL/LL、FreeRTOS、
CCM/DMA 和受保护生成物约束；入口和支持 reference 均删除强制“主人”称呼、固定
完成话术及对普通元任务的过宽触发。验证改为按影响范围选择。

### 3.5 Project_Base 维护提示词

文件：`.agents/工程维护Agent提示词.md`

- 从约 18.8 KB 缩减为约 4.5 KB。
- 只保留 Coffee2 事实源、公共/私有边界、安全不变量和验证触发条件。
- 明确两份 `CubeMX_Base/CubeMX_Genarate/...` IOC 是唯一维护输入。
- 明确废弃的 `CubeMX_Base/F407Base.ioc` 不得读取、恢复或使用。
- 六构建矩阵只在改动实际影响全部 GCC Preset 和两个 Keil Target，或用户明确要求
  时执行。
- 文档、Codex/Agent/Skill 配置只执行语法、链接、编码和冲突检查。
- 删除每次完整复制维护提示词和固定交付话术的要求。

## 4. 优化后路由

| 工作类型 | 默认执行者 | 推荐推理 |
| --- | --- | --- |
| 简短事实、路径查询、机械操作 | 根 Astra 或 Luna | low |
| 普通仓库维护 | 根 Astra | medium |
| 边界明确的普通实现 | Terra worker，根 Astra 验收 | high（Terra） |
| 大批量确定性改动或构建输出 | Luna executor/batch | xhigh（Luna） |
| 架构、协议、并发、性能、疑难诊断 | 根 Astra | high/xhigh |
| 高风险独立复核 | Terra reviewer；必要时根 Astra 再审 | high |
| Astra 不可用时的复杂分析 | Sol specialist fallback | high |

## 5. 验证结果

| 检查 | 结果 |
| --- | --- |
| 全局 `config.toml` 与 5 个 agent TOML 解析 | `PASS`，共 6 个 TOML |
| `embedded-dev` quick_validate | `PASS` |
| `stm32-keil-v5` quick_validate | `PASS` |
| 旧 root Sol、强制委派、Terra 占比、固定称呼、密钥样式配置检查 | `PASS`，目标内容已移除 |
| Astra 模型/默认推理检查 | `PASS`：`gpt-6-astra` / `medium` |
| 正式 CMake IOC SHA-256 | 未变化：`F88F869FCF75943835F8D851F37134B60287C02D27DE7E4F0538193D49570FF5` |
| 正式 MDK IOC SHA-256 | 未变化：`0E1298E9C16EE3636C5C056D137CD13104846890374329DF061AA2FD4EFDAD1C` |
| 固件构建、Keil、CubeMX、烧录 | `NOT_RUN`，配置/文档变更不影响固件构建图 |

修改后关键文件 SHA-256：

| 文件 | SHA-256 |
| --- | --- |
| 全局 `config.toml` | `848DEB079861F0B9F6C61098728CE4A558C62A274E9FAAA917D3243AF8BFEC50` |
| 全局 `AGENTS.md` | `9C751345BFD1BF65ADBD9C7A1B740659800AB337C5DD8BE9890C331D343B6AA5` |
| `sol-high-specialist.toml` | `060042495BAF73F0060CE992CC1724C72BEC2EAED6277F565915926D57F439B9` |
| `embedded-dev/SKILL.md` | `16E6AECDDEF840D33AF5B81484C0C0846554F355B8F46A356F138106A8E943DF` |
| `stm32-keil-v5/SKILL.md` | `BA8850A7E5E23D238F70FAE167080E8D7370D6F21DD0E2FC024F64A3380C16FA` |
| `stm32-keil-v5/references/style-and-naming.md` | `C21B6CCD317D86C8960217D453E7EA51934D33968C42CD5CC1A3E5F71D5C88AC` |
| `stm32-keil-v5/references/project-safety.md` | `AF138E3CB3084CB9E2E95A19206458D09C59DD84FFB1A888411DD0B85DF95F8D` |
| 项目维护提示词 | `92F79A3A811D3BEED230B78B1435D27F815FEA3939B78FA32910E1B3928BC8AE` |

## 6. 生效与限制

- 全局模型、Agent 和 Skill 文件已经落盘。为了确保 Codex 重新载入全部系统指令和
  自定义角色，建议从下一条新任务或重启 Codex 后按新规则使用；当前正在运行的任务
  可能仍持有启动时加载的旧指令快照。
- 本轮没有切换到 `max/ultra`，也没有启用自动委派实验。默认 `medium` 配合按需
  `high/xhigh` 更适合当前固件维护场景。
- 被删除的密钥样式文本是否曾对应有效凭据无法从本地确认；本报告只确认它已从当前
  配置移除。若它曾有效，仍需在对应服务端撤销或轮换。
- 未安装的推荐插件不会参与当前上下文，本轮未安装、删除或重配插件。
