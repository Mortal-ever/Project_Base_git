# Coffee2 单机调试阶段实施与自检报告

## 2026-08-12 Robot Coffee1 语义同步

Coffee2 Robot 已按 Coffee1 基线刷新和启动：基础状态 0..15 使用 FC02，3100..3139 使用 FC01；3100 只读，3120 是独立启动信号，普通动作清零限定为 FC15 3121..3139 共 19 位。启动每步间隔 200 ms，按基础 0..9 清零、报警/停止/退出拖拽/使能/启动顺序执行；活动订单恢复路径先读取保存的 command/result，result=1 直接 reconcile，command=1/result=0 等待，只有均为 0 且映射有效时才跳过 STOP 并保留动作/结果区。连接成功后以 enable、无 alarm 和 ready/run/idle 持续状态接受 warm attach，power_on 仅诊断。动作仍须严格 READY，链路错误才断开并永久退避。

日期：2026-07-31  
项目根目录：`C:\Users\13193\Desktop\Project_Base`  
Keil Target：`Coffee2`

## 1. 本阶段结论

Coffee2 已形成可编译、可通过 Modbus TCP 上位机进行单设备调试的基础工程：主控作为端口 6001、Unit ID 1 的 Modbus TCP Server，最多维护两个客户端槽；机器人为 Modbus TCP 主站任务；Bus2～Bus5 为四个独立 Modbus RTU 主站任务；设备命令统一经队列投递，设备状态和完成/异常结果通过每设备独立 EventGroup 发布；工作流按事件结果推进或进入异常路径。

本阶段没有移植 coffee1 的旧工作流，只参考了其中已经使用过的单设备功能，并以当前资料协议重新实现。

## 2. 任务与物理绑定

| 任务 | 角色 | 物理接口/端点 | 从站 |
| --- | --- | --- | --- |
| C2Log | 异步结构化日志 | USART1，115200 | 无接收调试命令 |
| C2Server | Modbus TCP Server | `0.0.0.0:6001` | Unit ID 1，2 个客户端槽 |
| C2Robot | Modbus TCP Client | `192.168.5.100:502` | Unit ID 1 |
| coffee2_bus2 | Modbus RTU Master | USART2 | 咖啡机，地址 1 |
| coffee2_bus3 | Modbus RTU Master | USART3 | 落杯机 1、糖浆机 2、落盖机 3 |
| coffee2_bus4 | Modbus RTU Master | UART4 | 制冰机 1、称重模块 2、电能表 3 |
| coffee2_bus5 | Modbus RTU Master | UART5 | 输入 IO 1、输出 IO 2 |
| C2Workflow | 订单与维护流程 | 无直接总线所有权 | 向上述设备任务发送标准命令 |

USART1 只用于日志；USART6 在 Coffee2 二次初始化时停用；不存在 Bus1 任务。RS232/RS485 不进入软件总线类型，软件只绑定 STM32 UART，电气输出方式由硬件选择。

## 3. 已实现的协议调试入口

所有 FC06/FC10 写成功仅表示命令已进入目标任务队列，不表示机械动作已经完成。动作完成结果应读取 `0x1000` 状态区、`0x1100` 监控区或查看 USART1 日志。

| 上位机寄存器 | 已实现行为 | 使用方法 |
| --- | --- | --- |
| `0x0030` | 机器人开始、停止、暂停、使能、下使能、清报警、进入/退出拖拽 | 写值 `0..7`；Robot 对 Coil `0..7` 先写 0 再写 1 |
| `0x0031` | HOME、冷热杯、落盖 A/B、咖啡机前/咖啡点、制冰点、打印点、取出餐、取/压盖、放出餐、放储藏位 | 按主协议枚举写入；保留值会记录 `UNSUPPORTED` 日志 |
| `0x0040` | 咖啡机制作饮品 | 写 UI 饮品编号 `0..0x28`，包括合法值 0 |
| `0x0042` | 咖啡机冲洗 | 当前只开放设备协议共同支持且未划掉的值 1、2 |
| `0x0047` | 取消咖啡任务 | 写 1 |
| `0x0050` | 冰杯、热杯、冰盖、热盖 | 写 `0..3`，分别路由到地址 1 的落杯机或地址 3 的落盖机 |
| `0x0060/0x0061` | 糖浆 A～D 与出液时间 | 先写 `0x0061`，再写 `0x0060=0..3`；设备通道参数转换为 1～4 |
| `0x0062` | 糖浆剩余时间 | 写入后下发到糖浆机寄存器 10 |
| `0x0063` | 糖浆管路清洗 | 写 1 |
| `0x0070/0x0071` | 制冰—称重联动单机测试 | 先写目标 `0x0071`，单位 0.1 g，再写 `0x0070=1` |
| `0x0000..0x0008` | 订单接收与核对 | 写订单参数、`0x0007=1`、`0x0008=1` 后进入工作流 |

机器人最新协议处理如下：基础控制使用 Coil `0..7` 上升沿；动作命令使用 `3120..3138`；完成状态使用 `3100..3118`。每个位置动作前，Coffee2 通过 FC15 清除 `3121..3139` 共 19 个动作命令线圈并读回全零，保留 3120 不清除；完成状态为 1 时执行写 0、读回 0 的确认握手。没有使用资料中空缺的 3139 作为结果映射。

## 4. 制冰与称重联动

称重模块会把原始值、单位和小数位归一为 0.1 g。当前接受资料中明确的单位值：2 表示 kg，4 表示 g；未知单位或异常小数位会产生协议错误。

出冰流程为：

1. 称重去皮并等待稳定。
2. 连续读取三次重量，使用中值拒绝单点抖动。
3. 使用一阶模型估算首次开阀时间：`t(ms) = 18 * weight(g) - 300`。
4. 开阀时间限制在 200～2000 ms（`COFFEE2_ICE_MAX_PULSE_MS=2000`），等待过程每 50 ms 检查取消请求。
5. 关闭阀门、等待 1000 ms 落冰稳定，再读取三次中值。
6. 允许最多两次按重量差补偿；误差容许为 ±2.0 g。
7. 超重、称重不稳、通信失败或补偿耗尽均进入失败路径；每次脉冲后都先关闭阀门。

上述 `18` 和 `-300` 是从旧设备经验点形成的初始标定值，只用于首轮硬件联调。量产前必须用当前制冰机、阀门、冰型和称重安装方式重新标定。

## 5. Server 状态与双客户端

- Listener backlog 与应用槽数量均为 2。
- 两个槽各自拥有独立 nanoMODBUS Server 上下文和请求/异常/断开统计。
- 第三个连接会立即关闭并记录 `SERVER_CLIENT_REJECTED`。
- 任意一个槽连接即认为 Server Online；第二个槽可作为调试连接。
- 任一槽断开不影响另一槽；PHY/IPv4 失效时关闭 Listener 和全部槽，网络恢复后重建 Listener。
- 首次成功处理请求后输出 `SERVER_STACK_MARGIN ... hwm_words=...`，用于现场确认用户调整后的 1024-word Server 栈余量。

状态投影修正：

- `0x1008` 使用 `1=制作中、2=完成、3=失败`。
- 已划掉的 `0x1041`、`0x1051..0x1054` 不再发布旧语义。
- `0x1050` 分别用低半字节和高半字节表示落杯机、落盖机状态。
- `0x1055..0x1058` 组合发布无料、需补料、电机异常位。
- `0x1070` 只使用 `0..3`，不再发布已划掉的值 4。
- `0x1073` 按制冰机“低水位以上”信号反向生成缺水状态。
- `0x107D` 发布称重模块通信/设备故障。
- `0x1100..0x117F` 保留为只读监控区，含任务、堆、设备事件、两个客户端槽、Robot 重连和四路 RTU 状态。

## 6. 日志闭环

格式统一为：

```text
[Level][Task][Module] EVENT result=... field=value...
```

没有 Debug 级别，也没有 USART 接收调试命令。启动时记录上电、复位原因、串口重初始化、模块初始化、逐任务创建、剩余堆和启动完成。运行期只记录状态变化或真实事务，例如：

```text
[INFO][C2Server][ModbusTcp] SERVER_LISTENING result=0 port=6001
[INFO][C2Server][ModbusTcp] SERVER_CLIENT_CONNECTED result=0 slot=0
[WARNING][C2Server][ModbusTcp] SERVER_CLIENT_DISCONNECTED result=-5 slot=0
[INFO][C2Robot][RobotTcp] ROBOT_CONNECT_BEGIN result=0 attempt=1
[WARNING][C2Robot][RobotTcp] ROBOT_CONNECT_FAILED result=-5 native_error=...
[INFO][C2Robot][RobotTcp] ROBOT_RETRY_SCHEDULED result=0 delay_ms=1000
[INFO][C2Workflow][Workflow] ICE_VALVE_PULSE result=0 pulse_ms=...
[INFO][C2Workflow][Workflow] ICE_WEIGHT_SAMPLE result=0 weight_dg=...
```

RTU 相同失败只在首次或结果变化时记录；恢复 Online 时记录一次。Robot 重连尝试属于真实事务，会逐次记录；相同失败状态本身不会周期刷屏。

## 7. 内存与构建结果

最终 Keil ARM Compiler V5.06u7 构建结果：

```text
Program Size: Code=165668 RO-data=3204 RW-data=472 ZI-data=137032
0 Error(s), 0 Warning(s)
```

Map 检查：

- `RW_CCM`：CCM `0x10000000`，不使用 `UNINIT`，启动时清零实际链接的 Zero RW 内容。
- `CCM_APP`：从 `0x10000000` 开始，包含日志队列、EventGroup、Server 镜像、Robot/Workflow 队列和设备状态，共约 6.2 KB。
- FreeRTOS `ucHeap`：CCM `0x100018E0`，32 KB；清零不增加 Flash 数据镜像，只增加一次启动清零时间。
- 当前 CCM 实际使用约 38.2 KB，剩余约 25.8 KB 仍可供后续 CPU-only 对象使用，不存在固定分区造成的容量闲置。
- `RW_IRAM1`：使用 `0x16040` / `0x1C000`，约 24 KB 余量。
- USART Transport 与 RTU Bus Transport 上下文保留在 `0x2000xxxx` 普通 SRAM。
- Ethernet `DMARxDscrTab`、`DMATxDscrTab` 位于 `0x20004E94`、`0x20004F34`，未进入 CCM。

CCM 放置采用逐对象审核，不要求整个模块进入 CCM：

- 保留在 CCM：32 KB FreeRTOS heap、静态 Queue/EventGroup 控制块及存储、日志与命令队列、设备/IO/工作流状态、Robot 状态、Server 寄存器镜像和纯 CPU 协议状态。
- 保留在普通 SRAM：UART/RTU Transport 上下文及其 DMA staging、HAL 外设句柄、Ethernet 描述符、LwIP 内存池、Crash 记录和启动关键状态。
- 当前 `CCM_APP` 约 6.2 KB，属于可选优化而非强制规则；只有确认对象全生命周期 CPU-only，并且能增加普通 SRAM 余量或具有明确访问收益时，才允许继续加入。
- 当前普通 SRAM 仍保留约 24 KB，CCM 仍保留约 25.8 KB；后续优化以实测 heap 最低余量、任务栈水位和 map 为依据，不以“填满 CCM”为目标。

## 8. 仍不能安全完成的项目

以下项目缺少确定的硬件映射、设备协议或标定数据，因此本次没有猜测实现：

1. 主协议 `0x0023..0x0025`、`0x0041`、`0x004E` 对应的水阀、紫外灯和水泵没有给出到 `g_xCoffee2Io` 的最终 X/Y 点位映射。
2. 糖浆 E/F 没有实际地址 5/6 设备通道；写值 4/5 会明确记录不可用日志。
3. 咖啡清洗值 6 在售卖机协议中未划掉，但 X 系列咖啡机协议只定义 1～5；当前不发送值 6。
4. 落杯落盖文档中个别传感器线圈地址存在重复描述；当前按连续的两组 10 Coil 镜像发布，需实机确认“需补料”位。
5. `0x007E` 只写“出冰比例系数”，没有单位、缩放、斜率/截距定义；当前使用编译期初始标定值，未把该寄存器直接用于安全控制。
6. 自动清洗 `0x0080..0x0083`、封闭式出餐机构 `0x0091..0x009D`、果乳 `0x00A1..0x00AF` 缺少已绑定的设备协议和物理执行点。
7. 固件升级 `0x0200..0x0202` 缺少 Bootloader、镜像布局、校验、断电恢复和升级传输协议，只保留寄存器镜像。
8. 机器人最新 Coil 协议只给出一个“放储藏位”和一个“放出餐口”动作，没有传递 1～12 位置号的数据寄存器；Coffee2 会保留位置参数，但当前机器人线上无法发送该参数，需机器人协议补充。
9. 本地 LwIP 没有启用 RAW API，也没有 ping 应用模块；本次不修改两个 Target 公用的 LwIP 配置。网络有效性使用 PHY link、netif、IPv4、Server 实际连接及 Robot Modbus 健康事务判断。
10. 编译无法替代硬件验收；两客户端并发、Robot 线圈沿、各 RTU 从站、真实 IO 极性、制冰标定和 `SERVER_STACK_MARGIN` 仍需上电日志验证。

## 9. 建议现场验收顺序

1. 上电收集从 `POWER_ON` 到 `STARTUP_COMPLETE` 的完整 USART1 日志。
2. 读取 `0x1100..0x117F`，确认任务掩码、堆、Bus2～Bus5 Ready。
3. 用两个 TCP Client 同时连接 6001，确认 slot0/slot1；连接第三个客户端确认被拒绝。
4. 分别执行 Robot、咖啡机、落杯、落盖、糖浆单机命令，并轮询对应 `0x10xx` 状态。
5. 在空载且可人工急停条件下执行小重量制冰测试，记录每次 `pulse_ms`、`weight_dg`，重新拟合斜率与截距。
6. 最后提交完整订单；任何异常均保存从命令接受到 `ORDER_FAILED` 或 `ORDER_DONE` 的连续日志。
