| Date | File | Action | Description |
| --- | --- | --- | --- |
| 2026-09-03 | `资料文档/02_工具链说明/Git工具链/Git使用与GitHub维护说明书.md`; `CHANGES.md` | Add/Modify | 新增当前 Project_Base Git/GitHub 初始化、上传、维护、冲突、缓存忽略、LFS 和故障排查说明。 |
| 2026-09-03 | `.gitignore`; `CHANGES.md` | Modify | 以恢复后的 Project_Base 本地内容重新建立独立 Git 仓库，补充 Codex/Agent、GCC、Keil 临时文件忽略规则；准备覆盖远程错误版本。 |
| 2026-08-29 | MDK-ARM/Objects/Coffee2/Coffee2_cache_audit_20260829.log | Verify | 使用 ARM Compiler V5.06u7 对当前 Coffee2 Target 执行全量 Rebuild：Code=191044、RO-data=5824、RW-data=532、ZI-data=150436，0 Error(s)、0 Warning(s)；未烧录。 |
| 2026-08-29 | AGENTS.md; 资料文档/00_README/工程基础缓存.md; 资料文档/00_README/00_README_索引.md; 资料文档/全局审查.md | Add/Modify | 建立 Coffee2 基础缓存与持久维护规则，固化 UserAPP 私有设备/协议选型与公共能力边界，明确 CommonTargets.h 是唯一启动组合适配器，并将带 MilkTea 语义的 DeviceModel/IO_State 标为 OUT_OF_SCOPE（legacy 遗留实现）；修订 MCU/Flash 冲突、启动任务、epoch 身份匹配、GCC 构建图冲突及历史证据标记；未烧录。 |
| 2026-08-28 | Application/DeviceProtocol/CoffeeMachine/f200_protocol.c/.h; Application/UserAPP/Coffee2App/Modbus_Rtu_Bus/coffee2_rtu_bus.c; Application/UserAPP/Coffee2App/Device/coffee2_device_image.c; MDK-ARM/STM32F407_Base.uvprojx | Add/Modify | Route Coffee2 F200 commands through the stateless public protocol entry, project returned status into the device image with temporary legacy synchronization, and include the entry in the Coffee2 Keil target. |
| 2026-08-27 | Application/Transport/Inc/transport_tcp.h | Modify | Add an optional per-client TCP connect deadline while preserving legacy blocking behavior when zero. |
| 2026-08-27 | Application/Transport/Src/transport_tcp.c | Modify | Implement bounded nonblocking Netconn connection attempts so a missing late-start server returns control to the application retry loop. |
| 2026-08-27 | Application/UserAPP/Coffee2App/Config/coffee2_app_config.h | Modify | Set the Coffee2 robot TCP connect attempt deadline to 3000 ms. |
| 2026-08-27 | Application/UserAPP/Coffee2App/Robot_Tcp/coffee2_robot_tcp.c | Modify | Add connect timing and protocol-layer diagnostic logs and ensure each failed bounded attempt enters the existing infinite backoff cycle. |
| 2026-08-27 | GCC-ARM Coffee2 Debug/Release | Verify | Both presets rebuilt successfully with the bounded Robot connect implementation; Debug FLASH 176344 B and Release FLASH 210760 B. |
| 2026-08-27 | MDK-ARM Coffee2 | Verify | ARM Compiler V5.06u7 build and link completed successfully; Code 197740 B, RO-data 5628 B, RW-data 560 B, ZI-data 141848 B. |
| 2026-08-21 | 资料文档/03_技术实现核心文档/店中店咖啡机工程核心实现资料/01_架构设计/Coffee2_架构总览（以源码为基准）.md | Rewrite | 以当前 Coffee2 源码重写唯一权威架构入口，补齐系统边界、技术栈、启动链、任务所有权、命令身份、Bus/设备绑定、公共设备库、Server、Workflow、机器人严格握手与恢复、IO 写后读回、OTA/Flash、公共日志、LwIP/崩溃诊断、双工具链、内存数据、完整调用链、源码阅读顺序及优缺点评价；明确区分已实现、待验证和未实现能力。 |
| 2026-08-13 | Application/UserAPP/Coffee2App/Robot_Tcp/coffee2_robot_tcp.c; Application/UserAPP/Coffee2App/Device/coffee2_device.c/.h; Application/UserAPP/Coffee2App/Modbus_Tcp_Server/coffee2_server.c; Application/UserAPP/Coffee2App/WorkFlow/coffee2_workflow.c; Application/UserAPP/Coffee2App/Config/coffee2_app_config.h | Finalize | Add strict command=0 acceptance with 100 ms owner polling, 60-second post-accept motion budget, edge-scoped PREPARED/SENT/ACCEPTED/COMPLETE_SIGNAL/RESULT_CLEARED logs, static command terminal history and manual supersession, latest-order replacement, urgent Robot base-command pass-through while startup is pending, indefinite transaction reconciliation across reconnect, and four-preset validation (Coffee2 Debug RAM 98,792/CCM 40,872/FLASH 224,704; Coffee2 Release RAM 98,832/CCM 40,872/FLASH 188,148; MilkTea Debug/Release exit 0). |
| 2026-08-13 | 资料文档/03_技术实现核心文档/店中店咖啡机工程核心实现资料/03_调试与测试/Coffee2_机器人单步闭环握手与日志排障说明.md | Add | Define the Coffee1 v2.7.23-based Robot single-step handshake blueprint, indefinite 100 ms command-accept polling, 60-second post-accept motion timing, reconnect reconciliation, action-number mapping, target log dictionary, and Keil Watch diagnostics without extending coils 3119, 3120, or 3139 |
| 2026-08-13 | Application/UserAPP/Coffee2App/Robot_Tcp/coffee2_robot_tcp.c | Fix | Distinguish local action-deadline timeout from confirmed TCP/Modbus loss; retain connected manual/workflow sessions on action timeout, reconcile workflow transactions on the same connection, and confirm timeout/protocol anomalies with at most two bounded FC01 coil-3100 probes using the existing Modbus fault snapshot before reconnecting |
| 2026-08-12 | Application/UserAPP/Coffee2App/Robot_Tcp/coffee2_robot_tcp.c | Fix | Adopt Coffee1 Robot semantics: FC02 base status refresh, FC01 control/status refresh, direct 200 ms startup order with recovery-safe STOP skips, maintained operational-state gating, warm attach and one-shot power-signal diagnostics, dedicated 3120 start signal, and action/result reconciliation without clearing 3100 or 3120 |
| 2026-08-12 | Application/UserAPP/Coffee2App/Robot_Tcp/coffee2_robot_tcp.c | Fix | Before recovery-safe startup, read both saved command and result coils; result=1 reconciles, command=1/result=0 waits, and only valid command=0/result=0 starts. Add edge-scoped ROBOT_ACTION_SENT logging |
| 2026-08-12 | Application/UserAPP/Coffee2App/Robot_Tcp/coffee2_robot_tcp.c | Fix | Preserve Coffee1 START_SIGNAL value when writing dedicated coil 3120; normalize recovery block braces without changing ordering safeguards |
| 2026-08-12 | Application/UserAPP/Coffee2App/Robot_Tcp/coffee2_robot_tcp.c | Fix | Add an 8-second bounded post-sequence startup wait using FC02/FC01 refresh polls; successful maintained operational feedback completes startup, while timeout preserves the connected session and uses existing retry/backoff |
| 2026-08-12 | Application/UserAPP/Coffee2App/Config/coffee2_app_config.h | Verify | Preserve the existing `COFFEE2_ROBOT_TASK_STACK=1024U` setting; include the unchanged config in the v2 synchronization package so deployments do not regress to the reported 512-word stack |
| 2026-08-12 | GCC-ARM Coffee2-Debug | Verify | Rebuilt after Coffee1 semantics update; RAM 98,792 B, CCMRAM 39,320 B, FLASH 218,340 B |
| 2026-08-12 | Application/UserAPP/Coffee2App/Robot_Tcp, Device, WorkFlow, Modbus_Tcp_Server, Config | Modify | Add permanent capped-backoff Robot reconnect, strict online/ready gating, recoverable workflow transaction reconciliation with safe single replay, result reset handshake, recovery timeout/cancel handling, and manual-command admission checks; preserve existing host registers |
| 2026-08-12 | GCC-ARM Coffee2-Debug | Verify | Configure/build passed with bundled arm-none-eabi GCC; RAM 98,792 B, CCMRAM 39,320 B, FLASH 216,900 B |
| 2026-08-12 | GCC-ARM Coffee2-Release | Verify | Configure/build passed with bundled arm-none-eabi GCC; RAM 98,832 B, CCMRAM 39,320 B, FLASH 181,664 B |
| 2026-08-12 | GCC-ARM MilkTea-Debug | Verify | Configure/build passed with bundled arm-none-eabi GCC; RAM 104,848 B, CCMRAM 32,768 B, FLASH 168,988 B |
| 2026-08-12 | GCC-ARM MilkTea-Release | Verify | Configure/build passed with bundled arm-none-eabi GCC; RAM 104,896 B, CCMRAM 32,768 B, FLASH 147,344 B |
| 2026-08-12 | MDK-ARM Coffee2/MilkTea | Verify | Keil UV4/ARMCC V5.06 executable and established safe build command were unavailable in this environment; no Keil build was run and no project files were modified |
| 2026-08-01 | 资料文档/店中店咖啡机工程核心实现资料/执行报告/COFFEE1_工作流与异常处理提取报告.md | Add | 从 Coffee1 实际状态机和下位机 Modbus TCP 协议提取订单握手、制作/交付分支、异常取消流程、已划线协议项及 Coffee2 迁移要求 |
| 2026-08-01 | 资料文档/店中店咖啡机工程核心实现资料/执行报告/COFFEE2_工作流订单事件防呆审查报告.md | Add | 审查每设备 EventGroup 缺少订单身份、旧命令迟到完成、取消屏障和新订单接收风险；给出最小代际隔离、结果快照、取消屏障和验收测试方案 |
| 2026-07-31 | Application/UserAPP/Coffee2App/Robot_Tcp/coffee2_robot_tcp.c and Device/coffee2_device.h | Fix | Align Robot controls with the latest coil protocol, add low-to-high base-command edges, complete action mappings through 3138, and clear only completion coils |
| 2026-07-31 | Application/UserAPP/Coffee2App/Modbus_Tcp_Server/coffee2_server.c | Fix | Accept valid zero-valued manual commands, map current single-device controls, correct struck status fields, expose cup/lid/ice/scale state, and add Server stack-margin diagnostics |
| 2026-07-31 | Application/DeviceProtocol/Coffee2Protocol/ and Application/UserAPP/Coffee2App/WorkFlow/ | Add/Modify | Normalize scale data to 0.1 g, implement linear ice timing with median feedback and bounded corrections, fix printer result semantics, and distinguish storage pickup from output placement |
| 2026-07-31 | Application/UserAPP/Coffee2App/Config/coffee2_app_config.h and MDK-ARM/ScatterFiles/Coffee2_CCM.sct | Modify | Place CPU-only queues, EventGroups, register images, IO state, and device status in CCM while retaining UART and Ethernet DMA resources in SRAM |
| 2026-07-31 | Application/UserAPP/Coffee2App/Coffee2_IMPLEMENTATION_REPORT.md | Add | Record implemented commands, two-slot Server behavior, logs, memory placement, commissioning sequence, and protocol items blocked by missing definitions |
| 2026-07-31 | MDK-ARM/Coffee2_final_build.log | Verify | Build Coffee2 with ARM Compiler V5.06u7 at zero errors and zero warnings and verify CCM/SRAM placement in the map file |
| 2026-07-31 | Application/UserAPP/Coffee2App/Modbus_Rtu_Bus/coffee2_rtu_bus.c | Fix | Deduplicate RTU failures by each device's previous result so interleaved slave polling cannot repeatedly emit the same error |
| 2026-07-31 | Application/UserAPP/Coffee2App/Task_Manager/coffee2_manager.c | Modify | Raise C2Log to application priority 2 so the startup burst drains without losing later task-running diagnostics |
| 2026-07-31 | 资料文档/店中店咖啡机工程核心实现资料/固件开发文档/Coffee2任务与通信架构设计.md | Modify | Document per-device RTU error edge detection and the corrected C2Log priority |
| 2026-07-31 | MDK-ARM/Coffee2_rtu_log_fix_build.log | Verify | Build the per-device RTU log fix with ARM Compiler V5.06u7 at zero errors and zero warnings and recheck CCM/SRAM placement |
| 2026-07-31 | Application/UserAPP/Coffee2App/Comm_Log/ | Modify | Move all Coffee2 normal and fatal logs from USART6 to USART1 and change the line format to `[Level][Task][Module] EVENT result=... field=value` without Tick |
| 2026-07-31 | Application/UserAPP/Coffee2App/Modbus_Rtu_Bus/ | Modify | Remove the UART1 RTU Bus1 owner and retain only Bus2 through Bus5 on UART2 through UART5 |
| 2026-07-31 | Application/UserAPP/Coffee2App/Task_Manager/ | Modify | Reinitialize USART1 for logs, create eight Coffee2 tasks, add direct RUN/ALM/TF network indicators, and retain the CCM FreeRTOS heap placement |
| 2026-07-31 | Application/UserAPP/Coffee2App/Robot_Tcp/ and Modbus_Tcp_Server/ | Modify | Emit initial connection states and suppress unchanged reconnect/listener failure logs while preserving TCP recovery |
| 2026-07-31 | Application/UserAPP/Coffee2App/Modbus_Rtu_Bus/ | Modify | Replace periodic identical RTU failure reports with first/change/recovery edge reporting |
| 2026-07-31 | 资料文档/店中店咖啡机工程核心实现资料/固件开发文档/Coffee2任务与通信架构设计.md | Modify | Synchronize UART ownership, four-bus topology, log schema, state-change policy, task mask, debug map, and three-LED behavior |
| 2026-07-31 | MDK-ARM/Coffee2_build_final.log and MilkTea_rebuild_final.log | Verify | Build Coffee2 and fully rebuild MilkTea with ARM Compiler V5.06u7 at zero errors and zero warnings; verify both FreeRTOS heaps remain in CCM |
| 2026-07-30 | Application/UserAPP/Coffee2App/Comm_Log/coffee2_log.h | Add | Define a minimal Coffee2 asynchronous USART6 log API and Server-readable status |
| 2026-07-30 | Application/UserAPP/Coffee2App/Comm_Log/coffee2_log.c | Add | Implement a static log queue, single USART6 Transport owner task, bounded formatting, and counters without Debug input |
| 2026-07-30 | Application/UserAPP/Coffee2App/Task_Manager/app_task_manager.h | Add | Define the initial Coffee2 task-manager and network-readiness interface |
| 2026-07-30 | Application/UserAPP/Coffee2App/Task_Manager/app_task_manager.c | Add | Initialize the Coffee2 log service from the default task without creating the MilkTea Debug task |
| 2026-07-30 | 资料文档/店中店咖啡机工程核心实现资料/固件开发文档/Coffee2任务与通信架构设计.md | Add | Define Coffee2 task ownership, per-device EventGroups, command queues, two-client Server slots, bus bindings, and global IO state |
| 2026-07-24 | Application/ProtocolStack/Modbus/ | Refactor | Replace the retired mixed TCP Client/Server service and adapter with a Client-only Modbus TCP implementation and a synchronous Modbus RTU Master |
| 2026-07-24 | Application/DeviceProtocol/MilkTea/ | Add | Implement the milk-tea machine register model, status polling, make command, and abort command from the supplied Server protocol |
| 2026-07-24 | Application/DeviceProtocol/Robot/ | Add | Add the robot Modbus TCP protocol boundary and raw holding-register storage |
| 2026-07-24 | Application/DeviceProtocol/IoModule/ | Add | Add standard coil, discrete-input, input-register, and holding-register arrays for IO modules |
| 2026-07-24 | Application/DeviceModel/IO_State/ | Add | Add the simplified global MachineIoState_t boolean arrays and centralized protocol/GPIO mapping function |
| 2026-07-24 | Application/UserAPP/Modbus_Tcp_Client/ | Add | Add independent Robot and MilkTea TCP owner tasks with reconnect, polling, and a MilkTea command queue |
| 2026-07-24 | Application/UserAPP/Modbus_Rtu_Bus/ | Add | Add one shared RTU task function with four UART-specific task instances and disabled placeholders for incomplete device protocols |
| 2026-07-24 | Application/UserAPP/WorkFlow/ | Add | Add the product workflow task and periodic unified IO-state update point |
| 2026-07-24 | Application/UserAPP/Task_Manager/ | Modify | Create LOG, two TCP owner tasks, four RTU bus instances, and WorkFlow while removing the retired ModbusService startup dependency |
| 2026-07-24 | Application/UserAPP/Comm_Log/ | Modify | Rename retired ExternalSystem Server log sources to MilkTea TCP Client terminology |
| 2026-07-24 | Application/ProductConfig and Application/UserAPP/Task_Manager/Config | Delete | Remove unreferenced legacy feature and task macro headers that still selected TCP Slave and ExternalSystem behavior |
| 2026-07-24 | Application/Transport/Src/transport_tcp.c | Modify | Keep generic TCP Server transport capability without ExternalSystem product terminology |
| 2026-07-24 | Application/UserAPP/Modbus_Service, Robot_TCP, MilkTea_App | Delete | Remove application modules replaced by the resource-owner task architecture |
| 2026-07-24 | Application/DeviceProtocol/ExternalSystem | Delete | Remove the incorrect external-system Server-side model |
| 2026-07-24 | 资料文档/奶茶机工程核心实现资料/固件开发文档/ | Rewrite | Synchronize the architecture overview and controller design with the Client-only TCP, four-bus RTU, WorkFlow, and simplified IO design |
| 2026-07-24 | 资料文档/奶茶机工程核心实现资料/modbus相关文档/ | Rewrite | Replace the retired Server-side instructions with current TCP Client, RTU Master, IO mapping, and commissioning guidance |
| 2026-07-24 | 资料文档/奶茶机工程核心实现资料/ | Modify | Mark long-form API, walkthrough, tutorial, log, and lwIP documents that still contain historical Server examples |
| 2026-07-24 | MDK-ARM/KEIL_MANUAL_SYNC.md | Add | List the exact source and include-path changes required in the protected Keil target |
| 2026-07-24 | MDK-ARM/MilkTea.uvprojx | Modify | Synchronize the active Target with the new TCP Client, RTU Master, device protocol, IO model, WorkFlow, and application task sources |
| 2026-07-24 | Middlewares/New_Party/Modbus_STM32_HAL_FreeRTOS/ | Delete | Remove the unreferenced legacy third-party Modbus implementation after replacing it with the active L2 TCP Client and RTU Master |
| 2026-07-24 | MDK-ARM/KEIL_MANUAL_SYNC.md and obsolete build logs | Delete | Remove the completed manual migration checklist and historical logs that could be mistaken for the current build result |
| 2026-07-24 | MDK-ARM/codex_arch_build.log | Verify | Complete Keil ARM Compiler V5.06u7 rebuild with zero errors and zero warnings |
| 2026-07-24 | MDK-ARM/MilkTea legacy intermediate files | Delete | Remove obsolete CRF, dependency, and object files belonging to the retired application, ExternalSystem, TCP Service, and third-party Modbus sources |
| 2026-07-24 | Application/DeviceProtocol/MilkTea/ | Modify | Add a dedicated FC03 status test that reads holding registers 0-1 and integrate register 50 into the periodic MilkTea poll |
| 2026-07-24 | Application/UserAPP/Modbus_Tcp_Client/Config/app_modbus_tcp_config.h | Modify | Document the periodic MilkTea FC03 status test and retain the safe disabled endpoint until its actual IP is configured |
| 2026-07-24 | Application/UserAPP/Modbus_Tcp_Client/Config/app_modbus_tcp_config.h | Clarify | State explicitly that this controller is the external-system Client and the MilkTea machine is the remote Server |
| 2026-07-24 | 资料文档/奶茶机工程核心实现资料/modbus相关文档/ | Modify | Document the external-system Client role and the MilkTea FC03 status test without referencing the retired manual-sync file |
| 2026-07-24 | Application/UserAPP/Modbus_Tcp_Client/ | Modify | Add independent Robot and MilkTea FC03 test-enable/period macros plus connection and transaction diagnostics |
| 2026-07-24 | 资料文档/奶茶机工程核心实现资料/modbus相关文档/ | Modify | Document endpoint/test macro dependencies and the requirement for a completed TCP handshake before FC03 transmission |
| 2026-07-17 | Core/Inc/FreeRTOSConfig.h | Modify | Let heap_4 use the application-provided FreeRTOS heap array |
| 2026-07-17 | Application/UserAPP/Task_Manager/Src/app_task_manager.c | Modify | Define the 32KB FreeRTOS heap in an 8-byte-aligned CCM section |
| 2026-07-17 | MDK-ARM/MilkTea_CCM.sct | Add | Map the FreeRTOS heap section to 64KB CCM while retaining the existing Flash and SRAM regions |
| 2026-07-17 | MDK-ARM/MilkTea.uvprojx | Modify | Select the CCM-aware ARM Compiler V5 scatter file |
| 2026-07-16 | Application/ServiceSupervisor/Inc/service_supervisor.h | Add | Define common service health states, fault envelope, events, snapshots, and recovery levels |
| 2026-07-16 | Application/ServiceSupervisor/Src/service_supervisor.c | Add | Implement static module registry, FreeRTOS event queue, health accounting, and owner-executed recovery requests |
| 2026-07-16 | Application/Transport/Inc/transport.h | Modify | Add generic transport operation status, native-error snapshots, counters, and readiness/disconnect results |
| 2026-07-16 | Application/Transport/Src/transport.c | Modify | Record transport operation results, timing, byte counters, and ISR receive/error events |
| 2026-07-16 | Application/Transport/Inc/transport_tcp.h | Modify | Store the last native LwIP error in each TCP backend context |
| 2026-07-16 | Application/Transport/Src/transport_tcp.c | Modify | Validate network readiness, preserve LwIP errors, and distinguish disconnect/not-ready failures |
| 2026-07-16 | Application/Transport/Inc/transport_uart.h | Modify | Store the last native HAL error in each UART backend context |
| 2026-07-16 | Application/Transport/Src/transport_uart.c | Modify | Publish HAL operation and UART interrupt errors through the generic transport snapshot |
| 2026-07-16 | Application/ProtocolStack/Modbus/Inc/modbus_tcp_service.h | Modify | Add Modbus TCP health binding and combined protocol/transport snapshot APIs |
| 2026-07-16 | Application/ProtocolStack/Modbus/Src/modbus_tcp_service.c | Modify | Report client and server protocol operations to the common service supervisor |
| 2026-07-16 | Application/ProtocolStack/Modbus/Inc/modbus_stack_adapter.h | Modify | Add RTU health configuration, snapshot, and owner-context recovery interfaces |
| 2026-07-16 | Application/ProtocolStack/Modbus/Src/modbus_stack_adapter.c | Modify | Report RTU lifecycle and master transactions and implement serialized protocol/transport recovery |
| 2026-07-16 | Application/UserAPP/Network_Service/ | Modify | Register TCP Modbus health modules, expose snapshots, and execute supervisor recovery requests in owner tasks |
| 2026-07-16 | MDK-ARM/MilkTea.uvprojx | Modify | Add ServiceSupervisor include path and source group to the Keil target |
| 2026-07-16 | 资料文档/通信服务健康监控与恢复机制.md | Add | Document the reusable monitoring, event, snapshot, and recovery architecture for Modbus TCP/RTU and future libraries |
| 2026-07-16 | 资料文档/工程技术栈与分层结构说明.md | Modify | Synchronize the project tree and technology ownership with ServiceSupervisor |
| 2026-07-16 | 资料文档/网口Modbus_TCP联调说明.md | Modify | Correct the static IPv4 description and add ServiceSupervisor diagnostic fields and APIs |
| 2026-07-15 | Application/Src/app_net_monitor.c | Modify | Remove direct lwIP netif down/up manipulation from the custom network monitor recovery path |
| 2026-07-15 | Middlewares/New_Party/Modbus_STM32_HAL_FreeRTOS/ | Add | Isolate and port the upstream Modbus STM32 HAL FreeRTOS library with its LGPL-2.1 license and port notes |
| 2026-07-15 | Middlewares/New_Party/Modbus_STM32_HAL_FreeRTOS/Config/ModbusConfig.h | Add | Configure the first-stage RTU port for six handlers and bounded buffers |
| 2026-07-15 | Middlewares/New_Party/Modbus_STM32_HAL_FreeRTOS/Inc/Modbus.h | Modify | Replace direct HAL UART and RS485 fields with a registered transport channel |
| 2026-07-15 | Middlewares/New_Party/Modbus_STM32_HAL_FreeRTOS/Src/Modbus.c | Modify | Route RTU I/O through transport operations and add bounded initialization error paths |
| 2026-07-15 | Middlewares/New_Party/Modbus_STM32_HAL_FreeRTOS/Src/ModbusTransport.c | Add | Bridge transport ISR events to the Modbus RTU frame timer and ring buffer |
| 2026-07-15 | Application/Transport/Inc/transport.h | Add | Define the common registered transport channel and function-pointer interface |
| 2026-07-15 | Application/Transport/Src/transport.c | Add | Implement the static transport channel registry and operation dispatch |
| 2026-07-15 | Application/Transport/Inc/transport_uart.h | Add | Define the STM32 HAL UART transport configuration and static RTOS context |
| 2026-07-15 | Application/Transport/Src/transport_uart.c | Add | Implement serialized interrupt-driven UART transport and centralized HAL callback routing |
| 2026-07-15 | Application/ProtocolStack/Modbus/Inc/modbus_stack_adapter.h | Add | Define the function-table Modbus service interface exposed to application modules |
| 2026-07-15 | Application/ProtocolStack/Modbus/Src/modbus_stack_adapter.c | Add | Implement RTU stack lifecycle and serialized synchronous master requests |
| 2026-07-15 | MDK-ARM/MilkTea.uvprojx | Modify | Add transport and Modbus source groups and include paths to the Keil target |
| 2026-07-16 | 资料文档/工程技术栈与分层结构说明.md | Add | Document the current file structure, software layers, technology stack ownership, Modbus placement, and future library extension rules |
| 2026-07-16 | Application/Transport/Inc/transport.h | Modify | Add a generic connection reset control for stream transports |
| 2026-07-16 | Application/Transport/Inc/transport_tcp.h | Add | Define the LwIP Netconn TCP client and server transport backend |
| 2026-07-16 | Application/Transport/Src/transport_tcp.c | Add | Implement single-owner TCP connect, listen, stream receive, timeout, and reconnect handling |
| 2026-07-16 | Application/ProtocolStack/Modbus/Inc/modbus_tcp_service.h | Add | Define Modbus TCP server data-model callbacks and FC03 client interface |
| 2026-07-16 | Application/ProtocolStack/Modbus/Src/modbus_tcp_service.c | Add | Implement MBAP framing, FC01/03/06/16 server handling, and FC03 client validation |
| 2026-07-16 | Application/DeviceProtocol/ExternalSystem/Inc/external_system_model.h | Add | Define the external-system register model and application event APIs |
| 2026-07-16 | Application/DeviceProtocol/ExternalSystem/Src/external_system_model.c | Add | Implement protocol register permissions, production triggers, and status mapping |
| 2026-07-16 | Application/Network_Service/Config/app_network_service_config.h | Add | Configure the external server and robot client endpoints and task resources |
| 2026-07-16 | Application/Network_Service/Inc/app_network_service.h | Add | Expose network startup, robot test status, and external production event APIs |
| 2026-07-16 | Application/Network_Service/Src/app_network_service.c | Add | Run one Modbus TCP server task and one periodic robot FC03 client task |
| 2026-07-16 | Core/Src/freertos.c | Modify | Start network services after LwIP initialization inside the USER CODE section |
| 2026-07-16 | LWIP/Target/lwipopts.h | Modify | Enable Netconn receive and send timeouts inside the USER CODE section |
| 2026-07-16 | MDK-ARM/MilkTea.uvprojx | Modify | Add network source groups and include paths to the Keil target |
| 2026-07-16 | Middlewares/New_Party/Modbus_STM32_HAL_FreeRTOS/PORTING.md | Modify | Clarify that project-side Modbus TCP uses the common transport backend |
| 2026-07-16 | 资料文档/工程技术栈与分层结构说明.md | Modify | Add the implemented TCP transport, network service, and external-system model structure |
| 2026-07-16 | 资料文档/网口Modbus_TCP联调说明.md | Add | Document endpoints, test frames, application APIs, startup order, and field checks |
| 2026-07-16 | Application/User/App/LED_Control/ | Move | Move the LED and network-monitor application module under the unified application-layer root |
| 2026-07-16 | Application/User/App/Network_Service/ | Move | Move network task orchestration and endpoint configuration under the unified application-layer root |
| 2026-07-16 | MDK-ARM/MilkTea.uvprojx | Modify | Synchronize application source paths and include paths with Application/User/App |
| 2026-07-16 | 资料文档/工程技术栈与分层结构说明.md | Modify | Make Application/User/App the documented application-layer entry and refresh the current tree |
| 2026-07-16 | 资料文档/网口Modbus_TCP联调说明.md | Modify | Update the network-service configuration path after the application-layer move |
| 2026-07-16 | 资料文档/工程技术栈与分层结构说明.md | Modify | Synchronize the documented tree and application ownership with the current Application/UserAPP structure |
| 2026-07-16 | 资料文档/网口Modbus_TCP联调说明.md | Modify | Synchronize network-service configuration paths with Application/UserAPP |
| 2026-07-16 | Application/UserAPP/Task_Manager/ | Add/Modify | Create only Modbus, RobotTcp, and MilkTeaApp tasks with direct task parameters |
| 2026-07-16 | Application/UserAPP/Modbus_Service/ | Add | Initialize and expose the external TCP server and robot TCP client Modbus instances |
| 2026-07-16 | Application/UserAPP/Robot_TCP/ | Add | Implement the periodic robot FC03 application task |
| 2026-07-16 | Application/UserAPP/MilkTea_App/ | Add | Merge external-system requests into the milk-tea application task |
| 2026-07-16 | Application/UserAPP/Comm_Log/ | Add | Store Modbus results and native LwIP errors in a static ring log |
| 2026-07-16 | Application/ProtocolStack/Modbus/Inc/modbus_tcp_service.h | Modify | Remove ServiceSupervisor types and keep the direct Modbus TCP API |
| 2026-07-16 | Application/ProtocolStack/Modbus/Src/modbus_tcp_service.c | Modify | Remove health reporting and return native protocol results directly |
| 2026-07-16 | Application/Transport/Src/transport_tcp.c | Modify | Keep a TCP server listener open after its active client disconnects |
| 2026-07-16 | Core/Src/freertos.c | Modify | Register application tasks through the Task Manager inside the USER CODE section |
| 2026-07-16 | MDK-ARM/MilkTea.uvprojx | Modify | Compile the simplified three-task architecture and retire legacy Supervisor sources |
| 2026-07-16 | 资料文档/当前Modbus架构与使用说明.md | Add | Document the active structure, configuration, APIs, logs, and extension method |
| 2026-07-16 | 资料文档/工程技术栈与分层结构说明.md | Modify | Point the architecture document to the active three-task structure |
| 2026-07-16 | Application/UserAPP/Comm_Log/Inc/app_comm_log.h | Modify | Replace RAM ring-log APIs with the LOG task, backend registration, and runtime status interfaces |
| 2026-07-16 | Application/UserAPP/Comm_Log/Src/app_comm_log.c | Modify | Remove the 16-entry history ring and send one transient structured record from the LOG task |
| 2026-07-16 | Application/UserAPP/Comm_Log/Inc/app_comm_log_uart.h | Add | Define the HAL UART logging backend context and factory interface |
| 2026-07-16 | Application/UserAPP/Comm_Log/Src/app_comm_log_uart.c | Add | Implement checked HAL UART transmission behind the logging function pointer |
| 2026-07-16 | Application/UserAPP/Comm_Log/Inc/app_comm_log_port.h | Add | Define the product logging-backend registration hook |
| 2026-07-16 | Application/UserAPP/Comm_Log/Src/app_comm_log_port.c | Add | Register J6 RS232 1-2 through huart1 on PA9 and PA10 |
| 2026-07-16 | Application/UserAPP/Task_Manager/Src/app_task_manager.c | Modify | Create the independent LOG task before the Modbus and application tasks |
| 2026-07-16 | MDK-ARM/MilkTea.uvprojx | Modify | Compile the UART and product-port logging backends in the active Keil target |
| 2026-07-16 | 资料文档/当前Modbus架构与使用说明.md | Modify | Document the four-task startup, USART1 logger, backend replacement, RAM policy, and build result |
| 2026-07-16 | 资料文档/工程技术栈与分层结构说明.md | Modify | Synchronize the active LOG module, task boundaries, source tree, and current technology stack |
| 2026-07-16 | Application/UserAPP/Network_Service/ | Delete | Remove the retired network-service implementation after explicit user confirmation |
| 2026-07-16 | Application/UserAPP/External_System/ | Delete | Remove the retired duplicate external-system application wrapper |
| 2026-07-16 | Application/UserAPP/Robot_Device/ | Delete | Remove the retired robot-device application wrapper |
| 2026-07-16 | Application/UserAPP/Modbus_RTU_Service/ | Delete | Remove the inactive legacy RTU service wrapper |
| 2026-07-16 | Application/ServiceSupervisor/ | Delete | Remove the inactive health supervisor implementation |
| 2026-07-16 | Application/UserAPP/Comm_Log/ | Modify | Add pre-scheduler USART1 probes and stage-specific network/Modbus diagnostic sources |
| 2026-07-16 | Application/UserAPP/Task_Manager/Src/app_task_manager.c | Modify | Report boot and individual task-creation failures before the scheduler starts |
| 2026-07-16 | Application/UserAPP/LED_Control/ | Modify | Log netif, interface, PHY link, IPv4, offline, and recovery transitions |
| 2026-07-16 | Application/UserAPP/Modbus_Service/ | Modify | Report robot OPEN/TX/RX/Modbus stages and expose the last Transport operation/result |
| 2026-07-16 | Application/Transport/Inc/transport.h | Modify | Add concise public API ownership comments |
| 2026-07-16 | Application/ProtocolStack/Modbus/Inc/modbus_tcp_service.h | Modify | Add concise server/client lifecycle and FC03 API comments |
| 2026-07-16 | MDK-ARM/MilkTea.uvprojx | Modify | Remove stale deleted-module include paths and keep active UserAPP paths synchronized |
| 2026-07-16 | 资料文档/当前Modbus架构与使用说明.md | Modify | Add categorized API reference, startup diagnostics, log decoding, and current build size |
| 2026-07-16 | 资料文档/工程技术栈与分层结构说明.md | Modify | Remove deleted legacy directories from the current architecture tree |
| 2026-07-16 | 资料文档/网口Modbus_TCP联调说明.md | Modify | Replace retired Network_Service and Supervisor APIs with the active four-task interfaces |
| 2026-07-16 | Application/UserAPP/Modbus_Service/Src/app_modbus_service.c | Modify | Keep protocol/exception diagnostics separate from stale Transport native errors |
| 2026-07-16 | MDK-ARM/codex_build.log | Verify | Complete ARM Compiler V5.06u7 rebuild with zero errors and zero warnings after legacy-directory deletion |
| 2026-07-16 | Application/UserAPP/ | Modify | Add module, task-context, blocking, status, and key call-chain comments to active application sources and headers |
| 2026-07-16 | Application/Transport/ | Modify | Document transport dispatch, TCP/UART backend ownership, timeout behavior, callbacks, and diagnostic APIs |
| 2026-07-16 | Application/ProtocolStack/Modbus/ | Modify | Document Modbus TCP server/client processing and important MBAP/PDU receive stages |
| 2026-07-16 | Application/DeviceProtocol/ExternalSystem/ | Modify | Document register-model ownership, callback binding, and application command handoff |
| 2026-07-16 | 资料文档/程序流程与代码阅读指南.md | Add | Add startup, task, client/server sequence, function-pointer, log, breakpoint, and reading-order guidance |
| 2026-07-16 | 资料文档/当前Modbus架构与使用说明.md | Modify | Link the architecture manual to the detailed program-flow and code-reading guide |
| 2026-07-16 | MDK-ARM/codex_build.log | Verify | Rebuild annotated sources with ARM Compiler V5.06u7: zero errors and zero warnings |
| 2026-07-17 | Application/Transport/Inc/transport.h | Modify | Define complete-send backend contract and record requested versus transferred byte lengths |
| 2026-07-17 | Application/Transport/Src/transport.c | Modify | Enforce full-send success and preserve length diagnostics for send and receive operations |
| 2026-07-17 | Application/Transport/Src/transport_tcp.c | Modify | Replace invalid timed netconn_write usage with deadline-bounded netconn_write_partly handling |
| 2026-07-17 | Application/Transport/Src/transport_uart.c | Modify | Return actual interrupt-driven UART transmit length through the backend contract |
| 2026-07-17 | Application/ProtocolStack/Modbus/ | Modify | Add optional fixed last-TX/RX frame snapshots for Keil Watch and document protocol types |
| 2026-07-17 | Application/UserAPP/Modbus_Service/ | Modify | Bind stable robot and external-system Modbus TCP debug snapshot symbols |
| 2026-07-17 | Application/UserAPP/Comm_Log/ | Modify | Print source-specific result names and complete LwIP error names while retaining numeric values |
| 2026-07-17 | Application/UserAPP/Task_Manager/ | Modify | Document task-manager results, status fields, and inactive legacy task configuration |
| 2026-07-17 | Application/UserAPP/LED_Control/ | Modify | Publish and use named network readiness flag definitions |
| 2026-07-17 | Application/UserAPP/MilkTea_App/ | Modify | Document product workflow status fields |
| 2026-07-17 | Application/DeviceProtocol/ExternalSystem/ | Modify | Document production results, running states, register fields, and pending commands |
| 2026-07-17 | Application/ProductConfig/Inc/product_config.h | Modify | Mark profile switches as reserved and synchronize inactive RTU feature values |
| 2026-07-17 | 资料文档/程序流程与代码阅读指南.md | Modify | Add complete log, LwIP, Transport, enum, structure, Watch, and naming dictionaries |
| 2026-07-17 | 资料文档/当前Modbus架构与使用说明.md | Modify | Synchronize readable log examples, Transport contract, debug snapshots, and current build size |
| 2026-07-17 | 资料文档/网口Modbus_TCP联调说明.md | Modify | Add stable Keil Watch frame symbols and requested/actual transfer diagnostics |
| 2026-07-17 | 资料文档/工程技术栈与分层结构说明.md | Modify | Clarify Transport reuse boundaries and preserve native diagnostic information |
| 2026-07-17 | MDK-ARM/codex_build.log | Verify | ARM Compiler V5.06u7 rebuild completed with zero errors and zero warnings |
| 2026-07-17 | Application/UserAPP/Task_Manager/Inc/app_device_task.h | Add | Define the common six-stage device-task communication states, transient events, retry timing, and diagnostic counters |
| 2026-07-17 | Application/UserAPP/Task_Manager/ | Modify | Separate LwIP stack initialization from dynamic PHY/netif/IPv4 readiness and publish current network state through the event group |
| 2026-07-17 | Application/UserAPP/LED_Control/ | Modify | Expose the current complete network-readiness result to the NetworkManager loop |
| 2026-07-17 | Application/UserAPP/Modbus_Service/ | Modify | Remove cross-task TCP recovery and expose owner-context open, close, state, listener, and peer APIs |
| 2026-07-17 | Application/UserAPP/Modbus_Service/Config/app_modbus_config.h | Modify | Add bounded client reconnect, server retry, and cooperative device-task loop timing |
| 2026-07-17 | Application/UserAPP/Robot_TCP/ | Modify | Implement the six-stage robot task template, application request gating, owner-only reconnect, hot-plug recovery, and Watch status snapshot |
| 2026-07-17 | Application/UserAPP/MilkTea_App/ | Modify | Implement the six-stage milk-tea task template, owner-only listener recovery, client hot-plug handling, business processing, and Watch status snapshot |
| 2026-07-17 | 资料文档/工程技术栈与分层结构说明.md | Modify | Establish the architecture baseline and document task ownership, six-stage templates, communication flags, and hot-plug rules |
| 2026-07-17 | 资料文档/当前Modbus架构与使用说明.md | Modify | Synchronize dynamic network readiness, owner APIs, task templates, Watch symbols, and current build size |
| 2026-07-17 | 资料文档/程序流程与代码阅读指南.md | Modify | Update startup, client/server sequences, task reading order, breakpoints, and device communication structure reference |
| 2026-07-17 | 资料文档/网口Modbus_TCP联调说明.md | Modify | Add stack/link state distinction, hot-plug test cases, and device-task Watch guidance |
| 2026-07-17 | MDK-ARM/codex_build_new.log | Verify | Preserve the full ARM Compiler V5.06u7 rebuild log with zero errors and zero warnings |
| 2026-07-17 | MDK-ARM/codex_build.log | Verify | Confirm the final incremental ARM Compiler V5.06u7 build with zero errors and zero warnings |
| 2026-07-17 | HEX_OUT/MilkTea.hex | Generate | Regenerate firmware after dynamic network recovery and device-task framework integration |
| 2026-07-17 | Application/UserAPP/Comm_Log/Inc/app_comm_log.h | Modify | Add external peer-transition and MilkTeaApp health diagnostic sources |
| 2026-07-17 | Application/UserAPP/Comm_Log/Src/app_comm_log.c | Modify | Format readable external peer, health-state, connection-count, and native LwIP diagnostics |
| 2026-07-17 | Application/UserAPP/Modbus_Service/Config/app_modbus_config.h | Modify | Add the fixed five-second external server health-log interval |
| 2026-07-17 | Application/UserAPP/MilkTea_App/Inc/app_milktea_app.h | Modify | Add fixed health flags, protocol activity counters, peer-transition counters, and diagnostic ticks |
| 2026-07-17 | Application/UserAPP/MilkTea_App/Src/app_milktea_app.c | Modify | Emit periodic server health snapshots and explicit external client connect/disconnect transitions |
| 2026-07-17 | 资料文档/当前Modbus架构与使用说明.md | Modify | Document external peer logs, health bits, Watch counters, and the latest build size |
| 2026-07-17 | 资料文档/程序流程与代码阅读指南.md | Modify | Add external server diagnostics to the log, state-bit, structure, and Watch dictionaries |
| 2026-07-17 | 资料文档/网口Modbus_TCP联调说明.md | Modify | Add the expected external hot-plug log sequence, Watch variables, and reproduction capture window |
| 2026-07-17 | 资料文档/工程技术栈与分层结构说明.md | Modify | Record the fixed-memory external server heartbeat and peer-transition diagnostics in the architecture baseline |
| 2026-07-17 | MDK-ARM/codex_build.log | Verify | ARM Compiler V5.06u7 diagnostic rebuild completed with zero errors and zero warnings |
| 2026-07-17 | HEX_OUT/MilkTea.hex | Generate | Regenerate firmware with external server hot-plug diagnostics |
| 2026-07-17 | Application/UserAPP/Comm_Log/Inc/app_comm_log.h | Modify | Add Robot connect-boundary, close-boundary, and task-heartbeat diagnostic sources |
| 2026-07-17 | Application/UserAPP/Comm_Log/Src/app_comm_log.c | Modify | Format readable Robot BEGIN/END boundaries, connection attempts, and health states |
| 2026-07-17 | Application/UserAPP/Modbus_Service/Config/app_modbus_config.h | Modify | Add the fixed five-second Robot task health-log interval |
| 2026-07-17 | Application/UserAPP/Robot_TCP/Inc/app_robot_tcp.h | Modify | Add fixed Robot operation, timing, close-count, heartbeat, and health-flag Watch fields |
| 2026-07-17 | Application/UserAPP/Robot_TCP/Src/app_robot_tcp.c | Modify | Trace blocking connect and close boundaries and emit periodic Robot owner-task health snapshots |
| 2026-07-17 | Application/UserAPP/Modbus_Service/Src/app_modbus_service.c | Modify | Remove the duplicate protocol-internal connection reset so the RobotTcp owner performs one visible cleanup |
| 2026-07-17 | 资料文档/当前Modbus架构与使用说明.md | Modify | Document Robot blocking-boundary logs, health flags, Watch operations, and latest build size |
| 2026-07-17 | 资料文档/程序流程与代码阅读指南.md | Modify | Add Robot connect, close, heartbeat, health-bit, and operation-state dictionaries |
| 2026-07-17 | 资料文档/网口Modbus_TCP联调说明.md | Modify | Add expected Robot recovery logs, stop-position rules, and Wireshark capture filter |
| 2026-07-17 | 资料文档/工程技术栈与分层结构说明.md | Modify | Record fixed-memory Robot owner-task blocking diagnostics and single-owner cleanup |
| 2026-07-17 | MDK-ARM/codex_build.log | Verify | ARM Compiler V5.06u7 Robot diagnostic rebuild completed with zero errors and zero warnings |
| 2026-07-17 | HEX_OUT/MilkTea.hex | Generate | Regenerate firmware with Robot connect, close, and heartbeat diagnostics |
| 2026-07-17 | LWIP/Target/lwipopts.h | Modify | Fix TCP PCB pool at 8 and enable keepalive, listen backlog, address reuse, and abortive-close support in the preserved USER section |
| 2026-07-17 | Application/Transport/Inc/transport_tcp.h | Modify | Add the reusable fixed Socket ClientSlot context and create/attach APIs |
| 2026-07-17 | Application/Transport/Src/transport_tcp.c | Modify | Add nonblocking Socket send/receive backend, fixed-slot lifecycle, errno mapping, and reset close with SO_LINGER |
| 2026-07-17 | Application/UserAPP/Modbus_Service/Config/app_modbus_config.h | Modify | Configure three external clients, select timing, partial-frame and idle guards, and TCP keepalive policy |
| 2026-07-17 | Application/UserAPP/Modbus_Service/Inc/app_modbus_service.h | Modify | Expose listener, three-client pool, per-slot Watch state, counters, and active-client query |
| 2026-07-17 | Application/UserAPP/Modbus_Service/Src/app_modbus_service.c | Modify | Replace the single-client Server path with one 1502 listener, three fixed slots, select dispatch, fair processing, and bounded cleanup protections |
| 2026-07-17 | Application/UserAPP/MilkTea_App/Inc/app_milktea_app.h | Modify | Add the current active external-client count to the product-task status snapshot |
| 2026-07-17 | Application/UserAPP/MilkTea_App/Src/app_milktea_app.c | Modify | Integrate aggregate three-slot state and accepted/disconnected counters into the MilkTeaApp six-stage task |
| 2026-07-17 | Application/UserAPP/Comm_Log/Inc/app_comm_log.h | Modify | Add external ClientSlot and connection-guard diagnostic sources |
| 2026-07-17 | Application/UserAPP/Comm_Log/Src/app_comm_log.c | Modify | Format ClientSlot, pool guard, and Socket errno diagnostics without a historical RAM log buffer |
| 2026-07-17 | 资料文档/工程技术栈与分层结构说明.md | Modify | Make the one-listener, three-slot, one-task Socket architecture and recovery policy the current baseline |
| 2026-07-17 | 资料文档/当前Modbus架构与使用说明.md | Modify | Document three-client APIs, configuration, Watch state, protections, LwIP resources, and final build size |
| 2026-07-17 | 资料文档/程序流程与代码阅读指南.md | Modify | Update the Server flow, breakpoints, log dictionary, errno rules, and fixed pool structure guide |
| 2026-07-17 | 资料文档/网口Modbus_TCP联调说明.md | Modify | Add concurrent-client, pool-full, slot reuse, partial-frame, dead-peer, and listener-recovery tests |
| 2026-07-17 | 资料文档/配置清单.txt | Modify | Synchronize Netconn and TCP PCB counts at 8 and record code-side LwIP USER configuration |
| 2026-07-17 | MDK-ARM/codex_build.log | Verify | Complete ARM Compiler V5.06u7 rebuild with zero errors and zero warnings after the three-client Server implementation |
| 2026-07-17 | HEX_OUT/MilkTea.hex | Generate | Regenerate firmware for the one-listener, three-fixed-slot MilkTea Modbus TCP Server |
| 2026-07-17 | Application/UserAPP/Modbus_Service/Src/app_modbus_service.c | Fix | Use the correct int-sized FIONREAD output, refresh Tick after accept, reject negative elapsed time, and preserve Socket errno during slot cleanup |
| 2026-07-17 | Application/UserAPP/Comm_Log/Src/app_comm_log.c | Modify | Print readable Socket errno for external guard failures and separate fixed-slot attach errors from Socket errors |
| 2026-07-17 | 资料文档/工程技术栈与分层结构说明.md | Modify | Record the LwIP ioctl ABI, post-accept Tick, and native errno rules in the architecture baseline |
| 2026-07-17 | 资料文档/当前Modbus架构与使用说明.md | Modify | Document the confirmed FIONREAD and stale-Tick faults, corrected guards, and final build size |
| 2026-07-17 | 资料文档/程序流程与代码阅读指南.md | Modify | Add the Socket receive ABI and timeout-order debugging rules |
| 2026-07-17 | 资料文档/网口Modbus_TCP联调说明.md | Modify | Update guard log examples to show real errno and fixed-slot attach results |
| 2026-07-17 | MDK-ARM/codex_build.log | Verify | ARM Compiler V5.06u7 rebuild completed with zero errors and zero warnings after the MilkTea Server receive fix |
| 2026-07-17 | HEX_OUT/MilkTea.hex | Generate | Regenerate firmware with corrected MilkTea FIONREAD handling and post-accept timeout checks |
| 2026-07-17 | LWIP/Target/lwipopts.h | Modify | Enable LWIP_SO_RCVBUF so the MilkTea TCP frame guard can use the LwIP FIONREAD implementation |
| 2026-07-17 | Application/UserAPP/Modbus_Service/Src/app_modbus_service.c | Modify | Add a compile-time requirement for LWIP_SO_RCVBUF to prevent ENOSYS runtime disconnect loops |
| 2026-07-17 | Application/UserAPP/Comm_Log/Src/app_comm_log.c | Modify | Map Socket errno ENOSYS to a readable diagnostic name |
| 2026-07-17 | 资料文档/工程技术栈与分层结构说明.md | Modify | Make LWIP_SO_RCVBUF a required part of the fixed three-slot Server architecture |
| 2026-07-17 | 资料文档/当前Modbus架构与使用说明.md | Modify | Document the ENOSYS root cause, required LwIP switch, compile guard, RAM delta, and final build size |
| 2026-07-17 | 资料文档/程序流程与代码阅读指南.md | Modify | Add ENOSYS and FIONREAD configuration diagnostics |
| 2026-07-17 | 资料文档/网口Modbus_TCP联调说明.md | Modify | Explain that ENOSYS indicates a missing local LwIP FIONREAD implementation |
| 2026-07-17 | 资料文档/配置清单.txt | Modify | Record LWIP_SO_RCVBUF and the updated linked RAM size |
| 2026-07-17 | MDK-ARM/codex_build.log | Verify | Full ARM Compiler V5.06u7 LwIP rebuild completed with zero errors and zero warnings after enabling receive-byte accounting |
| 2026-07-17 | HEX_OUT/MilkTea.hex | Generate | Regenerate firmware with the LwIP FIONREAD implementation enabled |
| 2026-07-21 | Application/UserAPP/Comm_Log/Inc/app_comm_log_uart.h | Modify | Add the static DMA completion object and DMA-accessible USART1 log staging buffer |
| 2026-07-21 | Application/UserAPP/Comm_Log/Src/app_comm_log_uart.c | Modify | Send runtime logs through USART1 DMA with bounded task wait, callback routing, abort recovery, and CCM-safe staging |
| 2026-07-21 | Application/UserAPP/Comm_Log/Src/app_comm_log_port.c | Modify | Preserve polling output before scheduler startup while binding runtime logs to the DMA backend |
| 2026-07-21 | MDK-ARM/codex_build_dma.log | Verify | Complete the ARM Compiler V5.06u7 DMA log rebuild with zero errors and zero warnings |
| 2026-07-21 | MDK-ARM/MilkTea.map | Verify | Confirm the 228-byte USART1 log context and DMA buffer are linked in SRAM at 0x200056A0 |
| 2026-07-21 | MDK-ARM/MilkTea/MilkTea.hex | Generate | Regenerate firmware with the CCM-safe USART1 DMA logging backend |
| 2026-07-22 | Application/DeviceProtocol/ExternalSystem/Inc/external_system_model.h | Modify | Add accepted, rejected, ignored, make, and abort command events for the slave application layer |
| 2026-07-22 | Application/DeviceProtocol/ExternalSystem/Src/external_system_model.c | Modify | Keep producing asserted until abort confirmation, clear pending identifiers, align trigger exceptions, and publish command outcomes |
| 2026-07-22 | Application/ProtocolStack/Modbus/Inc/modbus_tcp_service.h | Modify | Retain a compact transaction summary for each completed Modbus TCP server request |
| 2026-07-22 | Application/ProtocolStack/Modbus/Src/modbus_tcp_service.c | Modify | Capture server request summaries and align malformed request responses with the protocol's 0x02 exception policy |
| 2026-07-22 | Application/UserAPP/Modbus_Service/Inc/app_modbus_service.h | Modify | Expose command-outcome consumption to the MilkTea application layer |
| 2026-07-22 | Application/UserAPP/Modbus_Service/Src/app_modbus_service.c | Modify | Log every completed slave ADU and reserve EXTERNAL_REQUEST for transport or protocol failures |
| 2026-07-22 | Application/UserAPP/MilkTea_App/Inc/app_milktea_app.h | Modify | Define product workflow states, bounded timeouts, equipment executor callbacks, and detailed Watch status |
| 2026-07-22 | Application/UserAPP/MilkTea_App/Src/app_milktea_app.c | Modify | Implement command handling, UTF-16 validation, executor dispatch, progress, timeout, safe abort, and result writeback |
| 2026-07-22 | Application/UserAPP/Comm_Log/Inc/app_comm_log.h | Modify | Add Modbus frame and MilkTea workflow sources, extended details, and queue diagnostics |
| 2026-07-22 | Application/UserAPP/Comm_Log/Src/app_comm_log.c | Modify | Replace the transient slot with a 32-entry static queue and format every protocol and product workflow step |
| 2026-07-22 | 资料文档/奶茶机工程核心实现资料/日志系统与Modbus应用层排障手册.md | Add | Document every current log, workflow step, field, error code, expected sequence, Watch item, and troubleshooting path |
| 2026-07-22 | 资料文档/奶茶机工程核心实现资料/网口Modbus_TCP联调说明.md | Modify | Replace the application placeholder with the executor workflow and update the fixed log queue description |
| 2026-07-22 | 资料文档/奶茶机工程核心实现资料/当前Modbus架构与使用说明.md | Modify | Synchronize new frame and workflow logs, queue APIs, product executor API, and linked image size |
| 2026-07-22 | 资料文档/奶茶机工程核心实现资料/工程技术栈与分层结构说明.md | Modify | Record the 32-entry queue and CCM-safe USART1 DMA backend in the architecture tree |
| 2026-07-22 | 资料文档/奶茶机工程核心实现资料/程序流程与代码阅读指南.md | Modify | Update the log flow from one transient slot to a fixed queue and SRAM DMA staging path |
| 2026-07-22 | 资料文档/奶茶机工程核心实现资料/固件开发文档/architecture-overview.md | Modify | Replace the obsolete polling single-slot log example with the current static queue and DMA design |
| 2026-07-22 | 资料文档/奶茶机工程核心实现资料/固件开发文档/source-code-walkthrough.md | Modify | Synchronize the log storage walkthrough with the fixed queue implementation |
| 2026-07-22 | 资料文档/奶茶机工程核心实现资料/固件开发文档/固件开发原理教程.md | Modify | Update the bounded logging design principle to the current queue and UART DMA architecture |
| 2026-07-22 | 资料文档/奶茶机工程核心实现资料/固件开发文档/api-reference-manual.md | Modify | Synchronize log entry, queue status, extended details, SRAM staging, DMA completion, and timeout recovery APIs |
| 2026-07-22 | MDK-ARM/codex_build_app_layer.log | Verify | ARM Compiler V5.06u7 application-layer build completed with zero errors and zero warnings |
| 2026-07-22 | MDK-ARM/MilkTea.map | Verify | Confirm the 32-entry log queue at 0x200056EC, USART1 DMA context at 0x20005984, and FreeRTOS heap at 0x10000000 |
| 2026-07-22 | MDK-ARM/MilkTea/MilkTea.hex | Generate | Regenerate firmware with the Modbus slave application workflow and step-by-step diagnostics |
| 2026-07-22 | Application/UserAPP/MilkTea_App/Inc/app_milktea_app.h | Modify | Enable a clearly marked removable 10-second MilkTea communication simulator |
| 2026-07-22 | Application/UserAPP/MilkTea_App/Src/app_milktea_app.c | Modify | Register a non-blocking simulator that completes after 10 seconds and immediately confirms abort requests |
| 2026-07-22 | Application/UserAPP/Comm_Log/Inc/app_comm_log.h | Modify | Add explicit simulator enabled, start, complete, and abort workflow steps |
| 2026-07-22 | Application/UserAPP/Comm_Log/Src/app_comm_log.c | Modify | Format the new MilkTea simulator workflow steps for UART diagnostics |
| 2026-07-22 | 资料文档/奶茶机工程核心实现资料/日志系统与Modbus应用层排障手册.md | Modify | Document simulator behavior, logs, removal markers, and protocol test expectations |
| 2026-07-22 | 资料文档/奶茶机工程核心实现资料/网口Modbus_TCP联调说明.md | Modify | Describe the current 10-second simulated make, busy, abort, and completion flow |
| 2026-07-22 | 资料文档/奶茶机工程核心实现资料/当前Modbus架构与使用说明.md | Modify | Update linked image size after enabling the removable simulator |
| 2026-07-22 | MDK-ARM/codex_build_simulation.log | Verify | ARM Compiler V5.06u7 simulator build completed with zero errors and zero warnings |
| 2026-07-22 | MDK-ARM/MilkTea.map | Verify | Confirm the 8-byte simulator context is linked in DMA-accessible SRAM at 0x20000058 and the FreeRTOS heap remains in CCM |
| 2026-07-22 | MDK-ARM/MilkTea/MilkTea.hex | Generate | Regenerate firmware with the removable 10-second MilkTea communication simulator |
| 2026-07-22 | Application/UserAPP/MilkTea_App/Inc/app_milktea_app.h | Modify | Add register-50 running-state tracking and a five-second unchanged-state log interval |
| 2026-07-22 | Application/UserAPP/MilkTea_App/Src/app_milktea_app.c | Modify | Derive register 50 from startup, communication, executor, and workflow health; log changes immediately and unchanged state every five seconds |
| 2026-07-22 | Application/UserAPP/Comm_Log/Inc/app_comm_log.h | Modify | Add the RUNNING_STATE source and changed/heartbeat event definitions |
| 2026-07-22 | Application/UserAPP/Comm_Log/Src/app_comm_log.c | Modify | Format running-state names, event names, and previous-state diagnostics |
| 2026-07-22 | 资料文档/奶茶机对接其他项目协议资料/modbus_tcp_protocol_reference.md | Modify | Replace the obsolete always-zero register-50 note with the implemented health-state policy |
| 2026-07-22 | 资料文档/奶茶机工程核心实现资料/日志系统与Modbus应用层排障手册.md | Modify | Document RUNNING_STATE values, change logs, five-second heartbeats, and production-state separation |
| 2026-07-22 | 资料文档/奶茶机工程核心实现资料/网口Modbus_TCP联调说明.md | Modify | Add register-50 integration expectations and UART log examples |
| 2026-07-22 | 资料文档/奶茶机工程核心实现资料/当前Modbus架构与使用说明.md | Modify | Update linked image size after adding the register-50 monitor |
| 2026-07-22 | MDK-ARM/codex_build_running_state.log | Verify | ARM Compiler V5.06u7 running-state build completed with zero errors and zero warnings |
| 2026-07-22 | MDK-ARM/MilkTea.map | Verify | Confirm the final linked image after adding state tracking and heartbeat diagnostics |
| 2026-07-22 | MDK-ARM/MilkTea/MilkTea.hex | Generate | Regenerate firmware with dynamic register-50 state and change/heartbeat logs |
| 2026-07-22 | Application/UserAPP/Comm_Log/Inc/app_comm_log_config.h | Add | Add compile-time UART, future HTTP, dual-output, all-off, line-size, queue-size, and timeout configuration |
| 2026-07-22 | Application/UserAPP/Comm_Log/Inc/app_comm_log.h | Modify | Document router semantics, expose stable total-status Watch symbol, and retain the output-independent producer API |
| 2026-07-22 | Application/UserAPP/Comm_Log/Inc/app_comm_log_port.h | Modify | Add per-output status, stable Watch symbol, and the future HTTP network write hook contract |
| 2026-07-22 | Application/UserAPP/Comm_Log/Inc/app_comm_log_uart.h | Modify | Size the DMA-accessible USART1 staging buffer from the shared 160-byte line limit |
| 2026-07-22 | Application/UserAPP/Comm_Log/Src/app_comm_log.c | Refactor | Format level, owner task, module, and event; keep one queue and one formatting pass; add logging-off empty APIs |
| 2026-07-22 | Application/UserAPP/Comm_Log/Src/app_comm_log_port.c | Refactor | Route the same formatted buffer to selected UART and future HTTP outputs with independent counters and a weak network placeholder |
| 2026-07-22 | Application/UserAPP/Task_Manager/Src/app_task_manager.c | Modify | Create the LOG task and emit early UART probes only when their compile-time outputs are enabled |
| 2026-07-22 | 资料文档/奶茶机工程核心实现资料/日志架构与输出配置说明.md | Add | Document D/I/W/E levels, task ownership, macros, fan-out behavior, HTTP hook, all-off behavior, CCM rules, and verification |
| 2026-07-22 | 资料文档/奶茶机工程核心实现资料/日志系统与Modbus应用层排障手册.md | Modify | Convert examples and troubleshooting to the Level/Task/Module/Event format and add per-output diagnostics |
| 2026-07-22 | 资料文档/奶茶机工程核心实现资料/网口Modbus_TCP联调说明.md | Modify | Update Server, Robot, guard, state, and output-routing log examples |
| 2026-07-22 | 资料文档/奶茶机工程核心实现资料/当前Modbus架构与使用说明.md | Modify | Record the macro-selected router, HTTP hook, Watch symbols, final image size, and verified SRAM/CCM placement |
| 2026-07-22 | 资料文档/奶茶机工程核心实现资料/工程技术栈与分层结构说明.md | Modify | Update the Comm_Log tree and LOG task responsibility to the multi-output architecture |
| 2026-07-22 | 资料文档/奶茶机工程核心实现资料/程序流程与代码阅读指南.md | Modify | Update the task table, log flow diagram, format, and task/module interpretation |
| 2026-07-22 | 资料文档/奶茶机工程核心实现资料/固件开发文档/architecture-overview.md | Modify | Replace the obsolete single-UART backend path with the current fan-out registration and call chain |
| 2026-07-22 | 资料文档/奶茶机工程核心实现资料/固件开发文档/api-reference-manual.md | Modify | Add log configuration, complete sources, 160-byte buffer, route status, HTTP hook, and new format |
| 2026-07-22 | 资料文档/奶茶机工程核心实现资料/固件开发文档/source-code-walkthrough.md | Modify | Synchronize task creation, one-pass formatting, multi-output routing, and early-output behavior |
| 2026-07-22 | 资料文档/奶茶机工程核心实现资料/固件开发文档/固件开发原理教程.md | Modify | Update backend extension examples to compile-time routing and the future HTTP strong hook |
| 2026-07-22 | 资料文档/奶茶机工程核心实现资料/固件开发文档/函数指针详解.md | Fix | Correct the log callback example and show that the registered function is the output router |
| 2026-07-22 | MDK-ARM/codex_build_log_router_uart.log | Verify | ARM Compiler V5.06u7 UART-only configuration completed with zero errors and zero warnings |
| 2026-07-22 | MDK-ARM/codex_build_log_router_network_stub.log | Verify | Future-HTTP-hook-only configuration completed with zero errors and zero warnings |
| 2026-07-22 | MDK-ARM/codex_build_log_router_dual.log | Verify | UART plus future-HTTP-hook configuration completed with zero errors and zero warnings |
| 2026-07-22 | MDK-ARM/codex_build_log_architecture.log | Verify | Final default UART configuration completed with zero errors and zero warnings |
| 2026-07-22 | MDK-ARM/MilkTea.map | Verify | Confirm log queue and DMA context in SRAM, FreeRTOS heap in CCM, and stable total/per-output status symbols |
| 2026-07-22 | MDK-ARM/MilkTea/MilkTea.hex | Generate | Regenerate firmware with task-classified logging and compile-time output routing |
| 2026-07-22 | 资料文档/奶茶机工程核心实现资料/串口与网口通讯接口上位机一键配置方案.md | Add | Define staged Modbus configuration, hot UART/HTTP apply, rollback, Flash dual-copy persistence, boot recovery, concurrency, and acceptance tests |
| 2026-07-22 | Application/UserAPP/Comm_Log/Inc/app_comm_log_config.h | Modify | Add unified 5-second state heartbeat, repeated-error summary, and optional Modbus frame-trace controls |
| 2026-07-22 | Application/UserAPP/Comm_Log/Inc/app_comm_log.h | Modify | Add policy emission metadata, counters, state logging, and repeated-error logging APIs |
| 2026-07-22 | Application/UserAPP/Comm_Log/Src/app_comm_log.c | Modify | Implement immediate state changes, 5-second stable heartbeats, first-error delivery, and repeated-error summaries |
| 2026-07-22 | Application/UserAPP/MilkTea_App/Src/app_milktea_app.c | Modify | Route MilkTea health snapshots through the common state policy and use the common heartbeat period |
| 2026-07-22 | Application/UserAPP/MilkTea_App/Inc/app_milktea_app.h | Modify | Remove the duplicate running-state log period in favor of the common log policy |
| 2026-07-22 | Application/UserAPP/Robot_TCP/Src/app_robot_tcp.c | Modify | Route Robot health snapshots through the common change-or-heartbeat policy |
| 2026-07-22 | Application/UserAPP/Modbus_Service/Config/app_modbus_config.h | Modify | Remove duplicate health log periods now owned by Comm_Log configuration |
| 2026-07-22 | Application/UserAPP/Modbus_Service/Src/app_modbus_service.c | Modify | Deduplicate Robot results, rate-limit repeated communication faults, and disable normal frame traces by default |
| 2026-07-22 | 资料文档/奶茶机工程核心实现资料/日志架构与输出配置说明.md | Modify | Document the unified frequency policy, APIs, switches, counters, examples, and verification rules |
| 2026-07-22 | 资料文档/奶茶机工程核心实现资料/日志系统与Modbus应用层排障手册.md | Modify | Document optional frame tracing, changed-state delivery, heartbeats, and repeated-error summaries |
| 2026-07-22 | 资料文档/奶茶机工程核心实现资料/当前Modbus架构与使用说明.md | Modify | Synchronize Modbus log behavior with state deduplication and disabled-by-default frame tracing |
| 2026-07-22 | 资料文档/奶茶机工程核心实现资料/工程技术栈与分层结构说明.md | Modify | Update task health and error logging architecture descriptions |
| 2026-07-22 | 资料文档/奶茶机工程核心实现资料/网口Modbus_TCP联调说明.md | Modify | Update hot-plug examples for CHANGED and HEARTBEAT policy events |
| 2026-07-22 | 资料文档/奶茶机工程核心实现资料/程序流程与代码阅读指南.md | Modify | Update health source descriptions to immediate changes and 5-second stable heartbeats |
| 2026-07-22 | 资料文档/奶茶机工程核心实现资料/固件开发文档/architecture-overview.md | Modify | Update the diagnostic stage cadence description |
| 2026-07-22 | 资料文档/奶茶机工程核心实现资料/固件开发文档/api-reference-manual.md | Modify | Add policy macros, APIs, entry metadata, counters, and current source behavior |
| 2026-07-22 | 资料文档/奶茶机工程核心实现资料/固件开发文档/source-code-walkthrough.md | Modify | Replace fixed health writes with the common state-policy flow |
| 2026-07-22 | MDK-ARM/codex_build_log_policy.log | Verify | ARM Compiler V5.06u7 logging-policy build completed with zero errors and zero warnings |
| 2026-07-22 | MDK-ARM/codex_build_log_policy_frame_trace.log | Verify | Optional Modbus frame-trace configuration built with zero errors and zero warnings |
| 2026-07-22 | MDK-ARM/codex_build_log_policy_disabled.log | Verify | All-log-outputs-disabled configuration built with safe no-op APIs and zero errors or warnings |
| 2026-07-22 | MDK-ARM/MilkTea.map | Verify | Confirm policy state in SRAM, UART DMA context in SRAM, and FreeRTOS heap in CCM |
| 2026-07-22 | MDK-ARM/MilkTea/MilkTea.hex | Generate | Regenerate firmware with unified log throttling and immediate state-change reporting |
| 2026-07-23 | Application/Transport/Inc/transport_uart.h | Modify | Add the shared 256-byte DMA-accessible TX staging area, TX state, and optional RX enable flag |
| 2026-07-23 | Application/Transport/Src/transport_uart.c | Refactor | Make xTransportSend select pre-scheduler polling, runtime DMA, or runtime polling from scheduler and HAL handle state |
| 2026-07-23 | Application/UserAPP/Comm_Log/Src/app_comm_log_port.c | Refactor | Register the log UART as a Transport channel and route startup/runtime UART output through xTransportSend |
| 2026-07-23 | Application/UserAPP/Comm_Log/Src/app_comm_log.c | Modify | Remove runtime backend registration because Task Manager now registers outputs before task creation |
| 2026-07-23 | Application/UserAPP/Task_Manager/Inc/app_task_manager.h | Modify | Add a specific startup result for log Transport registration failure |
| 2026-07-23 | Application/UserAPP/Task_Manager/Src/app_task_manager.c | Refactor | Initialize Transport and register log outputs before startup probes and task creation |
| 2026-07-23 | Application/UserAPP/Modbus_Service/Src/app_modbus_service.c | Modify | Remove the late Transport manager reset that could erase the registered log UART |
| 2026-07-23 | Application/UserAPP/Comm_Log/Inc/app_comm_log_uart.h | Delete | Remove the obsolete log-specific UART DMA interface after migration to Transport |
| 2026-07-23 | Application/UserAPP/Comm_Log/Src/app_comm_log_uart.c | Delete | Remove duplicate DMA buffer, semaphore, timeout, and HAL callback ownership |
| 2026-07-23 | MDK-ARM/MilkTea.uvprojx | Modify | Remove the deleted app_comm_log_uart.c source entry from the Keil project |
| 2026-07-23 | MDK-ARM/MilkTea.uvoptx | Modify | Remove the deleted app_comm_log_uart.c user-option entry |
| 2026-07-23 | 资料文档/奶茶机工程核心实现资料/日志相关文档/串口统一发送接口与串口切换说明.md | Add | Document the one-interface UART TX policy, switching method, CCM boundary, recovery, and verification |
| 2026-07-23 | 资料文档/奶茶机工程核心实现资料/日志相关文档/日志架构与输出配置说明.md | Modify | Replace the log-specific DMA backend with the common UART Transport flow |
| 2026-07-23 | 资料文档/奶茶机工程核心实现资料/工程架构文档/工程技术栈与分层结构说明.md | Modify | Remove deleted log UART files and document Transport-only UART sending |
| 2026-07-23 | 资料文档/奶茶机工程核心实现资料/modbus相关文档/当前Modbus架构与使用说明.md | Modify | Update the log/Transport tree, APIs, and manager initialization ownership |
| 2026-07-23 | 资料文档/奶茶机工程核心实现资料/固件开发文档/architecture-overview.md | Modify | Update the log registration and function-pointer call chain |
| 2026-07-23 | 资料文档/奶茶机工程核心实现资料/固件开发文档/api-reference-manual.md | Modify | Replace the removed log UART backend API with TransportUartContext and automatic TX selection |
| 2026-07-23 | 资料文档/奶茶机工程核心实现资料/固件开发文档/source-code-walkthrough.md | Modify | Update startup ordering and early output to the common xTransportSend path |
| 2026-07-23 | 资料文档/奶茶机工程核心实现资料/固件开发文档/固件开发原理教程.md | Modify | Replace the obsolete log UART backend example with the common Transport design |
| 2026-07-23 | MDK-ARM/codex_build_uart_unified.log | Verify | ARM Compiler V5.06u7 default USART1 DMA build completed with zero errors and zero warnings |
| 2026-07-23 | MDK-ARM/MilkTea.map | Verify | Confirm log UART context in SRAM at 0x20005DF0 and FreeRTOS heap in CCM at 0x10000000 |
| 2026-07-23 | MDK-ARM/MilkTea/MilkTea.hex | Generate | Regenerate firmware with unified startup/runtime UART transmission |
| 2026-07-23 | MDK-ARM/codex_build_uart_blocking.log | Verify | Temporarily bind logging to USART3 and confirm the no-TX-DMA polling configuration builds with zero errors and zero warnings |
| 2026-07-23 | Application/UserAPP/Comm_Log/Src/app_comm_log_port.c | Restore | Restore the delivered log binding to USART1 after the USART3 polling-path build test |
| 2026-07-23 | MDK-ARM/codex_build_uart_final.log | Verify | Rebuild the delivered USART1 DMA configuration with ARM Compiler V5.06u7, zero errors, and zero warnings |
| 2026-07-23 | MDK-ARM/MilkTea/app_comm_log_uart.crf | Delete | Remove obsolete Keil intermediate data left by the deleted log-specific UART backend |
| 2026-07-23 | MDK-ARM/MilkTea/app_comm_log_uart.d | Delete | Remove the obsolete dependency file left by the deleted log-specific UART backend |
| 2026-07-23 | MDK-ARM/MilkTea/app_comm_log_uart.o | Delete | Remove the obsolete object file left by the deleted log-specific UART backend |
| 2026-07-23 | MDK-ARM/MilkTea.map | Verify | Reconfirm the final USART1 log context is DMA-accessible SRAM while the FreeRTOS heap remains in CCM |
| 2026-07-23 | MDK-ARM/MilkTea/MilkTea.hex | Generate | Regenerate the final deliverable firmware after restoring the USART1 log binding |
| 2026-07-23 | Application/Diagnostics/Config/app_crash_diag_config.h | Add | Add fatal-diagnostic, IWDG refresh, task-capacity, text-size, UART-bound, stack-range, and DWT-repeat configuration |
| 2026-07-23 | Application/Diagnostics/Inc/app_crash_diag.h | Add | Define fatal reasons, lightweight FreeRTOS trace entry points, ARMCC fault C entries, and the crash watchdog interface |
| 2026-07-23 | Application/Diagnostics/Src/app_crash_diag.c | Add | Freeze core/SCB registers and registered task metadata, scan stack high-water values only after a fatal event, and repeat output with DWT |
| 2026-07-23 | Application/Diagnostics/Src/app_crash_fault_armcc.s | Add | Capture R4-R11/MSP/PSP, select the Cortex-M exception frame, switch fatal RTOS hooks to MSP, and enter the C diagnostic path |
| 2026-07-23 | Application/UserAPP/Comm_Log/Inc/app_comm_log_port.h | Modify | Expose the fatal-only registered-UART polling write interface |
| 2026-07-23 | Application/UserAPP/Comm_Log/Src/app_comm_log_port.c | Modify | Add bounded direct LL UART output that clears DMAT and does not depend on DMA completion, RTOS objects, or HAL Tick |
| 2026-07-23 | Application/UserAPP/Comm_Log/Inc/app_comm_log_config.h | Fix | Add the missing final newline and remove repeated ARMCC end-of-file warnings |
| 2026-07-23 | Application/UserAPP/Comm_Log/Inc/app_comm_log.h | Fix | Add the missing final newline and remove repeated ARMCC end-of-file warnings |
| 2026-07-23 | Core/Inc/FreeRTOSConfig.h | Modify | Route configASSERT to diagnostics, record task stack high addresses, and add create/delete/switch trace hooks |
| 2026-07-23 | Core/Src/freertos.c | Modify | Remove the empty C fatal hook bodies so the ARMCC assembly implementations own the symbols |
| 2026-07-23 | Core/Src/main.c | Modify | Initialize the fatal diagnostic DWT time base after CubeMX peripheral initialization |
| 2026-07-23 | MDK-ARM/startup_stm32f407xx.s | Modify | Route HardFault, MemManage, BusFault, and UsageFault vectors to the diagnostic assembly wrappers |
| 2026-07-23 | MDK-ARM/MilkTea.uvprojx | Modify | Add the Diagnostics C/assembly files and include directories to the Keil target |
| 2026-07-23 | MDK-ARM/MilkTea.uvoptx | Modify | Add the Diagnostics source group to Keil user options |
| 2026-07-23 | 资料文档/奶茶机工程核心实现资料/日志相关文档/死机日志管理与排障说明.md | Add | Document fatal flow, every output field, CCM validation, stack-water scanning, registered UART, DWT, IWDG, map results, and hardware tests |
| 2026-07-23 | 资料文档/奶茶机工程核心实现资料/日志相关文档/日志架构与输出配置说明.md | Modify | Add the scheduler-independent fatal-log bypass and its boundary with the normal LOG task |
| 2026-07-23 | 资料文档/奶茶机工程核心实现资料/日志相关文档/串口统一发送接口与串口切换说明.md | Modify | Document the fatal-only direct polling exception while keeping one registered UART configuration point |
| 2026-07-23 | 资料文档/奶茶机工程核心实现资料/日志相关文档/日志解读与排障手册.md | Modify | Add the CRASH log triage order and stack high-water interpretation |
| 2026-07-23 | 资料文档/奶茶机工程核心实现资料/工程架构文档/工程技术栈与分层结构说明.md | Modify | Add the cross-task Diagnostics layer, file tree, task-overhead rule, maintenance rule, and implemented status |
| 2026-07-23 | 资料文档/奶茶机工程核心实现资料/固件开发文档/api-reference-manual.md | Modify | Add crash-port API, fatal reason enum, trace/fatal interfaces, and configuration macros |
| 2026-07-23 | 资料文档/奶茶机工程核心实现资料/固件开发文档/source-code-walkthrough.md | Modify | Explain the fatal assembly-to-C-to-UART chain and correct the fixed 32-entry queue heading |
| 2026-07-23 | 资料文档/奶茶机工程核心实现资料/固件开发文档/architecture-overview.md | Modify | Correct obsolete single-slot descriptions and add the fatal diagnostic design rule and source location |
| 2026-07-23 | 资料文档/奶茶机工程核心实现资料/固件开发文档/程序流程与代码阅读指南.md | Modify | Add the DWT diagnostic initialization point, Diagnostics reading entry, and corrected CubeMX connection-point boundary |
| 2026-07-23 | 资料文档/奶茶机工程核心实现资料/固件开发文档/固件开发原理教程.md | Fix | Correct the contents entry to the current fixed pending-queue design |
| 2026-07-23 | MDK-ARM/codex_build_crash_diag_final.log | Verify | ARM Compiler V5.06u7 fatal-diagnostic build completed with zero errors and zero warnings |
| 2026-07-23 | MDK-ARM/codex_build_crash_diag.log | Delete | Remove the superseded first-pass warning log after the clean final build |
| 2026-07-23 | MDK-ARM/MilkTea.map | Verify | Confirm fatal vectors/hooks are linked, diagnostics remain in SRAM, and the 32KB FreeRTOS heap remains in CCM |
| 2026-07-23 | MDK-ARM/MilkTea/MilkTea.hex | Generate | Regenerate firmware with fatal register snapshots and FreeRTOS task stack-water diagnostics |
| 2026-07-23 | Application/UserAPP/Comm_Log/Inc/app_comm_log_config.h | Modify | Enable the temporary LOG-task CCM-boundary fault-injection switch for board validation |
| 2026-07-23 | Application/UserAPP/Comm_Log/Src/app_comm_log.c | Modify | Intentionally write beyond the 64KB CCM boundary after the LOG startup entry to trigger the fatal diagnostic path |
| 2026-07-23 | 资料文档/奶茶机工程核心实现资料/日志相关文档/死机日志管理与排障说明.md | Modify | Document the active CCM boundary fault injection, expected output order, and mandatory removal steps |
| 2026-07-23 | MDK-ARM/codex_build_ccm_fault_injection.log | Verify | ARM Compiler V5.06u7 CCM boundary fault-injection build completed with zero errors and zero warnings |
| 2026-07-23 | MDK-ARM/MilkTea.map | Verify | Confirm the LOG task references the linked prvInjectCcmBoundaryFault implementation and fatal handlers remain linked |
| 2026-07-23 | MDK-ARM/MilkTea/MilkTea.hex | Generate | Regenerate the board-test firmware with the temporary CCM boundary fault injection enabled |
| 2026-07-23 | 资料文档/奶茶机工程核心实现资料/固件开发文档/architecture-overview.md | Modify | Define the five-layer baseline, add the explicit-task ownership table, classify LwIP and HTTP, and correct the current ExternalSystem register map |
| 2026-07-23 | 资料文档/奶茶机工程核心实现资料/固件开发文档/source-code-walkthrough.md | Modify | Correct task-count scope, Modbus callback ownership, data-model binding, register mapping, and the current External Server call chain |
| 2026-07-23 | 资料文档/奶茶机工程核心实现资料/固件开发文档/api-reference-manual.md | Modify | Synchronize UART Target status, command events, current structures, task-manager results, and ExternalSystem register access with the headers |
| 2026-07-23 | 资料文档/奶茶机工程核心实现资料/固件开发文档/程序流程与代码阅读指南.md | Modify | Replace the obsolete multi-layer diagram with the five-layer model and document explicit versus middleware tasks and future HTTP placement |
| 2026-07-23 | 资料文档/奶茶机工程核心实现资料/固件开发文档/固件开发原理教程.md | Modify | Align the teaching model with five layers and correct LwIP, HTTP, UART, and ExternalSystem execution-context explanations |
| 2026-07-23 | 资料文档/奶茶机工程核心实现资料/固件开发文档/奶茶机主控设计方案.md | Modify | Separate the current Keil baseline from target design and define four RTU bus tasks with up to five slaves per independent bus |
| 2026-07-27 | 资料文档/奶茶机工程核心实现资料/固件开发文档/architecture-overview.md | Rewrite | Synchronize the architecture with the current client-only TCP, four RTU bus instances, IO mapping, workflow, network addressing, and Keil project state |
| 2026-07-27 | 资料文档/奶茶机工程核心实现资料/固件开发文档/奶茶机主控设计方案.md | Rewrite | Replace the obsolete server-oriented design with the current communication-resource ownership design |
| 2026-07-27 | 资料文档/奶茶机工程核心实现资料/固件开发文档/source-code-walkthrough.md | Rewrite | Replace removed ExternalSystem and ModbusService call chains with the current task, protocol, transport, RTU, IO, and workflow call chains |
| 2026-07-27 | 资料文档/奶茶机工程核心实现资料/固件开发文档/api-reference-manual.md | Rewrite | Rebuild the API reference from current public headers and remove retired types and services |
| 2026-07-27 | 资料文档/奶茶机工程核心实现资料/固件开发文档/程序流程与代码阅读指南.md | Rewrite | Update startup, task, TCP client, RTU bus, IO, logging, and debugging guidance to current code |
| 2026-07-27 | 资料文档/奶茶机工程核心实现资料/固件开发文档/固件开发原理教程.md | Rewrite | Explain the current client-only TCP, resource-owned RTU, direct IO mapping, workflow, CCM, and error-recovery principles |
| 2026-07-27 | 资料文档/奶茶机工程核心实现资料/固件开发文档/函数指针详解.md | Modify | Add a scope note directing current architecture and API questions to the synchronized reference documents |
| 2026-07-27 | 资料文档/奶茶机工程核心实现资料/固件开发文档/指针语法补充.md | Modify | Add a scope note distinguishing generic C syntax from current project architecture |
| 2026-07-27 | 资料文档/奶茶机工程核心实现资料/固件开发文档/待定方案-上位机+下位机奶茶机器人实现.md | Add | Document the pending upper-computer OrderTask, custom serial link, lower-computer ScheduleTask, device queues, event feedback, and end-to-end order flow |
| 2026-07-28 | Application/DeviceProtocol/MilkTea/Inc/milktea_protocol.h | Rewrite | Replace combined make/poll calls with independent MilkTea semantic APIs and an optional result callback |
| 2026-07-28 | Application/DeviceProtocol/MilkTea/Src/milktea_protocol.c | Rewrite | Implement FC01/FC03/FC06/FC16 MilkTea steps, product zero-padding, parsed data-image updates, and callbacks |
| 2026-07-28 | Application/UserAPP/Debug | Add | Add the USART1 Debug 5.1 parser, per-device function mask, real Modbus TCP execution, frame output, and error reporting |
| 2026-07-28 | Application/UserAPP/Comm_Log/Inc/app_comm_log_port.h | Modify | Expose shared log-UART receive and bounded Debug write interfaces |
| 2026-07-28 | Application/UserAPP/Comm_Log/Src/app_comm_log_port.c | Modify | Enable full-duplex USART1 reception and route Debug RX/TX through the registered Transport channel |
| 2026-07-28 | Application/Transport/Src/transport_uart.c | Modify | Split long DMA transmissions into static 256-byte staging chunks while retaining the UART TX mutex |
| 2026-07-28 | Application/ProtocolStack/Modbus/Inc/modbus_tcp_client.h | Modify | Expand complete ADU capture and expose per-transaction TX/RX completion flags |
| 2026-07-28 | Application/ProtocolStack/Modbus/Src/modbus_tcp_client.c | Modify | Reset and update Debug frame completion flags for every Modbus TCP transaction |
| 2026-07-28 | Application/UserAPP/Modbus_Tcp_Client | Rewrite | Remove the temporary MilkTea command queue/test, serialize endpoints, and add service probing plus 1/2/5/10/30-second reconnect backoff |
| 2026-07-28 | Application/UserAPP/Task_Manager/Src/app_task_manager.c | Modify | Initialize Debug and create the USART1 Debug task |
| 2026-07-28 | MDK-ARM/MilkTea.uvprojx | Modify | Add Debug source and include paths to the Keil ARM Compiler V5 project |
| 2026-07-28 | 资料文档/奶茶机工程核心实现资料/日志相关文档/debug模块功能.md | Rewrite | Define the implemented per-device FC policy, real MilkTea commands, output rules, errors, locking, and reconnect behavior |
| 2026-07-28 | 资料文档/奶茶机工程核心实现资料/固件开发文档 | Modify | Synchronize architecture, API reference, source walkthrough, and reading guide with the MilkTea implementation |
| 2026-07-28 | MDK-ARM/codex_milktea_build.log | Verify | ARM Compiler V5.06u7 full build completed with zero errors and zero warnings |
| 2026-07-28 | MDK-ARM/MilkTea/MilkTea.hex | Generate | Regenerate firmware with MilkTea semantic APIs, TCP reconnect, and USART1 Debug commands |
| 2026-07-28 | Middlewares/New_Party/nanoMODBUS | Add | Import nanoMODBUS v1.23.0 at commit 91d6782930ee263bc760f27b0cbc5b82773c5f0d with its MIT license, upstream documentation, and centralized feature configuration |
| 2026-07-28 | Middlewares/New_Party/nanoMODBUS/Src/nanomodbus.c | Fix | Add receive bounds checks, correct the TCP MBAP maximum length, harden FC20/FC21 and raw-PDU sizes, and retain ARM Compiler V5 compatibility |
| 2026-07-28 | Middlewares/New_Party/nanoMODBUS/PORTING.md | Add | Record the exact upstream baseline, product configuration, local security fixes, and integration boundary |
| 2026-07-28 | Application/Transport/Inc/transport.h | Modify | Expose exact-length receive while preserving the existing partial-receive contract |
| 2026-07-28 | Application/Transport/Src/transport.c | Modify | Accumulate fragmented input under one wrap-safe absolute Tick deadline and record one logical receive operation |
| 2026-07-28 | Application/ProtocolStack/ModbusPort | Add | Add the unified nanoMODBUS-to-Transport port for runtime-selectable TCP/RTU, Client/Server roles, deadline clamping, fault details, and frame tracing |
| 2026-07-28 | Application/ProtocolStack/Modbus/README_LEGACY.md | Add | Mark the self-developed Modbus sources as retained reference code that is excluded from the active target |
| 2026-07-28 | Application/DeviceProtocol/MilkTea | Modify | Migrate MilkTea semantic APIs from the retired TCP-specific client to ModbusPort |
| 2026-07-28 | Application/DeviceProtocol/Robot | Modify | Migrate Robot register access from the retired TCP-specific client to ModbusPort |
| 2026-07-28 | Application/UserAPP/Modbus_Tcp_Client | Modify | Bind both TCP endpoints to ModbusPort and use the shared link-failure classification |
| 2026-07-28 | Application/UserAPP/Modbus_Rtu_Bus | Modify | Bind every UART bus instance to the same ModbusPort client implementation |
| 2026-07-28 | Application/UserAPP/Debug | Modify | Replace TCP-specific protocol types and frame snapshots with ModbusPort diagnostics |
| 2026-07-28 | MDK-ARM/MilkTea.uvprojx | Modify | Remove legacy Modbus sources from the target and add ModbusPort, nanoMODBUS, and their include paths |
| 2026-07-28 | 资料文档/奶茶机工程核心实现资料/固件开发文档/architecture-development-guidelines.md | Add | Define the authoritative five-layer, Transport contract, ModbusPort, timeout, error, ownership, extension, and agent modification rules |
| 2026-07-28 | 资料文档/奶茶机工程核心实现资料/固件开发文档/architecture-overview.md | Modify | Switch the L2 baseline to ModbusPort plus nanoMODBUS and identify the old self-developed implementation as reference-only |
| 2026-07-28 | 资料文档/奶茶机工程核心实现资料/固件开发文档/api-reference-manual.md | Modify | Replace obsolete TCP/RTU protocol-stack APIs with the unified ModbusPort and exact Transport receive APIs |
| 2026-07-28 | 资料文档/奶茶机工程核心实现资料/固件开发文档/source-code-walkthrough.md | Modify | Update the reading order and call chains for ModbusPort, nanoMODBUS, and exact Transport receive |
| 2026-07-28 | 资料文档/奶茶机工程核心实现资料/固件开发文档/奶茶机主控设计方案.md | Modify | Replace the retired TCP/RTU client object names with the unified ModbusPort ownership model |
| 2026-07-28 | 资料文档/奶茶机工程核心实现资料/固件开发文档/固件开发原理教程.md | Modify | Update the RTU extension-hook example to use ModbusPort |
| 2026-07-28 | 资料文档/奶茶机工程核心实现资料/日志相关文档/debug模块功能.md | Modify | Update the real Debug transaction path to ModbusPort and nanoMODBUS |
| 2026-07-28 | nanoMODBUS Server-enabled source configuration | Verify | Compile the complete nanoMODBUS core and ModbusPort Client/Server branches with ARM Compiler V5.06u7 without changing the delivered Client-only configuration |
| 2026-07-28 | MDK-ARM/codex_nanomodbus_final.log | Verify | Build the delivered ModbusPort plus nanoMODBUS target with ARM Compiler V5.06u7, zero errors, and zero warnings |
| 2026-07-28 | MDK-ARM/MilkTea/MilkTea.hex | Generate | Regenerate firmware with the unified TCP/RTU ModbusPort client and nanoMODBUS protocol core |
| 2026-07-28 | Application/DeviceModel and Application/DeviceProtocol | Document | Add file headers, public data-image, type, member, global, API, and private-helper documentation without changing behavior |
| 2026-07-28 | Application/ProtocolStack/ModbusPort | Document | Document configuration, roles, results, traces, faults, object ownership, every public API, and private deadline/Transport helpers |
| 2026-07-28 | Application/Transport | Document | Document Transport contracts, enums, faults, status, backend contexts, controls, public APIs, private helpers, DMA ownership, and HAL callback context |
| 2026-07-28 | Application/Diagnostics | Document | Document fatal reasons, bounded configuration, task records, captured globals, public entries, and private report-building helpers |
| 2026-07-28 | Application/UserAPP/Debug | Document | Document Debug configuration, permissions, registration descriptors, scratch ownership, public APIs, and parser/output helpers |
| 2026-07-28 | Application/UserAPP/Modbus_Tcp_Client | Document | Document endpoint configuration, lifecycle results, runtime status, object ownership, tasks, and reconnect helpers |
| 2026-07-28 | Application/UserAPP/Modbus_Rtu_Bus | Document | Document bus configuration, context and status ownership, task API, and device polling extension hook |
| 2026-07-28 | Application/UserAPP/Task_Manager, LED_Control, and WorkFlow | Document | Add file, macro, type, member, global, task, readiness, LED, and workflow comments |
| 2026-07-28 | 资料文档/奶茶机工程核心实现资料/固件开发文档/architecture-development-guidelines.md | Modify | Add the mandatory source-comment baseline for other agents |
| 2026-07-28 | MDK-ARM/codex_comment_pass1.log | Verify | ARM Compiler V5.06u7 comment-only build completed with zero errors and zero warnings |
| 2026-07-28 | Application/Diagnostics/Src/app_crash_fault_armcc.s | Document | Add the assembly file header and explain every exception, RTOS hook, common capture, and assertion entry |
| 2026-07-28 | MDK-ARM/codex_comment_final.log | Verify | Final ARM Compiler V5.06u7 documentation build completed with zero errors and zero warnings |
| 2026-07-30 | Application/UserAPP/Coffee2App/Config | Add | Define Coffee2 network, two-client Server, Robot, five RTU bus, UART override, queue, stack, and workflow parameters |
| 2026-07-30 | Application/UserAPP/Coffee2App/Comm_Log | Add | Add bounded USART6 INFO/WARN/ERROR logging and the fatal diagnostic polling-output compatibility port without a Debug task |
| 2026-07-30 | Application/UserAPP/Coffee2App/Device | Add | Add immutable device-to-route bindings, the standard command message, one static EventGroup per physical device, command routing, and observable status |
| 2026-07-30 | Application/UserAPP/Coffee2App/IO_State | Add | Add the global local-GPIO and 48-channel MB1/MB2 input/output image with atomic snapshot commits |
| 2026-07-30 | Application/DeviceProtocol/Coffee2Protocol | Add | Implement Coffee machine, cup, lid, syrup, ice, scale, power-meter, and Modbus IO device APIs over the shared ModbusPort |
| 2026-07-30 | Application/UserAPP/Coffee2App/Modbus_Rtu_Bus | Add | Add five serialized RTU owner tasks, device dispatch, Bus4 transaction-boundary baud switching, retries, and application-owned UART reinitialization |
| 2026-07-30 | Application/UserAPP/Coffee2App/Robot_Tcp | Add | Add Robot Modbus TCP actions, status/alarm checks, and 1/2/5/10/30-second reconnect backoff |
| 2026-07-30 | Application/UserAPP/Coffee2App/Modbus_Tcp_Server | Add | Add the port-6001 nanoModbus Server with exactly two reusable client slots, order and maintenance registers, status projection, and private monitoring registers |
| 2026-07-30 | Application/UserAPP/Coffee2App/WorkFlow | Add | Add the Coffee2 order state machine, per-command event waits, cancellation/error exit, scale-controlled ice dispensing, host printing, and idle device refresh |
| 2026-07-30 | Application/UserAPP/Coffee2App/Task_Manager | Add | Initialize the complete Coffee2 product, reapply UART/network parameters, create nine application tasks, and retain the 32KB FreeRTOS heap in CCM |
| 2026-07-30 | Application/Transport/Inc/transport.h | Modify | Increase the fixed Transport registry capacity to cover the Coffee2 log, five RTU buses, Robot, and two Server slots |
| 2026-07-30 | MDK-ARM/Coffee2_CCM.sct | Add | Preserve the common CCM heap layout while selecting Keil's Coffee2-specific app_task_manager_1.o object name |
| 2026-07-30 | MDK-ARM/STM32F407_Base.uvprojx | Modify | Complete the Coffee2 Target source groups, IncludeInBuild isolation, include paths, nanoModbus Server preinclude, output directory, and target-specific scatter file |
| 2026-07-30 | 资料文档/店中店咖啡机工程核心实现资料/固件开发文档/Coffee2任务与通信架构设计.md | Rewrite | Document the implemented target boundary, tasks, buses, commands, events, two-slot Server, protocols, workflow, CCM layout, build evidence, and field-validation limits |
| 2026-07-30 | MDK-ARM/Coffee2_build_final.log | Verify | Build Coffee2 with ARM Compiler V5.06u7, zero errors and zero warnings |
| 2026-07-30 | MDK-ARM/MilkTea_build_validation.log | Verify | Rebuild MilkTea with ARM Compiler V5.06u7, zero errors and zero warnings, confirming target isolation and the unchanged common CCM heap design |
| 2026-07-30 | Application/UserAPP/Coffee2App/Comm_Log | Modify | Match the MilkTea structured log format, increase the static queue to 32 entries, add result/detail fields, and preserve a pre-scheduler polling output path |
| 2026-07-30 | Application/UserAPP/Coffee2App/Task_Manager | Modify | Report reset cause, module initialization, all nine task creation/running states, heap state, LwIP readiness, and direct boot failures while retaining the CCM heap |
| 2026-07-30 | Application/UserAPP/Coffee2App/Robot_Tcp | Modify | Add observable connection lifecycle, 1/2/5/10/30-second retry scheduling, link-loss recovery, service-ready logs, and reconnect counters |
| 2026-07-30 | Application/UserAPP/Coffee2App/Modbus_Tcp_Server | Modify | Create Client slot protocol resources on demand, define one active slot as Server online, recreate failed listeners, log both-slot lifecycle, and extend private monitoring registers |
| 2026-07-30 | Application/UserAPP/Coffee2App/Modbus_Rtu_Bus | Modify | Add Bus readiness, rate-limited command failures, and per-device online/offline logs without adding TCP-style reconnect behavior |
| 2026-07-30 | Application/UserAPP/Coffee2App/WorkFlow | Modify | Add order, manual ice, step, queue, device-failure, timeout, cancellation, and completion logs |
| 2026-07-30 | 资料文档/店中店咖啡机工程核心实现资料/固件开发文档/Coffee2任务与通信架构设计.md | Modify | Document startup logging, lazy two-slot Server behavior, TCP recovery, RTU ownership, revised monitoring registers, and the second-pass requirement audit |
| 2026-07-30 | MDK-ARM/Coffee2_build_log_selfcheck.log | Verify | Rebuild the completed Coffee2 logging and connection-lifecycle implementation with ARM Compiler V5.06u7, zero errors and zero warnings |
| 2026-07-30 | MDK-ARM/MilkTea_build_validation_selfcheck.log | Verify | Rebuild MilkTea with zero errors and zero warnings after the Coffee2 second-pass changes |
| 2026-07-31 | MDK-ARM/ScatterFiles/Coffee2_CCM.sct | Fix | Split zero-initialized CCM application state from the UNINIT FreeRTOS heap so startup clears queue handles, EventGroup handles, and protocol state while retaining the 32 KB heap in CCM |
| 2026-07-31 | Application/UserAPP/Coffee2App/Coffee2_IMPLEMENTATION_REPORT.md | Modify | Correct the Coffee2 CCM map documentation to record the separate RW_CCM_APP and RW_CCM_HEAP execution regions |
| 2026-07-31 | MDK-ARM/Coffee2_ccm_zero_init_fix_build.log | Verify | Build Coffee2 with ARM Compiler V5.06u7 at zero errors and zero warnings after the CCM initialization fix |
| 2026-07-31 | MDK-ARM/MilkTea_after_coffee2_ccm_fix_build.log | Verify | Rebuild MilkTea at zero errors and zero warnings to confirm the Coffee2-only scatter change preserves target isolation |
| 2026-07-31 | MDK-ARM/ScatterFiles/Coffee2_CCM.sct | Optimize | Replace the fixed 32 KB CCM split with one startup-zeroed 64 KB CCM execution region, retaining the 32 KB FreeRTOS heap and allowing all remaining CCM capacity to be used by CPU-only application objects |
| 2026-07-31 | Application/UserAPP/Coffee2App/Coffee2_IMPLEMENTATION_REPORT.md | Modify | Document the unified zero-initialized CCM layout, startup cost, Flash behavior, and remaining capacity |
| 2026-07-31 | MDK-ARM/Coffee2_ccm_unified_zero_init_build.log | Verify | Build Coffee2 with ARM Compiler V5.06u7 at zero errors and zero warnings and verify one initialized 64 KB CCM region with 32 KB heap and reusable remaining capacity |
| 2026-07-31 | Application/UserAPP/Coffee2App/Config/coffee2_app_config.h | Clarify | Mark CCM_APP as reviewed CPU-only startup-cleared placement rather than a general application-data destination |
| 2026-07-31 | Application/UserAPP/Coffee2App/Coffee2_IMPLEMENTATION_REPORT.md | Optimize | Record the per-object CCM admission policy, retained CPU-only objects, SRAM-resident DMA boundaries, and memory headroom |
| 2026-07-31 | MDK-ARM/Coffee2_ccm_policy_build.log | Verify | Build Coffee2 with ARM Compiler V5.06u7 at zero errors and zero warnings and confirm the reviewed CCM_APP, heap, DMA staging, Ethernet, lwIP, and crash-record placements in the map file |
| 2026-08-01 | Application/UserAPP/Coffee2App/COFFEE2_使用与调试手册.md | Add | Document Coffee2 Modbus TCP/RTU topology, two-client Server operation, register-level single-device tests, Robot/RTU diagnostics, logs, order cancellation rules, and current protocol limits |
| 2026-08-01 | Application/UserAPP/Coffee2App/COFFEE2_工程拆解与闭环设计手册.md | Add | Decompose the Coffee2 five-layer architecture, task and queue ownership, per-device EventGroups, workflow closure, TCP/RTU boundaries, CCM policy, and order/epoch stale-event risk |
| 2026-08-01 | Application/UserAPP/Coffee2App/Task_Manager/coffee2_manager.c | Document | Add a UTF-8 Chinese Doxygen header for prvCreateTaskLogged, documenting every parameter, return status, ownership, and task-creation side effect without changing code behavior |
| 2026-08-01 | MDK-ARM/Objects/Coffee2/Coffee2.build_log.htm | Verify | Rebuild Coffee2 Target with ARM Compiler V5.06u7 after the comment-only change: 0 errors and 0 warnings |
| 2026-08-01 | Application/UserAPP/Coffee2App/Comm_Log, Device, IO_State, Modbus_Rtu_Bus, Modbus_Tcp_Server, Robot_Tcp, Task_Manager, WorkFlow | Document | Complete Chinese UTF-8 Doxygen headers for Coffee2 private functions, including parameters, return values, units, ownership, task context, and failure behavior; remove the duplicate prvCreateTaskLogged brief |
| 2026-08-01 | Application/DeviceProtocol/Coffee2Protocol | Document | Complete function headers for all Coffee2 device protocol APIs and private wait/conversion helpers |
| 2026-08-01 | Application/Transport, Application/ProtocolStack/ModbusPort, Application/Diagnostics | Document | Complete common Transport, TCP/UART, ModbusPort, and crash-diagnostic function headers used by Coffee2 |
| 2026-08-01 | Application/Transport/Inc/transport.h | Document | Add missing return-value documentation for xTransportReceive |
| 2026-08-01 | Application/UserAPP/Coffee2App/COFFEE2_注释审计与编译校验报告.md | Add | Record the full function-comment scan, coverage counts, UTF-8 validation, scope exclusions, and final Keil build result |
| 2026-08-01 | MDK-ARM/Objects/Coffee2/Coffee2.build_log.htm | Verify | Final Coffee2 comment-audit build with ARM Compiler V5.06u7: 0 errors and 0 warnings |
| 2026-08-01 | Application/UserAPP/Coffee2App, Application/Transport, Application/ProtocolStack/ModbusPort | Normalize | Convert audited-scope line comments to UTF-8 block comments without changing executable tokens |
| 2026-08-04 | GCC-ARM/CMakeLists.txt | Add | Build the shared Project_Base sources as isolated Coffee2Target or MilkTeaTarget firmware and emit ELF, HEX, BIN, MAP, and size summaries |
| 2026-08-04 | GCC-ARM/CMakePresets.json | Add | Add Coffee2/MilkTea Debug/Release configure and build presets with separate build directories |
| 2026-08-04 | GCC-ARM/cmake/gcc-arm-none-eabi.cmake | Add | Pin the project-local GNU Arm compiler, Cortex-M4F ABI, Debug/Release options, linker script, and memory usage reporting |
| 2026-08-04 | GCC-ARM/cmake/stm32cubemx/CMakeLists.txt | Add | Adapt the CubeMX source manifest to the shared Core, Drivers, Middlewares, and LWIP directories without duplicating source trees |
| 2026-08-04 | GCC-ARM/linker/STM32F407XX_FLASH.ld | Add | Add the GNU linker layout for Flash, SRAM, and startup-cleared CCM application storage |
| 2026-08-04 | GCC-ARM/startup_stm32f407xx.s | Add | Add the GNU assembler startup and vector table used only by the GCC shell |
| 2026-08-04 | GCC-ARM/scripts/build.ps1 | Add | Add an explicit Coffee2/MilkTea and Debug/Release preset build wrapper using only project-local tools |
| 2026-08-04 | GCC-ARM/scripts/flash.ps1 | Add | Add a guarded OpenOCD default programming flow and optional ST-LINK Utility compatibility backend |
| 2026-08-04 | GCC-ARM/scripts/setup_toolchain.ps1 | Add | Add pinned tool restoration with cache reuse, GCC archive checksum validation, and OpenOCD availability reporting |
| 2026-08-04 | GCC-ARM/.tools | Add local | Reuse the existing CMake, Ninja, GNU Arm GCC/GDB, cached GCC archive, and OpenOCD bundle without downloading duplicates; directory is ignored by Git |
| 2026-08-04 | Application/CMakeLists.txt | Add | Define common diagnostic/transport/Modbus object libraries and exact per-product Coffee2/MilkTea source manifests |
| 2026-08-04 | Application/Common/compiler_compat.h | Add | Normalize ARMCC V5 and GCC compiler detection plus CCM application/heap attributes for shared sources |
| 2026-08-04 | Application/Platform/Inc/app_ccm.h, Application/Platform/Src/app_ccm.c | Add | Clear the GNU .ccm_bss region before the RTOS starts while leaving ARMCC scatter initialization unchanged |
| 2026-08-04 | Application/Diagnostics/Src/app_crash_fault_gcc.S | Add | Add the GNU Cortex-M4 HardFault register-capture entry and reuse the common crash diagnostic recorder |
| 2026-08-04 | Core/Src/main.c | Modify | Guard ARMCC semihosting directives, initialize GCC CCM storage, and initialize common crash diagnostics inside CubeMX user sections |
| 2026-08-04 | Core/Src/freertos.c | Modify | Keep compiler-specific HardFault ownership clear and make the stack overflow hook warning-clean for GCC |
| 2026-08-04 | Application/UserAPP/Coffee2App/Config/coffee2_app_config.h, Application/UserAPP/Coffee2App/Task_Manager/coffee2_manager.c | Modify | Route Coffee2 CCM application data and FreeRTOS heap through the ARMCC/GCC compatibility macros |
| 2026-08-04 | Application/UserAPP/MilkTeaApp/Task_Manager/app_task_manager.c | Modify | Route the MilkTea FreeRTOS heap through the shared ARMCC/GCC CCM compatibility macro |
| 2026-08-04 | Application/ProtocolStack/ModbusPort/Inc/modbus_port.h, Application/DeviceProtocol/Coffee2Protocol/coffee2_rtu_protocol.c | Fix | Synchronize the reviewed Modbus port contract and Coffee2 RTU protocol corrections from the reference GCC migration |
| 2026-08-04 | Middlewares/New_Party/nanoMODBUS/CMakeLists.txt, Middlewares/New_Party/nanoMODBUS/Config/nanomodbus_config.h, Middlewares/New_Party/nanoMODBUS/UPSTREAM_VERSION.md | Modify | Integrate the reviewed nanoMODBUS configuration and record the upstream baseline in the multi-toolchain build |
| 2026-08-04 | Application/UserAPP/Coffee2App/Device, Application/UserAPP/Coffee2App/Modbus_Rtu_Bus, Application/UserAPP/Coffee2App/Robot_Tcp, Application/UserAPP/Coffee2App/WorkFlow | Fix | Synchronize reviewed device, RTU, robot TCP, cancellation, stale-event, timeout, and workflow safety corrections from Project_Base_cmake |
| 2026-08-04 | LWIP/Target/ethernetif.c | Fix | Synchronize the reviewed Ethernet interface implementation and raise the input task stack from 350 to 1024 words |
| 2026-08-04 | .vscode/c_cpp_properties.json, .vscode/settings.json | Add | Add product-specific compile database selection and project-local CMake/compiler paths for VS Code editing |
| 2026-08-04 | .vscode/tasks.json, .vscode/launch.json | Add | Add six build tasks, guarded OpenOCD programming tasks, and Cortex-Debug configurations for Coffee2 and MilkTea |
| 2026-08-04 | .gitignore | Add | Exclude project-local tool binaries and generated GCC build artifacts from version control |
| 2026-08-04 | GCC-ARM/build/Coffee2-Debug, Coffee2-Release, MilkTea-Debug, MilkTea-Release | Verify | Configure, compile, link, and generate ELF/HEX/BIN/MAP successfully with GNU Arm GCC 15.2.1 |
| 2026-08-04 | 资料文档/Project_Base_GCC-ARM多Target迁移实施与验收报告.md | Add | Document architecture, compiler compatibility, logging and crash fixes, manual CMake learning, build, flash, debug, CubeMX regeneration, migration, and validation results |
| 2026-08-05 | 资料文档/Coffee2_GCC-ARM开发操作手册.md | Add | Provide step-by-step GCC toolchain, manual CMake, scripts, OpenOCD flashing, VS Code/Cortex-Debug, troubleshooting, CubeMX regeneration, and new-PC recovery instructions |
| 2026-08-05 | CubeMX_Base/Middlewares/Third_Party | Merge | Add the generated FreeRTOS and LwIP baseline and retain both GCC/ARM_CM4F and RVDS/ARM_CM4F compiler ports in one formal shared CubeMX source tree |
| 2026-08-05 | Middlewares/New_Party/nanoMODBUS | Add shared | Establish the reviewed nanoMODBUS implementation as the canonical common dependency used by both MDK and GCC shells; retain the old unreferenced MDK copy for rollback |
| 2026-08-05 | GCC-ARM/CMakeLists.txt, GCC-ARM/cmake/stm32cubemx/CMakeLists.txt | Modify | Introduce CUBEMX_BASE_ROOT and redirect generated Core, Drivers, LWIP, and Third_Party middleware references to CubeMX_Base while keeping Application at the project root |
| 2026-08-05 | MDK-ARM/STM32F407_Base.uvprojx | Modify | Redirect MilkTea and Coffee2 to CubeMX_Base, retain shared Application and nanoMODBUS, and select the RVDS FreeRTOS port |
| 2026-08-05 | GCC-ARM/build/* | Verify | Fresh-configure and build Coffee2/MilkTea Debug/Release with GNU Arm GCC 15.2.1; all four ELF/HEX/BIN outputs linked successfully |
| 2026-08-05 | MDK-ARM/BuildLogs | Verify | Build MilkTea and Coffee2 with ARM Compiler V5.06u7; both Targets completed with zero errors and zero warnings |
| 2026-08-05 | ST-LINK/OpenOCD hardware validation | Verify | Probe STM32F407 over SWD, program and verify the Keil Coffee2 AXF, then program and verify GCC Coffee2-Debug ELF; the GCC image remains on the board |
| 2026-08-05 | 资料文档/双工具链/CubeMX_Base双工具链多Target迁移与验收报告.md | Add | Document source ownership, CubeMX merge rules, compiler-port isolation, build/flash/debug usage, actual validation evidence, and new-PC recovery |
| 2026-08-05 | 工程维护Agent提示词.md | Add | Add the maintenance-agent prompt governing CubeMX staging scans, safe path migrations, compiler-port isolation, six-build verification, flash authorization, and rollback-safe cleanup |
| 2026-08-05 | 工程维护Agent提示词.md | Fix | Normalize maintenance examples as valid one-line PowerShell commands rather than shell-style line continuations |
| 2026-08-05 | CubeMX_Base/Core/Src/dma.c | Modify | Enable DMA1 clock and configure 6 DMA interrupt priorities (Stream3/4/6/7, Stream6/7) for UART TX DMA |
| 2026-08-05 | CubeMX_Base/Core/Src/usart.c, Core/Inc/usart.h | Modify | Add 6 UART TX DMA handles (UART4/UART5/USART1/USART2/USART3/USART6) and MspInit/MspDeInit DMA configuration |
| 2026-08-05 | CubeMX_Base/Core/Src/stm32f4xx_it.c, Core/Inc/stm32f4xx_it.h | Modify | Add 5 DMA IRQ handlers (DMA1_Stream3/4/6/7, DMA2_Stream6) routing to the corresponding TX DMA handles |
| 2026-08-05 | MDK-ARM/BuildLogs/Coffee2_txdma.log, MilkTea_txdma.log | Verify | Rebuild Coffee2 and MilkTea with ARM Compiler V5.06u7 after TX DMA merge: both 0 errors and 0 warnings |
| 2026-08-05 | GCC-ARM/build/* | Verify | Rebuild four GCC Presets (Coffee2/MilkTea Debug/Release) after TX DMA merge: all four ELF linked successfully |
| 2026-08-05 | Application/ProtocolStack/ModbusPort/Src/modbus_port.c | Add | Add prvWaitFrameSilence() to enforce 3.5-character RTU frame silence using the current baud rate before every transaction, fixing slave frame-boundary mis-detection |
| 2026-08-05 | Application/UserAPP/Coffee2App/Config/coffee2_app_config.h, Device/coffee2_device.c | Modify | Enable a 20 ms IO-module minimum frame interval for the Bus5 IO input/output bindings |
| 2026-08-05 | GCC-ARM/scripts/build.ps1 | Fix | Switch to the GCC-ARM shell root before invoking cmake so --preset resolves CMakePresets.json from any working directory |
| 2026-08-05 | 根目录 cmake/ 冗余遗留 | Note | 顶层 cmake/ 为 CubeMX 生成遗留目录,当前无构建引用;正式 GCC 构建使用 GCC-ARM/cmake |
| 2026-08-07 | Application/UserAPP/Coffee2App/Comm_Log/coffee2_log.h, coffee2_log.c; Modbus_Rtu_Bus/coffee2_rtu_bus.c | Fix | Preserve MBRtu for bus-level events while assigning Coffee, Cup, Syrup, Lid, Ice, Weigh, EnergyMeter, IoInput, and IoOutput modules to device-level RTU online, offline, and failure logs |
| 2026-08-07 | GCC-ARM/build/Coffee2-Debug, Coffee2-Release; MDK-ARM/Objects/Coffee2/Coffee2.build_log.htm | Verify | Build Coffee2 Debug and Release with GNU Arm GCC 15.2.1, then build the Keil Coffee2 Target with ARM Compiler V5.06u7; all three firmware links completed successfully and the Keil build reported 0 errors and 0 warnings |
| 2026-08-10 | 资料文档/工程总览2026/01_架构和技术栈梳理.md | Add | 梳理 STM32F407 双目标架构、目录边界、启动链、任务/同步、协议、IO、诊断、CCM 与双工具链事实 |
| 2026-08-10 | 资料文档/工程总览2026/02_源码阅读指南.md | Add | 提供上电阅读路线、Coffee2 本地/远程 IO 与冰量动作端到端调用链、MilkTea 主链、断点和反查方法 |
| 2026-08-10 | 资料文档/工程总览2026/03_双工具链详细说明手册.md | Add | 说明 ARMCC V5.06u7 与 GCC/CMake/Ninja 入口、target、产物、CCM/链接差异、安全脚本和一致性验收 |
| 2026-08-10 | Application/UserAPP/Coffee2App/Modbus_Tcp_Server/coffee2_server.h | Modify | Define the fixed 0x0084-0x0086 manual IO operation and 0x1084-0x108F status register map |
| 2026-08-10 | Application/UserAPP/Coffee2App/Modbus_Tcp_Server/coffee2_server.c | Modify | Validate and submit fixed IO commands, log assumed manual cup state, and publish external IO bitmaps and command diagnostics |
| 2026-08-10 | Application/UserAPP/Coffee2App/Device/coffee2_device.c | Modify | Add centralized SERVER manual-command running and terminal-result logs with raw action results |
| 2026-08-10 | Application/DeviceProtocol/Coffee2Protocol/coffee2_rtu_protocol.c | Modify | Verify IO FC05 writes with same-unit FC02/FC01 48-point reads and commit the coherent image |
| 2026-08-10 | Application/UserAPP/Coffee2App/Config/coffee2_app_config.h | Modify | Limit ice valve pulses to 2000 ms and add the integer compensation factor |
| 2026-08-10 | Application/UserAPP/Coffee2App/WorkFlow/coffee2_workflow.c | Modify | Apply the integer ice compensation factor without changing the factor-one pulse result |
| 2026-08-10 | GCC-ARM/build/Coffee2-Debug | Verify | Configure and build Coffee2-Debug with bundled CMake 4.3.4/Ninja and GNU Arm GCC 15.2.1; link succeeded with generated-source warnings only |
| 2026-08-10 | 资料文档/MDK工具链/店中店咖啡机工程核心实现资料/固件开发文档/调试手册/Coffee2_单机设备测试使用说明书.md | Add | Document the source-truth single-machine commissioning procedure, fixed host registers, per-device FC06/FC16 sequences, IO bitmap verification, safety gates, result codes, and unimplemented-command prohibition |
| 2026-08-10 | 资料文档/MDK工具链/店中店咖啡机工程核心实现资料/固件开发文档/调试手册/Coffee2_Keil_V5仿真与源码排查手册.md | Add | Document MDK-ARM/ARMCC V5.06 Simulator versus ST-Link/J-Link boundaries, Watch symbols, breakpoint chains, result-code triage, IO readback timeout isolation, and HardFault/stack checks |
| 2026-08-10 | Application/UserAPP/Coffee2App/Modbus_Tcp_Server/coffee2_server.c | Fix | Let IO operation 2/3 submit direct input/output refresh without validating ignored point/value fields; retain strict validation for operation 1 |
| 2026-08-10 | GCC-ARM/build/Coffee2-Debug | Verify | Rebuild Coffee2-Debug after the IO refresh validation fix with bundled CMake 4.3.4/Ninja and GNU Arm GCC; compile and link completed successfully |
| 2026-08-11 | Application/UserAPP/Coffee2App/Config/coffee2_app_config.h | Modify | Unify Bus4 device serial profiles by setting scale and power-meter baud macros to 19200 while retaining all other Coffee2 baud rates |
| 2026-08-11 | 资料文档/MDK工具链/店中店咖啡机工程核心实现资料/固件开发文档/调试手册/Coffee2_单机设备测试使用说明书.md | Modify | Update the single-machine route table with UART/Unit/8N1/no-parity/no-flow-control settings and the unified Bus4 19200 requirement |
| 2026-08-11 | 资料文档/MDK工具链/店中店咖啡机工程核心实现资料/固件开发文档/架构设计/Coffee2任务与通信架构设计.md | Modify | Correct Bus4 binding and runtime documentation to show scale and power meter at 19200 and describe defensive-only profile switching |
| 2026-08-11 | 资料文档/MDK工具链/店中店咖啡机工程核心实现资料/固件开发文档/架构设计/Coffee2设备挂载无缝衔接.md | Modify | Replace stale mixed-baud Bus4 examples with the current all-19200 scenario while retaining the real compatibility path |
| 2026-08-11 | 资料文档/MDK工具链/店中店咖啡机工程核心实现资料/固件开发文档/调试手册/Coffee2_波特率统一修改同步说明_2026-08-11.md | Add | Provide same-baseline relative-path synchronization, backup/rollback, static checks, GCC/Keil rebuild, hardware settings, and RTU log acceptance guidance |
| 2026-08-11 | GCC-ARM/build/Coffee2-Debug | Verify | Rebuild Coffee2-Debug after Bus4 scale/power baud unification with bundled CMake 4.3.4/Ninja; compile and link completed successfully |
| 2026-08-11 | 资料文档/03_技术实现核心文档/店中店咖啡机工程核心实现资料/04_执行报告与审计/Coffee1与Coffee2单机测试兼容性对比报告_2026-08-11.md | Add | Compare Coffee1 and Coffee2 host commands, bus bindings, downstream device semantics, compatibility gaps, minimum migration work, and staged hardware acceptance for single-device commissioning only |
| 2026-08-11 | 交付包/Coffee2_Bus4_19200_同步包_2026-08-11.zip | Add | Package the six changed source, documentation, synchronization, and change-log files with project-relative paths for same-baseline computers |
| 2026-08-11 | Application/UserAPP/Coffee2App/Comm_Log/coffee2_log.c, Comm_Log/coffee2_log.h | Modify | Replace the 32-entry static log queue with an overwrite-oldest ring and static binary wake signal; retain recent records on transport/task failure, expose buffer/transport/task/output-pause states, count overwrites/retries, and apply bounded send backoff without changing DMA staging |
| 2026-08-11 | Application/UserAPP/Coffee2App/Task_Manager/coffee2_manager.c, Task_Manager/coffee2_manager.h | Modify | Treat log UART/Transport as degradable, create business tasks before the optional C2Log task, and report ucLogReady only when Transport and C2Log are ready |
| 2026-08-11 | Application/UserAPP/Coffee2App/Modbus_Rtu_Bus/coffee2_rtu_bus.c, Modbus_Rtu_Bus/coffee2_rtu_bus.h | Modify | Split log UART default configuration from business UART defaults so a log HAL error cannot block RTU startup |
| 2026-08-11 | 资料文档/03_技术实现核心文档/店中店咖啡机工程核心实现资料/06_日志/日志架构与输出配置说明.md, 日志解读与排障手册.md | Modify | Document Coffee2 ring overwrite semantics, static signal wakeup, degradable output states, task ordering, bounded retry, and Watch/DMA diagnostics |
| 2026-08-11 | GCC-ARM/build/Coffee2-Debug | Verify | Final rebuild with bundled CMake 4.3.4/Ninja and GNU Arm GCC; compile and link completed successfully (RAM 98792 B, CCMRAM 39280 B, FLASH 212464 B) |
| 2026-08-12 | Application/UserAPP/Coffee2App/Robot_Tcp/coffee2_robot_tcp.c | Fix | Replace immediate startup-state checks with bounded drag/enable/ready waits, explicit stage/state-mask diagnostics, local startup-not-ready and safety outcomes, connected-session startup retry, and preserve permanent reconnect only for link failures |
| 2026-08-12 | Application/UserAPP/Coffee2App/Config/coffee2_app_config.h | Verify | Keep the reviewed Coffee2 Robot task stack baseline at `COFFEE2_ROBOT_TASK_STACK=1024U` words for synchronization and stack-high-water safety |
| 2026-08-12 | 资料文档/03_技术实现核心文档/店中店咖啡机工程核心实现资料/04_执行报告与审计/Coffee2机器人启动等待修复报告_2026-08-12.md | Add | Record Coffee2 Robot startup sequence, outcome classification, diagnostics, validation, 1024-word stack baseline, and same-baseline synchronization instructions |
| 2026-08-13 | Application/UserAPP/Coffee2App/Robot_Tcp/coffee2_robot_tcp.c; Application/UserAPP/Coffee2App/Device/coffee2_device.c/.h; Application/UserAPP/Coffee2App/Modbus_Tcp_Server/coffee2_server.c; Application/UserAPP/Coffee2App/WorkFlow/coffee2_workflow.c; Application/UserAPP/Coffee2App/Config/coffee2_app_config.h | Enhance | Add a 100 ms owner-loop Robot handshake with unlimited command-acceptance wait, 60 s post-acceptance motion timeout, strict command/result reconciliation, static terminal history for manual supersession, urgent manual Robot routing, accepted-phase Workflow timing, and latest pending order replacement without new RTOS resources. |
| 2026-08-13 | GCC-ARM/build/Coffee2-Debug, Coffee2-Release, MilkTea-Debug, MilkTea-Release | Verify | Build all four GCC presets successfully after the Robot handshake integration; Coffee2 Debug uses RAM 98792 B, CCMRAM 40872 B, FLASH 224704 B, and Coffee2 Release uses RAM 98832 B, CCMRAM 40872 B, FLASH 188148 B. |
| 2026-08-14 | Application/UserAPP/Coffee2App/Ota/coffee2_ota_flash.c, coffee2_ota_flash.h | Add | Add the static Coffee2 Flash staging writer, vector/MSP validation, bootloader-compatible padded CRC, readback checks, exact four-word metadata commit, and abort-safe sector handling. |
| 2026-08-14 | Application/UserAPP/Coffee2App/Ota/coffee2_ota_http.c, coffee2_ota_http.h | Add | Add the single-session raw-lwIP port-80 multipart browser upload endpoint with bounded parser storage, deterministic extra-client rejection, progress/error logs, delayed reset, and no heap allocation. |
| 2026-08-14 | Application/UserAPP/Coffee2App/Task_Manager/coffee2_manager.c | Modify | Start the Coffee2 OTA listener through the tcpip callback after the existing network initialization. |
| 2026-08-14 | Application/UserAPP/Coffee2App/Modbus_Tcp_Server/coffee2_server.c | Modify | Reject new Modbus write/order/manual/IO submissions while Coffee2 OTA admission or upload is active without changing the register ABI. |
| 2026-08-14 | Application/CMakeLists.txt | Modify | Compile and expose the Coffee2-only OTA source files and include directory. |
| 2026-08-14 | GCC-ARM/CMakeLists.txt | Modify | Select the relocated Coffee2 linker at the target boundary, keep MilkTea on the original linker, and retain four product/build presets. |
| 2026-08-14 | GCC-ARM/cmake/gcc-arm-none-eabi.cmake | Modify | Keep common GNU Arm flags in the toolchain and use Coffee2-only `-Og` Debug initialization while leaving MilkTea Debug at `-O0`. |
| 2026-08-14 | GCC-ARM/linker/STM32F407XX_COFFEE2_OTA_FLASH.ld | Add | Copy the complete base linker layout and relocate only Coffee2 FLASH to `0x0800C000`/`0x34000`, with `_estack` eight bytes below the SRAM end for the bootloader MSP mask. |
| 2026-08-14 | MDK-ARM/ScatterFiles/Coffee2_CCM.sct | Modify | Relocate only the Coffee2 Keil load/execute regions to `0x0800C000` with a `0x34000` hard limit; retain the MilkTea scatter file. |
| 2026-08-14 | MDK-ARM/STM32F407_Base.uvprojx | Modify | Add Coffee2-only OTA sources/include path and vector definitions, select the Coffee2 scatter file, and leave the MilkTea target excluded. |
| 2026-08-14 | 资料文档/03_技术实现核心文档/店中店咖啡机工程核心实现资料/Coffee2_OTA同步说明_2026-08-14.md | Add | Document exact same-baseline file synchronization, initial programming addresses, browser upload of `Coffee2Target.bin`, admission gates, and four-preset verification. |
| 2026-08-14 | GCC-ARM/build/Coffee2-Debug, Coffee2-Release, MilkTea-Debug, MilkTea-Release | Verify | Fresh-configure and build all four presets with bundled CMake 4.3.4/Ninja and GNU Arm GCC 15.2.1; Coffee2 vectors are `0x0800C000`, MilkTea vectors remain `0x08000000`, and all images fit their linker regions. |
| 2026-08-17 | Application/UserAPP/Coffee2App/Task_Manager/coffee2_manager.c; Modbus_Tcp_Server/coffee2_server.c; Ota/coffee2_ota_http.c | Fix | Align the Coffee2 OTA trigger flow with coffee_close_v2.7.23: do not listen on HTTP port 80 at boot, start it through the lwIP tcpip callback only after FC06 `0x0200=1`, allow listener-init retry, retain FC06 `0x0201=1` as a 50 ms delayed software reset, and expose the legacy upload-success marker without adding RTOS resources. |
| 2026-08-17 | MDK-ARM/STM32F407_Base.uvprojx | Modify | Add the Coffee2-only AfterMake command `fromelf --bin -o "$L@L.bin" "#L` so every successful Keil Coffee2 build emits the OTA `.bin`; keep the MilkTea AfterMake settings unchanged. |
| 2026-08-17 | 资料文档/03_技术实现核心文档/店中店咖啡机工程核心实现资料/Coffee2_OTA同步说明_2026-08-14.md | Modify | Document the `0x0200` command-triggered port-80 lifecycle, `0x0201` delayed reset, `.bin`-only upload rule, Keil AfterMake output, and same-baseline synchronization procedure. |
| 2026-08-17 | GCC-ARM/build/Coffee2-Debug, Coffee2-Release, MilkTea-Debug, MilkTea-Release | Verify | Fresh-configure and build all four presets with bundled CMake/Ninja and GNU Arm GCC 15.2.1; Coffee2 Debug uses RAM 101680 B, CCMRAM 40872 B, FLASH 163232 B; Coffee2 Release uses RAM 101728 B, CCMRAM 40872 B, FLASH 195988 B; MilkTea Debug/Release remain successful at FLASH 168996 B/147352 B. |
| 2026-08-17 | MDK-ARM/Objects/Coffee2 | Verify | Rebuild Coffee2 with Keil uVision 5.39 and ARM Compiler V5.06u7: 0 errors, 0 warnings; AfterMake generated `Coffee2.bin` at 190076 B with MSP `0x2001E000`, reset vector `0x0800C389`, and 22916 B application-slot headroom. |
| 2026-08-17 | Application/UserAPP/Coffee2App/Ota/coffee2_ota_flash.c | Modify | Align OTA admission with coffee_close_v2.7.23 by removing Workflow and device Busy/Online gates; retain the atomic active-session guard and all Flash, vector, length, CRC, metadata, and abort checks. |
| 2026-08-17 | 资料文档/03_技术实现核心文档/店中店咖啡机工程核心实现资料/Coffee2_OTA同步说明_2026-08-14.md | Modify | Clarify that the host controls OTA timing, the firmware no longer rejects OTA because Workflow or device Busy state is active, and the upload-session write lock remains. |
| 2026-08-17 | GCC-ARM/build/Coffee2-Debug, GCC-ARM/build/Coffee2-Release | Verify | Fresh-configure and build after removing OTA admission gates; Coffee2 Debug uses FLASH 163180 B and Coffee2 Release uses FLASH 195880 B, both within the 0x34000 application region. |
| 2026-08-17 | MDK-ARM/Objects/Coffee2 | Verify | Rebuild Coffee2 with Keil uVision 5.39 and ARM Compiler V5.06u7: 0 errors, 0 warnings; generated `Coffee2.bin` is 190008 B with MSP `0x2001E000`, reset vector `0x0800C389`, and SHA-256 `02DA0AAD85D2EC04B36E187DC7D0855B21D321F075BBEC6CD2D0A0A4F01317A3`. |
| 2026-08-17 | Application/UserAPP/Coffee2App/Ota/coffee2_ota_http.c | Enhance | On HTTP `-5`/multipart `-6` rejection, log the current `http://<device-ip>/upload.cgi` URL and up to 512 received header bytes directly through the existing bounded logger; do not log firmware body data or add RTOS resources. |
| 2026-08-17 | GCC-ARM/build/Coffee2-Debug, GCC-ARM/build/Coffee2-Release | Verify | Fresh-configure and build after adding rejected-header diagnostics; Coffee2 Debug uses RAM 101680 B, CCMRAM 40872 B, FLASH 163916 B, and Coffee2 Release uses RAM 101728 B, CCMRAM 40872 B, FLASH 196700 B. |
| 2026-08-17 | MDK-ARM/Objects/Coffee2 | Verify | Rebuild rejected-header diagnostics with Keil uVision 5.39 and ARM Compiler V5.06u7: 0 errors, 0 warnings; generated `Coffee2.bin` is 190760 B with MSP `0x2001E000`, reset vector `0x0800C389`, and SHA-256 `C08172A40161DB068A1EED8BC28D2DE2EA434D4E1E3F6C569D8D1598B22F2F9D`. |
| 2026-08-20 | 资料文档/03_技术实现核心文档/通用设备库与配置架构/01_线上GPT建议方案审核报告.md | Add | 审核线上 GPT 通用设备库建议与新增奶茶机资料，区分 Coffee2 已实现母体、MilkTea 骨架和目标业务，给出奥卡姆剃刀约束、资源风险、配置阶段及实施门槛。 |
| 2026-08-20 | 资料文档/03_技术实现核心文档/通用设备库与配置架构/02_当前工程完整架构与通用设备库实现蓝图.md | Add | 为无法读取仓库的线上 GPT 汇总双产品、双工具链、公共协议栈、任务/Route 所有权、设备调用链、MilkTea 新业务目标及静态设备库完整实现蓝图。 |
| 2026-08-20 | GCC-ARM/build/Coffee2-Debug, Coffee2-Release, MilkTea-Debug, MilkTea-Release | Verify | 重新配置并构建四个 GCC preset；全部成功，Coffee2 Debug/Release FLASH 为 163916 B/196700 B，MilkTea Debug/Release FLASH 为 169068 B/147396 B。 |
| 2026-08-20 | 资料文档/03_技术实现核心文档/通用设备库与配置架构/03_物理Bus多协议适配与设备配置实施方案.md | Add | 明确物理 Route、协议模型和设备实例的边界，设计启动时按 Route 互斥选择 Modbus RTU 或确定私有协议的静态配置、owner task、校验、执行链、资源预算、迁移阶段和单机验收方案；私有协议 Route 不创建 ModbusPort。 |
| 2026-08-20 | 资料文档/03_技术实现核心文档/通用设备库与配置架构/05_线上建议复核与最终收敛方案.md | Add | 对比线上 `04` 建议与 `03` 方案及当前 ELF/map，吸收 Driver/Protocol/Route 分离和产品 const 配置，收紧产品级裁剪、替换式迁移、Serial 先行和 TCP 后置；记录当前 Bus/context/task RAM 基线，并设定公共层 RAM 与 Coffee2 Flash 增量门槛。 |
| 2026-08-20 | 资料文档/03_技术实现核心文档/通用设备库与配置架构/06_机器人同协议多语义映射兼容方案.md | Add | 针对相同越疆 Modbus TCP/3100 地址在不同产品和 Robot1/2/3 中表示不同工位语义的问题，新增 Driver Program Profile 设计；通过产品 TargetId 到 command/result coil 的只读映射复用同一 Robot task、握手和断线恢复，并评估近零 RAM 成本。 |
| 2026-08-20 | 资料文档/03_技术实现核心文档/通用设备库与配置架构/07_奥卡姆剃刀审查与机器人映射最小方案.md | Add | 按 RAM、逻辑复杂度、扩展性顺序复审 Program Profile；拒绝独立 Profile ID/registry/manager，收敛为 Robot Instance 通过既有 pvDriverConfig 指向按 TargetId 直接索引的 const point table，实现 0 B 常驻 RAM 和约 76 B/19 点 Robot 的 Flash 数据成本，并建立后续架构新增强制审查卡。 |
| 2026-08-20 | 资料文档/03_技术实现核心文档/通用设备库与配置架构/08_通用设备库与多协议多实例最终实施方案.md | Add | 汇总最终权威设备库、多协议、多实例、多机器人、IO、Route/Transport/Driver/Protocol 边界，明确同品类不同协议、同协议不同语义、资源预算、Coffee2 到 MilkTea 的替换式迁移和新增架构审查卡。 |
| 2026-08-20 | 资料文档/03_技术实现核心文档/通用设备库与配置架构/09_目标架构与完整调用链说明.md | Add | 提供面向工程师的目标架构阅读指南、一页总图、启动链、配置链、Modbus/私有 UART/IO/Robot2 四条完整调用链、多机器人关系、调试断点、源码映射和当前未实现清单。 |
| 2026-08-20 | Application/DeviceLibrary/Inc/device_library.h; Robot/Dobot/dobot_robot_device.c/.h | Add | 建立无任务、无注册表、无动态分配的公共设备身份和越疆机器人静态驱动配置；支持旧版 3100～3139 与新版 3100～3150，新版限制 Robot1/Robot2，节卡 Robot3 仅保留未启用身份。 |
| 2026-08-20 | Application/DeviceLibrary/CoffeeMachine/coffee_machine_modbus.c/.h; coffee_machine_f200.c/.h | Add | 增加咖乐美 O/X Modbus 公共行为和咖博士 F200 26 字节私有 UART 组帧、校验、解析、请求响应能力；F200 直接使用 Transport，不创建 ModbusPort。 |
| 2026-08-20 | Application/UserAPP/Coffee2App/Robot_Tcp/coffee2_robot_tcp.c/.h; Config/coffee2_app_config.h | Modify | Coffee2 默认保持旧版越疆协议，允许编译期切换新版 51 点镜像；将动作地址集中为 const 语义点位表并通过公共范围/角色校验，保留 3121～3139 动作清理和原握手行为。 |
| 2026-08-20 | Application/DeviceProtocol/Coffee2Protocol/coffee2_rtu_protocol.c/.h; Application/UserAPP/Coffee2App/Device/coffee2_device.c/.h | Modify | 让 Coffee2 咖啡机兼容适配器调用公共 X 系列 Driver，并为当前全部设备 Binding 补 Category、Role、DriverId、ProtocolId；其他设备执行路径不迁移。 |
| 2026-08-20 | Application/CMakeLists.txt; MDK-ARM/STM32F407_Base.uvprojx | Modify | 仅在 Coffee2 Target 编译并包含公共设备库；MilkTea Target 不链接、不增加源组或 include path。Keil 工程 XML 校验通过，当前环境未发现 UV4.exe，未执行 ARMCC Rebuild。 |
| 2026-08-20 | 资料文档/03_技术实现核心文档/通用设备库与配置架构/08_通用设备库与多协议多实例最终实施方案.md; 09_目标架构与完整调用链说明.md; 10_公共设备库首阶段实现说明.md | Modify/Add | 同步首阶段实际落地、机器人版本/角色边界、咖啡机三协议、节卡保留项、资源实测、完整调用链和同基线电脑复制清单。 |
| 2026-08-20 | GCC-ARM/build/Coffee2-Debug, Coffee2-Release, MilkTea-Debug, MilkTea-Release | Verify | 四个 GCC preset 构建成功；默认旧版 Coffee2 Debug 为 RAM 101680 B、CCMRAM 40872 B、FLASH 164296 B，Release 为 RAM 101728 B、CCMRAM 40872 B、FLASH 197352 B；MilkTea 不含 DeviceLibrary 符号且产物保持 RAM 104848/104896 B、CCMRAM 32768 B、FLASH 169068/147396 B。 |
| 2026-08-20 | CubeMX_Base/LWIP/Target/lwipopts.h | Modify | 仅对 Coffee2 启用精简 LwIP/MEM/MEMP 统计并关闭协议统计显示；MilkTea 保持统计关闭，不增加任务、队列或动态内存。 |
| 2026-08-20 | Application/UserAPP/Coffee2App/Comm_Log/coffee2_log.c, coffee2_log.h | Modify | 增加 LwIP 资源失败诊断接口，按错误计数边沿输出 heap、标准 memp 及以太网零拷贝 RX_POOL 的使用量、容量和累计失败次数，复用现有日志环形队列。 |
| 2026-08-20 | Application/UserAPP/Coffee2App/Modbus_Tcp_Server/coffee2_server.c; Robot_Tcp/coffee2_robot_tcp.c; Ota/coffee2_ota_http.c | Modify | 在 Server listener/select/accept、Robot TCP open、OTA tcpip/tcp_new/tcp_bind/tcp_listen 失败路径接入资源诊断，不改变原有业务错误处理和重连策略。 |
| 2026-08-20 | GCC-ARM/CMakeLists.txt | Modify | 将当前产品宏传递给独立 LwIP object target，保证 Coffee2 的 LwIP 统计实现与应用侧配置一致，同时保持 MilkTea 统计关闭。 |
| 2026-08-20 | GCC-ARM/build/Coffee2-Debug, Coffee2-Release, MilkTea-Debug, MilkTea-Release | Verify | 四个 GCC preset 构建成功；LwIP 资源诊断后 Coffee2 Debug 为 text 165856 B、data 168 B、bss 142680 B，Release 为 text 198812 B、data 168 B、bss 142696 B；相对基线静态 RAM 分别增加 296 B/288 B，CCMRAM、FreeRTOS heap 和任务栈不变。MilkTea 统计关闭，产物保持不变。当前环境未发现 UV4.exe，未执行 ARMCC Rebuild。 |
| 2026-08-20 | Application/Common/Log/app_log.c/.h; Application/UserAPP/Coffee2App/Comm_Log/coffee2_log.c/.h | Add/Modify | 抽取产品无关的固定格式异步日志核心，保留32条覆盖最旧环形缓冲、静态信号、状态快照、EarlyWrite、Transport重开退避和buffer-only降级；Coffee2仅保留来源表、USART1 Transport适配及LwIP资源诊断，全部既有Coffee2日志调用点和输出格式兼容。 |
| 2026-08-20 | Application/CMakeLists.txt; MDK-ARM/STM32F407_Base.uvprojx | Modify | 仅将公共app_log.c加入Coffee2 GCC/Keil编译组；MilkTea目标的Coffee2组继续IncludeInBuild=0，未接入MilkTea日志源码。 |
| 2026-08-21 | GCC-ARM/build/Coffee2-Debug, Coffee2-Release | Verify | 公共日志迁移后两套 Coffee2 GCC preset 构建成功；Debug 为 RAM 101992 B、CCMRAM 40872 B、FLASH 166320 B，Release 为 RAM 102024 B、CCMRAM 40872 B、FLASH 199116 B；按上一版 LwIP 诊断 ELF 的 data+bss 口径，公共化分别新增 16 B/8 B 静态 RAM，未新增任务、队列或动态分配。 |
| 2026-08-21 | Application/UserAPP/Coffee2App/Comm_Log/coffee2_log.c | Fix | 保留 USART1 Transport 创建失败和 HAL 实例未就绪的具体错误码，避免公共核心的 buffer-only 降级将其统一记为 NOT_READY。 |
| 2026-08-21 | MDK-ARM/STM32F407_Base.uvprojx | Verify | XML 解析通过；MilkTea 目标 Coffee2/App 组 IncludeInBuild=0 且 app_log.c 仅作为排除项存在，Coffee2 目标同组包含 app_log.c；当前环境未发现 UV4.exe，未执行 ARMCC Rebuild。 |
| 2026-08-21 | 资料文档/03_技术实现核心文档/通用设备库与配置架构/11_当前设备库与协议迁移盘点.md | Add | 按实际源码区分公共设备库已实现驱动、仅 DriverId 身份占位及仍位于 Coffee2/MilkTea 产品目录的协议，明确所有设备线协议公共化的边界、最小目录、迁移顺序、资源约束和完成判定。 |
| 2026-08-21 | 资料文档/03_技术实现核心文档/通用设备库与配置架构/11_当前设备库与协议迁移盘点.md | Modify | 补充晟枢落杯落盖合体式/分体式拓扑：协议与寄存器不变，仅由 Target Binding 选择 Cup/Lid 同为 Unit1 或分别为 Unit1/Unit3；明确复用单一 CupLidController Driver，不复制协议或新增运行时管理层。 |
| 2026-08-21 | Application/DeviceLibrary/Inc/device_library.h; Robot/Dobot/dobot_robot_device.c/.h | Modify | 将越疆变体统一命名为协议1/2/3，保留3100～3159公共容量及3160保留边界；Coffee2选择协议1，协议2/3使用协议1地址占位并标记未验证，节卡仍仅保留身份。 |
| 2026-08-21 | Application/DeviceLibrary/CupLidController/ShengShu; SyrupMachine/CurrentModbus; IceMachine/CurrentModbus; Scale/BSQ_DG_V2; PowerMeter/DDSU666; IoModule/ModbusDigitalIo | Add | 将晟枢杯盖、当前糖浆机、制冰机、BSQ-DG-V2、DDSU666及数字量IO线协议迁入公共设备库；使用调用方镜像和取消回调，不新增任务、队列、动态内存或产品依赖。 |
| 2026-08-21 | Application/DeviceLibrary/CoffeeMachine/coffee_machine_f200.c/.h | Enhance | 为F200公共私有UART驱动补齐查询、制作、取消、状态轮询、超时和协作取消闭环；保持26字节帧及Transport直连。 |
| 2026-08-21 | Application/DeviceProtocol/Coffee2Protocol/coffee2_rtu_protocol.c/.h; Application/UserAPP/Coffee2App/Device/coffee2_device.c/.h; IO_State/coffee2_io.c/.h; WorkFlow/coffee2_workflow.c; Modbus_Tcp_Server/coffee2_server.h | Modify | 将Coffee2Protocol收敛为公共Driver薄适配，绑定F200、合体杯盖、糖浆、冰机、称重、电表和16点IO；按电气图增加本地/外部IO语义并保留FC05后FC01读回闭环。 |
| 2026-08-21 | Application/UserAPP/Coffee2App/Config/coffee2_app_config.h; Modbus_Rtu_Bus/coffee2_rtu_bus.c/.h | Modify | 让每个物理Bus编译期独占一种协议；Bus2以115200运行F200且不创建ModbusPort，Bus3～5分别按9600/19200/38400运行RTU；按协议配置紧凑分配三份ModbusPort并移除运行时波特率切换。 |
| 2026-08-21 | Application/CMakeLists.txt; MDK-ARM/STM32F407_Base.uvprojx | Modify | Coffee2只编译当前选中的F200及七类公共设备源，完整排除Kalerm O/X执行器；Keil将未选源放入IncludeInBuild=0组并补全当前公共Driver路径，MilkTea保持未接入。 |
| 2026-08-21 | 资料文档/03_技术实现核心文档/通用设备库与配置架构/11_当前设备库与协议迁移盘点.md; 12_公共设备库完整迁移与Coffee2选型实施报告.md | Rewrite/Add | 更新全部公共协议、Coffee2装配、Bus协议独占、杯盖拓扑、电气图IO、编译裁剪、资源实测、硬件待验项及新增Driver流程。 |
| 2026-08-21 | GCC-ARM/build/Coffee2-Debug, Coffee2-Release, MilkTea-Debug, MilkTea-Release | Verify | 四个GCC preset fresh configure/build成功；最终Coffee2 Debug为RAM101304B/CCM40744B/FLASH167888B，Release为RAM101336B/CCM40744B/FLASH201832B；MilkTea保持RAM104848/104896B、CCM32768B、FLASH169068/147396B。Coffee2 ELF无未选Kalerm符号；Keil XML通过，环境未发现UV4.exe。 |
| 2026-08-21 | 资料文档/03_技术实现核心文档/订单上下文与双流水线架构/01_方案审核与关键决策.md | Add | 审核上位机订单号、双槽流水线和订单关联日志方案；确认16位协议字段、0000/F123边界和Snapshot，否决删除Epoch/CommandId及按订单号数值排序，给出协议冲突、槽释放和RAM边界。 |
| 2026-08-21 | 资料文档/03_技术实现核心文档/订单上下文与双流水线架构/02_Coffee单订单订单号贯穿流程设计.md | Add | 设计Coffee2现有Host OrderId到Workflow、DeviceCommand、Route和日志的单订单贯穿链，并说明Coffee1 v2.7.23_ccram仅作为协议与业务时序参考、调试F123及订单替换防串号规则。 |
| 2026-08-21 | 资料文档/03_技术实现核心文档/订单上下文与双流水线架构/03_MilkTea双槽双机器人流水线流程设计.md | Add | 设计MilkTea两个静态活动订单槽、Robot1 Front与Robot2 Back固定Worker、WAIT_BACK背压、AdmissionSequence FIFO、按OrderId取消、掉线占槽及分阶段验收。 |
| 2026-08-21 | 资料文档/03_技术实现核心文档/订单上下文与双流水线架构/04_通用订单上下文与日志架构总结.md | Add | 汇总通用订单身份、DeviceCommand、公共Logger订单字段和新格式、Target来源表、Coffee2/MilkTea并发边界、设备库关系、资源约束与渐进实施顺序。 |
| 2026-08-21 | Application/Common/Log/app_log.c/.h; Application/UserAPP/Coffee2App/Comm_Log/coffee2_log.c/.h | Modify | 为公共静态日志环增加16位订单标识和order-aware API；旧API默认系统订单0000，格式统一为`[%04XLEVEL][TASK:MODULE]`，订单字段复用对齐空洞，不新增日志任务、队列或动态内存。 |
| 2026-08-21 | Application/UserAPP/Coffee2App/Modbus_Tcp_Server/coffee2_server.c; WorkFlow/coffee2_workflow.c; Device/coffee2_device.c | Modify | Coffee2真实订单使用上位机0x0000订单寄存器，手动/调试命令固定使用F123；拒绝0000/F123真实订单并为订单接收、Workflow、手动命令终态输出对应订单日志。 |
| 2026-08-21 | Application/UserAPP/Coffee2App/Modbus_Rtu_Bus/coffee2_rtu_bus.c; Application/DeviceProtocol/Coffee2Protocol/coffee2_rtu_protocol.c; Robot_Tcp/coffee2_robot_tcp.c | Modify | 为有可靠命令上下文的RTU失败/重试、IO写回读、Robot动作闭环日志绑定OrderId；系统启动、网络和后台刷新仍使用0000。 |
| 2026-08-21 | GCC-ARM/build/Coffee2-Debug, Coffee2-Release | Verify | Coffee2 Debug/Release增量构建成功；Debug RAM 101304 B、CCMRAM 40744 B、FLASH 168432 B，Release RAM 101336 B、CCMRAM 40744 B、FLASH 202408 B；日志环`s_axLogRing`为0xE80（3712 B，32条×116 B），订单字段未导致环形缓冲增长。Keil XML未改，当前环境未发现UV4.exe，未执行ARMCC Rebuild。 |
| 2026-08-21 | Application/UserAPP/Coffee2App/Task_Manager/coffee2_manager.c | Modify | 将绕过公共Logger的早期BOOT直写日志同步为`[0000LEVEL][BOOT:Module]`格式，确保Coffee2从上电到任务运行只输出一种订单关联前缀。 |
| 2026-08-21 | GCC-ARM/build/Coffee2-Debug, Coffee2-Release | Verify | 补齐BOOT前缀后对Coffee2两套preset执行fresh configure/build成功；Debug RAM 101304 B、CCMRAM 40744 B、FLASH 168468 B，Release RAM 101336 B、CCMRAM 40744 B、FLASH 202444 B；仅保留CubeMX生成ethernetif.c既有未使用参数警告。 |
| 2026-08-24 | 资料文档/03_技术实现核心文档/店中店咖啡机工程核心实现资料/01_架构设计/Coffee2_新业务与设备适配修改方案.md | Add | 结合越疆机器人表格/截图、Coffee1 v2.7.23_ccram 原型、电气图第13～15页和当前Coffee2源码，形成设备协议外包、Workflow业务把控、越疆三组10+10地址、Bus3电表迁移、IO语义层、维护状态机、启动设备清单日志及低RAM分阶段实施方案；节卡明确不在当前范围。 |
| 2026-08-24 | 资料文档/03_技术实现核心文档/店中店咖啡机工程核心实现资料/01_架构设计/Coffee2_完整业务流程设计.md | Add | 将错误订单校验、冷热杯、制冰、独立果乳出口、咖啡/奶个性化组合、落盖出餐、热水、咖啡机清洗、果乳清洗、越疆动作握手、继续/取消和订单关联日志整理为完整Coffee2业务状态机，并标出待确认硬件与产品边界。 |
| 2026-08-24 | 资料文档/03_技术实现核心文档/店中店咖啡机工程核心实现资料/01_架构设计/Coffee2_新业务与设备适配修改方案.md | Modify | 按正式四路糖浆协议与Coffee1 v2.7.23_ccram经验补齐Bus3 Unit2寄存器、逐通道闭环、恢复/清洗边界和低RAM评估；将设备协议清单改为由实际Robot/Bus Owner使用现有设备级日志源打印。 |
| 2026-08-24 | 资料文档/03_技术实现核心文档/店中店咖啡机工程核心实现资料/01_架构设计/Coffee2_完整业务流程设计.md | Modify | 补充糖浆1～4订单字段、串行Step55、继续判断、清洗/剩余时间维护、订单关联日志及可选打印Step65，并修正空杯判定和目标制作顺序。 |
| 2026-08-24 | 资料文档/03_技术实现核心文档/店中店咖啡机工程核心实现资料/01_架构设计/Coffee2_糖浆流程与启动日志审核报告.md | Add | 汇总正式糖浆协议、Coffee1原型、Coffee2当前能力/缺口、四路目标闭环、启动日志Owner修正、遗漏流程审计和资源边界；本轮未修改源码。 |
| 2026-08-24 | 资料文档/03_技术实现核心文档/店中店咖啡机工程核心实现资料/01_架构设计/Coffee2_新业务与设备适配修改方案.md | Modify | 增加上电残杯检查的架构边界、0x1008/0x1020状态分离、Workflow 800～899初始化步骤、Robot原子动作缺口、杯盖四位置传感器、资源预算和实施前P0确认项；本轮未修改源码。 |
| 2026-08-24 | 资料文档/03_技术实现核心文档/店中店咖啡机工程核心实现资料/01_架构设计/Coffee2_完整业务流程设计.md | Modify | 结合Coffee2 3D空间、电气图X01/X02、晟枢0x1008/0x100D/0x1012/0x1017及Coffee1启动回收经验，新增接单前残杯检查、报警/人工复查、日志和验收流程；明确不继承门、升降和出餐电机。 |
| 2026-08-24 | 资料文档/03_技术实现核心文档/店中店咖啡机工程核心实现资料/01_架构设计/Coffee2_完整程序编排就绪度与嵌入式实施交接书.md | Add | 评估当前完整程序编排就绪度，列出P0事实缺口、最小代码工作包、整机状态映射、初始化状态机、RAM边界、测试矩阵、Definition of Done和嵌入式/机器人/电气签字项；本轮未修改源码。 |
| 2026-08-24 | 资料文档/03_技术实现核心文档/店中店咖啡机工程核心实现资料/01_架构设计/Coffee2_设备库机器人电气与上位机协议重新审查报告.md | Add | 交叉复核上位机协议、Coffee1 v2.7.23_ccram、越疆Excel、公共设备库、Coffee2源码、电气图和3D图；确认Coffee2使用协议1开放式两出餐口Profile、0x0005为落冰克重、地址命名空间必须分离、电表目标Bus3/Unit3及Owner日志责任边界；本轮未修改源码。 |
| 2026-08-24 | Coffee2_新业务与设备适配修改方案.md; Coffee2_完整业务流程设计.md; Coffee2_完整程序编排就绪度与嵌入式实施交接书.md; 通用设备库与配置架构/11_当前设备库与协议迁移盘点.md; 12_公共设备库完整迁移与Coffee2选型实施报告.md | Correct | 纠正将Host TCP地址与Bus3/Unit1/FC01线圈混用、将冰量误称温度、将Coffee2误切到奶茶机60点协议、将协议1的3134/3114误判为缺失取压盖位、以及忽略上位机出餐口1/2选择的文档错误；同步电能表Bus3目标配置。 |
| 2026-08-25 | Application/UserAPP/Coffee2App/Device/coffee2_device.c; Comm_Log/coffee2_log.c; Modbus_Rtu_Bus/coffee2_rtu_bus.c | Modify | 按产品接线将DDSU666电能表绑定到Bus3/9600/Unit3并修正日志Owner；由Robot和各Bus owner在启动时逐设备打印实际选用的协议、物理链路、波特率和站号。 |
| 2026-08-25 | Application/DeviceLibrary/Robot/Dobot/dobot_robot_device.h; Application/UserAPP/Coffee2App/Robot_Tcp/coffee2_robot_tcp.c | Modify | 补齐越疆本体控制/状态常量，Coffee2继续使用协议1；出餐口1/2分别映射3131/3111与3132/3112，参数仅允许1或2，不扩展3119/3139或猜测果乳点位。 |
| 2026-08-25 | Application/DeviceLibrary/CoffeeMachine/coffee_machine_f200.c; CupLidController/ShengShu/cup_lid_shengshu.c/.h; SyrupMachine/CurrentModbus/syrup_machine_modbus.c/.h | Refactor | 将设备Driver收敛为通信闭环：从站确认命令后立即释放Bus；杯盖到位、糖浆完成和F200应用完成由Workflow周期刷新并判定，不再由Bus owner长时间占用串口等待业务动作。 |
| 2026-08-25 | Application/UserAPP/Coffee2App/WorkFlow/coffee2_workflow.c/.h; Config/coffee2_app_config.h | Enhance | 实现上电全部产品输出复位与残杯检查、冷热杯订单、冰量克重闭环、F200制作、四路糖浆、打印、落盖压盖和双出餐口确认；增加Workflow管理的热水并行状态机、果乳泵阀联动、果乳/糖浆维护及逐步日志，业务传感器等待不设置动作超时，仅通信事务保留超时重试。 |
| 2026-08-25 | Application/UserAPP/Coffee2App/Modbus_Tcp_Server/coffee2_server.c/.h | Enhance | 接入热水0x0080/0x0081、糖浆清洗0x0082、咖啡管路清洗0x0083、果乳手动0x00A1/0x00A2及果乳清洗0x00A3/0x00A4，投影整机/热水/清洗/果乳状态，并保留订单号和调试F123日志上下文。 |
| 2026-08-25 | Coffee2 product boundaries | Note | 当前F200正式协议未定义咖啡/奶管清洗指令，越疆协议1未定义独立果乳出口点位；相关请求确定性拒绝并记录COFFEE_CLEAN_UNSUPPORTED或FRUIT_ROBOT_POINT_UNDEFINED，未伪造设备报文或机器人地址。 |
| 2026-08-25 | GCC-ARM/build/Coffee2-Debug, Coffee2-Release, MilkTea-Debug, MilkTea-Release | Verify | 四个preset执行fresh configure/build成功；最终Coffee2 Debug为RAM101304B/CCMRAM40824B/FLASH174436B，Release为RAM101336B/CCMRAM40824B/FLASH208532B，OTA App区仅余4460B；MilkTea保持RAM104848/104896B、CCMRAM32768B、FLASH169068/147396B。Coffee2向量为0x0800C000，MilkTea为0x08000000；仅有CubeMX ethernetif.c既有未使用参数警告。 |
| 2026-08-25 | MDK-ARM/STM32F407_Base.uvprojx | Verify | Keil工程XML解析通过且本轮未新增工程文件；当前环境未发现UV4.exe，未执行ARMCC V5.06 Rebuild，需在安装Keil的电脑上完成最终双工具链验收。 |
| 2026-08-25 | Application/UserAPP/Coffee2App/Ota/coffee2_ota_flash.c/.h; GCC-ARM/CMakeLists.txt; GCC-ARM/linker/STM32F407XX_COFFEE2_OTA_FLASH.ld; MDK-ARM/ScatterFiles/Coffee2_CCM.sct; MDK-ARM/STM32F407_Base.uvprojx | Modify | 将Coffee2切换为STM32F407VGT6 1MiB Flash非对称OTA分区：Boot 48KiB、Metadata 16KiB、App 320KiB、Staging 384KiB、Reserved 256KiB；Application基址和VTOR统一为0x08010000。 |
| 2026-08-25 | C:/Users/13193/Desktop/coffee/coffee_bootloader_v2.0/bootloader/bootloader.h; MDK-ARM/bootloader.uvprojx; MDK-ARM/bootloader/bootloader.sct | Modify | 对齐现成Bootloader的VG芯片容量、Metadata/Application/Staging地址和320KiB/384KiB尺寸校验；保留原CRC、Flash搬运和跳转流程。 |
| 2026-08-25 | Application/DeviceLibrary/Robot/Dobot/dobot_robot_device.c/.h; Application/UserAPP/Coffee2App/Device/coffee2_device.h; Robot_Tcp/coffee2_robot_tcp.c; Modbus_Tcp_Server/coffee2_server.c | Modify | 按最新robot寄存器表重写Dobot P1 3100～3139映射，命令预清改为3111～3119与3130～3139两段，增加3129/3139果乳糖浆共享工位及越疆自动/手动本体命令。 |
| 2026-08-25 | Application/UserAPP/Coffee2App/WorkFlow/coffee2_workflow.c | Modify | 将果乳与四路糖浆编排到机器人3129/3139共享出口，果乳保持Workflow IO泵阀、糖浆保持Bus3 Modbus RTU；修正热水任务为下液位到达后关供水阀再开加热。 |
| 2026-08-25 | Application/ProtocolStack/ModbusPort; Application/New_Party/nanoMODBUS | Audit | 确认RTU/TCP响应按站号/事务号、功能码及响应负载校验；FC06只接受FC06且地址/值回显一致，FC03不会误确认写命令，无需修改协议栈。 |
| 2026-08-25 | GCC/Keil Coffee2 and external Bootloader | Verify | GCC Coffee2 Debug/Release为FLASH174788/208904B，App剩余152892/118776B；MilkTea两preset回归通过。Keil Coffee2为0 error/120条历史重复输入警告并生成201900B BIN，Bootloader为0 error/0 warning并生成9892B BIN；向量与分区校验通过。 |
| 2026-08-25 | 资料文档/03_技术实现核心文档/店中店咖啡机工程核心实现资料/04_执行报告与审计/Coffee2_VGT6分区机器人协议与设备库修正报告_2026-08-25.md | Add | 记录分区ABI、机器人完整映射、共享果乳/糖浆工位、热水流程、Modbus期待响应、上电残杯检查、设备库选型、构建产物和实机必验项。 |
| 2026-08-25 | Application/UserAPP/Coffee2App/Ota/coffee2_ota_flash.c/.h; GCC-ARM/CMakeLists.txt; GCC-ARM/linker/STM32F407XX_COFFEE2_OTA_FLASH.ld; MDK-ARM/ScatterFiles/Coffee2_CCM.sct; MDK-ARM/STM32F407_Base.uvprojx | Modify | 按最终扇区规划恢复Coffee2应用基址为0x0800C000，Metadata改用扇区1，App覆盖扇区3～6（336KiB），Staging覆盖扇区7～9（384KiB），GCC/Keil链接长度与VTOR同步为0x54000/0xC000。 |
| 2026-08-25 | C:/Users/13193/Desktop/coffee/coffee_bootloader_v2.0/bootloader/bootloader.c/.h; MDK-ARM/bootloader.uvprojx; MDK-ARM/bootloader/bootloader.sct | Modify | 经用户明确授权，保持Bootloader CRC、搬运和跳转业务不变，恢复Metadata 0x08004000和App跳转0x0800C000，Staging改为0x08060000，App擦除改为扇区3～6，Bootloader链接上限恢复16KiB。 |
| 2026-08-25 | GCC/Keil Coffee2; Keil Bootloader | Verify | Coffee2 GCC Debug/Release通过，为174788/208904B，App剩余169276/135160B；Keil Coffee2为0 error/120条历史重复输入警告并生成201896B BIN；Bootloader ARMCC5为0 error/0 warning并生成9888B BIN，向量地址校验通过。 |
| 2026-08-25 | Application/Common/Ota/app_ota_flash.c/.h; Application/Common/Ota/app_ota_http.c/.h | Add | 将流式Flash写入、向量/CRC/Metadata提交和raw-lwIP HTTP multipart上传迁移为公共OTA实现；通过一份静态AppOtaConfig_t接收分区、CRC、日志、端口、复位延时与可选lwIP诊断绑定，不新增任务、队列或动态内存。 |
| 2026-08-25 | Application/UserAPP/Coffee2App/Ota/coffee2_ota_flash.c/.h; coffee2_ota_http.c/.h | Refactor | Coffee2 OTA收敛为336KiB App/384KiB Staging静态配置及兼容入口，保留xCoffee2OtaHttpInitialize、ucCoffee2OtaHttpIsActive、原事件文本和0x0200触发链；lwIP资源统计继续通过Coffee2诊断钩子输出。 |
| 2026-08-25 | Application/CMakeLists.txt; MDK-ARM/STM32F407_Base.uvprojx | Modify | GCC Coffee2服务目标及Keil Coffee2 App组各添加一次Common/Ota两个实现文件；MilkTea的Coffee2组仍为IncludeInBuild=0，未引入OTA源码。 |
| 2026-08-25 | 资料文档/03_技术实现核心文档/店中店咖啡机工程核心实现资料/04_执行报告与审计/Coffee2_OTA公共层迁移工作报告_2026-08-25.md | Add | 记录公共OTA与Coffee2静态配置边界、完整升级链、336KiB App/384KiB Staging ABI、GCC/Keil双工具链结果、RAM/Flash变化、实机验收项及其他电脑同步清单。 |
| 2026-08-25 | Application/Common/CommonTargets.h; Application/Common/AppsConfigfile.h | Rename | Rename the common target entry header to CommonTargets.h; aggregate compiler compatibility, logging, OTA, and selected target manager headers without runtime objects. |
| 2026-08-25 | GCC-ARM/Platform/gcc_ccm.h; GCC-ARM/Platform/app_ccm.h; GCC-ARM/Platform/app_ccm.c; CubeMX_Base/Core/Src/main.c | Rename/Modify | Rename the GNU CCM initialization header to gcc_ccm.h and update GNU-only includes; retain the implementation file and CMake source ownership. |
| 2026-08-25 | CubeMX_Base/Core/Src/freertos.c | Modify | Replace the legacy common configuration include with CommonTargets.h inside the preserved USER CODE includes section. |
| 2026-08-26 | Application/UserAPP/Coffee2App/Config/coffee2_app_config.h; Task_Manager/coffee2_manager.c | Modify | 发布Coffee2OpenV3.0.0版本标识，将咖啡逻辑配方上限收敛到0x0021，并在启动及版本查询链输出明确固件版本。 |
| 2026-08-26 | Application/DeviceLibrary/CoffeeMachine/coffee_machine_f200.c/.h; Application/DeviceProtocol/Coffee2Protocol/coffee2_rtu_protocol.c | Enhance | 完成F200逻辑配方+1映射、六类清洗命令、响应命令字匹配及应用状态4失败判定，避免其他合法帧误确认当前事务。 |
| 2026-08-26 | Application/UserAPP/Coffee2App/Modbus_Tcp_Server/coffee2_server.c/.h; WorkFlow/coffee2_workflow.c | Modify | 按Coffee1 MBP与新增上位机说明收敛Coffee2开放式流程：仅0x000A选择双出餐口，0x0009/0x000C保留，移除打印/储杯主线，F200完成后机器人取杯，出餐后回Home；状态区改为显式投影并补齐电能、物料和液位语义。 |
| 2026-08-26 | 资料文档/01_项目资料/店中店咖啡机对接其他项目/STM32作为从站/咖啡售卖机下位机-通信协议_Coffee2OpenV3.0.0.docx | Add | 在保留原始协议DOCX不变的前提下生成Coffee2OpenV3.0.0副本，更新配方、双出餐、Robot调试、F200清洗、状态寄存器及F200警告/故障附录；Word渲染36页并逐页视觉复核。 |
| 2026-08-26 | 资料文档/03_技术实现核心文档/店中店咖啡机工程核心实现资料/01_架构设计/Coffee2OpenV3.0.0_上位机协议与完整业务流程.md; Coffee2_完整业务流程设计.md; Coffee2_完整程序编排就绪度与嵌入式实施交接书.md | Add/Modify | 建立V3.0.0软件事实基线，完整记录上位机订单闭环、订单主线、维护任务、设备事务判定、初始化、状态和日志；旧文档增加纠正说明。 |
| 2026-08-26 | GCC Coffee2 Debug/Release; MDK-ARM Coffee2 | Verify | GCC Debug/Release构建成功，FLASH分别175724/210100B（336KiB App区）；Keil ARMCC5为0 error/120条既有重复输入警告并生成202720B BIN，向量与0x0800C000应用基址一致。 |
| 2026-08-26 | 资料文档/03_技术实现核心文档/店中店咖啡机工程核心实现资料/04_执行报告与审计/Coffee2OpenV3.0.0_实现完成报告_2026-08-26.md | Add | 汇总事实来源、实现范围、协议DOCX哈希与版式审计、双工具链证据、同步清单及必须实机验证的机械/传感器边界。 |
| 2026-08-26 | MDK-ARM/STM32F407_Base.uvprojx; GCC-ARM/CMakeLists.txt; MDK-ARM/Coffee2_warning_fix_aligned_groups.log; GCC-ARM/Coffee2-*-Rebuild-20260826.log | Fix/Verify | 对齐MilkTea与Coffee2的Keil公共组序号，Coffee2链接输入由279/159 unique收敛为159/159 unique，消除120条L6304W并以ARMCC5 Rebuild验证0 error/0 warning；GCC仅对STM32Cube HAL/BSP及生成的ethernetif.c关闭无意义的unused-parameter诊断，应用源码继续保留-Wall/-Wextra。Coffee2 Debug/Release全量重建均为0 warning/0 error，FLASH分别175724B/210100B，向量仍为0x0800C000。 |
| 2026-08-27 | Application/Common/TcpClientSession/tcp_client_session.c/.h; Application/Transport/Inc/transport_tcp.h; Application/Transport/Src/transport_tcp.c; Application/UserAPP/Coffee2App/Robot_Tcp/coffee2_robot_tcp.c; Application/Common/CommonTargets.h; Application/CMakeLists.txt; MDK-ARM/STM32F407_Base.uvprojx | Add/Modify | 新增无任务、无队列、无动态分配的公共TCP Client Session静态实例机制；Coffee2 Robot由Session统一管理网络等待、真实TCP ESTABLISHED确认、首次Modbus协议探测和1/2/5/10/30秒永久退避。Transport在TCP/IP core内检查非阻塞连接标志、PCB存在和ESTABLISHED状态，禁止仅以Netconn状态结束判定成功；首次Probe任何失败关闭会话并退避，ONLINE阶段的Modbus Exception仍按当前命令失败处理。新增按状态变化输出的连接、Probe、协议故障码和重试日志，并将公共源仅加入Coffee2 Keil Target一次，避免重复输入。 |
# 2026-08-27 GCC 启动修复与 Coffee2 Server 四槽扩展

- 修复 GCC Coffee2 构建中 `USER_VECT_TAB_ADDRESS`/`VECT_TAB_OFFSET` 只传递给最终 ELF、未传递给实际编译 `system_stm32f4xx.c` 的 `STM32_Drivers` 目标问题，确保 `SystemInit()` 将 `SCB->VTOR` 设置为应用基址 `0x0800C000`。
- Coffee2 Modbus TCP Server 客户端槽位由 2 个扩展为 4 个，状态数组和槽位名称统一由 `COFFEE2_SERVER_MAX_CLIENTS` 管理。
- Server 任务栈由 1024 words 提升为 1536 words，覆盖新增两个 `Coffee2ServerSlot_t` 局部对象造成的约 1552 B 栈增量，并保留运行余量。
- 当前 lwIP 配置无需调整：4 个 Server 客户端、1 个 Robot TCP 客户端及 OTA HTTP 并发仍处于现有 PCB、NETCONN 和监听 PCB配额内。
# 2026-08-27 公共 LwIP 资源事件预警

- 新增 `Application/Common/LwipAlert` 公共模块，供多个 Target 在网络 API 失败事件发生时读取 LwIP MEM/MEMP 资源统计并输出诊断预警。
- 预警采用纯事件触发：不新增轮询、任务、定时器、队列或动态内存；正常运行路径没有周期 CPU 开销。
- Coffee2 保留原 `vCoffee2LogLwipResourceFailure()` 接口作为薄适配层，现有 Server、Robot、OTA 调用方无需修改。
- 原 Coffee2 私有的资源枚举、统计读取和错误计数状态迁移至公共模块；错误计数仅在累计值增长时输出，避免计数回落造成误报。
- GCC 与 Keil Coffee2 工程均加入公共源文件；`CommonTargets.h` 统一导出公共预警头文件。
# 2026-08-27 TCP 连接成功端点日志

- 新增公共 `ucTransportTcpFormatIpv4Endpoint()`，使用有界整数转换输出 `IPv4:port`，不引入 `stdio`、`snprintf`、动态内存或静态共享缓冲。
- Coffee2 Server连接成功日志增加实际上位机对端IP和源端口，同时保留`slot`字段；本地监听端口继续由`SERVER_LISTENING`日志表示。
- Coffee2 Robot连接成功日志增加配置的机器人远端IP和端口，同时保留`attempt`字段。
- 端点事件使用64B局部缓冲，日志接口在返回前复制文本，局部缓冲生命周期安全。
- GCC Coffee2 Debug/Release构建均通过，RAM/CCMRAM占用保持不变，FLASH分别为177656B/213808B；Keil Coffee2 Target继续引用同一组共享源码，无需修改工程文件。

# 2026-08-28 Coffee2Open 应用层架构迁移方案修订

| Date | File | Action | Description |
| --- | --- | --- | --- |
| 2026-08-28 | 资料文档/03_技术实现核心文档/店中店咖啡机工程核心实现资料/01_架构设计/Coffee2_Open应用层五层架构终版迁移方案.md | Add | 审批并修订五层架构交接规格；固化 Coffee2=Open、Coffee3=Close 命名，建立 Coffee2 源码缓存，保持命令 ABI 并收敛公共 DeviceProtocol 边界 |
| 2026-08-28 | 资料文档/03_技术实现核心文档/店中店咖啡机工程核心实现资料/01_架构设计/Coffee2_Open应用层五层架构终版迁移方案.md | Update | 固化公共命令 POD/ABI、C 风格产品上下文、MilkTea 双订单隔离及跨 Target 全量回归门槛；明确现有 MilkTea 源码不作为架构依据并后置独立重写 |
| 2026-08-28 | Application/Common/Command/app_command.h; Application/UserAPP/Coffee2App/Device/coffee2_device.h | Add/Modify | P1 将 32-byte 应用命令 ABI、命令来源和动作数值迁入公共头；Coffee2 保留原类型和宏名称的兼容别名，不改变队列消息布局、路由或业务行为。 |
| 2026-08-28 | Application/DeviceProtocol/Modbus/*.c; Application/DeviceProtocol/Modbus/*.h | Add | P2 新增 Scale、Power Meter、IO、Ice、Syrup、Cup/Lid 无状态 Modbus 命令转发入口；仅调用既有 DeviceLibrary API，不持有 Coffee2 状态、日志、队列或任务。 |
| 2026-08-28 | Application/UserAPP/Coffee2App/Device/coffee2_device_image.c; Application/UserAPP/Coffee2App/Device/coffee2_device_image.h | Add | P2 新增 Coffee2 产品侧设备图像存储与 F200/Cup/Lid 提交辅助函数，隔离 DeviceProtocol 与产品状态投影。 |
| 2026-08-28 | Application/UserAPP/Coffee2App/Modbus_Rtu_Bus/coffee2_rtu_bus.c | Modify | P2 RTU owner 接入六类新 DeviceProtocol，负责取消回调、调用方 image 和 IO/状态提交；保留 F200 旧路径及旧图像同步，待 P3 排除旧协议文件。 |
| 2026-08-28 | Application/UserAPP/Coffee2App/Modbus_Rtu_Bus/coffee2_rtu_bus.c | Fix | 为公共 DeviceCancelCheck_t 增加 Coffee2 owner 签名适配并补齐 IO 状态接口声明，消除 P2 接入产生的 ARMCC 类型与隐式声明警告。 |
| 2026-08-28 | MDK-ARM/Objects/Coffee2/Coffee2.build_log.htm | Verify | P2 新增 7 个源文件接入后执行 ARMCC V5.06 Rebuild，结果 Code=191004、RO-data=5740、RW-data=532、ZI-data=150628，0 Error(s)、0 Warning(s)。 |
| 2026-08-28 | MDK-ARM/STM32F407_Base.uvprojx | Modify/Verify | Coffee2 Target 的 Application/Coffee2/DeviceProtocol 组接入 P2 六个 Modbus 实现和 coffee2_device_image.c，并加入 ../Application/DeviceProtocol/Modbus include path；保留旧 coffee2_rtu_protocol.c 原状态。 |
| 2026-08-28 | MDK-ARM/Objects/Coffee2/Coffee2.build_log.htm | Verify | 指定 UV4 Coffee2 Rebuild 编译七个新增源文件各一次；Code=190992，RO-data=5740，RW-data=532，ZI-data=150628；0 Error(s)、10 Warning(s)，警告来自 coffee2_rtu_bus.c 回调类型不匹配及隐式声明，未修改源码。 |
| 2026-08-28 | Application/DeviceProtocol/CoffeeMachine/f200_protocol.c/.h; Application/UserAPP/Coffee2App/Modbus_Rtu_Bus/coffee2_rtu_bus.c; Application/UserAPP/Coffee2App/Device/coffee2_device_image.c; MDK-ARM/STM32F407_Base.uvprojx | Add/Modify | P2 将 F200 AppCommand 动作映射迁入无状态公共协议入口，Coffee2 Bus owner 提交产品状态并临时同步旧投影；保留旧 Coffee2Protocol 文件与 Target 编译状态。 |
| 2026-08-28 | MDK-ARM/Objects/Coffee2/Coffee2.build_log.htm | Verify | F200 公共入口接入后串行执行 ARMCC V5.06 Rebuild，结果 Code=191024、RO-data=5740、RW-data=532、ZI-data=150628，0 Error(s)、0 Warning(s)。 |
| 2026-08-28 | Application/UserAPP/Coffee2App/Modbus_Rtu_Bus/coffee2_rtu_bus.c; Application/UserAPP/Coffee2App/Device/coffee2_device_image.c/.h | Fix | 恢复 Server IO 写入事务前的 IO_WRITE_EXPECTED 日志，并为 P2 临时新旧 Coffee2 状态镜像同步增加 ARMCC 编译期尺寸约束。 |
| 2026-08-28 | MDK-ARM/Objects/Coffee2/Coffee2.build_log.htm | Verify | 独立审查整改后串行执行 ARMCC V5.06 Rebuild，结果 Code=191124、RO-data=5740、RW-data=532、ZI-data=150628，0 Error(s)、0 Warning(s)。 |
| 2026-08-28 | Application/DeviceProtocol/Modbus/dobot_protocol.c/.h; Application/UserAPP/Coffee2App/Robot_Tcp/coffee2_robot_tcp.c; MDK-ARM/STM32F407_Base.uvprojx | Add/Modify | P2 将 Dobot P1 点表、DriverConfig 和 AppCommand 本体/action 解析迁入无状态公共 Modbus 协议层；Coffee2 TCP owner 保留 START_SIGNAL 直写、连接、刷新、事务、超时、取消、日志及状态职责。 |
| 2026-08-28 | MDK-ARM/Objects/Coffee2/Coffee2.build_log.htm | Verify | ARMCC V5.06 Coffee2 Target 串行 Rebuild 编译 dobot_protocol.c 和 coffee2_robot_tcp.c，结果 Code=191188、RO-data=5772、RW-data=532、ZI-data=150628，0 Error(s)、0 Warning(s)。 |
| 2026-08-28 | Application/UserAPP/Coffee2App/Device/coffee2_device_image.c/.h; Modbus_Rtu_Bus/coffee2_rtu_bus.c; Modbus_Tcp_Server/coffee2_server.c; WorkFlow/coffee2_workflow.c; MDK-ARM/STM32F407_Base.uvprojx | Modify | P3 将 Coffee2 状态读写统一到产品 image，移除旧 Coffee2Protocol 头、符号和同步副本；Coffee2 文件项显式 IncludeInBuild=0，MilkTea 节点与磁盘旧文件保留。 |
| 2026-08-28 | MDK-ARM/Objects/Coffee2/Coffee2_P3_final.log | Verify | GUI 退出后使用 uVision.com 干净 Rebuild；活动源清单不再包含 coffee2_rtu_protocol.c，结果 Code=191032、RO-data=5772、RW-data=532、ZI-data=150436，0 Error(s)、0 Warning(s)。 |
| 2026-08-28 | Application/UserAPP/Coffee2App/Device/coffee2_device_image.c; WorkFlow/coffee2_workflow.c; Modbus_Tcp_Server/coffee2_server.c; Modbus_Rtu_Bus/coffee2_rtu_bus.c; MDK-ARM/STM32F407_Base.uvprojx | Modify | P3 排除 Coffee2 Target 的旧 Coffee2Protocol 源而保留磁盘文件；Workflow、Server、Bus 仅使用 Coffee2 产品 image，删除旧状态镜像同步，不改变事务、在线、重试或日志路径。 |
| 2026-08-28 | Application/DeviceProtocol/Coffee2Protocol/coffee2_rtu_protocol.c/.h | Delete | 删除已由 Coffee2 Open 五层架构取代的产品专属 RTU 协议实现。 |
| 2026-08-28 | Application/CMakeLists.txt; MDK-ARM/STM32F407_Base.uvprojx | Modify | 清理 Coffee2 对已删除 Coffee2Protocol 的 GCC 目标和 Keil 源文件项；MilkTea Target 保持原样，等待整体重写。 |
| 2026-08-28 | Application/DeviceProtocol/MilkTeaProtocol/** | Delete | 按 MilkTea 整 Target 后续删除重写的边界，提前移除错误旧源码中的产品专属协议层；现有 MilkTea 工程节点将在整 Target 重写时统一替换。 |
| 2026-08-29 | `Application/CMakeLists.txt`; `Application/UserAPP/Coffee2App/Modbus_Rtu_Bus/coffee2_rtu_bus.c` | Modify | 修复 Coffee2 GCC 构建图：加入新 Modbus/CoffeeMachine 协议源及 include 路径、加入 `coffee2_device_image.c`、统一 F200 头文件 include；保留旧 `Coffee2Protocol/coffee2_rtu_protocol.c` 文件但不再编译。待 GCC 验证。 |
| 2026-08-29 | `GCC-ARM/build/Coffee2-Debug` | Verify | CMake 配置已成功；原 build tree 的 Ninja 锁导致标准增量构建卡住，未删除中间文件。按生成的 `compile_commands.json` 手动编译 8 个新协议源及 `coffee2_device_image.c`（9/9 成功），再用 170 个目标对象执行 GCC 链接验证（成功，RAM 109776/131072、CCMRAM 40880/65536、FLASH 171784/344064）。未烧录。 |
| 2026-08-29 | `MDK-ARM/STM32F407_Base.uvprojx` | Modify/Verify | 按 Coffee2 GCC 协议迁移同步补充 Keil Coffee2 Target 的 `../Application/DeviceProtocol/CoffeeMachine` include 路径；XML 解析和路径存在性检查通过，未修改其他 Target、源文件编译状态或生成配置，未烧录。 |
| 2026-08-29 | `资料文档/00_README/工程基础缓存.md`; `资料文档/全局审查.md` | Correct | 同步 GCC 协议迁移后的 9/9 新源编译与等价直接链接证据；将标准 Ninja `.ninja_lock` 限制单独标注；发现 `CubeMX_Base/F407Base.ioc` 当前缺失，仅保留历史哈希并标为 UNKNOWN，未恢复或修改任何 CubeMX 文件。 |
| 2026-08-29 | `资料文档/全局审查.md` | Correct | 修正协议构建矩阵及 HSE 证据中的过时表述，使 GCC/Keil 新协议清单与当前缺失 IOC 的状态一致。 |
| 2026-08-29 | `资料文档/全局审查.md` | Correct | 将 PLL/SYSCLK 的 IOC 引用明确标为历史快照，当前时钟结论优先回溯 `main.c`，避免把缺失 IOC 当作现存证据。 |
| 2026-08-29 | `Application/DeviceProtocol/**`; `Application/Common/Command/app_command.h` | Delete | 删除已确认没有独立复用价值的 DeviceProtocol 薄转发层及公共 AppCommand 头；Coffee2 命令类型、动作映射和设备结果处理回归 `UserAPP/Coffee2App` 私有 owner，公共设备能力保留在 DeviceLibrary。 |
| 2026-08-29 | `Application/UserAPP/Coffee2App/Device/coffee2_device.*`; `Modbus_Rtu_Bus/coffee2_rtu_bus.c`; `Robot_Tcp/coffee2_robot_tcp.c`; `Ota/coffee2_ota.*` | Refactor | Coffee2 直接调用公共 `device_*` 设备库；F200、杯盖、糖浆、制冰、秤、电表、IO 和 Dobot 均由私有 owner 编排；合并私有 OTA 入口并保留 Coffee2 业务状态/image。 |
| 2026-08-29 | `Application/CMakeLists.txt`; `GCC-ARM/CMakeLists.txt` | Refactor | 将 `app_*`/`device_*` 公共目标和公共 include 路径移到产品分支外，Coffee2 私有 include 改为 `coffee2_app PRIVATE`；移除全局暴露的 Coffee2 Task_Manager 路径，确保未来 target 可选择性复用公共层。 |
| 2026-08-29 | `MDK-ARM/STM32F407_Base.uvprojx` | Refactor | Keil 当前仅保留 Coffee2 target；公共 Common/Transport/ModbusPort/DeviceLibrary/nanoMODBUS 组各出现一次，Coffee2 私有源集中在 Application/Coffee2App，移除 DeviceProtocol/MilkTeaApp 活动组。 |
| 2026-08-29 | `MDK-ARM/STM32F407_Base.uvprojx` | Clean | 清理 RTE targetInfo 和 LayerInfo 中残留的 MilkTea 元数据；XML 仍保持单一 Coffee2 target 且所有活动文件路径存在。 |
| 2026-08-29 | `Application/Common/CommonTargets.h` | Scope | 当前组合根仅保留 Coffee2 manager 选择；移除未进入任何活动 target 的 MilkTea manager 头引用，后续 MilkTea 重写时由其独立组合根重新接入。 |
| 2026-08-29 | `资料文档/00_README/当前工程架构与公共私有边界.md`; `资料文档/00_README/工程基础缓存.md`; `资料文档/00_README/00_README_索引.md`; `资料文档/全局审查.md`; `AGENTS.md` | Document | 发布四层定稿、公共/私有边界、命令策略、CMake/Keil 组合规则及 CubeMX_Genarate 维护入口；旧全局审查架构段标记为历史证据。 |
| 2026-08-29 | `CubeMX_Base/F407Base.ioc` | Policy | 按用户决定将根目录 IOC 视为废弃事实源；正式维护输入为 `CubeMX_Base/CubeMX_Genarate/CMAKE/F407Base_CMAKE.ioc` 与 `.../MDK/F407Base_MDK.ioc`，本次未编辑任何 IOC 或生成文件。 |
| 2026-08-29 | GCC/Keil/static audit | Verify | CMake 配置/生成通过；Keil uvprojx XML_OK、target 数=1；关键 Coffee2 源按生成命令编译通过；公共层无 UserAPP 反向引用，DeviceProtocol/app_command/旧 OTA 路径均无活动文件。完整 Ninja 在当前执行阶段挂起，未烧录。 |
| 2026-08-29 | `Application/Common/Diagnostics/*`; `Application/Common/compiler_compat.h`; `Application/UserAPP/Coffee2App/Comm_Log/coffee2_crash_log_port.c` | Refactor | 将崩溃输出从公共诊断对 Coffee2 `app_comm_log_port.h` 的反向依赖改为弱公共 `lAppCrashDiagWrite()` 钩子；Coffee2 提供强绑定，其他 target 可提供自己的输出或使用默认空实现。 |
| 2026-08-29 | `Application/UserAPP/Coffee2App/Robot_Tcp/coffee2_robot_tcp.c` | Fix | 按协议变体条件编译 Dobot 配置表，消除未选协议配置在 GCC `-Wall -Wextra` 下的未使用警告。 |
| 2026-08-29 | `GCC-ARM/CMakeLists.txt`; `GCC-ARM/CMakePresets.json` | Scope | 当前 GCC 仅保留 Coffee2 preset；MilkTea 入口停止作为可构建 target 暴露，待后续独立重写。 |
| 2026-08-29 | `GCC-ARM/build/Coffee2-Release` | Clean | 清理引用已不存在桌面路径的旧 CMakeCache；该目录为可再生构建产物，未触及源码。当前环境对全新 Release ABI 探测会在执行器阶段挂起，未宣称 Release 构建通过。 |
| 2026-08-29 | `GCC-ARM/build/Coffee2-Debug/compile_commands.json` | Verify | 当前生成命令逐一编译 12 个 Coffee2 私有 C 源、19 个公共 C/ASM 源及生成的 freertos.c（共 32 个），0 error、0 warning；弱/强崩溃输出钩子可重定位链接通过。 |
| 2026-08-31 | `D:/Project_Items/Coffee1/coffee_bootloader_v4.0/BOOTLOADER_ARCHITECTURE.md` | Document | 梳理 Bootloader V4.0 的技术栈、Flash 分区、CONFIG 元数据协议、VE/VG 布局选择、升级状态机、应用跳转约束、Coffee2 OTA 联动、Keil/GCC 下载边界及发布检查清单。 |
| 2026-08-29 | `D:/Project_Items/Coffee1/coffee_bootloader_v4.0/bootloader/bootloader.c` | Fix | 修复应用跳转时切换 MSP 后经过普通 C 函数尾声导致的栈越界；保留 Bootloader 清理流程，改用 ARM Compiler V5 汇编直设 MSP 并 BX 到应用 Reset_Handler。 |
| 2026-08-31 | `Application/DeviceLibrary/CoffeeMachine/coffee_machine_modbus.c/.h` | Rename | 按协议型号将旧 Kalerm O/X 通用文件更名为 `coffee_machine_O.c/.h`，O 型状态区固定为 0x1000 起始的 16 个寄存器。 |
| 2026-08-31 | `Application/DeviceLibrary/CoffeeMachine/coffee_machine_O.c/.h`; `coffee_machine_X.c/.h`; `coffee_machine_m50.c/.h` | Add/Refactor | 新增 O、X、M50 三个 target-neutral Modbus RTU 设备库；O/X 分别支持 16/24 状态寄存器、制作/暂停/恢复/电源/清洗/取消/故障复位，M50 按 Coffee1 寄存器 0x1000、0x2000、0x200C、0x200D、0x200E 实现。 |
| 2026-08-31 | `Application/CMakeLists.txt`; `MDK-ARM/STM32F407_Base.uvprojx`; `Application/DeviceLibrary/Inc/device_library.h` | Build | 增加 `device_coffee_o`、`device_coffee_x`、`device_coffee_m50` 公共目标，Keil DeviceLibrary 组替换旧 generic 文件并纳入三源，新增 M50 驱动身份枚举。 |
| 2026-08-31 | `Application/DeviceLibrary/CoffeeMachine/coffee_machine_X.c` | Correct | 按 X 系列 V1.4 协议将 0x2001 恢复值修正为 0x0000；O 系列保持 0x0002。 |
| 2026-08-31 | `资料文档/00_README/当前工程架构与公共私有边界.md`; `资料文档/00_README/工程基础缓存.md` | Document | 更新公共 CoffeeMachine 目录清单和 O/X/M50 Modbus RTU 型号能力，明确三者可被任意 target 选择性链接且不绑定 Coffee2。 |
| 2026-08-31 | `资料文档/全局审查.md` | Document | 在当前审查入口增加 O/X/M50 公共 Modbus RTU 集成快照，标明旧 generic 文件不再是活动源码，并保留历史章节的追溯属性。 |
| 2026-08-31 | `GCC-ARM/build/Coffee2-Debug`; `MDK-ARM/STM32F407_Base.uvprojx` | Verify | CMake 重新配置/生成成功；从最新 `compile_commands.json` 直接编译 O/X/M50 三个公共源均 0 error/0 warning；Keil XML 与三条源路径检查通过。完整 Ninja 仍受既有并行进程锁阻塞，未强杀、未烧录。 |
| 2026-08-31 | `Application/UserAPP/Coffee2App/Modbus_Tcp_Server/coffee2_server.c/.h` | Refactor | 删除 Coffee2 Server 的 0x0084-0x0086 IO 调试写命令及 0x1084-0x108F 私有调试状态；旧命令地址写入统一返回非法地址。按上位机协议新增只读 0x10CC/0x10CD/0x10CE 三个位图寄存器，分别发布本机 8DI+8DO、外部 16DI、外部 16DO。状态读取前即时刷新本机 GPIO，外部模组沿用各总线 owner 的缓存镜像。 |
| 2026-09-01 | `资料文档/03_技术实现核心文档/06_日志/日志系统总体实现说明.md` | Add | 新增 Coffee2 日志系统总体实现说明：分层、调用链、API 清单、18 个 source 表、行为约束、问题审查与状态标签；标注 DeviceProtocol 与 AGENTS.md 现状为 CONFLICT，旧 MilkTea 死机文档为 HISTORICAL/OUT_OF_SCOPE。 |
| 2026-09-01 | `资料文档/03_技术实现核心文档/06_日志/日志说明_Coffee2.md` | Add | 新增 Coffee2 独立日志说明：接入公共日志方式、边界核查、Workflow/Robot/RTU/Server/IO/OTA 模块日志清单与示例；仅记录已实现与建议，不臆断。 |
| 2026-09-01 | `资料文档/03_技术实现核心文档/06_日志/崩溃死机日志机制说明.md` | Add | 新增崩溃死机日志机制说明：异常入口、崩溃上下文逐项 IMPLEMENTED/PARTIAL/NOT_IMPLEMENTED、输出路径与崩溃后行为；Coffee2=INTEGRATED，MilkTea=OUT_OF_SCOPE。 |
| 2026-08-31 | `资料文档/99_其他资料/ETH+TO+6U8I8O+V11原理图.pdf`; `CubeMX_Base/CubeMX_Genarate/{CMAKE,MDK}/F407Base_*.ioc`; `CubeMX_Base/Core/Inc/main.h`; `CubeMX_Base/Core/Src/gpio.c` | Verify | 只读核对本机 IO 映射：X1-X8=PE0-PE7 输入，Y1-Y8=PE8-PE15 输出；现有 Coffee2 GPIO 描述表与原理图、两份维护 IOC 和生成代码一致，未修改 IOC 或生成 GPIO 配置。 |
| 2026-08-31 | `资料文档/00_README/工程基础缓存.md`; `资料文档/全局审查.md`; GCC Coffee2 Debug | Document/Verify | 更新 IO 寄存器快照与硬件映射证据；CMake 配置/生成成功，`coffee2_server.c` 按最新 `compile_commands.json` 直接编译 0 error/0 warning。完整 Ninja 再次无输出挂起，已中止等待，未宣称完整链接通过；未执行 Keil 构建或烧录。 |
| 2026-08-31 | `Application/UserAPP/Coffee2App/Modbus_Rtu_Bus/coffee2_rtu_bus.c` | Fix | 为 IO 输出设备补齐 `COFFEE2_ACTION_REFRESH` 分支，使用 FC01 读取 16 路输出并提交状态镜像；保留 `COFFEE2_ACTION_IO_WRITE` 的 FC05 控制路径，避免周期刷新被错误判定为 NOT_SUPPORTED。 |
| 2026-08-31 | `Application/UserAPP/Coffee2App/IO_State/coffee2_io.c`; `Application/UserAPP/Coffee2App/Modbus_Tcp_Server/coffee2_server.c/.h` | Add/Fix | 按低电平有效修正本机 PE0-PE7 DI 逻辑；新增协议定义的 0x0208（本机8路输出掩码）与 0x0209（外部16路输出掩码）单寄存器调试写入，FC06/FC10 单次提交、返回一次响应。外部掩码按变化位投递既有 FC05 控制，内部工作流与状态刷新路径复用原有机制。 |
| 2026-08-31 | `Application/UserAPP/Coffee2App/Modbus_Tcp_Server/coffee2_server.c` | Fix | 将 0x0208/0x0209 IO 调试寄存器接入正确的 FC03/FC06/FC16 回调；读取返回本机和外置输出镜像，连续写入按寄存器逐项提交，不再错误地在读取回调中执行写操作。 |
| 2026-08-31 | `Application/Common/Log/app_log.c/.h`; `Application/UserAPP/Coffee2App/Comm_Log/coffee2_log.c/.h` | Add | 增加无 result/field 后缀的人类可读日志入口及有界 printf 风格适配，保留原结构化日志接口和前缀格式，支持关键路径直接输出中文说明。 |
| 2026-08-31 | `Application/UserAPP/Coffee2App/WorkFlow/coffee2_workflow.c` | Fix | 初始化重试仍然保留，但初始化步骤不再周期输出 WORKFLOW_STEP_START/DONE/DEVICE_FAILED；每个初始化设备首次失败输出一条中文错误，设备成功后允许下一次失败重新提示，避免重试机制造成日志刷屏和设备含义不清。 |
| 2026-08-31 | `Application/UserAPP/Coffee2App/Config/coffee2_io_names.h`; `Application/UserAPP/Coffee2App/IO_State/coffee2_io.c` | Add | 为本机8路DI/DO、外置16路输入和16路输出建立 Coffee2 私有英文宏名与中文显示名；首次采样建立基线，后续仅在逐点状态边沿变化时输出中文日志，避免周期刷新刷屏。 |
| 2026-08-31 | `Application/UserAPP/Coffee2App/Robot_Tcp/coffee2_robot_tcp.c`; `Application/UserAPP/Coffee2App/Modbus_Rtu_Bus/coffee2_rtu_bus.c` | Modify | 为机器人开始/完成/超时动作和 RTU 设备命令失败增加中文可读说明，同时保留设备 source 前缀、错误码和底层结构化诊断能力。 |
| 2026-08-31 | `Application/UserAPP/Coffee2App/Config/coffee2_io_names.h`; `Application/UserAPP/Coffee2App/WorkFlow/coffee2_workflow.c`; `Application/UserAPP/Coffee2App/Modbus_Rtu_Bus/coffee2_rtu_bus.c`; `Application/UserAPP/Coffee2App/Modbus_Tcp_Server/coffee2_server.c`; `Application/UserAPP/Coffee2App/IO_State/coffee2_io.c`; `Application/UserAPP/Coffee2App/Robot_Tcp/coffee2_robot_tcp.c` | Fix | 将运行时日志和 IO 名称统一为 ASCII 英文宏名/字段，移除 Coffee2 活动源码中的中文字符串字面量，兼容 ARMCC V5.06 与 GCC。 |
| 2026-08-31 | `资料文档/00_README/工程基础缓存.md` | Update | 记录 IO、设备和机器人日志改为 ASCII 宏名，中文仅保留在外部对照文档。 |
| 2026-08-31 | `Application/UserAPP/Coffee2App/Config/coffee2_io_names.h` | Modify | 按最新确认的英文命名表替换全部板载 DI/DO、X01-X16 输入及 Y11-Y26 输出名称。 |
| 2026-08-31 | `资料文档/00_README/日志设计与诊断行为规则.md`; `资料文档/00_README/工程基础缓存.md` | Add/Update | 固化跨平台日志行为约束：强调可定位、可读、边沿触发、异常去重和工具链兼容，不固定具体日志格式。 |
| 2026-09-01 | `AGENTS.md` | Clarify | 明确历史构建日志中的 DeviceProtocol 路径不代表当前目录或构建引用，当前状态必须以源码、CMake 和 Keil 实际配置为准。 |
| 2026-09-01 | `.agents/工程维护Agent提示词.md` | Fix | 更新工程根路径、CubeMX 事实源、公共/私有 target 边界，移除当前不存在的 DeviceProtocol 目录描述，并加入不固定格式但必须可定位问题的日志行为约束。 |
| 2026-09-01 | `Application/UserAPP/Coffee2App/Modbus_Tcp_Server/coffee2_server.h/.c` | Fix | 按最新协议扩展 4300-4308（0x10CC-0x10D4）有效寄存器窗口；当前填充板载 IO、第一组 16 路输入和第一组 16 路输出，其余模块槽位保留并返回 0。 |
| 2026-09-01 | `Application/UserAPP/Coffee2App/Modbus_Tcp_Server/coffee2_server.h/.c` | Fix | 按更新协议改为开放 IO 页面 4336-4351（0x10F0-0x10FF）32 路兼容窗口；实时填充板载 DI/DO、第一组 16DI 和第一组 16DO，其余高低 16 位及扩展槽位保持 0。 |
| 2026-09-01 | `Application/UserAPP/Coffee2App/Modbus_Tcp_Server/coffee2_server.c` | Fix | 将板载 IO 输出调试位图由高 8 位 0x0100-0x8000 改为低 8 位 0x0001-0x0080；读取和写入语义同步调整，外置 16 路输出调试保持不变。 |
| 2026-09-01 | `资料文档/03_技术实现核心文档/Coffee3Close技术资料/Coffee3Close业务流程与设备职责初稿.md` | Add | 基于 Coffee3Close 初稿、配置说明、Coffee2 参考文档和 Coffee1 业务逻辑，建立物理模型、IO 语义、设备职责、初始化/残杯、订单主流程、报警边界及待确认需求的第一版方案。 |
| 2026-09-01 | `资料文档/03_技术实现核心文档/Coffee3Close技术资料/Coffee3Close业务流程与设备职责初稿.md`; `Coffee3Close第二阶段详细业务流程说明.md` | Update/Add | 固化残杯检查失败即初始化锁定、只能人工处理后整机复位的规则；以 Coffee1 封闭式流程为对照，输出 Coffee3Close 初始化、订单、线上暂存、现场直出、取餐、出液、清洗、水路、机构、安全、取消和断线的第二阶段详细目标流程。 |
| 2026-09-01 | `资料文档/00_README/工程基础缓存.md` | Update | 记录 Coffee3Close 第二阶段业务设计状态和残杯初始化锁定规则。 |
| 2026-09-03 | `资料文档/03_技术实现核心文档/Coffee3Close技术资料/Coffee3Close第四版详细业务流程设计.md` | Add/Correct | 基于V3审核稿和Coffee1 v2.7/v2.8源码形成Coffee3Close第四版：按实机验证时序区分线上/线下制作完成点，取消未经证实的制作/放杯ACK，仅保留0x000B客户取走确认；补充28项任务、三张流程图、IO/寄存器映射、异常/并行/日志/测试，并列出A01-A18审核事项。 |
| 2026-09-03 | `资料文档/00_README/工程基础缓存.md`; `资料文档/全局审查.md` | Update | 记录Coffee3Close V4业务基线候选及Coffee1核对证据；标记当前上位机DOCX中0x0007清零所有权、0x1008完成示例、0x0208位图和0x100B值域的协议内部冲突。本次未修改DOCX、源码、工程配置，未构建或烧录。 |
