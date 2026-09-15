可以。前面我们已经把 `nanoMODBUS` 的内部机制读过一遍了，现在第二次细读 `modbus_port.h/.c`，重点就不再是“这个函数干什么”，而是看清楚：

> **ModbusPort 为什么存在、它怎样把 nanoMODBUS 嵌进你的工程、错误/超时/诊断为什么要再包一层，以及一笔事务到底怎么从 DeviceLibrary 穿到 Transport。**

先给这层一个定位：

```text
DeviceLibrary
     │
     │  xModbusPortReadHolding(...)
     ▼
┌──────────────────────────────┐
│          ModbusPort          │
│                              │
│  参数/角色检查               │
│  总事务 Deadline             │
│  RTU 3.5字符间隔             │
│  nanoMODBUS 错误归一化       │
│  Trace / Fault               │
│                              │
│  ┌────────────────────────┐  │
│  │        nmbs_t          │  │
│  │  Modbus协议编解码      │  │
│  └──────────┬─────────────┘  │
│             │ read/write     │
│       prvRead/prvWrite       │
└─────────────┼────────────────┘
              │
              ▼
          Transport
              │
       UART / TCP / ...
```

这和工程已经定下来的依赖方向完全一致：

```text
UserAPP -> DeviceLibrary -> ModbusPort -> Transport -> 平台
```

而 `modbus_port.h` 自己第一句也直接说明它的职责是 **Bind nanoMODBUS to the project Transport abstraction**。

------

# 一、先读 `modbus_port.h`：它实际上定义了这层的“边界”

## `ModbusPortTransport_e`

```c
typedef enum {
    MODBUS_PORT_TRANSPORT_RTU = 0,
    MODBUS_PORT_TRANSPORT_TCP = 1
} ModbusPortTransport_e;
```

注意这里没有直接把 nanoMODBUS 的：

```c
NMBS_TRANSPORT_RTU
NMBS_TRANSPORT_TCP
```

暴露给上层。

这是有意义的。

ModbusPort 自己建立了一套：

```text
工程自己的类型
        ↓
适配
        ↓
第三方 nanoMODBUS 类型
```

所以：

```text
DeviceLibrary
```

不需要依赖 nanoMODBUS 枚举的具体值。

这属于很典型的**隔离第三方库**。

后面你更换 Modbus 库的时候：

```c
MODBUS_PORT_TRANSPORT_RTU
```

理论上可以继续不变。

------

# 二、`ModbusPortRole_e`：一个对象明确只有一个角色

```c
typedef enum {
    MODBUS_PORT_ROLE_CLIENT = 0,
    MODBUS_PORT_ROLE_SERVER = 1
} ModbusPortRole_e;
```

于是一个：

```c
ModbusPort_t
```

初始化完成后，要么是：

```text
Client
```

要么：

```text
Server
```

而不是调用函数时临时决定。

后面 `prvBegin()` 会明确要求：

```c
pxPort->xRole == MODBUS_PORT_ROLE_CLIENT
```

Server Poll 也会明确要求：

```c
pxPort->xRole == MODBUS_PORT_ROLE_SERVER
```

因此这其实属于对象状态的一部分。

------

# 三、`ModbusPortResult_e` 是这一层非常重要的设计

nanoMODBUS 本来已经有：

```c
nmbs_error
```

Transport 也已经有：

```c
TransportResult_e
```

为什么这里还要再搞：

```c
ModbusPortResult_e
```

因为上层不应该同时理解三套错误语义。

这里建立的是一个**工程稳定错误域**：

```c
MODBUS_PORT_RESULT_OK
MODBUS_PORT_RESULT_INVALID_ARG
MODBUS_PORT_RESULT_NOT_READY
MODBUS_PORT_RESULT_BUSY
MODBUS_PORT_RESULT_TIMEOUT
MODBUS_PORT_RESULT_TRANSPORT
MODBUS_PORT_RESULT_PROTOCOL
MODBUS_PORT_RESULT_EXCEPTION
MODBUS_PORT_RESULT_NOT_SUPPORTED
MODBUS_PORT_RESULT_CANCELED
```



于是 DeviceLibrary 可以只理解：

```text
成功
参数错
没准备好
忙
超时
传输故障
协议故障
Modbus异常
不支持
取消
```

不用知道下面到底发生的是：

```text
NMBS_ERROR_CRC
NMBS_ERROR_INVALID_TCP_MBAP
TRANSPORT_RESULT_NOT_OPEN
HAL_TIMEOUT
ERR_RST
```

这是一种非常重要的分层思想：

```text
                   上层稳定错误语义
                          ↑
                 ModbusPortResult_e
                          ↑
             ┌────────────┴───────────┐
             │                        │
         nmbs_error            TransportResult_e
             │                        │
        Modbus协议层               IO层
```

因此 `ModbusPort` 不只是 Adapter，它还承担了一个 **error translation / anti-corruption boundary**。

------

# 四、为什么还有 `ModbusPortFault_t`

如果只留下统一错误：

```c
MODBUS_PORT_RESULT_TRANSPORT
```

调试的时候信息又太少了。

所以这里采用了一个很漂亮的双层模型：

```text
程序判断
    ↓
ModbusPortResult_e

工程诊断
    ↓
ModbusPortFault_t
```

结构是：

```c
typedef struct {
    ModbusPortResult_e xResult;
    TransportResult_e xTransportResult;
    int32_t lProtocolCode;
    int32_t lNativeError;
    uint8_t ucExceptionCode;
} ModbusPortFault_t;
```



相当于保留：

```text
工程层
xResult

Transport层
xTransportResult

nanoMODBUS层
lProtocolCode

HAL/LwIP/Socket层
lNativeError

Modbus标准异常
ucExceptionCode
```

例如一次操作最后上层拿到：

```c
MODBUS_PORT_RESULT_TIMEOUT
```

进一步诊断可能看到：

```text
xResult           = TIMEOUT
xTransportResult  = TRANSPORT_RESULT_TIMEOUT
lProtocolCode     = NMBS_ERROR_TIMEOUT
lNativeError      = 某个 HAL/LwIP 原始码
```

所以：

> **错误给程序看的，要稳定；错误给人排查的，要详细。**

这块设计是很合理的。

------

# 五、Trace 不是简单“保存一个 buffer”

结构：

```c
typedef struct {
    uint32_t ulSequence;
    uint16_t usLength;
    uint16_t usCapturedLength;
    uint8_t aucData[MODBUS_PORT_TRACE_LENGTH];
} ModbusPortFrame_t;
```

关键是同时保存：

```text
usLength
        实际观察到了多少字节

usCapturedLength
        实际保存了多少字节
```



这样即使：

```text
实际帧比诊断缓冲区大
```

也不会因为截断而错误地认为：

```text
原帧只有 capturedLength 那么长
```

你当前配置的：

```c
MODBUS_PORT_TRACE_LENGTH = 260
```

又刚好覆盖标准 Modbus TCP 最大 ADU 尺度。

Trace 里面还有：

```c
ucTxSucceeded;
ucRxSucceeded;
```

所以能区别：

```text
“我捕获到了准备发送的数据”
```

和：

```text
“这帧确实完整发送成功”
```

这两个概念。

------

# 六、然后来到整层最核心的 `ModbusPort_t`

```c
typedef struct {
    nmbs_t xNmbs;
    nmbs_bitfield aucBitfield;

    TransportChannel_t *pxChannel;
    ModbusPortTrace_t *pxTrace;

    ModbusPortFault_t xLastFault;

    TickType_t xOperationStart;
    TickType_t xOperationBudget;

    uint32_t ulByteTimeoutMs;
    uint32_t ulTraceSequence;

    ModbusPortTransport_e xTransport;
    ModbusPortRole_e xRole;

    TransportResult_e xLastTransportResult;

    uint8_t ucOperationActive;
    uint8_t ucInitialized;
} ModbusPort_t;
```



这次不要把它看作一堆变量，要按职责分：

```text
ModbusPort_t

协议对象
    xNmbs

协议适配临时区
    aucBitfield

向下绑定
    pxChannel

诊断
    pxTrace
    xLastFault
    ulTraceSequence
    xLastTransportResult

事务 Deadline
    xOperationStart
    xOperationBudget
    ucOperationActive

静态配置
    ulByteTimeoutMs
    xTransport
    xRole

生命周期
    ucInitialized
```

所以它其实是：

> **一个完整 Modbus Endpoint 的工程上下文。**

而不是单纯封了一下 nanoMODBUS API。

------

# 七、这里和我们刚才讲的 C++ 思想正好接上

前面说过 nanoMODBUS：

```c
read + write + flush + void *arg
```

很像：

```text
虚函数表 + this
```

现在看看 `prvInit()`：

```c
nmbs_platform_conf_create(pxPlatform);

pxPlatform->read  = prvRead;
pxPlatform->write = prvWrite;
pxPlatform->flush = prvFlush;

pxPlatform->arg = pxPort;
```



这就彻底串起来了。

nanoMODBUS 之后调用：

```c
platform.read(..., platform.arg);
```

实际上等于：

```c
prvRead(..., pxPort);
```

然后：

```c
pxPort = (ModbusPort_t *)pvArgument;
```

所以：

```text
platform.arg
    ↓
ModbusPort_t*
    ↓
类似 C++ this
```

而：

```c
prvRead()
prvWrite()
prvFlush()
```

就是这个“对象”的三个实现方法。

如果把这块强行翻译成 C++，大概就是：

```cpp
class ModbusPort : public NanoModbusPlatform {
public:
    int read(...) override;
    int write(...) override;
    void flush() override;

private:
    TransportChannel* channel;
};
```

而现在 C 版本就是手动实现这一切。

------

# 八、`xModbusPortClientInit()`：实际上相当于构造一个 Client 对象

代码的三个步骤写得很清楚：

```c
prvInit(... ROLE_CLIENT ..., &xPlatform);

nmbs_client_create(
    &pxPort->xNmbs,
    &xPlatform);

pxPort->ucInitialized = 1U;
```



这里有一个很值得注意的生命周期问题。

`xPlatform` 是：

```c
nmbs_platform_conf xPlatform;
```

局部栈变量。

函数退出就没了。

为什么安全？

因为我们上一轮已经看到 nanoMODBUS：

```c
nmbs->platform = *platform_conf;
```

是**复制整个结构体**，不是保存 `platform_conf` 指针。

所以：

```text
xPlatform 临时变量
        ↓ copy
pxPort->xNmbs.platform
```

初始化之后原变量销毁没有问题。

------

# 九、`prvInit()` 是真正的 ModbusPort 构造器

开头先严格验证：

```c
pxPort != NULL
pxChannel != NULL
pxPlatform != NULL
byteTimeout != 0
byteTimeout <= MAX
transport == RTU || TCP
```

随后：

```c
memset(pxPort, 0, sizeof(*pxPort));
```

然后建立完整的两方向连接：

```text
nanoMODBUS
      │
      │ arg = pxPort
      ▼
ModbusPort
      │
      │ pxChannel
      ▼
Transport
```

源码：

```c
pxPlatform->arg = pxPort;

pxPort->pxChannel = pxChannel;
```



这两句是整个文件最值得记住的两句之一。

它们把三个层级连接起来了。

------

# 十、但是这里第二次细读能发现一个小问题：`SetTrace` 的语义有冲突

头文件写：

```c
vModbusPortSetTrace(...)
```

可以用于：

> Initialized or zeroed port.



所以看文档，好像可以：

```c
ModbusPort_t port = {0};

vModbusPortSetTrace(&port, &trace);

xModbusPortClientInit(&port, ...);
```

但实际：

```c
prvInit()
{
    memset(pxPort, 0, sizeof(*pxPort));
    ...
}
```

会把刚刚设置的：

```c
pxPort->pxTrace
```

清掉。

所以**根据当前实现**，正确顺序应该是：

```c
xModbusPortClientInit(...);

vModbusPortSetTrace(...);
```

否则初始化会把 Trace 绑定覆盖掉。

这是这一轮细读值得记录的一个“接口注释与实现行为不完全一致”的地方。

------

# 十一、现在进入整个 `.c` 最关键的三个桥接函数

这三个一定要真正读懂：

```c
prvRead()
prvWrite()
prvFlush()
```

它们就是：

```text
nanoMODBUS世界
       ↓
   ModbusPort
       ↓
Transport世界
```

------

# 十二、`prvRead()` 最精妙的地方是“错误翻译”

核心：

```c
xResult = xTransportReceiveExact(
    pxPort->pxChannel,
    pucData,
    usCount,
    &usReceived,
    ulTimeoutMs);
```



然后：

```c
if (xResult == TRANSPORT_RESULT_OK)
    return usReceived;

if (xResult == TRANSPORT_RESULT_TIMEOUT)
    return usReceived;

return -1;
```

为什么 Transport Timeout 居然不返回 `-1`？

因为我们刚读过 nanoMODBUS 的契约：

```text
return == count   成功
0 <= return < count
                  Timeout

return < 0        Transport Error
```

所以这里故意这样转换：

```text
Transport OK
   ↓
返回完整字节数
   ↓
nanoMODBUS SUCCESS


Transport TIMEOUT
   ↓
返回“已经收到的部分字节数”
   ↓
小于 count
   ↓
nanoMODBUS NMBS_ERROR_TIMEOUT


其他 Transport 错误
   ↓
返回 -1
   ↓
nanoMODBUS NMBS_ERROR_TRANSPORT
```

这一段不是普通 wrapper。

它是在**严格实现 nanoMODBUS 的 platform contract**。

------

# 十三、`prvRead()` 同时承担 Trace

Transport 返回：

```c
usReceived
```

以后立即：

```c
prvAppendFrame(
    &pxTrace->xLastRx,
    pucData,
    usReceived);
```



所以哪怕：

```text
应该收 20 bytes
实际只收到 7 bytes
然后 timeout
```

Trace 也能留下那 7 个字节。

这对于查：

```text
CRC错
半包
设备只回了一半
UART DMA截断
```

特别有用。

------

# 十四、`prvWrite()` 同样是 nanoMODBUS → Transport 的翻译器

核心：

```c
xResult = xTransportSend(
    pxPort->pxChannel,
    pucData,
    usCount,
    ulTimeoutMs);
```

然后：

```text
Transport OK
   → return usCount

Transport TIMEOUT
   → return 0

其他错误
   → return -1
```



nanoMODBUS 看到：

```text
0 < count
```

自然会把它判断成：

```c
NMBS_ERROR_TIMEOUT
```

所以这里再次完成错误语义适配。

------

# 十五、`prvWrite()` 开头那个判断很有门道

```c
if (pxPort->xLastTransportResult != TRANSPORT_RESULT_OK) {
    return -1;
}
```



一开始可能会疑惑：

> 新的一次发送为什么要管“上一次 Transport 状态”？

答案和 nanoMODBUS 的调用顺序有关。

我们前面看到 nanoMODBUS Client 每次请求：

```text
msg_state_req()
    ↓
platform.flush()
    ↓
构造请求
    ↓
platform.write()
```

现在你自定义的：

```c
platform.flush = prvFlush;
```

如果：

```c
prvFlush()
```

失败了，它会：

```c
pxPort->xLastTransportResult = xResult;
```

然后走到：

```c
prvWrite()
```

就发现：

```text
Transport已经不正常
```

直接拒绝发送。

所以这里形成：

```text
prvBegin()
    ↓
xLastTransportResult = OK
    ↓
nanoMODBUS msg_state_req()
    ↓
prvFlush()
    │
    ├─成功 → 状态仍然OK
    │          ↓
    │      prvWrite() 真发送
    │
    └─失败 → 保存错误
               ↓
           prvWrite()
               ↓
           直接 -1
```

这个细节设计得挺漂亮。

------

# 十六、为什么 `prvFlush()` 只处理 RTU

```c
if ((pxPort == NULL) ||
    (pxPort->xTransport != MODBUS_PORT_TRANSPORT_RTU)) {
    return;
}
```

真正 RTU 清缓存：

```c
xTransportControl(
    pxPort->pxChannel,
    TRANSPORT_CTRL_RX_FLUSH,
    NULL);
```



这和 nanoMODBUS 默认 flush 有明显区别。

nanoMODBUS 默认实现是：

```text
非阻塞 read 一次，把残留数据消费掉
```

而你的 Port 明确变成：

```text
RTU
 ↓
调用 Transport 正式 RX_FLUSH 控制接口

TCP
 ↓
什么都不做
```

从代码职责上看，这说明工程不希望 nanoMODBUS 自己通过“偷读数据”来清底层，而是让 Transport 自己知道：

> 我的 UART/DMA/ring buffer 到底应该怎么清。

这是非常合理的抽象。

------

# 十七、接下来是本文件的灵魂：`prvBegin()`

几乎每个 Client API 都是：

```c
xResult = prvBegin(...);

if (xResult != OK)
    return xResult;

xError = nmbs_xxx(...);

return prvFinish(...);
```

所以可以把一次 Modbus 操作抽象成：

```text
Begin
 ↓
真正协议操作
 ↓
Finish
```

这就已经很接近一个 Transaction Scope。

------

# 十八、`prvBegin()` 第一层：检查对象是否可用

检查：

```c
pxPort != NULL
ucInitialized != 0
role == CLIENT
timeout != 0
timeout <= MAX
```



这里有个值得注意的地方：

虽然 `ModbusPortResult_e` 有：

```c
MODBUS_PORT_RESULT_NOT_READY
```

但：

```c
ucInitialized == 0
```

这里返回的是：

```c
MODBUS_PORT_RESULT_INVALID_ARG
```

不是 `NOT_READY`。

也就是说当前实现里：

```text
NOT_READY
```

主要是拿来映射底层 Transport 的：

```text
NOT_OPEN / NOT_READY
```

而不是表示 ModbusPort 没初始化。

------

# 十九、Client 事务开始前先做 RTU 帧间静默

```c
prvWaitFrameSilence(pxPort);
```

而且这一句发生在：

```c
xOperationStart = xTaskGetTickCount();
```

之前。

这一点很关键：

> **RTU frame-silence 等待不计入调用者给出的 `ulTimeoutMs` 总事务预算。**

所以头文件说：

```text
Total transaction timeout
```

准确来说是：

```text
RTU帧间等待
+
ulTimeoutMs事务预算
```

这是一个有意识或无意识的行为差异。

如果上层要求非常严格的 wall-clock deadline，这里以后值得再评估。

------

# 二十、`prvWaitFrameSilence()` 做的是什么

它只对 RTU 生效。

先：

```c
xTransportControl(
    channel,
    TRANSPORT_CTRL_GET_BAUD_RATE,
    &ulBaudRate);
```

失败就：

```c
ulBaudRate = 9600;
```

然后假定：

```text
8N1
= 1 start + 8 data + 1 stop
= 10 bit / character
```

因此：

```c
ulSilenceUs = 35000000UL / ulBaudRate;
```

也就是：

```text
3.5 character × 10 bit × 1,000,000 / baud
```

最后向上取整到毫秒，并：

```c
vTaskDelay(...)
```



所以这里不是忙等：

```c
while(...)
```

而是把 CPU 让出去。

这符合 RTOS 体系。

不过代码本身明确建立了一个前提：

```text
串口 = 8N1
```

如果未来某个 RTU 设备使用：

```text
8E1
8O1
8N2
```

字符位数就不再是固定 10 bit，这个计算需要跟着改。

------

# 二十一、总事务超时机制，是 ModbusPort 相比裸 nanoMODBUS 很重要的增强

`prvBegin()`：

```c
xOperationStart = xTaskGetTickCount();

xOperationBudget =
    prvMsToTicks(ulTimeoutMs);

ucOperationActive = 1;
```

然后同时告诉 nanoMODBUS：

```c
nmbs_set_read_timeout(
    &xNmbs,
    ulTimeoutMs);

nmbs_set_byte_timeout(
    &xNmbs,
    ulByteTimeoutMs);
```



这里实际上有三层时间概念：

```text
总事务预算
ulTimeoutMs

第一响应字节等待
nano read_timeout

后续单阶段/字节等待
ulByteTimeoutMs
```

但 ModbusPort 又做了一层限制。

------

# 二十二、`prvGetEffectiveTimeout()` 是理解 Deadline 的核心

每一次：

```c
prvRead()
prvWrite()
```

真正进入 Transport 之前都会调用：

```c
prvGetEffectiveTimeout(
    pxPort,
    lTimeoutMs);
```

它先计算：

```c
remaining = transaction_budget - elapsed;
```

然后：

```c
effective_timeout =
    min(nanoMODBUS_requested_timeout,
        remaining_transaction_timeout);
```

如果 nanoMODBUS 请求：

```c
-1
```

也就是无限等待：

```c
return remaining;
```



所以裸 nanoMODBUS 原本可能是：

```text
第1次 read 等 100ms
第2次 read 再等 100ms
第3次 read 再等 100ms
...
```

总时间可能不断累加。

经过 ModbusPort 后：

```text
总预算 = 200ms

第一次已经花 80ms
剩 120ms

下一阶段想等 100ms
→ 可以给100ms

又花了90ms
剩30ms

下一阶段想等100ms
→ 实际只给30ms
```

因此：

> **局部 timeout 永远不能突破整个 Transaction Deadline。**

这就是 `xOperationStart / xOperationBudget` 存在的真正原因。

------

# 二十三、再把上一轮 nanoMODBUS 的 timeout 知识接起来

之前我们知道 nanoMODBUS 收响应时：

```text
等待第一字节
        ↓
read_timeout

第一字节收到以后
        ↓
byte_timeout
```

那么经过 ModbusPort，真实情况变成：

```text
                    整个事务剩余时间
                           │
               ┌───────────┴───────────┐
               │                       │
         nano read_timeout      nano byte_timeout
               │                       │
               └───────────┬───────────┘
                           ▼
                  prvGetEffectiveTimeout
                           │
                           ▼
                min(请求时间, 剩余预算)
                           │
                           ▼
                       Transport
```

这就是为什么 ModbusPort 不只是“转发 nanoMODBUS”。

它加入了项目自己的事务控制语义。

------

# 二十四、FreeRTOS Tick 转换也是刻意做成向上取整

```c
static TickType_t prvMsToTicks(...)
{
    xTicks = pdMS_TO_TICKS(...);

    return (xTicks == 0U) ? 1U : xTicks;
}
```

意味着：

```text
任何正 timeout
至少得到 1 Tick
```

不会因为整数转换：

```text
很短的毫秒数 → 0 Tick
```

然后瞬间超时。

反方向：

```c
prvTicksToMsCeil()
```

也通过：

```c
ticks * 1000 + tickRate - 1
```

做向上取整，而且用：

```c
uint64_t
```

避免中间乘法溢出。

这是嵌入式时间处理里比较严谨的写法。

------

# 二十五、`prvFinish()` 是整个事务的统一收口

```c
pxPort->ucOperationActive = 0U;

xResult = prvMapError(pxPort, xError);

...

prvUpdateFaultDetail(...);

return xResult;
```



所以所有 Client API 都不用各自处理：

```text
nanoMODBUS错误转换
Trace状态
Fault诊断
Deadline结束
```

统一在 Finish 做。

这种：

```text
Begin → Operation → Finish
```

结构非常值得保留。

------

# 二十六、`prvMapError()` 是三套错误域真正汇合的位置

先直接处理 nanoMODBUS：

```c
NMBS_ERROR_NONE
    → OK

Modbus Exception
    → EXCEPTION

NMBS_ERROR_TIMEOUT
    → TIMEOUT

NMBS_ERROR_INVALID_ARGUMENT
    → INVALID_ARG
```

然后遇到：

```c
NMBS_ERROR_TRANSPORT
```

才继续看：

```c
pxPort->xLastTransportResult
```

例如：

```text
TRANSPORT_RESULT_TIMEOUT
    → MODBUS_PORT_RESULT_TIMEOUT

TRANSPORT_RESULT_BUSY
    → MODBUS_PORT_RESULT_BUSY

NOT_OPEN / NOT_READY
    → MODBUS_PORT_RESULT_NOT_READY

NOT_SUPPORTED
    → MODBUS_PORT_RESULT_NOT_SUPPORTED
```

其他 nanoMODBUS 错误统一：

```c
MODBUS_PORT_RESULT_PROTOCOL
```



因此像：

```text
CRC错误
INVALID_UNIT_ID
INVALID_TCP_MBAP
INVALID_RESPONSE
```

全部被归入：

```c
MODBUS_PORT_RESULT_PROTOCOL
```

这正是“对上层稳定，对下层保留细节”。

------

# 二十七、这里还有两个第二次细读才容易发现的地方

`MODBUS_PORT_RESULT_CANCELED` 虽然在 public enum 里：

```c
MODBUS_PORT_RESULT_CANCELED = -9
```

但当前 `modbus_port.c` 的：

```c
prvMapError()
```

没有任何路径产生它。

所以从当前源码证据看，它更像是：

```text
为上层 workflow / future cancellation 预留的结果
```

而不是现在 ModbusPort 自己真的支持事务取消。

另一个是：

```c
MODBUS_PORT_RESULT_BUSY
```

并不意味着 ModbusPort 自己有互斥锁。

它只是底层 Transport 返回 BUSY 后映射出来。

------

# 二十八、这也解释了头文件那个 Warning

```c
@warning One owner may execute a transaction
         on this object at a time.
```



当前代码没有：

```c
SemaphoreTake()
Mutex
atomic lock
if (ucOperationActive) return BUSY;
```

甚至 `prvBegin()` 都没有：

```c
if (pxPort->ucOperationActive)
```

检查。

所以同一个：

```c
ModbusPort_t
```

如果两个 Task 同时调用：

```c
xModbusPortReadHolding()
```

状态很可能互相覆盖：

```text
xNmbs.msg
xOperationStart
xOperationBudget
aucBitfield
Trace
xLastTransportResult
```

因此：

> **单 owner 是调用契约，不是 ModbusPort 内部强制保证。**

这点以后 DeviceLibrary/Bus Task 的所有权设计必须遵守。

------

# 二十九、Trace 的实现也值得认真看

每次事务：

```c
prvResetTrace(pxPort);
```

执行：

```c
memset(trace, 0, ...);

ulTraceSequence++;

Tx.sequence = ulTraceSequence;
Rx.sequence = ulTraceSequence;
```



所以 TX/RX 可以通过 sequence 明确知道：

```text
这是同一笔事务
```

然后 `prvAppendFrame()`：

```c
if (还有捕获空间)
    memcpy(...);

usLength += usLength;
```

并且 `usLength` 超过 `UINT16_MAX` 时饱和，而不是溢出回 0。

这个属于非常典型的诊断代码：

> **诊断逻辑本身也不能成为新的 bug 来源。**

------

# 三十、`ucRxSucceeded` 的定义也很讲究

Finish：

```c
if (Rx有数据 &&
    (result == OK ||
     result == EXCEPTION)) {

    ucRxSucceeded = 1;
}
```



为什么 Exception 也算 Rx succeeded？

因为：

```text
Modbus Exception
```

其实代表：

```text
通信成功
帧合法
CRC/MBAP合法
Unit/TID合法
FC异常格式合法
对端明确回应了错误
```

所以：

```text
事务业务失败
≠
接收失败
```

这正好呼应我们第一次读 nanoMODBUS 时说的：

```text
负数 = 本地协议/通信错误
正数 = 对端合法 Modbus Exception
```

这个状态划分是对的。

------

# 三十一、`prvUpdateFaultDetail()`：把故障一路追到底

它先记录：

```c
xResult
xLastTransportResult
xError
```

如果是 Modbus Exception：

```c
ucExceptionCode = xError;
```

如果 Transport 有错误，则：

```c
xTransportGetStatus(
    channel,
    &xStatus);
```

继续取：

```c
xStatus.xLastFault.lNativeError
```



于是诊断链最终是：

```text
Coffee2 / Device Driver
       │
       ▼
ModbusPortResult
       │
       ▼
nanoMODBUS error
       │
       ▼
TransportResult
       │
       ▼
HAL / LwIP / Socket native error
```

这一块已经体现出比较成熟的嵌入式分层思路了。

------

# 三十二、为什么 Coil API 比寄存器 API 多了一层转换

比如：

```c
xModbusPortReadCoils(...)
```

上层 API 给的是：

```c
bool *pbValues
```

但是 nanoMODBUS 要：

```c
nmbs_bitfield
```

所以：

```c
nmbs_bitfield_reset(pxPort->aucBitfield);

nmbs_read_coils(
    ...,
    pxPort->aucBitfield);

for (...) {
    pbValues[i] =
        nmbs_bitfield_read(
            pxPort->aucBitfield,
            i);
}
```



写 Coil 反过来：

```text
bool[]
  ↓
aucBitfield
  ↓
nanoMODBUS
```



这说明 `aucBitfield`：

```c
nmbs_bitfield aucBitfield;
```

不是业务状态。

只是一个：

> **ModbusPort 内部格式适配 scratch buffer。**

所以它放在 `ModbusPort_t` 里，而不暴露给 DeviceLibrary，非常合理。

------

# 三十三、寄存器 API 为什么显得特别“薄”

例如 FC03：

```c
if (pusValues == NULL)
    return INVALID_ARG;

xResult = prvBegin(...);

xError =
    nmbs_read_holding_registers(
        &pxPort->xNmbs,
        usAddress,
        usQuantity,
        pusValues);

return prvFinish(...);
```



注意 ModbusPort 没有重新检查：

```text
quantity <= 125
address + quantity <= 65536
```

因为 nanoMODBUS 自己已经检查了。

这体现了一种不错的原则：

> **Port 只检查自己必须保证的参数，不重复实现协议库已经负责的协议规则。**

否则两套校验规则以后很容易漂移。

------

# 三十四、Server 方向也使用同一个 Deadline 框架

`xModbusPortServerPoll()`：

```c
xOperationStart = xTaskGetTickCount();
xOperationBudget = prvMsToTicks(ulPollTimeoutMs);
ucOperationActive = 1;

nmbs_set_read_timeout(...);
nmbs_set_byte_timeout(...);

xError = nmbs_server_poll(...);

return prvFinish(...);
```



所以 Client：

```text
一次 request-response
```

有预算。

Server：

```text
一次 poll / 最多处理一个 request
```

同样有预算。

区别只是 Server 不需要：

```c
prvWaitFrameSilence()
```

因为它不是主动开始一笔 Client RTU 请求。

------

# 三十五、Server callbacks 的生命周期注释还有一个值得修正的地方

`modbus_port.h` 写：

```text
Persistent server data-model callbacks
```



但我们上一轮已经读到：

```c
nmbs->callbacks = *callbacks;
```

也就是 nanoMODBUS 创建 Server 时**复制 callback 结构体**。

所以严格来说：

```c
nmbs_callbacks callbacks;
xModbusPortServerInit(..., &callbacks, ...);
```

调用结束之后，`callbacks` 这个结构体本身不需要继续存在。

真正需要长期有效的是它里面的：

```c
callbacks.arg
```

所指向的业务上下文，以及函数指针指向的代码。

因此这里的 `"Persistent callbacks"` 注释比实际实现要求更严格，可以考虑以后改准确。

------

# 三十六、RawRequest 是这层故意留下的“逃生口”

```c
xModbusPortRawRequest(...)
```

调用：

```c
nmbs_send_raw_pdu(...);

nmbs_receive_raw_pdu_response(...);
```



它意味着：

> 即使未来设备用了 nanoMODBUS 没有封装的私有 FC，也不用绕开整个 ModbusPort。

你仍然可以享受：

```text
Transport适配
Deadline
RTU/TCP封装
CRC
错误映射
Trace
Fault
```

只是：

```text
PDU Data的字节序
功能码特有的参数校验
Response Data语义
```

由调用者自己负责。头文件也明确 warning 了这一点。

这其实是一个很好的扩展点。

------

# 三十七、RawRequest 对 Broadcast 还专门处理了一次

代码：

```c
if ((xError == NMBS_ERROR_NONE) &&
    !((xTransport == RTU) &&
      (ucUnitId == NMBS_BROADCAST_ADDRESS))) {

    nmbs_receive_raw_pdu_response(...);
}
```



因为正常 nanoMODBUS 的：

```c
nmbs_write_single_register()
```

自己知道 Broadcast 不应该收响应。

但 RawRequest 是：

```text
Send Raw
Receive Raw
```

分开的两个 API。

所以 ModbusPort 必须自己补上：

```text
RTU Broadcast → 不调用 Receive
```

这属于 wrapper 必须承担的语义。

------

# 三十八、现在用 FC03 把两次细读彻底串起来

假设 DeviceLibrary 调：

```c
xModbusPortReadHolding(
    &port,
    3,       // Unit ID
    100,     // Address
    2,       // Quantity
    regs,
    500);    // 总事务 timeout
```

完整调用链实际上是：

```text
xModbusPortReadHolding()
        │
        ▼
    prvBegin()
        │
        ├── 检查 initialized / client / timeout
        │
        ├── prvWaitFrameSilence()
        │       └── Transport GET_BAUD_RATE
        │
        ├── 建立 500ms 总 Deadline
        ├── nmbs read_timeout = 500ms
        ├── nmbs byte_timeout = ulByteTimeoutMs
        ├── destination RTU address = 3
        └── reset Trace
        │
        ▼
nmbs_read_holding_registers()
        │
        ▼
nanoMODBUS read_registers(fc=3)
        │
        ▼
msg_state_req()
        │
        ▼
platform.flush()
        │
        ▼
    prvFlush()
        │
        ▼
Transport RX_FLUSH
        │
        ▼
nanoMODBUS 构造
03 + address + quantity
        │
        ▼
send_msg()
        │
        ▼
platform.write()
        │
        ▼
    prvWrite()
        │
        ├── 计算剩余 Deadline
        ├── Trace TX
        │
        ▼
xTransportSend()
        │
        ▼
       UART
        │
        │  从站响应
        ▼
nanoMODBUS recv_res_header()
        │
        ▼
platform.read()
        │
        ▼
    prvRead()
        │
        ├── 计算剩余 Deadline
        │
        ▼
xTransportReceiveExact()
        │
        ├── RX Trace
        ▼
nanoMODBUS
解析 Unit / FC / ByteCount /
Registers / CRC
        │
        ▼
   nmbs_error
        │
        ▼
   prvFinish()
        │
        ├── prvMapError()
        ├── 更新 RxSucceeded
        ├── UpdateFaultDetail
        └── OperationActive = 0
        │
        ▼
ModbusPortResult_e
        │
        ▼
   DeviceLibrary
```

现在这条链应该已经完全能和上一轮的 nanoMODBUS 源码对上了。

------

# 三十九、这一次读完后，我会把 ModbusPort 定义成四个角色

| 角色                       | ModbusPort 实际承担的职责                             |
| -------------------------- | ----------------------------------------------------- |
| **Adapter**                | 把 nanoMODBUS `read/write/flush` 适配到 `Transport`   |
| **Facade**                 | 给 DeviceLibrary 提供统一 `xModbusPortXXX()` API      |
| **Transaction Controller** | 给裸 nanoMODBUS 增加总事务 Deadline、RTU 帧间静默     |
| **Diagnostic Boundary**    | 统一错误域，同时保留 Trace / Transport / native fault |

这比“Modbus封装层”这个说法准确得多。

而它**不应该**承担：

```text
设备型号语义
某寄存器代表什么
某命令代表出糖还是出杯
Coffee2 route
设备 ID
订单
工作流
业务状态
物理总线选型
```

这些都应该继续留在它上面的 DeviceLibrary / UserAPP。

------

# 四十、这一轮细读后值得记下的几个源码观察

这是我认为后续继续往上读之前最值得做标记的一组：

1. `ModbusPort_t` **没有内部 Mutex**，单 owner 是调用契约；`BUSY` 主要来自 Transport，而不是防止两个 Task 同时调用同一 Port。
2. `prvBegin()` 的 RTU `prvWaitFrameSilence()` 发生在 Deadline 启动之前，所以实际 API wall-clock 时间可以比 `ulTimeoutMs` 多一个帧间等待。
3. RTU silence 当前固定按 **8N1 = 10 bit/char** 计算；其他串口格式不适用。
4. `vModbusPortSetTrace()` 文档说可以对 zeroed port 使用，但 `prvInit()` 会 `memset(pxPort)`，因此**初始化前绑定 Trace 会丢失**。
5. `MODBUS_PORT_RESULT_CANCELED` 当前源码没有产生路径，是预留语义。
6. Server 的 `pxCallbacks` 注释写成 Persistent，但 nanoMODBUS 实际复制 callback struct；真正需要持续有效的是 callback 指向的上下文。
7. `prvRead()` 返回“部分收到字节数”而不是直接返回错误，是为了让 nanoMODBUS 正确区分 **Timeout** 与 **Transport Error**。这是整个 Adapter 最关键的契约之一。

前六个里有几处可以以后决定是否调整；第七个则属于**千万不要看着奇怪就随手改掉的代码**。

到这里，`ModbusPort` 这一层基本就可以算真正读透了。下一层如果继续按架构自底向上，应该进入 **`Transport` 层**：先读 `transport.h` 的抽象接口和 `TransportChannel_t`，然后读 `transport.c` 的注册、send/receive/control，再分别下钻 UART/TCP backend。这样你会看到现在 `prvRead → xTransportReceiveExact` 后面的世界到底是怎么组织的。