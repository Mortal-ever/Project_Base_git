# Coffee2 独立日志说明

> 状态标签：CONFIRMED / HISTORICAL / UNKNOWN / CONFLICT / OUT_OF_SCOPE
> 编写日期：2026-09-01

---

## 1. Coffee2 基本信息

CONFIRMED（来源：`coffee2_manager.c`、`coffee2_log.c`、工程配置）。

| 项 | 值 |
| --- | --- |
| Keil Target 名称 | `Coffee2`（`MDK-ARM/STM32F407_Base.uvprojx`，CONFIRMED） |
| GCC/CMake target 名称 | `Coffee2Target`；应用库 `coffee2_app`（`Application/CMakeLists.txt`） |
| Coffee2 入口任务 | `vCoffee2WorkflowTask` 等，日志任务 `vCoffee2LogTask`（`coffee2_manager.c:315`） |
| 日志初始化位置 | `coffee2_manager.c` 的 `xAppTaskManagerCreateTasks()` 内 `xCoffee2LogInitWithTransport()`（CONFIRMED，manager 启动流程） |
| 日志输出通道 | USART1，`transport_uart` |
| 日志任务 | `C2Log`，栈 256，优先级 idle+2 |
| 日志队列 | 静态 ring 32 条，`APP_CCM_DATA` |
| 当前编译器 | Keil ARMCC V5.06 / GCC arm-none-eabi |
| ARMCC V5.06 | 目标要求 0 Error/0 Warning（历史验收要求） |
| GCC 状态 | 同源构建；当前工作区存在构建进度（见第一份文档附录 CONFLICT） |

---

## 2. Coffee2 如何接入公共日志

CONFIRMED。

- **使用哪些公共 API**：`xAppLogInit/Write*`、`vAppLogTask`、`vAppLogSetTaskReady`、`vAppLogGetStatus`、`lAppLogEarlyWrite`。Coffee2 适配层在 `coffee2_log.c` 中封装。
- **私有适配层**：`coffee2_log.c` 保存 source 前缀表 (`s_axCoffee2LogSources`)、创建 USART1 Transport、`vCoffee2LogTask` 包一层后调用 `vAppLogTask`。
- **source 定义**：`coffee2_log.h:56-76` 的 `Coffee2LogSource_e`（18 个）。
- **设备名称**：source 前缀表提供（如 `Coffee`、`Cup`）。设备 ID 由 `coffee2_device.h` 定义，日志通过 `device=` 字段携带。
- **IO 名称**：`coffee2_io_names.h` 的 `COFFEE2_IO_NAME_*` 宏（CONFIRMED，如 `DI1_OUTLET_DOOR_UPPER_LIMIT`）。
- **机器人动作名**：`coffee2_robot_tcp.c` 的 `prvRobotActionName()`（CONFIRMED，`ROBOT_ACTION_START=%s`）。
- **公共层是否引用 Coffee2App**：公共 `app_log.c` **不含** `coffee2_*` 头文件（CONFIRMED，`app_log.c:16-22` 只 include Log/app_log.h、compiler_compat.h、semphr.h、task.h）。
- **边界是否正确**：公共核心 target-neutral、不引用 Coffee2App；Coffee2 适配层位于私有 `UserAPP/Coffee2App/Comm_Log`。CONFIRMED。唯一例外 `Common/CommonTargets.h` 是启动期组合适配器（AGENTS.md 认可）。

---

## 3. Coffee2 具体模块日志清单

### 3.1 Workflow

| 事件 | 状态 | 说明 |
| --- | --- | --- |
| `MACHINE_INIT_BLOCKED` | CONFIRMED | `coffee2_workflow.c:414`，去重输出 |
| `MACHINE_INIT_COMPLETE` | CONFIRMED | `coffee2_workflow.c:436` |
| `WORKFLOW_STEP_START/DONE/DEVICE_FAILED/TIMEOUT/CANCELED` | CONFIRMED | `coffee2_workflow.c:663,739,774` 等 |
| 初始化失败重复 | 已去重 | `lLastInitError`，同错误值不重复（CONFIRMED） |
| 设备恢复工作流 | UNKNOWN | 未在本轮行程中定位到独立恢复事件 |

### 3.2 Robot

| 事件 | 状态 | 说明 |
| --- | --- | --- |
| `ROBOT_ACTION_START=%s` | CONFIRMED | `coffee2_robot_tcp.c:972`，用 `prvRobotActionName` |
| `ROBOT_ACTION_COMPLETE` | CONFIRMED | `coffee2_robot_tcp.c:1714,2008`，含 `action` 字段 |
| `ROBOT_ACTION_COMPLETE_SIGNAL` | CONFIRMED | `coffee2_robot_tcp.c:1918,1961`，含 `coil` |
| `ROBOT_ACTION_SENT/ACCEPTED/MOVING/RESULT_CLEARED` | CONFIRMED | 源码存在（Coffee2_完整业务流程设计.md 记录） |
| TCP 连接/断开 | CONFIRMED | `ROBOT_TCP_CONNECTED`、`ROBOT_DISCONNECTED:NETWORK_DOWN` 等 |
| 动作名清晰 | 已实现 | `prvRobotActionName` 提供英文名 |

### 3.3 Modbus RTU

| 事件 | 状态 | 说明 |
| --- | --- | --- |
| `BUS_COMMAND_FAILED` | CONFIRMED | `coffee2_rtu_bus.c:578`，去重，含 `device` |
| `RTU_DEVICE_ONLINE/OFFLINE` | CONFIRMED | `coffee2_rtu_bus.c` |
| 设备命令发送 | CONFIRMED | `DEVICE_PROTOCOL:XXX`、`DEVICE_LINK:XXX` 启动绑定日志 |
| 超时/CRC/异常区分 | UNKNOWN | 未单独确认 CRC 事件；结果码 -4/-6 区分 |

### 3.4 Modbus TCP Server

| 事件 | 状态 | 说明 |
| --- | --- | --- |
| `SERVER_LISTENING` | CONFIRMED | `coffee2_server.c:359`，含 `port` |
| `SERVER_LISTEN_FAILED` | CONFIRMED | `coffee2_server.c:347`，含 `native_error` |
| `SERVER_CLIENT_CONNECTED/DISCONNECTED` | CONFIRMED | 源码存在（历史手册记录） |
| 非法功能码/寄存器/值 | PARTIAL | 出现 `OTA_HTTP_REJECTED` 等；完整寄存器语义未枚举 |
| 请求来源 IP/端口 | CONFIRMED | CHANGES.md:756 记录已增加对端 IP 与端口 |

### 3.5 IO State

| 事件 | 状态 | 说明 |
| --- | --- | --- |
| `IO_STATE_CHANGED NAME=%s %u->%u` | CONFIRMED | `coffee2_io.c:184,257`，边沿触发 |
| 首次采样基线 | CONFIRMED | 注释声明首次仅建基线 |
| 点位英文名 | CONFIRMED | `coffee2_io_names.h` |

### 3.6 OTA

| 事件 | 状态 | 说明 |
| --- | --- | --- |
| `OTA_HTTP_BEGIN/COMMIT/ABORT` | CONFIRMED | `app_ota_flash.c:236,350,358` |
| `OTA_HTTP_FLASH_FAILED` | CONFIRMED | `app_ota_flash.c:183,224,229,255...` |
| `OTA_HTTP_CRC_FAILED` | CONFIRMED | `app_ota_flash.c:302,307` |
| `OTA_HTTP_REJECTED` | CONFIRMED | `app_ota_http.c:825,835,848,856` |
| `OTA_HTTP_TRIGGER` | CONFIRMED | `coffee2_server.c:857` |
| 阶段定位 | 已具备 | 事件名区分阶段/原因 |

---

## 4. 日志示例（当前工程风格）

以下示例基于实际接口与事件名，前缀格式 `[<seq>LEVEL][Task:Module] EVENT result=N field=V`（CONFIRMED）。

```text
[0001INFO][C2Workflow:IO] IO_STATE_CHANGED NAME=DI1_OUTLET_DOOR_UPPER_LIMIT 0->1
[F123WARN][C2Bus5:IoOutput] BUS_COMMAND_FAILED result=-4 device=10
[1234INFO][C2Robot:MBTcpClient] ROBOT_ACTION_START=ROBOT_HOME
[1234WARN][C2Robot:MBTcpClient] ROBOT_ACTION_TIMEOUT result=-2 action=114
[1234ERROR][C2Bus2:Coffee] CUP_MACHINE_INIT_FAILED result=-4
[1234INFO][C2Server:MBTcpServer] SERVER_LISTENING result=0 port=6001
```

状态说明：
- `IO_STATE_CHANGED`：**已实现**（`coffee2_io.c:184`）。
- `BUS_COMMAND_FAILED`：**已实现**（`coffee2_rtu_bus.c:578`）。
- `ROBOT_ACTION_START=ROBOT_HOME`：**已实现**（`coffee2_robot_tcp.c:972`，`prvRobotActionName`）。
- `ROBOT_ACTION_TIMEOUT`：**已实现/部分**（存在 `WORKFLOW_STEP_TIMEOUT` 等；具体 `ROBOT_ACTION_TIMEOUT` 需核 action 名）。
- `CUP_MACHINE_INIT_FAILED`：**非现有确切事件**（建议风格示例，需与 workflow/rtu_bus 实际事件对齐）。
- `SERVER_LISTENING`：**已实现**（`coffee2_server.c:359`）。

---

## 5. 接入状态汇总

- 公共层不引用 Coffee2App：CONFIRMED。
- Coffee2 已完整接入公共日志 ring + USART1 Transport + 独立崩溃 port：CONFIRMED。
- 崩溃日志：见《崩溃死机日志机制说明》，Coffee2 强绑定 `lAppCrashDiagWrite`。
- 历史基线：MilkTea 时期 `死机日志管理与排障说明.md` 为 `OUT_OF_SCOPE` / `HISTORICAL`。

*结束。*
