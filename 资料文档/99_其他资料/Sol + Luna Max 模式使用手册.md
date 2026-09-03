# Sol + Luna Max 模式使用手册

> 适用环境：Codex 桌面应用、Codex CLI 和 Codex IDE 扩展  
> 当前状态：已启用  
> 最后核对日期：2026-08-10

## 1. 模式概述

本模式采用“Sol 决策、Luna 执行、Sol 验收”的协作结构：

- 主 Agent 使用 `gpt-5.6-sol`，负责理解需求、制定方案、拆分任务、判断风险、处理审批和验收结果。
- 自定义子 Agent 使用 `gpt-5.6-luna`，推理强度固定为 `max`，名称为 `luna_max_worker`。
- Luna 只接收目标、范围和验收标准均明确的执行工单；遇到需要重新决策的情况时，必须停止并返回 `NEEDS_SOL`。
- Luna 的完成报告只是验收证据，最终是否完成由 Sol 判断。

该结构的目的，是把需求讨论、架构判断和最终结论保留在主线程，把文件检索、机械修改、测试日志和批量执行等中间过程放到独立的 Agent 线程中。

## 2. 当前安装位置

全局自定义 Agent：

```text
C:\Users\13193\.codex\agents\luna-max-worker.toml
```

全局路由规则：

```text
C:\Users\13193\.codex\AGENTS.md
```

主模型配置：

```text
C:\Users\13193\.codex\config.toml
```

当前主模型保持为：

```toml
model = "gpt-5.6-sol"
model_reasoning_effort = "high"
```

Luna 子 Agent 固定为：

```toml
name = "luna_max_worker"
model = "gpt-5.6-luna"
model_reasoning_effort = "max"
```

## 3. 生效条件

Codex 会在每个新会话启动时读取全局 `AGENTS.md`。安装或修改配置后，需要满足以下任一条件：

1. 新建一个 Codex 任务；
2. 重启 Codex 会话；
3. 在 CLI 中退出当前会话后重新启动。

已启动的旧会话不会自动重新读取全局路由规则。

## 4. Sol 与 Luna 的职责边界

### 4.1 由 Sol 负责

- 需求不明确、存在冲突或需要用户选择；
- 架构设计、技术选型和公共接口定义；
- 根因和解决方向尚不清楚的复杂问题；
- 新增或升级依赖；
- 权限、凭据、安全边界和隐私相关决策；
- 删除、覆盖、发布、部署或外部系统写入；
- 任务拆解、子 Agent 调度、结果整合和最终验收。

### 4.2 可以交给 Luna

- 在指定文件和明确规则下实现代码；
- 机械性、重复性或批量修改；
- 运行构建、测试、格式化和静态检查；
- 收集日志、错误信息、文件清单或调用关系；
- 对明确范围进行只读检索和证据整理；
- 按既定模板更新文档；
- 执行已经由 Sol 决定的修复步骤。

### 4.3 Luna 必须停止并返回 `NEEDS_SOL` 的情况

- 工单目标、范围或完成标准不完整；
- 实际修改会超出允许范围；
- 现有代码或证据与 Sol 的方案冲突；
- 验证失败，并且继续处理需要新的技术决策；
- 需要修改架构、公共接口、依赖、权限或安全边界；
- 需要执行破坏性、不可逆或外部写入操作。

## 5. 路由判定规则

Sol 只有在下列五项全部满足时，才会把任务交给 `luna_max_worker`：

1. 目标没有歧义；
2. 允许读取或修改的范围已经限定；
3. 约束和禁止事项已经明确；
4. 结果可以通过命令或具体证据验证；
5. 不存在尚未解决的架构、产品、安全、权限或破坏性操作决策。

如果任务暂时不满足条件，Sol 会先调查、澄清或拆分任务，条件满足后再委派 Luna。

默认一次只运行一个具有写权限的 Luna。只有彼此独立、输出不冲突的只读任务才适合并行运行。

## 6. 使用方法

### 6.1 自动路由

在新任务中直接描述最终需求即可。Sol 会先判断任务是否适合委派：

```text
检查项目中的构建警告，修复其中边界明确且不会改变公共接口的问题，并运行构建验证。
```

预期工作流：

1. Sol 分析需求和风险；
2. Sol 确定允许修改的文件及完成标准；
3. Sol 向 `luna_max_worker` 下发工单；
4. Luna 执行并返回文件、命令和验证结果；
5. Sol 检查修改和验证证据；
6. Sol 向用户提交最终报告。

### 6.2 明确指定 Luna

需要强制检查是否可委派时，可以这样写：

```text
请由 Sol 先确定边界，然后使用 luna_max_worker 完成以下任务：
检查指定目录内的重复代码，并只修改我明确允许的文件。
等待 Luna 完成后，由 Sol 验收并汇总结果。
```

明确指定 Luna 不会绕过边界检查。如果任务仍然含糊，Sol 会先完成拆解，不会直接让 Luna 猜测。

### 6.3 只让 Sol 工作

单次任务不想使用 Luna 时，在请求中明确写：

```text
本任务禁止调用 Luna 或其他子 Agent，全部由 Sol 完成。
```

## 7. 标准 Luna 工单模板

Sol 委派 Luna 时应使用以下结构：

```text
执行者：luna_max_worker

目标：
[描述唯一、可验证的结果]

允许读取范围：
[目录、文件或系统]

允许修改范围：
[精确文件或目录；只读任务写“禁止修改”]

约束和禁止事项：
[不可修改的接口、依赖、行为和文件]

验证方式：
[构建、测试、检查命令或证据要求]

完成标准：
[满足哪些条件才算完成]

以下情况返回 NEEDS_SOL：
[列出需要 Sol 重新决策的条件]
```

## 8. Luna 的返回报告格式

Luna 完成工作后应返回：

```text
结果：成功 / 部分完成 / NEEDS_SOL
修改或检查的文件：
执行的命令和工具：
验证结果：
剩余风险或注意事项：
需要 Sol 决定的问题：
```

Sol 必须检查相关 diff、产物和测试结果，确认没有越界修改后才能宣布任务完成。

## 9. 查看和管理 Agent 线程

### Codex 桌面应用

任务触发子 Agent 后，可以在主任务中看到子 Agent 活动。打开对应 Agent 线程可以检查进度和返回结果。

### Codex CLI

在交互式 CLI 中使用：

```text
/agent
```

可以查看和切换活动中的 Agent 线程。也可以直接要求 Sol 停止、引导或关闭指定子 Agent。

## 10. 权限与安全规则

- 子 Agent 继承父任务当前的沙箱和审批设置；
- 父任务是只读模式时，Luna 也不能修改文件；
- 需要新审批但当前工作流无法展示审批时，操作会失败并返回主线程；
- Luna 不得自行执行不可逆删除、外部发布、部署或安全边界变更；
- 多个写入型 Agent 不应同时修改同一工作区；
- 用户现有的未提交修改必须保留，不得擅自覆盖或回退。

## 11. 启用、暂停与关闭

### 11.1 启用当前模式

确认以下两个文件存在，然后新建任务或重启 Codex：

```text
C:\Users\13193\.codex\agents\luna-max-worker.toml
C:\Users\13193\.codex\AGENTS.md
```

Codex 的多 Agent 功能默认启用，因此当前配置不需要额外设置 `[agents].enabled`。

### 11.2 暂停自动路由

最简单的单次暂停方式，是在任务中明确要求只使用 Sol。

需要全局临时暂停时，可以创建：

```text
C:\Users\13193\.codex\AGENTS.override.md
```

并写入：

```markdown
# Temporary override

Do not delegate work to `luna_max_worker`. Complete all work in the main Sol agent.
```

非空的 `AGENTS.override.md` 会临时取代同目录下的 `AGENTS.md`。移除该覆盖文件并新建任务，即可恢复本模式。

### 11.3 仅关闭 Luna Max 模式

1. 删除 `AGENTS.md` 中从 `BEGIN SOL_LUNA_ROUTING` 到 `END SOL_LUNA_ROUTING` 的路由区块；
2. 将 `luna-max-worker.toml` 改名为 `luna-max-worker.toml.disabled`，或移出 `agents` 目录；
3. 新建任务或重启 Codex。

### 11.4 关闭全部子 Agent

在 `C:\Users\13193\.codex\config.toml` 中加入：

```toml
[agents]
enabled = false
```

保存后新建任务或重启 Codex。此设置会关闭全部多 Agent 工具，不仅是 Luna。

恢复时将其改为：

```toml
[agents]
enabled = true
```

## 12. 常见问题

### 安装后为什么当前任务没有自动调用 Luna？

`AGENTS.md` 在会话启动时读取一次。配置完成前就已经启动的任务不会动态刷新，需要新建任务或重启会话。

### 为什么 Luna 返回 `NEEDS_SOL`？

这表示 Luna 发现任务需要新的决策、存在越界风险或无法按工单验证。应由 Sol 重新分析并下发更精确的工单，而不是要求 Luna 猜测。

### 为什么没有并行启动多个 Luna？

多个写入型 Agent 同时修改一个工作区容易发生冲突。本模式默认只运行一个写入型 Luna，只读且互不依赖的任务才考虑并行。

### Luna Max 是否一定比 Medium 更快？

不一定。`max` 会增加推理时间和 Token 使用量，适合对执行质量要求较高的任务。本模式按照项目约定固定使用 Luna Max，而不是以最低延迟为目标。

### 如何确认使用的是 Luna Max？

检查 Agent 线程信息，或核对自定义 Agent 文件中的：

```toml
model = "gpt-5.6-luna"
model_reasoning_effort = "max"
```

## 13. 官方参考

- [OpenAI：Subagents 与自定义 Agent 配置](https://learn.chatgpt.com/docs/agent-configuration/subagents)
- [OpenAI：使用 AGENTS.md 配置持久指令](https://learn.chatgpt.com/docs/agent-configuration/agents-md)
- [OpenAI：GPT-5.6 Luna 模型](https://developers.openai.com/api/docs/models/gpt-5.6-luna)

