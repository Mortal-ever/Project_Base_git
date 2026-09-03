# nanoMODBUS 工作原理详解（含解耦架构分析）

---

## 一、nanoMODBUS 定位与设计哲学

**nanoMODBUS** 是一个为 MCU 设计的极简 Modbus 协议库。它的核心设计哲学是：

> **"我不管数据从哪来、到哪去，我只需要你给我提供两个函数：一个能读字节，一个能写字节。"**

nanoMODBUS 本身**完全不了解**底层硬件——不知道是 UART、TCP Socket 还是 Netconn，不知道波特率、DMA、中断。它只通过一组**函数指针回调**与外界交互，这就是它实现解耦的根本方式。

**本工程中的封装层级**：

```
┌─────────────────────────────────────────────────────────────┐
│ 应用层 (Task / WorkFlow / DeviceProtocol)                    │
│     调用: xModbusPortReadHolding() 等                       │
└───────────────────┬─────────────────────────────────────────┘
                    ▼
┌─────────────────────────────────────────────────────────────┐
│ ModbusPort 适配层 (modbus_port.c)                            │
│     提供: prvRead() / prvWrite() / prvFlush() 回调           │
│     内部: 调用 Transport API                                 │
└───────────────────┬─────────────────────────────────────────┘
                    ▼
┌─────────────────────────────────────────────────────────────┐
│ nanoMODBUS 库 (nanomodbus.c / nanomodbus.h)                  │
│     职责: 帧封装、帧解析、CRC校验、状态机、超时              │
│     对外: nmbs_platform_conf(回调) + nmbs_t(实例)             │
│     关联: 不关心底层，只调 platform.read/write/crc/flush     │
└───────────────────┬─────────────────────────────────────────┘
                    ▼
┌─────────────────────────────────────────────────────────────┐
│ Transport 层 (transport.c → 后端)                            │
│     xTransportSend() / xTransportReceiveExact()              │
│     pxOps->xSend() / pxOps->xReceive() (函数指针多态)        │
└───────────────────┬─────────────────────────────────────────┘
                    ▼
┌─────────────────────────────────────────────────────────────┐
│ TCP 后端 | UART 后端                                         │
│ netconn_write_partly / netconn_recv  | DMA / 轮询 / 中断     │
└─────────────────────────────────────────────────────────────┘
```

---

## 二、核心数据结构：解耦的基石

### 2.1 `nmbs_platform_conf` — 平台回调配置（nanomodbus.h:163-174）

这是**整个解耦机制的核心**。它是 nanoMODBUS 与底层之间的唯一接口。

```c
typedef struct nmbs_platform_conf {
    nmbs_transport transport;   // 传输类型: RTU=1 / TCP=2
    int32_t (*read)(uint8_t* buf, uint16_t count, int32_t byte_timeout_ms, void* arg);
    int32_t (*write)(const uint8_t* buf, uint16_t count, int32_t byte_timeout_ms, void* arg);
    uint16_t (*crc_calc)(const uint8_t* data, uint32_t length, void* arg);  // 可选
    void (*flush)(nmbs_t* nmbs, void* arg);                                 // 可选
    void* arg;              // 用户数据指针，会传给上述所有回调
    uint32_t initialized;   // 魔数校验位 (0xFFFFDEBE)
} nmbs_platform_conf;
```

**关键约定（头文件注释）**：
- `read()` / `write()` 应阻塞，直到读/写完 `count` 字节，或 `byte_timeout_ms >= 0` 超时
- `byte_timeout_ms < 0` 表示无限超时
- `byte_timeout_ms == 0` 表示非阻塞，读/写一次立即返回
- 返回值 = 实际读/写的字节数；`< 0` 表示传输错误
- 返回值在 `0` 到 `count-1` 之间 = 视为超时
- `arg` 是任意用户数据，nanoMODBUS 原样传给回调

### 2.2 `nmbs_t` — 实例结构体（nanomodbus.h:250-273）

```c
struct nmbs_t {
    struct {
        uint8_t buf[260];        // 收发帧缓冲区（TCP最大: 4 MBAP + 1 UnitID + 1 FC + 254 数据 = 260）
        uint16_t buf_idx;        // 当前缓冲区读写游标
        uint8_t unit_id;         // 当前帧的目标单元ID
        uint8_t fc;              // 当前帧的功能码
        uint16_t transaction_id; // TCP 事务ID（匹配请求/响应）
        bool broadcast;          // 是否广播帧
        bool ignored;            // 该帧是否匹配本机地址
        bool complete;           // TCP 头是否已完整接收
    } msg;

    nmbs_callbacks callbacks;        // 仅 Server 模式使用
    int32_t byte_timeout_ms;         // 字节间隔超时（调用 read/write 时的超时）
    int32_t read_timeout_ms;         // 读超时（首发数据的等待超时）
    nmbs_platform_conf platform;     // ← 平台回调（解耦的关键）
    uint8_t address_rtu;             // 本机 RTU 地址（Server 用）
    uint8_t dest_address_rtu;        // 目标 RTU 地址（Client 用）
    uint16_t current_tid;            // TCP 事务ID计数器
};
```

---

## 三、初始化流程：回调注入

### 3.1 `nmbs_platform_conf_create()`（nanomodbus.c:266-272）

```c
void nmbs_platform_conf_create(nmbs_platform_conf* platform_conf) {
    memset(platform_conf, 0, sizeof(nmbs_platform_conf));
    platform_conf->crc_calc = nmbs_crc_calc;   // 设置默认 CRC 计算
    platform_conf->flush = flush;              // 设置默认 flush
    platform_conf->initialized = 0xFFFFDEBE;   // 设置魔数
}
```

**要点**：调用者必须调用此函数初始化配置，`initialized` 魔数用于后续校验调用者是否使用了旧的未初始化代码。

### 3.2 `nmbs_create()`（nanomodbus.c:232-253）

```c
nmbs_error nmbs_create(nmbs_t* nmbs, const nmbs_platform_conf* platform_conf) {
    memset(nmbs, 0, sizeof(nmbs_t));          // 清零实例

    nmbs->byte_timeout_ms = -1;               // 默认无限超时
    nmbs->read_timeout_ms = -1;

    // 校验平台配置
    if (!platform_conf || platform_conf->initialized != 0xFFFFDEBE)
        return NMBS_ERROR_INVALID_ARGUMENT;
    // 校验传输类型
    if (platform_conf->transport != NMBS_TRANSPORT_RTU &&
        platform_conf->transport != NMBS_TRANSPORT_TCP)
        return NMBS_ERROR_INVALID_ARGUMENT;
    // 校验 read/write 回调必须提供
    if (!platform_conf->read || !platform_conf->write)
        return NMBS_ERROR_INVALID_ARGUMENT;

    // ★★★★★ 关键：将平台配置（含所有函数指针）复制到实例内部 ★★★★★
    nmbs->platform = *platform_conf;

    return NMBS_ERROR_NONE;
}
```

**解耦第一步**：`nmbs->platform = *platform_conf` 把回调函数指针"注射"进实例。此后 nanoMODBUS 所有收发都通过 `nmbs->platform.read` 和 `nmbs->platform.write` 间接调用。

### 3.3 Client 创建包装（本工程）

```c
// modbus_port.c:68
xError = nmbs_client_create(&pxPort->xNmbs, &xPlatform);
```

内部就是调用 `nmbs_create()`。初始化完成后，`xPlatform` 栈上变量即可丢弃，因为回调地址已复制进 `pxPort->xNmbs.platform`。

---

## 四、数据收发核心：recv() / send() 与平台回调

这是 nanoMODBUS 内部所有收发动作的**唯一出口**，也是解耦机制的铁证。

### 4.1 `send()`（nanomodbus.c:173-187）

```c
static nmbs_error send(const nmbs_t* nmbs, uint16_t count) {
    // ★ 直接调用外部注入的回调 write
    const int32_t ret = nmbs->platform.write(nmbs->msg.buf, count,
        nmbs->byte_timeout_ms, nmbs->platform.arg);

    if (ret == count)
        return NMBS_ERROR_NONE;             // 完全发送
    if (ret < count) {
        if (ret < 0) return NMBS_ERROR_TRANSPORT;  // 负数=传输错误
        return NMBS_ERROR_TIMEOUT;          // 0~count-1 = 超时
    }
    return NMBS_ERROR_TRANSPORT;            // 超过 count = 传输错误
}
```

### 4.2 `recv()`（nanomodbus.c:147-170）

```c
static nmbs_error recv(nmbs_t* nmbs, uint16_t count) {
    if (nmbs->msg.complete) return NMBS_ERROR_NONE;   // TCP 头已完整则直接返回

    // 缓冲区溢出保护
    if (nmbs->msg.buf_idx > sizeof(nmbs->msg.buf) ||
        count > sizeof(nmbs->msg.buf) - nmbs->msg.buf_idx)
        return NMBS_ERROR_INVALID_RESPONSE;

    // ★ 直接调用外部注入的回调 read
    const int32_t ret = nmbs->platform.read(nmbs->msg.buf + nmbs->msg.buf_idx,
        count, nmbs->byte_timeout_ms, nmbs->platform.arg);

    if (ret == count) return NMBS_ERROR_NONE;
    if (ret < count) {
        if (ret < 0) return NMBS_ERROR_TRANSPORT;
        return NMBS_ERROR_TIMEOUT;
    }
    return NMBS_ERROR_TRANSPORT;
}
```

### 4.3 解耦本质

**nanoMODBUS 只在两个地方碰外部世界**：
1. `nmbs->platform.write()` — 发帧
2. `nmbs->platform.read()` — 收帧
3. `nmbs->platform.crc_calc()` — CRC（RTU）
4. `nmbs->platform.flush()` — 清缓冲

**它不关心**：
- 底层是 UART 轮询、UART DMA、TCP Netconn 还是 Socket
- 波特率、数据位、校验位
- 中断、DMA 通道、RS485 方向控制

**本工程中这些回调指向**（modbus_port.c:529-609）：

| nanoMODBUS 回调     | 本工程实现                                | 内部调用                   |
| ------------------- | ----------------------------------------- | -------------------------- |
| `platform.read`     | `prvRead()` (modbus_port.c:529)           | `xTransportReceiveExact()` |
| `platform.write`    | `prvWrite()` (modbus_port.c:559)          | `xTransportSend()`         |
| `platform.flush`    | 默认 `flush()` (nanomodbus.c:190)         | 或 `prvFlush()` RTU 专用   |
| `platform.crc_calc` | 默认 `nmbs_crc_calc()` (nanomodbus.c:285) | 软件 CRC16                 |

而 `xTransportReceiveExact()` / `xTransportSend()` 又通过 `pxOps->xReceive()` / `pxOps->xSend()` 函数指针，进一步解耦到具体后端（TCP/UART）。

---

## 五、帧封装：put_msg_header() 与缓冲区操作

### 5.1 缓冲区辅助函数（nanomodbus.c:43-144）

```c
static void put_1(nmbs_t* nmbs, uint8_t data) {        // 写1字节
    nmbs->msg.buf[nmbs->msg.buf_idx] = data;
    nmbs->msg.buf_idx++;
}
static void put_2(nmbs_t* nmbs, uint16_t data) {       // 写2字节，大端序
    nmbs->msg.buf[nmbs->msg.buf_idx] = (uint8_t)((data >> 8) & 0xFFU);
    nmbs->msg.buf[nmbs->msg.buf_idx + 1] = (uint8_t)data;
    nmbs->msg.buf_idx += 2;
}
static void put_regs(nmbs_t* nmbs, const uint16_t* data, uint16_t n) {  // 写寄存器数组，翻转字节序
    uint16_t* msg_buf_ptr = (uint16_t*)(nmbs->msg.buf + nmbs->msg.buf_idx);
    nmbs->msg.buf_idx += n * 2;
    while (n--) {
        msg_buf_ptr[n] = (data[n] << 8) | ((data[n] >> 8) & 0xFF);
    }
}
```

**注意**：寄存器数据以**大端序**（网络字节序）写入缓冲区，方便 Modbus 协议传输。

### 5.2 `put_msg_header()` — 帧头封装（nanomodbus.c:393-407）

```c
static void put_msg_header(nmbs_t* nmbs, uint16_t data_length) {
    msg_buf_reset(nmbs);                       // 游标归零

    if (nmbs->platform.transport == NMBS_TRANSPORT_RTU) {
        put_1(nmbs, nmbs->msg.unit_id);        // RTU: [UnitID][FC]...
    }
    else if (nmbs->platform.transport == NMBS_TRANSPORT_TCP) {
        put_2(nmbs, nmbs->msg.transaction_id);   // TCP MBAP: [TransID 2B][ProtoID 2B][Length 2B][UnitID 1B][FC 1B]...
        put_2(nmbs, 0);                          // Protocol ID = 0 (Modbus)
        put_2(nmbs, (uint16_t)(1 + 1 + data_length)); // Length = UnitID(1) + FC(1) + Data
        put_1(nmbs, nmbs->msg.unit_id);
    }
    put_1(nmbs, nmbs->msg.fc);                 // 功能码
}
```

**TCP 帧结构验证**：
```
[Transaction ID: 2B] [Protocol ID: 2B=0] [Length: 2B] [Unit ID: 1B] [FC: 1B] [Data...]
```

### 5.3 `send_msg()` — 帧发送（nanomodbus.c:422-433）

```c
static nmbs_error send_msg(nmbs_t* nmbs) {
    if (nmbs->platform.transport == NMBS_TRANSPORT_RTU) {
        // RTU 模式：附加 CRC16
        const uint16_t crc = nmbs->platform.crc_calc(nmbs->msg.buf,
            nmbs->msg.buf_idx, nmbs->platform.arg);
        put_2(nmbs, crc);                      // 追加到帧尾
    }
    const nmbs_error err = send(nmbs, nmbs->msg.buf_idx);  // ← 调用平台 write
    return err;
}
```

---

## 六、帧解析：recv_msg_header() 与 recv_res_header()

### 6.1 `recv_msg_header()` — 收帧头（nanomodbus.c:322-390）

```c
static nmbs_error recv_msg_header(nmbs_t* nmbs, bool* first_byte_received) {
    // 第一个字节等待 read_timeout，之后字节等待 byte_timeout
    int32_t old_byte_timeout = nmbs->byte_timeout_ms;
    nmbs->byte_timeout_ms = nmbs->read_timeout_ms;
    msg_state_reset(nmbs);
    *first_byte_received = false;

    if (nmbs->platform.transport == NMBS_TRANSPORT_RTU) {
        err = recv(nmbs, 1);                   // ← 平台 read 1字节 (等 read_timeout)
        nmbs->byte_timeout_ms = old_byte_timeout;
        nmbs->msg.unit_id = get_1(nmbs);       // 解析 UnitID
        err = recv(nmbs, 1);                   // ← 平台 read 1字节 (等 byte_timeout)
        nmbs->msg.fc = get_1(nmbs);            // 解析功能码
    }
    else { // TCP
        err = recv(nmbs, 1);                   // 读第1字节
        nmbs->byte_timeout_ms = old_byte_timeout;
        discard_1(nmbs);                       // 丢弃(其实不是，这是为了强制第一个字节等 read_timeout)
        err = recv(nmbs, 7);                   // 读剩余 MBAP 头 (7字节)
        msg_buf_reset(nmbs);                   // 重置游标重新解析
        nmbs->msg.transaction_id = get_2(nmbs);
        const uint16_t protocol_id = get_2(nmbs);
        const uint16_t length = get_2(nmbs);
        nmbs->msg.unit_id = get_1(nmbs);
        nmbs->msg.fc = get_1(nmbs);

        if (length < 2 || length > 254) return NMBS_ERROR_INVALID_TCP_MBAP;
        err = recv(nmbs, length - 2);          // 读数据部分
        if (protocol_id != 0) return NMBS_ERROR_INVALID_TCP_MBAP;
        nmbs->msg.complete = true;             // 标记帧完整
    }
    return NMBS_ERROR_NONE;
}
```

**注意 TCP 的第一个字节处理技巧**：先 `recv(1)` 再 `discard_1()`，其实是"丢弃"了一个字节的游标，这样后面的 `recv(7)` 读到的才是完整的 MBAP 头。实际上这个"1"是留给 read_timeout 用的 —— 保证第一个字节等待 read_timeout，之后读 7 字节头+数据都用 byte_timeout。

### 6.2 `recv_res_header()` — 响应头校验（nanomodbus.c:478-521）

```c
static nmbs_error recv_res_header(nmbs_t* nmbs) {
    // 保存请求的信息用于匹配
    const uint16_t req_transaction_id = nmbs->msg.transaction_id;
    const uint8_t req_unit_id = nmbs->msg.unit_id;
    const uint8_t req_fc = nmbs->msg.fc;

    err = recv_msg_header(nmbs, &first_byte_received);

    // TCP: 校验事务ID一致
    if (nmbs->platform.transport == NMBS_TRANSPORT_TCP) {
        if (nmbs->msg.transaction_id != req_transaction_id)
            return NMBS_ERROR_INVALID_TCP_MBAP;
    }
    // RTU: 校验 UnitID 一致
    if (nmbs->platform.transport == NMBS_TRANSPORT_RTU &&
        nmbs->msg.unit_id != req_unit_id)
        return NMBS_ERROR_INVALID_UNIT_ID;

    // 功能码校验：正常响应 FC 相同；异常响应 FC+0x80
    if (nmbs->msg.fc != req_fc) {
        if (nmbs->msg.fc - 0x80 == req_fc) {
            err = recv(nmbs, 1);              // 读异常码
            const uint8_t exception = get_1(nmbs);
            err = recv_msg_footer(nmbs);      // RTU 校验 CRC
            if (exception < 1 || exception > 4)
                return NMBS_ERROR_INVALID_RESPONSE;
            return (nmbs_error)exception;     // ← 返回正数异常码（1~4）
        }
        return NMBS_ERROR_INVALID_RESPONSE;   // FC 不匹配且不是异常
    }
    return NMBS_ERROR_NONE;
}
```

**错误码体系设计（nanomodbus.h:58-75）**：
- `<= 0`：库内部错误（超时、传输错误、CRC 错误等）
- `> 0`（1~4）：Modbus 协议异常码（非法功能、非法地址、非法值、设备故障）

这就是为什么 `nmbs_error_is_exception(e)` 定义为 `(e) > 0 && (e) < 5`。

---

## 七、一次完整 Client 事务流程（以 FC03 读保持寄存器为例）

`nmbs_read_holding_registers()` 是 Client API 之一（nanomodbus.h:382）。其执行流程（结合 nanomodbus.c 核心函数）：

### 阶段1：构造请求

```
调用 nmbs_read_holding_registers(nmbs, addr, qty, registers_out)
    │
    ├── ① msg_state_req(nmbs, 0x03)                  (nanomodbus.c:213-229)
    │     ├── current_tid++                         ← 递增事务ID
    │     ├── platform.flush(nmbs, arg)             ← 刷新残留数据
    │     ├── msg_state_reset()                     ← 清空缓冲区状态
    │     ├── msg.unit_id = dest_address_rtu        ← 设置目标地址
    │     ├── msg.fc = 0x03                         ← 设置功能码
    │     └── msg.transaction_id = current_tid      ← 记录本次事务ID
    │
    ├── ② put_req_header(nmbs, 5)                   (nanomodbus.c:525-539)
    │     └── put_msg_header()                      ← TCP: [TID][0][Len=7][UnitID][FC]
    │                                                 RTU: [UnitID][FC]
    │
    ├── ③ put_2(nmbs, address)                     ← 写入起始地址 (2B)
    ├── ④ put_2(nmbs, quantity)                    ← 写入寄存器数量 (2B)
    │
    └── ⑤ send_msg(nmbs)                           ← RTU 附加CRC + 调用 platform.write()
    │
    │  此时 msg.buf 中:
    │   TCP:  [TID 2B][0 2B][Len=7 2B][UnitID 1B][FC=03 1B][Addr 2B][Qty 2B]  = 12B
    │   RTU:  [UnitID 1B][FC=03 1B][Addr 2B][Qty 2B][CRC 2B]                 = 8B
```

### 阶段2：接收响应

```
    ├── ⑥ recv_read_registers_res(nmbs, qty, registers_out)   (nanomodbus.c:586-618)
    │     ├── recv_res_header()
    │     │     ├── recv_msg_header()
    │     │     │     ├── 读 MBAP/UnitID+FC (TCP 一次读8B头, RTU 读2B)
    │     │     │     ├── 校验 TransactionID / UnitID
    │     │     │     └── 校验 FC 匹配（或检测异常响应 FC+0x80）
    │     │     └── 若异常 → 返回正数异常码
    │     │
    │     ├── recv(1)                             ← 读 ByteCount (1B)
    │     ├── 校验 byte_count == quantity*2
    │     ├── recv(byte_count)                    ← 读寄存器数据
    │     ├── 逐字解析 get_2() → registers_out[]  ← 大端序转主机序
    │     └── recv_msg_footer()                   ← RTU 校验CRC
    │
    │  响应帧结构:
    │   TCP:  [TID 2B][0 2B][Len 2B][UnitID 1B][FC=03 1B][ByteCnt 1B][Regs..]
    │   RTU:  [UnitID 1B][FC=03 1B][ByteCnt 1B][Regs..][CRC 2B]
    │
    └── ⑦ 返回 nmbs_error
```

### 阶段3：返回给协议层

```
    ┌── xModbusPortReadHolding() (modbus_port.c:227)
    │     ├── prvBegin()               ← 记录开始时间、设置超时预算
    │     ├── nmbs_read_holding_registers()   ← 上述完整流程
    │     └── prvFinish()             ← 映射错误码 + 记录故障详情
```

---

## 八、本工程的解耦实现：两层函数指针

### 8.1 第一层：nanoMODBUS → modbus_port（平台回调）

```c
// modbus_port.c:625-632
nmbs_platform_conf_create(pxPlatform);
pxPlatform->transport = NMBS_TRANSPORT_TCP;      // 或 RTU
pxPlatform->read  = prvRead;                     // ← 函数指针注入
pxPlatform->write = prvWrite;                    // ← 函数指针注入
pxPlatform->flush = prvFlush;                    // ← 函数指针注入
pxPlatform->arg   = pxPort;                      // ← 上下文
```

**prvRead 的实际实现**（modbus_port.c:529-556）：

```c
static int32_t prvRead(uint8_t *pucData, uint16_t usCount,
    int32_t lTimeoutMs, void *pvArgument)
{
    ModbusPort_t *pxPort = (ModbusPort_t *)pvArgument;

    // 计算剩余超时（不超过事务总超时）
    ulTimeoutMs = prvGetEffectiveTimeout(pxPort, lTimeoutMs);

    // 通过 Transport 层精确接收 usCount 字节
    xResult = xTransportReceiveExact(pxPort->pxChannel, pucData,
        usCount, &usReceived, ulTimeoutMs);

    // 帧追踪
    if (pxPort->pxTrace != NULL)
        prvAppendFrame(&pxPort->pxTrace->xLastRx, pucData, usReceived);

    if (xResult == TRANSPORT_RESULT_OK) return (int32_t)usReceived;
    if (xResult == TRANSPORT_RESULT_TIMEOUT) return (int32_t)usReceived;
    return -1;   // 错误
}
```

**prvWrite 的实际实现**（modbus_port.c:559-590）：

```c
static int32_t prvWrite(const uint8_t *pucData, uint16_t usCount,
    int32_t lTimeoutMs, void *pvArgument)
{
    // 上一次传输失败则拒绝后续写入
    if (pxPort->xLastTransportResult != TRANSPORT_RESULT_OK) return -1;

    ulTimeoutMs = prvGetEffectiveTimeout(pxPort, lTimeoutMs);

    // 帧追踪
    prvAppendFrame(&pxPort->pxTrace->xLastTx, pucData, usCount);

    // 通过 Transport 层发送
    xResult = xTransportSend(pxPort->pxChannel, pucData, usCount, ulTimeoutMs);

    if (xResult == TRANSPORT_RESULT_OK) return (int32_t)usCount;
    if (xResult == TRANSPORT_RESULT_TIMEOUT) return 0;
    return -1;
}
```

**解耦效果**：nanoMODBUS 调用 `platform.read(buf, count, timeout, arg)` 时，**它认为这是在真实地"读 count 字节"**。但实际上，这个调用经 `prvRead` → `xTransportReceiveExact` → `pxOps->xReceive` → `netconn_recv`（TCP）或 `xStreamBufferReceive`（UART）才真正到达硬件。

### 8.2 第二层：modbus_port → Transport（操作表）

```c
// transport_tcp.c:75-83（TCP 后端）
static const TransportOps_t s_xTcpOps = {
    prvOpen, prvClose, prvSend, prvReceive, prvControl, prvGetState, prvGetNativeError
};

// transport_uart.c:66-74（UART 后端）
static const TransportOps_t s_xUartOps = {
    prvOpen, prvClose, prvSend, prvReceive, prvControl, prvGetState, prvGetNativeError
};
```

`xTransportSend()` 通过 `pxChannel->pxOps->xSend()` 调度到具体后端。这层解耦让**上层（modbus_port）完全不感知**后端是 TCP 还是 UART —— 它只拿到一个统一的 `TransportChannel_t*`。

---

## 九、超时管理机制

### 9.1 nanoMODBUS 内部的两个超时

| 超时         | 字段                    | 用途                       | 本工程设置值                                                 |
| ------------ | ----------------------- | -------------------------- | ------------------------------------------------------------ |
| read_timeout | `nmbs->read_timeout_ms` | 等待**帧首字节**的最长时间 | `APP_MODBUS_TCP_IO_TIMEOUT_MS` (1000ms) 或 `COFFEE2_RTU_IO_TIMEOUT_MS` (500ms) |
| byte_timeout | `nmbs->byte_timeout_ms` | 相邻两个字节之间的最长时间 | 同上（prvInit 中 ulByteTimeoutMs）                           |

### 9.2 超时传递链

```
modbus_port.c: prvBegin() 
    ├── nmbs_set_read_timeout(ulTimeoutMs)     ← 整体超时
    └── nmbs_set_byte_timeout(ulByteTimeoutMs) ← 字节间隔超时

nanoMODBUS 内部: recv() / send()
    └── nmbs->platform.read(buf, count, nmbs->byte_timeout_ms, arg)
          │
          ▼
modbus_port.c: prvRead() / prvWrite()
    ├── prvGetEffectiveTimeout()   ← 关键: 把超时钳制在事务总预算内
    │     ├── prvGetRemainingMs()  ← 事务已用时间，计算剩余
    │     └── 返回 min(请求超时, 剩余总超时)
    └── xTransportReceiveExact() / xTransportSend()
```

**超时保护的意义**：nanoMODBUS 内部可能多次调用 recv/send（如先读帧头再读数据），如果每次都允许完整超时，总耗时可能远超调用者期望。`prvGetEffectiveTimeout()` 确保整个事务不超过 `ulTimeoutMs`。

### 9.3 错误映射链

```
底层错误                        → 传输层                    → 协议层
─────────────────────────────────────────────────────────────────────
netconn_recv 返回 ERR_TIMEOUT  → xReceive 返回 TIMEOUT      → prvRead 返回 0
netconn_recv 返回 ERR_RST      → xReceive 返回 DISCONNECTED → prvRead 返回 -1
netconn_write_partly 部分发送  → xSend 返回 IO_ERROR        → prvWrite 返回 -1
                                                              ↓
                                                      nanoMODBUS: send()/recv()
                                                              ↓
                                              NMBS_ERROR_TIMEOUT / NMBS_ERROR_TRANSPORT
                                                              ↓
                                                      modbus_port.c: prvMapError()
                                                              ↓
                                             MODBUS_PORT_RESULT_TIMEOUT / TRANSPORT
```

---

## 十、解耦架构完整总结

| 层级       | 文件                                   | 解耦方式                         | 职责                                     |
| ---------- | -------------------------------------- | -------------------------------- | ---------------------------------------- |
| 协议库层   | `nanomodbus.c/h`                       | 只依赖 `nmbs_platform_conf` 回调 | 帧封装/解析、CRC、状态机、事务ID、错误码 |
| 回调适配层 | `modbus_port.c`                        | 提供 `prvRead/prvWrite/prvFlush` | 超时钳制、帧追踪、错误映射、故障记录     |
| 传输抽象层 | `transport.c/h`                        | `TransportOps_t` 函数指针表      | 通道注册/查找、生命周期、收发调度、诊断  |
| 后端实现层 | `transport_tcp.c` / `transport_uart.c` | 实现 `TransportOps_t` 7个函数    | Netconn/Socket/DMA/中断等具体硬件操作    |

**解耦的核心收益**：
1. **nanoMODBUS 不感知硬件**：换硬件只需要改 modbus_port 回调内部实现，库本身零改动
2. **modbus_port 不感知后端**：TCP/UART 通过统一的 `TransportChannel_t` 接口调用，切换只需换 `pxOps` 指针
3. **Transport 不感知协议**：不知道也不关心收发的是 Modbus 还是其他协议字节流
4. **每一层都只暴露稳定接口**，层与层之间通过函数指针或接口表绑定

**一句话总结**：nanoMODBUS 通过 `nmbs_platform_conf` 只暴露 `read`/`write`/`crc_calc`/`flush` 四个回调作为**唯一外界入口**，本工程通过 `modbus_port.c` 实现这些回调（内部调用 Transport 层），Transport 层再通过 `TransportOps_t` 函数指针表调度到具体后端 —— 这样就形成了 `协议库 → 回调适配 → 传输抽象 → 后端具体实现` 的四层解耦架构。