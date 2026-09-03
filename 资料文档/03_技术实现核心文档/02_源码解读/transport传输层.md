---

# Transport 传输层架构详解(重写版)
> 涉及源码(全部真实路径/行号):
> - `Application/Transport/Inc/transport.h`
> - `Application/Transport/Src/transport.c`
> - `Application/Transport/Inc/transport_tcp.h`、`Src/transport_tcp.c`
> - `Application/Transport/Inc/transport_uart.h`、`Src/transport_uart.c`
> - 协议适配层 `Application/ProtocolStack/ModbusPort/Src/modbus_port.c`

---

## 第 0 章 一张图看懂整层

先给你全局观,后面每一节都是把这图的某条线展开。

```
┌────────────────────────────────────────────────────────────────────┐
│  应用层 (app_task_manager / app_modbus_tcp_client / app_modbus_rtu_bus) │
│  职责: 启动时初始化传输层、创建通道、发起 Modbus 事务                     │
└──────────────────────────┬─────────────────────────────────────────┘
                           │  vTransportManagerInit() / xTransport...()
                           ▼
┌────────────────────────────────────────────────────────────────────┐
│  传输层 transport.c / transport.h (后端无关的"调度壳")                │
│  • 通道注册表 s_apxChannels[10]                                      │
│  • 生命周期: Init → Register → Find → Open/Close                    │
│  • 数据: Send / Receive / ReceiveExact                               │
│  • 控制: Control(RX_PAUSE/RESUME/FLUSH/BAUD/RESET)                  │
│  • 诊断: prvRecordOperation() 记计数/故障                             │
│  • ISR 事件转发: vTransportNotifyEventFromISR()                      │
│  ★ 核心: TransportChannel_t.pxOps → TransportOps_t 函数指针表        │
└────────────┬────────────────────────────────────────────────────────┘
             │  通过 pxChannel->pxOps->xSend() 等"间接调用"
     ┌───────┴───────────────┬──────────────────┐
     ▼                       ▼                  ▼
┌────────────────┐  ┌────────────────┐  ┌────────────────┐
│ transport_tcp.c│  │ transport_tcp.c│  │ transport_uart.c│
│ Netconn 后端    │  │ Socket 后端     │  │ HAL UART/RS485  │
│ s_xTcpOps       │  │ s_xTcpSocketOps│  │ s_xUartOps      │
└────────────────┘  └────────────────┘  └────────────────┘
              ┌──────────┴──────────┐
              ▼                     ▼
┌────────────────────────────────────────────────────────────────────┐
│  协议层 modbus_port.c (nanoMODBUS 适配)                              │
│  prvRead→xTransportReceiveExact / prvWrite→xTransportSend           │
│  prvGetEffectiveTimeout / prvMapError                               │
└────────────────────────────────────────────────────────────────────┘
```

**一句话概括**:`transport.c` 不碰任何网卡/UART 寄存器,它只负责"拿着一个函数指针表(pxOps)去调"——至于表里装的是 TCP 还是 UART 的函数,`transport.c` 根本不关心。这就是这一层存在的全部意义。

---

## 第 1 章 阅读前置:C 语言函数指针必读(不懂这章,后面全晕)

### 1.1 函数名就是地址

在 C 里,函数名**就是**它的入口地址,取地址符 `&` 可写可不写:

```c
static TransportResult_e prvSend(void *ctx, ...);   // 定义
prvSend;     // 就是一个"地址值"(函数指针常量)
&prvSend;    // 等价,也是那个地址
```

所以你在 `transport_tcp.c:178` 看到:

```c
static const TransportOps_t s_xTcpOps = {
    prvOpen,        // 等价于 &prvOpen
    prvClose,       // &prvClose
    prvSend,        // &prvSend
    ...
};
```

这不是"把函数调用结果赋值",而是**把函数地址塞进表里**。

### 1.2 拆解一句 typedef 函数指针

看 `transport.h:132` 的成员声明:

```c
TransportResult_e (*xSend)(void *pvContext,
                           const uint8_t *pucData,
                           uint16_t usDataLen,
                           uint16_t *pusSentLen,
                           uint32_t ulTimeoutMs);
```

逐段拆:

| 位置                     | 含义                                     |
| ------------------------ | ---------------------------------------- |
| `TransportResult_e`      | **返回值类型**                           |
| `(*xSend)`               | 括号说明 `xSend` 是**指针变量**,不是函数 |
| `(void *pvContext, ...)` | **参数类型列表**(不写参数名都可以)       |

读法口诀:**"xSend 是一个指向「返回 TransportResult_e、接收这 5 个参数」的函数的指针。"**

签名**必须完全一致**——`TransportOps_t` 表里说"接收 5 个参数",你填进去的 `prvSend` 也得恰好接收这 5 个参数(类型匹配),否则编译报错。这是 C 编译器帮你保证的"接口契约"。

### 1.3 槽位 vs 函数:最容易混的一对

| 名字      | 是什么                                     | 位置                                         |
| --------- | ------------------------------------------ | -------------------------------------------- |
| `xSend`   | **表里的"槽位"(struct 成员,一个指针变量)** | `transport.h:132`                            |
| `prvSend` | **表里真正存的那个函数(函数本体)**         | `transport_tcp.c:41` / `transport_uart.c:40` |

调用 `pxOps->xSend(...)` 时,`xSend` 只是一个**用来装地址的格子**,真正被执行的、里面有代码的,是 `prvSend`。**成员名 ≠ 函数**,这是大多数人反复问"xSend 到底是谁"的根源。

> 顺带:前缀 `prv` = private(文件内静态函数),`x` = 返回枚举/结构,`v` = void,`px` = 返回指针,`ul`/`us`/`uc` = uint32/uint16/uint8——这是工程命名规范,通篇如此。

### 1.4 "绑定"三件套

一个后端要想被 transport 层调用,必须走完三步:

```
① 声明 (transport.h:129)  TransportOps_t 结构体类型 —— 定义"表长什么样"
② 定义 (每个后端 .c 里)   prvOpen/prvSend/... —— 写"真正的函数"
③ 填表 (transport_tcp.c:178 或 transport_uart.c:158)  —— 把函数地址填进表 = 绑定
```

别人口中说的"绑定",就是指**第 ③ 步那一次大括号初始化**。

---

## 第 2 章 C 语言怎么模拟"虚函数表"(多态)

学过面向对象的话,这张对照表一秒看懂:

| C++ / Java 概念       | 本工程里的对应物                                             |
| --------------------- | ------------------------------------------------------------ |
| 接口 / 抽象类         | `TransportOps_t`(transport.h:129)                            |
| 虚函数表 vtable       | 每个后端的 `s_xTcpOps` / `s_xTcpSocketOps` / `s_xUartOps`(static const) |
| 派生类重写的函数      | `prvOpen/prvSend/...`(各后端 .c 里的静态函数)                |
| 对象 this 指针        | `pvContext`(void*,指向各后端的上下文结构体)                  |
| 多态调用 obj.method() | `pxChannel->pxOps->xSend(pvContext, ...)`                    |
| 基类指针              | `TransportChannel_t *pxChannel`                              |

**为什么用 `static const` 表?**

1. **可共享**:一张表写死,所有 TCP 通道共用同一个 `s_xTcpOps`,不重复占内存。
2. **只读**:放在只读区,不会被意外改写,`const` 让编译器帮你把关。
3. **链接期确定**:地址在链接时就定好了,运行期零查找成本。

`pvContext` 为什么是 `void *`?因为 transport.c 不知道也不想知道后端长什么样。后端自己在函数第一行把它"还原"回自己的类型:

```c
static TransportResult_e prvSend(void *pvContext, ...) {
    TransportTcpContext_t *pxContext = (TransportTcpContext_t *)pvContext;  // 还原
    ...
}
```

这就是 `void*` 的用法:**通用接口传递,专用函数内部强转**。

---

## 第 3 章 绑定链的完整生命周期(你反复问的那个问题)

`prvSend` 到底怎么变成能通过 `xSend` 调用的?分**编译期 → 创建期 → 调用期**三个阶段,每阶段恰一行核心代码:

### 阶段① 编译期:填表,绑定发生在这

`transport_tcp.c:178-186`(Each 后端都有一份):

```c
static const TransportOps_t s_xTcpOps = {
    prvOpen, prvClose, prvSend, prvReceive, privControl, prvGetState, prvGetNativeError
};
```

**此刻 `prvSend` 的地址已经写进了 `s_xTcpOps` 的第 3 个槽位。** 这就是"绑定动作",没有第二次,没有运行期赋值。

### 阶段② 创建期:表挂到通道

`transport_tcp.c:227-231`(`xTransportTcpCreate`):

```c
pxChannel->pcName    = pcName;            // 通道名,如 "RobotTcp"
pxChannel->pxOps     = &s_xTcpOps;        // ← 关键:把表地址装进通道
pxChannel->pvContext = pxContext;         // ← 后端上下文(数据)
pxChannel->xState    = TRANSPORT_STATE_CLOSED;
```

`pxChannel->pxOps = &s_xTcpOps` 这一行,就是"这个通道用的是 TCP 后端"的那一笔。

### 阶段③ 调用期:取地址 → 间接调用

`transport.c:182`:

```c
xResult = pxChannel->pxOps->xSend(pxChannel->pvContext, pucData, usDataLen, &usSentLen, ulTimeoutMs);
```

### 地址流转全图(三张表、三个名字的关系)

```
  prvSend 函数本体(transport_tcp.c)
      │ 它的地址
      ▼
 s_xTcpOps.xSend 槽位  (transport_tcp.c:178 编译期填好)
      │ &s_xTcpOps
      ▼
 pxChannel->pxOps      (transport_tcp.c:229 创建期挂上)
      │ ->xSend
      ▼
 transport.c:182 间接调用 prvSend(pvContext, ...)
      │
      ▼
 真正执行 lwip 发送循环的代码
```

> **一句话回答你**:绑定根本不需要你手动"调一次 setXxx"——`static const ... = { prvSend, ... }` 那行初始化,在编译期就已经把绑定完成了。你唯一需要做的,是保证创建通道时把 `pxOps` 指向对应的表。

---

## 第 4 章 一次 `xTransportSend()` 的完整旅程(带数据流 + 错误反传)

### 4.1 发送方向(一个字节怎么从上层到网卡)

```
modbus_port.c:prvWrite()                          --- 协议层发起写
   │  调 xTransportSend(pxChannel, buf, len, timeout)
   ▼
transport.c:166 xTransportSend()
   │  ① 判空/判len(171-175) → INVALID_ARG
   │  ② pxOps->xSend(pvContext, buf, len, &usSentLen, timeout)(182)
   │  ③ 若返回OK但 usSentLen != len → 记 IO_ERROR(184-187)
   │  ④ prvRecordOperation 记计数/故障(188)
   ▼
transport_tcp.c:prvSend()                          --- TCP 后端
   │  把 len 拆成 netbuf / netconn_write_partly 循环,用总截止时间
   │  返回: OK(全发完) / TIMEOUT / DISCONNECTED / NO_RESOURCE ...
   ▼
lwip_send / netconn_write  → TCP/IP 协议栈 → 网卡 → 对端
```

每层只做各自的事:**modbus_port 只想"把帧写完" → transport 只想"找到后端调一下" → 后端只想"把这段字节交给栈/硬件"。**

### 4.2 接收方向(精确读若干字节)

```
modbus_port.c:prvRead() → xTransportReceiveExact(pxChannel, buf, need, &got, tmo)
   ▼
transport.c:226 xTransportReceiveExact()
   │  ★ 总超时预算,循环拼装(见下面 4.3)
   │  内部循环调 prvReceiveOnce() → pxOps->xReceive(pvContext, ...)
   ▼
TCP:  netconn_recv / netbuf_copy_partial,保留未消费部分(usRxOffset)
UART: xStreamBufferReceive(pxContext->xRxStream, ...)  ← 中断填进来的字节
   ▼
上层 buf
```

### 4.3 重点:`xTransportReceiveExact` 的"总超时预算不刷新"(最喜欢被读晕的地方)

看 `transport.c:226-325` 的核心逻辑,它是**把"要 N 个字节"拆成多次"每次收一部分"**,关键点:

- `xStart = xTaskGetTickCount()`、`xBudget = prvMsToTicks(ulTimeoutMs)`(272-273)——**只记一次起点和总预算。**
- 每次循环根据 `xRemaining = xBudget - xElapsed`(282)算出**剩余毫秒**,传给后端(283)。也就是说**每次调用的超时都在递减,不是每次都重新给满 `ulTimeoutMs`**。
- 循环条件 `while(usOffset < usExpectedLen)`(275),把收到的字节累进 `pucData[usOffset]`(285),收满 == 期望长度 → OK(295-297)。
- 中途后端报 TIMEOUT 但预算还没到:`vTaskDelay(1)` 让出 CPU 再试(308-315)。这专门解决**非阻塞 Socket 后端在总截止时间之前"没数据先报超时"**的问题。
- 预算耗尽仍没收满 → 返回 TIMEOUT,`*pusReceivedLen` 给出已收到的部分(278/321)。

> 读源码时的笔记:注释在 `transport.c:220-224`,反复说 "without renewing the total timeout"。**这就是"总超时预算不刷新"**。

### 4.4 错误正向传播 vs 反向回拨

```
正向(错误来源): LwIP err_t / socket errno / HAL_Status
   ▼ 后端 prvMapLwipError() / prvMapSocketError()(transport_tcp.c:839/809)
   ▼ 归一化为 TransportResult_e(-1 ~ -10)
反向(向上反馈): xTransportSend/Receive 的返回值 → modbus_port.prvMapError()
   ▼ modbus_port.c 转成 ModbusPortResult_e → nanoMODBUS 拿到
```

三层 error 各不相同,靠"归一化 + 映射"连通(第 8 章给全表)。

---

## 第 5 章 公共 API 全解

10 个通道级接口 + 3 个后端 create 接口。统一点:除 `vTransportSetEventCallback`(void)/`xTransportGetState`(枚举)/`pxTransportFind`(指针)外,**回调式接口都返回 `TransportResult_e`**。

### 5.1 返回码速查(transport.h:26-38)

| 枚举                  | 值   | 含义                            |
| --------------------- | ---- | ------------------------------- |
| `TRANSPORT_RESULT_OK` | 0    | 成功                            |
| `INVALID_ARG`         | -1   | 指针/长度非法                   |
| `NOT_FOUND`           | -2   | 按名找不到通道                  |
| `BUSY`                | -3   | 资源占用(重名/UART 句柄被占)    |
| `TIMEOUT`             | -4   | 总截止时间到                    |
| `IO_ERROR`            | -5   | 后端 IO 失败 / 发不全           |
| `NO_RESOURCE`         | -6   | RTOS 对象/内存不足              |
| `NOT_OPEN`            | -7   | 通道没有活动端点                |
| `NOT_SUPPORTED`       | -8   | 后端不支持该操作                |
| `NOT_READY`           | -9   | 链路/硬件未就绪(含调度器未启动) |
| `DISCONNECTED`        | -10  | 对端关闭/复位                   |

### 5.2 逐个接口

**① `vTransportManagerInit()`** — `transport.c:63`
清空注册表。启动时、任务创建**前**调一次。用 `taskENTER_CRITICAL` 保护。

**② `xTransportRegister(pxChannel)`** — `transport.c:77`
校验 pcName/pxOps/pvContext 非空(81-84)→ 查重名(BUSY,88-91)→ 查满位(NO_RESOURCE)→ 写入注册表、置 CLOSED(99-103)。返回 OK/INVALID_ARG/BUSY/NO_RESOURCE。

**③ `pxTransportFind(pcName)`** — `transport.c:110`
线性查名,返回通道指针或 NULL。这是应用层"拿到通道"的入口。

**④ `xTransportOpen(pxChannel)`** — `transport.c:129`
调 `pxOps->xOpen(pvContext)`(138),成功→OPEN,失败→ERROR(139-140),记 OPEN 操作。

**⑤ `xTransportClose(pxChannel)`** — `transport.c:146`
调 `xClose`,成功→CLOSED。

**⑥ `xTransportSend(pxChannel, pucData, usDataLen, ulTimeoutMs)`** — `transport.c:166`
- `usDataLen == 0` → INVALID_ARG(173)。
- 调 `xSend`,**语义是"要么全发,要么报错"**:后端返回 OK 但实际发送字节 != 长度 → 记 IO_ERROR(184-187)。
- 返回 OK / TIMEOUT / IO_ERROR / 其它。

**⑦ `xTransportReceive(pxChannel, pucData, usMaxLen, pusReceivedLen, ulTimeoutMs)`** — `transport.c:196`
单次收,最多 `usMaxLen` 字节。允许"收到 0 字节但后端给 OK"的部分成功语义(见头文件 218-224 注释)。

**⑧ `xTransportReceiveExact(pxChannel, pucData, usExpectedLen, pusReceivedLen, ulTimeoutMs)`** — `transport.c:226`
收**正好** `usExpectedLen` 字节,内部循环拼装、总预算不刷新(见第 4.3 节)。Modbus 读响应就靠它。

**⑨ `xTransportControl(pxChannel, xCommand, pvArgument)`** — `transport.c:328`
把 `TRANSPORT_CTRL_*`(transport.h:97-103)分发给后端。`RX_PAUSE/RESUME/FLUSH`、`GET_BAUD_RATE`(UART)、`CONNECTION_RESET`(重置连接)。

**⑩ `xTransportGetState(pxChannel)`** — `transport.c:349`
返回 `pxOps->xGetState(pvContext)`,非法输入回 UNINITIALIZED。

**⑪ `xTransportGetStatus(pxChannel, pxStatus)`** — `transport.c:360`
临界区内整份拷贝 `xStatus` 快照,再补一次实时 state。调试排障的主力。

**⑫ `vTransportSetEventCallback(pxChannel, cb, ctx)`** — `transport.c:375`
设置/清空异步事件回调(NULL 关闭)。临界区保护。

**⑬ `vTransportNotifyEventFromISR(...)`** — `transport.c:390`
**ISR 专用**,不阻塞。更新诊断计数,再调用户回调(若注册)。参数含 `pxHigherPriorityTaskWoken` 用于 `portYIELD_FROM_ISR`。

### 5.3 后端 create 接口

| 接口                             | 文件:行              | 做的事                                                       |
| -------------------------------- | -------------------- | ------------------------------------------------------------ |
| `xTransportTcpCreate(...)`       | transport_tcp.c:201  | 建 Netconn 通道,拿 `&s_xTcpOps`,注册                         |
| `xTransportTcpSocketCreate(...)` | transport_tcp.c:239  | 建 Socket 复用通道,拿 `&s_xTcpSocketOps`,注册                |
| `xTransportTcpSocketAttach(...)` | transport_tcp.c:259  | 把一个已 accept 的 socket 挂到复用通道                       |
| `xTransportUartCreate(...)`      | transport_uart.c:170 | 建 UART 通道,初始化互斥量/信号量/流缓冲(全静态存储),拿 `&s_xUartOps`,注册 |

---

## 第 6 章 三个后端的实现对照(同一接口,三种实现)

### 6.1 Netconn 后端(transport_tcp.c)

- 上下文: `TransportTcpContext_t`(transport_tcp.h:36-46)——listener/connection/netbuf、`usRxOffset` 读取游标、`lLastNativeError`。
- 配置: `TransportTcpConfig_t`(transport_tcp.h:28-33)——CLIENT/SERVER、远程 IP、端口、`ulIoTimeoutMs`。
- 打开:`prvOpen` → `netconn_new` + (客户)`netconn_connect` / (服务端)`netconn_listen` + `netconn_accept`。
- 发送:`prvSend` → `netconn_write_partly` 循环,遵守总截止。
- 接收:`prvReceive` → `netconn_recv` 拿 netbuf,`netbuf_copy_partial` 拷走,**未消费部分留在 netbuf + usRxOffset 继续读**。
- 超时: `netconn_set_*timeout`。
- 错误映射: `prvMapLwipError`(transport_tcp.c:839)。

### 6.2 Socket 后端(transport_tcp.c)

- 上下文: `TransportTcpSocketContext_t`(transport_tcp.h:49-54)——就一个 `int lSocket` + 状态。
- 特点:**可复用**的接受会话通道;`prvOpen` 只校验已 attach;`CONNECTION_RESET` → `SO_LINGER` 立即断开 + `prvClose`(transport_tcp.c:765-786)。
- 发送/接收:非阻塞 socket + `sys_now()` 轮询做总超时;recv==0 → DISCONNECTED(750-754);EWOULDBLOCK/EAGAIN → TIMEOUT(756-758)。
- 错误映射: `prvMapSocketError`(transport_tcp.c:809)。

### 6.3 UART / RS485 后端(transport_uart.c)

- 上下文: `TransportUartContext_t`(transport_uart.h:41-62)——互斥量 `xTxMutex`、发送完成 `xTxDone`、接收流 `xRxStream`(全部**静态存储**),`ulRxDropCount`、`ulErrorCount`。
- 打开:`prvOpen`(transport_uart.c:231)→ 置 open,`HAL_UART_Receive_IT` 启**单字节中断接收**(251-252)。
- 发送:`prvSend`(296)按调度器状态分流——**启动前**走 `prvSendBeforeScheduler`(有界轮询),**运行期**走 `prvSendRuntime`(互斥量串行化 + DMA 暂存/轮询,`xTxMutex` 保护多任务并发送)。
- 接收:`prvReceive` → `xStreamBufferReceive(xRxStream)`,中断往流里喂字节。
- RS485 半双工: `prvSetDirection` 切 DE 脚电平(transport_uart.h:33-35、transport_uart.c:154)。
- 错误: `HAL_UART_ErrorCallback`(transport_uart.c:812)记错、给发送任务信号。

### 6.4 对照总结

| 能力   | Netconn                    | Socket            | UART                    |
| ------ | -------------------------- | ----------------- | ----------------------- |
| 操作表 | `s_xTcpOps`                | `s_xTcpSocketOps` | `s_xUartOps`            |
| 打开   | netconn_new+connect/listen | 校验 attach       | HAL_Receive_IT          |
| 发送   | netconn_write_partly       | lwip_send 循环    | DMA/轮询+互斥量         |
| 接收   | netbuf_copy_partial        | lwip_recv         | StreamBufferReceive     |
| 超时   | netconn_set_timeout        | sys_now 轮询      | xSemaphore ticks        |
| 中断   | —                          | —                 | RxCplt/Error/Abort 回调 |
| 半双工 | —                          | —                 | DE 脚切换               |

**这就是多态的价值**:上层看到一个统一的 `TransportResult_e xSend(...)`,底下三种实现互不干扰,想换后端只换"填表+创建",上层零改动。

---

## 第 7 章 生命周期状态机

状态定义在 `transport.h:41-47`:

```
           vTransportManagerInit / xTransportRegister
                     │
                     ▼
             ┌────────────────┐
             │ UNINITIALIZED  │  (仅通道无效时出现)
             └────────────────┘
                     │ register 成功
                     ▼
             ┌────────────────┐      xTransportOpen() 成功
             │   CLOSED       │ ──────────────────────► ┌──────────┐
             └────────────────┘                          │  OPEN    │
                     ▲                                   └──────────┘
                     │ xTransportClose() 成功                    │
                     │                                        IO 进行中
              ┌──────┴──────┐                                  ▼
              │             │                          ┌──────────┐
              │ ERROR ◄─────┼─────────────────────────│  BUSY    │
              └─────────────┘  Open/IO/底层错误        └──────────┘
```

| 转移         | 触发                     | 关键代码              |
| ------------ | ------------------------ | --------------------- |
| →CLOSED      | register 成功            | transport.c:101-103   |
| CLOSED→OPEN  | `xTransportOpen` 返回 OK | transport.c:139-140   |
| CLOSED→ERROR | open 失败                | transport.c:140       |
| OPEN→BUSY    | IO/生命周期操作进行中    | 后端自行维护          |
| →ERROR       | 后端报 IO 错误/底层断链  | tcp.c:752、uart.c:798 |
| OPEN→CLOSED  | `xTransportClose` OK     | transport.c:156-158   |

> 状态实时例:UART 中断里 `HAL_UART_Receive_IT` 失败会把 `xChannel->xState = TRANSPORT_STATE_ERROR`(transport_uart.c:798);Socket recv 返回 0 也置 ERROR(transport_tcp.c:752)。

---

## 第 8 章 错误处理与诊断实战

### 8.1 三层错误映射全表

```
底层原生                                  → 传输层 TransportResult_e
────────────────────────────────────────────────────────────────
LwIP ERR_OK / errno 0 / HAL_OK           → OK
ERR_TIMEOUT/ERR_WOULDBLOCK / EWOULDBLOCK
   /EAGAIN/ETIMEDOUT                     → TIMEOUT
ERR_MEM/ERR_BUF / ENOMEM/ENOBUFS         → NO_RESOURCE
ERR_RST/ERR_ABRT/ERR_CLSD/ERR_CONN /
   ECONNRESET/ECONNABORTED/ENOTCONN/EPIPE→ DISCONNECTED
ERR_RTE/ERR_IF / ENETDOWN/ENETUNREACH/
   EHOSTUNREACH                          → NOT_READY
ERR_INPROGRESS/ERR_ALREADY               → BUSY
其它                                     → IO_ERROR
────────────────────────────────────────────────────────────────
传输层 TransportResult_e
   → modbus_port.prvMapError() → ModbusPortResult_e(给 nanoMODBUS)
```

映射实现:`prvMapLwipError`(transport_tcp.c:839)、`prvMapSocketError`(transport_tcp.c:809)。

### 8.2 实战例 1:机器人 TCP 超时怎么查

1. 调 `xTransportGetStatus(&pxChannel, &xStatus)`(transport.c:360)。
2. 看 `xStatus.xLastFault`(transport.h:80):
   - `xOperation` → 哪个阶段(SEND/RECEIVE/OPEN...)
   - `xResult` → 归一化错误(如 TIMEOUT)
   - `lNativeError` → 底层原生码(如 LwIP err_t / errno)
   - `usRequestedLength`/`usTransferredLength` → 要多少、给了多少
   - `xTimestamp` → 何时发生
3. 再看 `xState`,判断通道现在是否 ERROR。

### 8.3 实战例 2:UART 丢字节怎么查

看 `xStatus.ulRxByteCount` vs `xStatus.ulRxOperationCount` 对不上,或直接看后端字段:

- `pxContext->ulRxDropCount`(transport_uart.h:59)——**流缓冲满被丢弃的字节数**,非 0 说明收太快/缓冲 256 太小/任务读得慢。
- `pxContext->ulErrorCount`(transport_uart.h:60)——HAL 错误次数。
- `lLastNativeError`(transport_uart.h:61)——具体 HAL 错误码。
- `HAL_UART_ErrorCallback`(transport_uart.c:812)会把这些记下来并置 ERROR。

### 8.4 怎么验证"当前是哪个后端"

调试器里看 `pxChannel->pxOps` 的地址:它等于 `&s_xTcpOps` / `&s_xTcpSocketOps` / `&s_xUartOps` 中的哪一个,就知道这个通道挂的是哪个后端。或者直接看 `pxOps->xSend` 指向的函数名(应为 `prvSend` 的某个版本)。

---

## 第 9 章 正确用法 vs 反模式

### ✅ 正确:modbus_port 式的用法套路

```
启动: vTransportManagerInit()                     (transport.c:63,任务创建前)
创建: xTransportUartCreate / xTransportTcpCreate  (填名字+配置,内部已 register)
查找: pxTransportFind("RobotTcp") 得到 pxChannel
打开: xTransportOpen(pxChannel)
收发: xTransportSend / xTransportReceiveExact(带超时)
诊断: xTransportGetStatus 定期快照
```

### ❌ 反模式(每一条都会咬你)

| 反模式                                 | 后果                 | 正确做法                              |
| -------------------------------------- | -------------------- | ------------------------------------- |
| 传 NULL 通道/缓冲/长度 0               | INVALID_ARG          | create 后先判空,数据长度>0            |
| `ulTimeoutMs = 0` 还想要"等待数据"     | 立即返回或 NOT_READY | 给个真实超时                          |
| 在 ISR 里调 `xTransportSend`/`Receive` | 阻塞/非法            | 只允许 `vTransportNotifyEventFromISR` |
| 多个任务同时 `xTransportSend`(TCP)     | 数据错乱             | UART 有互斥量;TCP 侧由上层串行化      |
| 忘了先 `Register` 就 Open              | 找不到/判空失败      | 严格走 Init→Create→Find→Open          |
| 只调 `Register` 不 `Open`              | 后端端点不存在       | 确认 Open 返回 OK                     |
| 读了 `xStatus.xLastFault` 就丢         | 错过排障信息         | 出错时立刻快照                        |

---

## 第 10 章 FAQ(把你踩过的坑全收进来)

**Q:prvSend 和 xSend 到底怎么绑定?没有显式动作吗?**
A:没有显式运行期动作。`transport_tcp.c:178` 的 `static const TransportOps_t s_xTcpOps = { prvSend, ... }` 在**编译期**就把 `prvSend` 的地址写进了 `s_xTcpOps.xSend` 槽位;创建通道时(`transport_tcp.c:229`)把 `pxChannel->pxOps = &s_xTcpOps`。调用时 `pxOps->xSend` 取回地址间接调用。**"填表"就是绑定**。

**Q:想换 UART 后端,要改哪些代码?**
A:几乎只动后端那一份:写一组 `prvOpen/.../prvGetNativeError` 并填一张 `static const TransportOps_t` 表,再写个 `xTransportXxxCreate` 把 `pxChannel->pxOps` 指向你的表。上层 `transport.c` 和 modbus_port 完全不用动——这正是该层存在的原因。

**Q:为什么函数指针表要 `const`?**
A:只读、防误改、可共享、链接期定址(见第 2 章)。

**Q:`pvContext` 为什么是 `void*`?后端怎么还原?**
A:transport.c 不想知道后端细节,通用传 `void*`;后端在函数第一行 `(TransportTcpContext_t *)pvContext` 强转回自己的类型。

**Q:`pxOps` 指向 NULL 会怎样?**
A:所有通道级接口第一道判空(如 transport.c:171-174)会直接返回 `TRANSPORT_RESULT_INVALID_ARG`,不会崩。但前提是**别越过 transport API 直接去解引用**。

**Q:多路通道怎么区分?**
A:靠 `pcName`(transport.h:151)。注册时查重名(BUSY),查找用 `pxTransportFind` 按名拿对应通道。日志 UART / Bus2-5 / Robot TCP 各是一个独立 `TransportChannel_t`,各自有自己的 pxOps/pxContext/xStatus。

---

## 第 11 章 一页速查表(可打印)

### 状态枚举(transport.h:41)
`UNINITIALIZED=0 / CLOSED=1 / OPEN=2 / BUSY=3 / ERROR=4`

### 结果枚举(transport.h:26)——负值
`OK=0 / INVALID_ARG=-1 / NOT_FOUND=-2 / BUSY=-3 / TIMEOUT=-4 / IO_ERROR=-5 / NO_RESOURCE=-6 / NOT_OPEN=-7 / NOT_SUPPORTED=-8 / NOT_READY=-9 / DISCONNECTED=-10`

### 通道生命周期
`Init → Register → Find → Open → (Send/Receive/ReceiveExact/Control) → Close`

### 绑定链
`[槽位 xSend] ← prvSend 函数地址(编译期填表) ← &s_xTcpOps(s_xTcpOps) ← pxChannel->pxOps → transport.c:182 间接调用`

### 三张操作表
| 表                | 后端           | 定义                 |
| ----------------- | -------------- | -------------------- |
| `s_xTcpOps`       | Netconn        | transport_tcp.c:178  |
| `s_xTcpSocketOps` | Socket 会话    | transport_tcp.c:189  |
| `s_xUartOps`      | HAL UART/RS485 | transport_uart.c:158 |

### 常用 API(transport.c 行号)
`Init:63 · Register:77 · Find:110 · Open:129 · Close:146 · Send:166 · Receive:196 · ReceiveExact:226 · Control:328 · GetState:349 · GetStatus:360`

### 错误映射口诀
`超时类→TIMEOUT · 内存类→NO_RESOURCE · 断链类→DISCONNECTED · 路由类→NOT_READY · 其它→IO_ERROR`

### 排障三连
1. `xTransportGetStatus` 看 `xLastFault`(阶段/归一化错误/原生码/字节数/时间)。
2. UART 丢字节查 `ulRxDropCount`、`ulErrorCount`。
3. 看 `pxOps` 指向哪张表,确认挂的是哪个后端。

---

