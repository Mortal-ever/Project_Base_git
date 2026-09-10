# nanoMODBUS 到 ModbusPort：第一层源码教材

审查日期：2026-09-09  
学习层级：最底层第三方协议库 -> 工程公共适配层  
源码状态：nanoMODBUS 为第三方源码；ModbusPort 为本工程公共源码。

## 本章目标

读者完成本章后，应能解释一次 Modbus 事务为什么必须经过 Transport，指出 nanoMODBUS 的回调由谁提供，说明 RTU/TCP 的差异，并把一次超时从原始错误追到工程统一结果。

## 1. 先确定边界

本工程没有把设备协议直接写进 nanoMODBUS。nanoMODBUS 只负责 Modbus 帧、功能码、CRC/MBAP、客户端/服务器事务和回调协议。它不知道 STM32 HAL、FreeRTOS 队列、咖啡机寄存器或 Coffee3 订单。

工程自己的 `ModbusPort` 负责三件事：把 Transport 通道接成 nanoMODBUS 的 `read/write/flush` 回调；为一次事务建立总截止时间和 RTU 帧间静默；把 nanoMODBUS、Transport 和后端错误映射为稳定的 `ModbusPortResult_e`，同时保存可诊断的原始错误和帧跟踪。

```text
HAL/UART 或 TCP 已完成配置
  -> TransportChannel_t 已创建并可收发
  -> ModbusPort_t::xNmbs 绑定平台回调
  -> nanoMODBUS 生成/解析 ADU
  -> ModbusPort 映射结果
  -> DeviceLibrary 或 Target Server 消费结果
```

## 2. 文件和构建事实

| 文件 | 所属 | 作用 | 状态 |
| --- | --- | --- | --- |
| `Application/New_Party/nanoMODBUS/Inc/nanomodbus.h` | 第三方 | 类型、回调、客户端/服务器 API | SOURCE/BUILD，具体 Target 以清单为准 |
| `Application/New_Party/nanoMODBUS/Src/nanomodbus.c` | 第三方 | Modbus 协议实现 | SOURCE/BUILD |
| `Application/New_Party/nanoMODBUS/Config/nanomodbus_config.h` | 第三方配置 | 功能裁剪和编译开关 | SOURCE/BUILD |
| `Application/ProtocolStack/ModbusPort/Inc/modbus_port.h` | 公共层 | 工程接口和错误域 | SOURCE/BUILD |
| `Application/ProtocolStack/ModbusPort/Src/modbus_port.c` | 公共层 | Transport 接入、事务和结果映射 | SOURCE/BUILD/RUNTIME |

Keil 通过 Group 和 `Include in Target Build` 决定是否编译；GCC 通过 CMake 源清单和目标链接决定是否编译。必须分别确认 `SOURCE`、`BUILD` 和 `RUNTIME`，文件存在不代表固件正在使用。

## 3. nanoMODBUS 先学什么

### 3.1 错误枚举是协议层事实

`nmbs_error` 中负值表示库、传输或帧校验错误，正值表示对端返回的 Modbus 异常码。例如 `NMBS_ERROR_TIMEOUT=-3` 表示没有在期限内完成读写，`NMBS_ERROR_CRC=-5` 表示 RTU 校验失败，`NMBS_EXCEPTION_ILLEGAL_DATA_ADDRESS=2` 表示对端合法地拒绝了地址。它们不能直接当作业务失败原因，必须由上层结合 Unit ID、地址、功能码和设备阶段解释。

### 3.2 平台配置是函数指针契约

`nmbs_platform_conf` 保存 `transport`、`read`、`write`、可选 `crc_calc`、可选 `flush` 和 `arg`。nanoMODBUS 调用 `read/write` 时只知道字节数和超时，不知道底层是 UART DMA、阻塞串口还是 TCP Socket。`arg` 通常指向本工程的 `ModbusPort_t`，因此回调可以找到绑定的 `TransportChannel_t`。

`read/write` 必须返回实际字节数；返回小于要求数量会被库视为超时，负值会被视为传输错误。这是后续排查“设备无响应”和“协议异常”的第一条边界。

### 3.3 位图宏对应 FC01/FC02/FC15

`nmbs_bitfield` 用字节数组保存线圈或离散输入。位 `b` 位于 `b >> 3` 的字节、该字节的 `b & 7` 位。它是协议缓冲区，不是工程 IO 镜像；工程仍需在 DeviceLibrary 或 Target 层完成极性转换、设备含义和寄存器映射。

## 4. ModbusPort 的类型设计

### `ModbusPortResult_e`

| 值 | 含义 | 常见来源 |
| --- | --- | --- |
| `OK` | 事务成功 | nanoMODBUS 返回 NONE |
| `INVALID_ARG` | 参数或角色错误 | 空指针、数量、超时越界 |
| `NOT_READY` | 通道或端口未就绪 | Transport 未打开 |
| `BUSY` | 同一端口已有事务 | 并发所有权冲突 |
| `TIMEOUT` | 事务截止时间耗尽 | nanoMODBUS timeout 或底层短读 |
| `TRANSPORT` | 字节传输失败 | UART/TCP 后端错误 |
| `PROTOCOL` | 响应格式/CRC/MBAP 无效 | nanoMODBUS 校验失败 |
| `EXCEPTION` | 对端返回异常响应 | 异常码保存在 `ucExceptionCode` |
| `NOT_SUPPORTED` | 当前配置未提供能力 | 被裁剪的功能码等 |
| `CANCELED` | 调用者取消 | Target 工作流终止 |

### `ModbusPort_t`

`xNmbs` 是嵌入式第三方实例，由端口对象拥有；`pxChannel` 指向调用者拥有且必须长于端口的 Transport 通道；`pxTrace` 是可选的帧跟踪对象；`xLastFault` 保存最新一次规范化结果和原始码；`xOperationStart/xOperationBudget` 描述当前事务的总期限；`ucOperationActive` 和 `ucInitialized` 表示生命周期状态。

同一个端口一次只能由一个拥有者执行事务。该结构体没有动态分配，也不能把其中的临时缓冲交给生命周期已经结束的调用者。

## 5. 初始化顺序：为什么必须先有 HAL

一个可工作的客户端链路至少按以下顺序建立：

1. CubeMX/HAL 完成 UART、GPIO、时钟、DMA/中断等硬件配置；
2. Transport 创建并绑定 `UART_HandleTypeDef` 或 TCP 后端；
3. `xModbusPortClientInit()` 调用 `prvInit()`，把通道和超时写入 `ModbusPort_t`；
4. `prvInit()` 填充 `nmbs_platform_conf` 的函数指针和 `arg`；
5. `nmbs_client_create()` 初始化 nanoMODBUS 实例；
6. 端口标记 `ucInitialized=1`；
7. DeviceLibrary 或 Target 才能发起 FC01、FC02、FC03、FC04、FC05、FC06、FC15、FC16 等操作。

HAL 未完成时，Transport 没有可用外设句柄，`read/write` 无法交付字节；即使 nanoMODBUS 对象创建成功，也只是协议对象存在，不能代表链路可用。

## 6. 一次客户端读操作如何执行

以 `xModbusPortReadDiscreteInputs()` 为例，按源码理解应分为以下阶段：

1. 参数和角色校验；
2. `prvBegin()` 保存 Unit ID、起始 tick 和总预算，拒绝同端口并发；
3. `prvWaitFrameSilence()` 等待 RTU 帧间静默，TCP 直接跳过；
4. 调用 `nmbs_read_discrete_inputs()`，库生成地址、数量和 CRC/MBAP；
5. nanoMODBUS 调用 `prvWrite()`，交给 `TransportChannel_t`；
6. nanoMODBUS 调用 `prvRead()` 获取响应，端口使用剩余事务预算；
7. nanoMODBUS 检查 Unit ID、功能码、长度、CRC 或 MBAP，并写入位图；
8. `prvFinish()` 调用 `prvMapError()`，保存原始错误和异常码并清除活动状态；
9. DeviceLibrary 根据结果决定是否提交设备镜像，Workflow 决定重试、失败或进入下一步。

“函数返回 OK”只表示本次协议事务完成，不表示设备机械动作已经完成。动作完成条件属于设备库或私有 Workflow。

## 7. 服务器路径

服务器初始化需要 `nmbs_callbacks`。Target 提供读写回调，把本地 IO、状态和调试寄存器映射为 Modbus 数据模型。`xModbusPortServerPoll()` 设置轮询预算后调用 `nmbs_server_poll()`；nanoMODBUS 解析功能码并调用 Target 回调，回调返回的异常码再由库编码为响应。

因此上位机访问 `4300` 或 `0x10F0` 时，地址是否有效由 Target Server 回调的数据模型决定，不由 nanoMODBUS 自动创建。协议库只负责传输和格式。

## 8. 错误定位方法

```text
设备动作失败
  -> DeviceLibrary 返回 ModbusPortResult_e
  -> xLastFault.xResult / xTransportResult / lProtocolCode
  -> 区分 timeout、transport、protocol、exception
  -> Target 记录设备、Unit ID、地址、阶段
  -> Workflow 决定重试、初始化失败或业务终止
```

观察变量建议：`ModbusPort_t.ucOperationActive`、`xOperationStart`、`xOperationBudget`、`xLastFault`、`xLastTransportResult`、`xLastFault.lProtocolCode`，以及 `ModbusPortTrace_t.xLastTx/xLastRx`。如果 `xLastRx.usLength` 为零，应先查 Transport/线路；如果有响应但 `lProtocolCode` 为正数，应查对端异常码和地址映射；如果 CRC/MBAP 错误，应查波特率、帧边界、缓冲和总线并发。

## 9. 学习练习

1. 在 `xModbusPortClientInit()` 设置断点，确认 HAL、Transport、nanoMODBUS 的初始化顺序。
2. 对一个 FC02 请求记录 Unit ID、地址、数量和 `xLastTx/xLastRx`，确认位图如何交给上层。
3. 人为制造一个不存在的寄存器地址，观察 `NMBS_EXCEPTION_ILLEGAL_DATA_ADDRESS` 如何变成 `MODBUS_PORT_RESULT_EXCEPTION`。
4. 让 Transport 返回短读，确认它为什么最终表现为超时，而不是协议异常。
5. 对照 Keil 的 Group 和 GCC 的 CMake 清单，确认 `nanomodbus.c` 与 `modbus_port.c` 是否都进入当前 Target。

## 10. 下一层

读完本章后进入 `05_Transport_UART与TCP传输对象.md`，重点学习 `prvRead/prvWrite` 最终如何落到 UART/TCP；随后再读 DeviceLibrary，理解设备寄存器和设备状态如何使用本章提供的公共接口。
