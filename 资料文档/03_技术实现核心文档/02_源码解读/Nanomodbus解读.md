**`nanomodbus.h + nanomodbus.c + nanomodbus_config.h`** 阅读笔记明确分成：

```text
第一层：nanoMODBUS
        ↓
第二层：ModbusPort
        ↓
第三层：Transport
        ↓
UART / TCP / 平台
```

---

# nanoMODBUS 源码细读整理

## 1. nanoMODBUS 在整个架构里的定位

`nanomodbus.h` 对自己的定位很直接：这是一个面向 MCU 的紧凑 C 语言 Modbus RTU/TCP 协议库。

因此它主要解决的是：

```text
业务参数
  │
  ▼
Modbus 功能码
  │
  ▼
PDU 编码/解析
  │
  ▼
RTU / TCP ADU 封装
  │
  ├── RTU：CRC
  └── TCP：MBAP
  │
  ▼
read / write 回调
```

它**不负责具体 UART、Socket、HAL、FreeRTOS**。

也就是说：

```text
nanoMODBUS
负责“这些字节应该是什么”

Transport
负责“这些字节怎么真正发出去”
```

而中间连接二者的核心，就是：

```c
nmbs_platform_conf
```

---

# 2. `nanomodbus_config.h`：编译期裁剪

当前工程配置是：

```c
#define NANOMODBUS_CFG_CLIENT_ENABLED 1
#define NANOMODBUS_CFG_SERVER_ENABLED 1
```

即 Client 和 Server 都打开。配置文件还特别强调：这些宏必须在 `nanomodbus.c` 和所有使用 `nanomodbus.h` 的编译单元中保持一致，因为它们会影响公开结构体布局。

这说明 nanoMODBUS 很重视嵌入式场景里的：

```text
不用的功能
   ↓
编译阶段直接裁掉
```

而不是运行时判断。

---

# 3. `nmbs_error`：协议库自己的错误体系

```c
typedef enum nmbs_error {
    NMBS_ERROR_INVALID_REQUEST = -8,
    NMBS_ERROR_INVALID_UNIT_ID = -7,
    NMBS_ERROR_INVALID_TCP_MBAP = -6,
    NMBS_ERROR_CRC = -5,
    NMBS_ERROR_TRANSPORT = -4,
    NMBS_ERROR_TIMEOUT = -3,
    NMBS_ERROR_INVALID_RESPONSE = -2,
    NMBS_ERROR_INVALID_ARGUMENT = -1,
    NMBS_ERROR_NONE = 0,

    NMBS_EXCEPTION_ILLEGAL_FUNCTION = 1,
    NMBS_EXCEPTION_ILLEGAL_DATA_ADDRESS = 2,
    NMBS_EXCEPTION_ILLEGAL_DATA_VALUE = 3,
    NMBS_EXCEPTION_SERVER_DEVICE_FAILURE = 4
} nmbs_error;
```

这里最重要的是符号：

```text
< 0    nanoMODBUS / 通信本身出问题
= 0    成功
> 0    对端合法返回 Modbus Exception
```

对应判断：

```c
#define nmbs_error_is_exception(e) ((e) > 0 && (e) < 5)
```



所以：

```c
NMBS_ERROR_TIMEOUT  没正常完成通信。
```

和：

```c
NMBS_EXCEPTION_ILLEGAL_DATA_ADDRESS  通信其实成功了，从站明确告诉你地址非法。
```

这就是后来 `ModbusPort` 把它们分别映射成：

```text
TIMEOUT
EXCEPTION
```

的基础。

---

# 4. `nmbs_bitfield`：Coil 的内部表示

nanoMODBUS 默认支持：

```c
#define NMBS_BITFIELD_MAX 2000
#define NMBS_BITFIELD_BYTES_MAX (NMBS_BITFIELD_MAX / 8)

typedef uint8_t nmbs_bitfield[NMBS_BITFIELD_BYTES_MAX];
```

所以：

```text
2000 Coil
    ↓
250 byte
```

不是 `bool[2000]`。

它还定义：

```c
nmbs_bitfield_read()
nmbs_bitfield_set()
nmbs_bitfield_unset()
nmbs_bitfield_write()
nmbs_bitfield_reset()
```

本质都是：

```text
第 b 个 bit
↓
byte = b / 8
bit  = b % 8
```

源码没有用 `/ 8` 和 `% 8`，而用了位运算：
```text

b >> 3 等价于  b / 8

b & 7 等价于 b % 8

((bool) ((bf)[(b) >> 3] & (0x1 << ((b) & (8 - 1))))) 
=>  找到第 `b` 位所在的字节，然后构造一个只包含那一位的 mask，再通过 `&` 检查这一位是否为 1。
```


这也解释了为什么 `ModbusPort_t` 里会专门有：

```c
nmbs_bitfield aucBitfield;
```

它就是 `bool[]` 和 nanoMODBUS 位压缩格式之间的转换缓冲区。

---

# 5. `nmbs_transport`：RTU 与 TCP 共用同一个协议核

```c
typedef enum nmbs_transport {
    NMBS_TRANSPORT_RTU = 1,
    NMBS_TRANSPORT_TCP = 2,
} nmbs_transport;
```



它并不是：

```text
FC03_RTU()
FC03_TCP()
```

两套实现。

而是：

```text
              FC03
               │
               ▼
          同一个 PDU
               │
        ┌──────┴──────┐
        ▼             ▼
       RTU           TCP
    Unit + CRC     MBAP Header
```

差异集中在公共报文封装层。

---

# 6. 整个库最关键的抽象：`nmbs_platform_conf`

```c
typedef struct nmbs_platform_conf {
    nmbs_transport transport;

    int32_t (*read)(...);
    int32_t (*write)(...);

    uint16_t (*crc_calc)(...);
    void (*flush)(...);

    void* arg;

    uint32_t initialized;
} nmbs_platform_conf;
```

nanoMODBUS 明确规定：

```text
read/write 应尝试完成 count 字节

返回 == count
    成功

返回 0 ~ count-1
    Timeout

返回 < 0
    Transport Error
```

同时：

```text
timeout < 0    无限等待
timeout = 0    非阻塞
timeout > 0    有限等待
```



这一份接口契约非常重要，因为后来：

```c
ModbusPort::prvRead()
ModbusPort::prvWrite()
```

就是严格按照这个规则返回数据的。

---

# 7. 它和 C++ 的关系

我们之前已经讨论过，这里可以压缩成：

```text
C                         C++

read 函数指针      ≈     virtual read()
write 函数指针     ≈     virtual write()
flush 函数指针     ≈     virtual flush()

void *arg           ≈     this

platform_conf       ≈     接口对象 / Strategy
```

例如：

```c
platform.read = prvRead;
platform.write = prvWrite;
platform.arg = pxPort;
```

以后 nanoMODBUS：

```c
platform.read(..., platform.arg);
```

实际上就是：

```c
prvRead(..., pxPort);
```

非常像：

```cpp
pxPort->read(...);
```

所以：

> `nmbs_platform_conf` 可以理解成 C 语言手工实现的“虚函数接口 + this 上下文”。

---

# 8. `nmbs_callbacks`：这是另外一个方向的接口

注意别把：

```c
nmbs_platform_conf
```

和：

```c
nmbs_callbacks
```

搞混。

它们方向相反：

```text
                 业务数据模型
                       ↑
                 nmbs_callbacks
                       ↑
               ┌──────────────┐
               │  nanoMODBUS  │
               └──────────────┘
                       ↓
              nmbs_platform_conf
                       ↓
                 UART / TCP
```

例如 Server 收到 FC03 后，可以调用：

```c
read_holding_registers(
    address,
    quantity,
    registers_out,
    unit_id,
    arg);
```

写寄存器则有：

```c
write_single_register(...)
write_multiple_registers(...)
```



nanoMODBUS 只负责：

> “客户端要读地址 100 的 5 个寄存器。”

至于：

> “地址100在你的产品里代表什么？”

它不知道。

这部分交给 Server callback。

---

# 9. nmbs_t：真正的 nanoMODBUS 实例

核心结构大致可以整理成：

```c
/*
 * nanoMODBUS 核心实例结构体。
 *
 * 一个 nmbs_t 表示一个独立的 Modbus 协议实例。
 * 它保存：
 *  1. 当前正在收发/解析的 Modbus 报文；
 *  2. Server 回调函数；
 *  3. 接收超时配置；
 *  4. 底层平台 read/write/flush 等接口；
 *  5. RTU 本机地址、目标地址；
 *  6. TCP Transaction ID 状态。
 *
 * 可以把它理解成 nanoMODBUS 的“对象实例”。
 */
struct nmbs_t {

    /*
     * 当前 Modbus 报文的内部状态。
     *
     * nanoMODBUS 不会为每个字段单独申请内存，
     * 而是使用一个固定 buf[] 保存当前 ADU/PDU，
     * 再通过 buf_idx 作为“游标”顺序解析或构造报文。
     */
    struct {

        /*
         * 当前 Modbus 报文的原始字节缓冲区。
         *
         * 发送时：
         *   put_1()/put_2()/put_n() 等函数不断向这里写入数据。
         *
         * 接收时：
         *   platform.read() 把收到的数据写入这里，
         *   get_1()/get_2()/get_n() 再从这里解析。
         *
         * 所以这个数组既是 TX Buffer，也可以作为 RX Buffer 使用。
         */
        uint8_t buf[260];

        /*
         * buf[] 当前操作位置，也就是“报文游标”。
         *
         * 例如：
         *
         *   buf:
         *   01 03 00 64 00 02
         *         ^
         *       buf_idx
         *
         * get_1():
         *      读取一个字节，然后 buf_idx += 1
         *
         * get_2():
         *      读取两个字节，然后 buf_idx += 2
         *
         * put_1()/put_2() 同样会推进 buf_idx。
         *
         * 因此 nanoMODBUS 的报文解析本质上就是：
         *
         *      buffer + cursor(buf_idx)
         */
        uint16_t buf_idx;


        /*
         * 当前报文的 Unit Identifier。
         *
         * RTU：
         *   就是从站地址 / Slave Address。
         *
         * Client 发送请求时，
         * msg_state_req() 会把 dest_address_rtu 复制到这里。
         *
         * Server 接收到 RTU 请求后，
         * 会用这个地址判断：
         *
         *   1. 是不是发给本机；
         *   2. 是不是广播地址 0。
         *
         * TCP 中它也会作为 MBAP Header 中的 Unit Identifier 使用。
         */
        uint8_t unit_id;

        /*
         * Function Code，Modbus 功能码。
         *
         * 例如：
         *
         *   0x01  Read Coils
         *   0x02  Read Discrete Inputs
         *   0x03  Read Holding Registers
         *   0x04  Read Input Registers
         *   0x06  Write Single Register
         *   0x10  Write Multiple Registers
         *
         * Client 发请求时：
         *      msg_state_req(nmbs, fc)
         * 会把请求功能码保存到这里。
         *
         * 收到响应时还会用它检查：
         *
         *      response FC == request FC
         *
         * 如果收到 FC + 0x80，
         * 则表示 Modbus Exception Response。
         */
        uint8_t fc;

        /*
         * Modbus TCP Transaction Identifier。
         *
         * TCP 的 MBAP Header 中包含一个 16 bit Transaction ID，
         * 用于将“响应”和之前的“请求”对应起来。
         *
         * Client 发请求时：
         *
         *      current_tid
         *          ↓
         *      transaction_id
         *
         * 收到 TCP 响应后 nanoMODBUS 会检查：
         *
         *      response.transaction_id
         *              ==
         *      request.transaction_id
         *
         * 不一致则认为收到的 TCP 响应不是当前请求对应的响应。
         *
         * RTU 没有 Transaction ID 字段，
         * 因而该字段主要对 TCP 有意义。
         */
        uint16_t transaction_id;

        /*
         * 当前 RTU 请求是否为广播请求。
         *
         * Modbus RTU 地址 0 是广播地址。
         *
         * Client：
         *   如果目标地址为 0，
         *   msg_state_req() 会设置 broadcast = true。
         *
         * 广播请求发送以后不能等待从站响应，
         * 所以很多写操作中会看到：
         *
         *      if (!nmbs->msg.broadcast)
         *          recv_xxx_response(...);
         *
         * Server：
         *   收到 Unit ID == 0 时也会设置这个标志。
         *
         * 广播请求通常执行命令，但不发送响应。
         */
        bool broadcast;

        /*
         * Server 当前收到的 RTU 请求是否应该忽略
         * 例如 Server 本机 RTU 地址为：
         *      address_rtu = 1
         * 但收到：
         *      Unit ID = 2
         * 则：
         *      ignored = true
         * 后面的功能码处理代码仍可能把完整报文接收/解析完，
         * 但不会真正执行本机的数据模型回调。
         * 这个字段主要用于 Server RTU 地址过滤。
         */
        bool ignored;

        /*
         * 当前接收报文是否已经完整进入 buf[]。
         *
         * 这个字段主要用于 Modbus TCP。
         *
         * TCP 的 MBAP Header 中自带 Length 字段，
         * 因此 nanoMODBUS 读完 MBAP Header 后，
         * 可以知道后面还剩多少字节，
         * 一次把剩余报文全部读进 buf[]。
         *
         * 完整接收后：
         *      complete = true;
         * 之后协议解析代码再次调用 recv() 时，
         * recv() 发现 complete == true，
         * 就不会继续调用底层 platform.read()，
         * 而只是继续从已有 buf[] 中解析。
         *
         * RTU 没有类似的 MBAP Length 字段，
         * 因而接收方式不同。
         */
        bool complete;

    } msg;


    /*
     * Modbus Server 数据模型回调表。
     * 只有作为 Server 使用时才真正发挥作用。
     * 例如收到：
     *      FC03 Read Holding Registers
     * nanoMODBUS 完成协议解析后会调用类似：
     *      callbacks.read_holding_registers(...)
     * 再由用户代码提供真正的寄存器内容。
     *
     * 因此它是：
     *      nanoMODBUS
     *           ↓
     *      用户数据模型 的接口。
     *
     * 注意它和 platform 的方向正好相反：
     *
     *      callbacks
     *          ↑
     *      nanoMODBUS
     *          ↓
     *      platform
     */
    nmbs_callbacks callbacks;


    /*
     * 字节间超时时间，单位 ms。
     *
     * 主要用于：
     *   已经开始收到一个报文之后，
     *   等待后续字节时的超时。
     *
     * nanoMODBUS 的定义：
     *
     *   < 0 ：无限等待
     *   = 0 ：非阻塞，只尝试一次
     *   > 0 ：最多等待指定毫秒
     *
     * 例如：
     *
     *   已经收到 Unit ID
     *          ↓
     *   等 FC
     *          ↓
     *   等 Address
     *          ↓
     *   等 Data
     *
     * 后续这些阶段主要使用 byte_timeout_ms。
     */
    int32_t byte_timeout_ms;

    /*
     * 一次请求/响应开始阶段的等待超时，单位 ms。
     *
     * Client：
     *   请求发送以后，用它等待响应的“第一个字节”。
     *
     * Server：
     *   nmbs_server_poll() 等待新请求时也使用它。
     *
     * recv_msg_header() 中会暂时执行：
     *
     *      byte_timeout_ms = read_timeout_ms;
     *
     * 用 read_timeout 等待第一字节。
     *
     * 第一字节收到以后，
     * 再恢复原来的 byte_timeout_ms，
     * 用较短的字节间 timeout 接收后续内容。
     */
    int32_t read_timeout_ms;


    /*
     * 平台适配配置。
     *
     * 它把 nanoMODBUS 和具体硬件/操作系统隔离开。
     *
     * 里面主要包含：
     *
     *      transport   RTU / TCP
     *
     *      read()      底层读取函数
     *      write()     底层发送函数
     *
     *      crc_calc()  CRC计算函数
     *      flush()     清理残留接收数据
     *
     *      arg         用户上下文指针
     *
     * nanoMODBUS 只会调用：
     *
     *      platform.read(...)
     *      platform.write(...)
     *
     * 在当前工程中，这两个接口最终由 ModbusPort
     * 的 prvRead()/prvWrite() 连接到 Transport。
     *
     * nmbs_create() 中采用结构体按值复制：
     *
     *      nmbs->platform = *platform_conf;
     *
     * 因此 nmbs_t 内部保存的是配置结构体的一份副本。
     */
    nmbs_platform_conf platform;


    /*
     * Server 自己的 RTU 地址。
     * 主要用于 RTU Server。
     * 例如：
     *      address_rtu = 1
     * 收到：
     *      Unit ID = 1
     * 表示请求发给自己。
     *
     * 收到：
     *      Unit ID = 2
     * 则会把 msg.ignored 设置为 true。
     *
     * 所以：
     *      address_rtu
     * 表示“我是谁”。
     */
    uint8_t address_rtu;

    /*
     * Client 下一次请求的目标 Unit ID。
     *
     * 主要通过：
     *      nmbs_set_destination_rtu_address() 设置
     * Client 开始请求时：
     *      msg.unit_id = dest_address_rtu;
     * 所以：
     *      address_rtu
     *          = Server 自己是谁
     *
     *      dest_address_rtu
     *          = Client 要访问谁
     *
     * 虽然变量名字中带 "_rtu"，
     * 但实现中 msg_state_req() 不区分 RTU/TCP，
     * 都会把它复制到 msg.unit_id；
     * TCP 封包时这个值会进入 MBAP Unit Identifier。
     */
    uint8_t dest_address_rtu;

    /*
     * Client 当前 Transaction ID 计数器。
     * 每开始一个新的 Client 请求：
     *      current_tid++
     * 然后：
     *      msg.transaction_id = current_tid;
     * 达到 UINT16_MAX 后不会继续溢出为 0，
     * 而是重新从 1 开始。
     *
     * 实现大致为：
     *      if (current_tid == UINT16_MAX)
     *          current_tid = 1;
     *      else
     *          current_tid++;
     *
     * 它主要服务于 Modbus TCP 的 Transaction Identifier。
     *
     * 可以把两者理解为：
     *      current_tid
     *          = 下一笔/当前递增计数状态
     *      msg.transaction_id
     *          = 当前这一帧实际使用的 TID
     */
    uint16_t current_tid;
};
```

源码明确说这些成员应该视为 private，不建议调用者直接修改。

可以把它理解成：

```text
nmbs_t
=
当前报文状态 msg
+ Server 数据回调 callbacks
+ 超时策略
+ 底层平台接口 platform
+ RTU 地址状态
+ TCP Transaction ID 状态
```

所以 `nmbs_t` 本质上就是：

> **一个 Modbus Client/Server 协议对象的全部内部状态。**

---

# 10. `get_1/get_2/put_1/put_2`：最底层编解码工具

可以把四个函数压成一张表：

| 函数      | 方向                | 处理大小 | `buf_idx`变化 |
| --------- | ------------------- | -------- | ------------- |
| `get_1()` | buffer → 变量       | 1 byte   | `+1`          |
| `put_1()` | 变量 → buffer       | 1 byte   | `+1`          |
| `get_2()` | buffer → `uint16_t` | 2 bytes  | `+2`          |
| `put_2()` | `uint16_t` → buffer | 2 bytes  | `+2`          |

它们真正重要的不只是“读写”，而是把：

```
buffer
+
buf_idx
```

变成了一个很轻量的协议解析器。

比如 Modbus FC03 请求：

```
01 03 00 64 00 02 CRC CRC
```

nanoMODBUS 可以这么理解：

```
unit_id  = get_1(nmbs);   // 01
fc       = get_1(nmbs);   // 03
address  = get_2(nmbs);   // 0064
quantity = get_2(nmbs);   // 0002
```

你会发现代码非常干净。

如果不用这些函数，就得反复写：

```
unit_id = buf[i++];
fc = buf[i++];

address =
    ((uint16_t)buf[i] << 8) |
    buf[i + 1];

i += 2;
```

会很乱。

反过来组包也是：

```
put_1(nmbs, unit_id);
put_1(nmbs, fc);
put_2(nmbs, address);
put_2(nmbs, quantity);
```

结果自然形成：

```
UnitID | FC | Address_H | Address_L | Qty_H | Qty_L
```

所以这四个函数本质上就是：

> **nanoMODBUS 的最基础序列化 / 反序列化工具。**

还可以再注意一个细节：它们和 `set_1()/set_2()` 不一样。`get/put` 会自动推进 `buf_idx`，适合“顺序读写报文”；而 `set_1/set_2` 是直接修改指定 `index`，不会改变解析游标，适合回头修改已经构造好的某个字段，比如 TCP 长度字段。源码里两类函数是分开的。

如果你接下来继续往下读，我建议下一步就看 **`get_n / put_n / get_regs / put_regs / swap_regs`**。这几个正好是在 `get_1/get_2` 基础上的“批量版”，而且会顺带把 Modbus 的寄存器字节序问题彻底讲透。

---

# 12. `recv()` / `send()`：nanoMODBUS 与外界真正接触的地方

接收：

```c
nmbs->platform.read(
    nmbs->msg.buf + nmbs->msg.buf_idx,
    count,
    nmbs->byte_timeout_ms,
    nmbs->platform.arg);
```

然后根据返回值：

```text
ret == count
    → NMBS_ERROR_NONE

0 <= ret < count
    → NMBS_ERROR_TIMEOUT

ret < 0
    → NMBS_ERROR_TRANSPORT
```

发送同样通过：

```c
platform.write(...)
```

处理。

所以 nanoMODBUS 的真正底部边界就是：

```text
recv()
  ↓
platform.read()

send()
  ↓
platform.write()
```

再往下就是 `ModbusPort` 的职责了。

---

# 13. `flush()`：开始新 Client 请求之前清旧数据

默认实现：

```c
static void flush(nmbs_t* nmbs, void* arg)
{
    nmbs->platform.read(
        nmbs->msg.buf,
        sizeof(nmbs->msg.buf),
        0,
        nmbs->platform.arg);
}
```

也就是：

```text
timeout = 0
    ↓
非阻塞读取
    ↓
把残留数据扔掉
```



但在我们的 `ModbusPort` 中，这个默认实现被：

```c
pxPlatform->flush = prvFlush;
```

替换掉了。

这就是 nanoMODBUS 和 ModbusPort 第一个明显的连接点。

---

# 14. msg_state_reset()：一帧的状态归零

```c
msg.buf_idx = 0;

msg.unit_id = 0;
msg.fc = 0;
msg.transaction_id = 0;

msg.broadcast = false;
msg.ignored = false;
msg.complete = false;
```



所以每笔报文事务都有自己的：

```text
Unit ID
FC
Transaction ID
Broadcast
Ignored
Complete
```

状态。

---

# 15. msg_state_req()：Client 每笔事务真正的起点

它做：

```c
current_tid++;

platform.flush(...);

msg_state_reset(...);

msg.unit_id = dest_address_rtu;
msg.fc = fc;
msg.transaction_id = current_tid;
```

如果：

```c
unit_id == 0 && transport == RTU
```

则：

```c
msg.broadcast = true;
```



因此 Client 的所有功能码在真正组包前，都先做：

```text
事务 ID 更新
      ↓
清理旧 RX
      ↓
报文状态清零
      ↓
设置 Unit ID
      ↓
设置 FC
      ↓
判断 Broadcast
```

这是非常核心的公共入口。

---

# 16. nmbs_create()：真正的基础构造函数

`ClientCreate` 和 `ServerCreate` 最后都会依赖基础创建逻辑。

这里先：

```c
memset(nmbs, 0, sizeof(nmbs_t));

nmbs->byte_timeout_ms = -1;
nmbs->read_timeout_ms = -1;
```

然后验证：

```text
platform_conf 已正确初始化
transport 是 RTU/TCP
read 不为空
write 不为空
```

最后：

```c
nmbs->platform = *platform_conf;
```

注意是**结构体复制**。

所以：

```c
nmbs_platform_conf xPlatform;
```

可以是一个临时局部变量。

调用：

```c
nmbs_client_create(..., &xPlatform);
```

之后就可以销毁，因为内容已经复制进：

```c
nmbs_t.platform
```

这正是我们读 `ModbusPortClientInit()` 时看到 `xPlatform` 放在栈上仍然安全的原因。

---

# 17. nmbs_platform_conf_create()：C 语言版本的“构造初始化”

```c
memset(platform_conf, 0, ...);

platform_conf->crc_calc = nmbs_crc_calc;
platform_conf->flush = flush;

platform_conf->initialized = 0xFFFFDEBE;
```



于是：

```text
crc_calc
flush
```

有默认实现。

而：

```text
read
write
transport
```

由用户填写。

`initialized` 则用于检测：

> 有没有按 nanoMODBUS 规定的初始化路径创建这个结构体。

这非常像 C++：

```cpp
PlatformConf::PlatformConf()
{
    crc = default_crc;
    flush = default_flush;
}
```

只不过 C 里要手动调用：

```c
nmbs_platform_conf_create();
```

---

# 18. 两种 timeout 必须区分

`nmbs_t` 有：

```c
read_timeout_ms;
byte_timeout_ms;
```

头文件分别说明：

```text
read_timeout
    Request/Response timeout

byte_timeout
    连续字节收发之间的 timeout
```



实现更能说明问题。

在：

```c
recv_msg_header()
```

开始时：

```c
old = byte_timeout;

byte_timeout = read_timeout;
```

只为了等第一字节。

收到第一字节后马上恢复：

```c
byte_timeout = old_byte_timeout;
```



所以实际语义：

```text
             read_timeout
                  ↓
          等响应第一字节
                  │
                  ▼
              收到以后
                  │
           byte_timeout
                  ▼
          等后续报文字节
```

这正是 `ModbusPort` 后面又加入“总事务 Deadline”的基础。

---

# 19. put_msg_header() :  RTU 与 TCP 的报文头在哪里分叉

关键函数：

```c
put_msg_header()
```

RTU：

```c
put_1(nmbs, unit_id);
```

TCP：

```c
put_2(transaction_id);
put_2(0);
put_2(length);
put_1(unit_id);
```

最后两者统一：

```c
put_1(nmbs, fc);
```



所以：

```text
PDU层逻辑
     │
     ▼
put_msg_header
     │
 ┌───┴───┐
 ▼       ▼
RTU     TCP
```

这就是为什么一个：

```c
nmbs_read_holding_registers()
```

能同时支持 RTU 和 TCP。

---

# 20. send_msg() ：RTU CRC 统一在这里补

```c
if (transport == RTU) {
    crc = platform.crc_calc(...);
    put_2(nmbs, crc);
}

send(nmbs, msg.buf_idx);
```

TCP 不加 CRC。

因此具体 FC 函数完全不用关心：

```text
我是 RTU 吗？
我要不要算 CRC？
```

只需要：

```c
send_msg();
```

---

# 21. nmbs_crc_calc:  CRC 为什么最后交换一次字节

默认：

```c
nmbs_crc_calc()
```

最终：

```c
return (crc << 8) | (crc >> 8);
```

这是因为：

```c
put_2() 统一按照高字节先写。
```

CRC 函数提前做一次交换后，再经过 `put_2()`，就能得到源码所需要的 RTU CRC 字节排列。

这属于实现技巧，第一次看很容易觉得“怎么算完 CRC 又 swap”。

---

# 22.recv_msg_footer():  接收并校验报文尾部

RTU：

```c
crc = crc_calc(
    buf,
    当前已经解析的长度);

recv(nmbs, 2);

recv_crc = get_2(nmbs);

if (recv_crc != crc)
    return NMBS_ERROR_CRC;
```

TCP 则没有这个 CRC Footer 检查。

所以 CRC 校验完全被公共报文层吸收了。

---

# 23. TCP 为什么有 `msg.complete`

TCP 的 MBAP 中带长度信息。

因此 `recv_msg_header()` 在解析 TCP Header 后：

```c
length = get_2(...);

recv(nmbs, length - 2);

msg.complete = true;
```



而最底层：

```c
recv()
```

一开始：

```c
if (nmbs->msg.complete)
    return NMBS_ERROR_NONE;
```



所以 TCP 路径是：

```text
收到 MBAP Length
      ↓
一次把剩余帧收完整
      ↓
complete = true
      ↓
后续 recv()
其实只是在已有 buffer 中解析
```

RTU 没有这个长度字段，因此处理方式不同。

---

# 24. Client 接收区分从站号的响应

关键函数：

```c
recv_res_header()
```

在读响应之前，先保存请求：

```c
req_transaction_id
req_unit_id
req_fc
```

收到响应之后：

TCP 检查：

```c
response.transaction_id == request.transaction_id
```

RTU 检查：

```c
response.unit_id == request.unit_id
```

还要检查：

```c
response.fc == request.fc
```

否则尝试判断：

```c
response.fc - 0x80 == request.fc
```

即 Modbus Exception。

因此 Client 并不是：

> 收到数据就信。

而是：

```text
Transaction ID / Unit ID
          ↓
Function Code
          ↓
Exception
          ↓
具体响应数据
          ↓
CRC
```

层层验证。

---

# 25. 正常响应与异常响应

例如请求：

```text
FC = 03
```

你可以把 nanoMODBUS 的错误检查理解成一条流水线：

```
收到帧
  │
  ▼
recv_msg_header()
  │
  ├─ RTU/TCP头是否合法
  ├─ MBAP是否合法
  └─ Unit/FC等取出来
  │
  ▼
recv_res_header()
  │
  ├─ 是不是当前请求的响应
  ├─ FC是否匹配
  └─ 是否是Exception Response
  │
  ▼
recv_xxx_res()
  │
  ├─ Byte Count对不对
  ├─ Quantity对不对
  ├─ Echo地址/值对不对
  └─ Data格式对不对
  │
  ▼
recv_msg_footer()
  │
  └─ RTU CRC是否正确
  │
  ▼
最终成功
```

而 Server 则是另一条：

```
收到请求
  │
  ▼
recv_req_header()
  │
  ▼
handle_req_fc()
  │
  ▼
handle_xxx()
  │
  ├─ Quantity是否合法
  ├─ Address是否合法
  ├─ Byte Count是否合法
  ├─ callback是否存在
  └─ callback执行结果
  │
  ├──────── 错误
  │             ↓
  │     send_exception_msg()
  │             ↓
  │       发 83 02 等
  │
  └──────── 正常
                ↓
           发送正常响应
```

所以你刚才的疑问可以一句话概括：

> **`recv_res_header()` 只负责 Client 端识别“Server 是否已经返回了异常响应”；真正决定“该返回哪个 Exception Code”的逻辑在 Server 的各个 `handle_xxx()` 中。后续数据长度、Byte Count、CRC 等错误则由更下游的具体响应解析函数继续检查。**

# 26. Client 功能码实现其实高度统一

以 FC16 为例：

```c
nmbs_write_multiple_registers(...)
{
    参数校验;

    msg_state_req(nmbs, 16);

    put_req_header(...);

    put_2(address);
    put_2(quantity);
    put_1(byte_count);

    for (...) {
        put_2(register);
    }

    send_msg();

    if (!broadcast)
        recv_write_multiple_registers_res(...);
}
```



所以大多数 Client API 可以压缩为：

```text
检查参数
   ↓
msg_state_req(FC)
   ↓
put_req_header()
   ↓
put_1 / put_2
   ↓
send_msg()
   ↓
recv_xxx_res()
```

这条调用模板要记住。

---

# 27. Response 解析同样有统一模板

例如读寄存器：

```c
recv_read_registers_res()
```

逻辑：

```text
recv_res_header()
     ↓
收 Byte Count
     ↓
验证 Byte Count == quantity × 2
     ↓
读取数据
     ↓
get_2() 解寄存器
     ↓
recv_msg_footer()
```

代码还会拒绝错误长度的响应。

写寄存器则验证回显：

```c
address_res == address
value_res   == value_req
```

否则：

```c
NMBS_ERROR_INVALID_RESPONSE
```



因此 nanoMODBUS 不只是编解码器，它也是：

> **Modbus 响应合法性验证器。**

---

# 28. Broadcast 的处理

例如 FC06：

```c
send_msg();

if (!nmbs->msg.broadcast)
    return recv_write_single_register_res(...);

return NMBS_ERROR_NONE;
```



所以：

```text
普通 RTU Request
    发 → 等 Response

Broadcast
    发 → 不等 Response
```

由 nanoMODBUS 自己处理。

---

# 29. Server 方向是 Client 的镜像

Server 逻辑可以压缩为：

```text
nmbs_server_poll()
       ↓
recv_req_header()
       ↓
判断 Unit ID / Broadcast
       ↓
根据 FC 分发
       ↓
handle_xxx()
       ↓
解析地址和数量
       ↓
检查协议参数
       ↓
nmbs_callbacks.xxx()
       ↓
生成 Response
       ↓
send_msg()
```

Server 收到别的 RTU Unit ID 时，会标记：

```c
msg.ignored = true;
```

广播则：

```c
msg.broadcast = true;
```



---

# 30. Server callback 错误会自动变成 Modbus Exception

例如处理写多个寄存器：

```c
err = callbacks.write_multiple_registers(...);

if (err != NMBS_ERROR_NONE) {
    if (nmbs_error_is_exception(err))
        return send_exception_msg(nmbs, err);

    return send_exception_msg(
        nmbs,
        NMBS_EXCEPTION_SERVER_DEVICE_FAILURE);
}
```



所以业务层 callback 可以返回：

```c
NMBS_EXCEPTION_ILLEGAL_DATA_ADDRESS
```

nanoMODBUS 会自动生成异常响应。

---

# 31. `send_exception_msg()` 怎么做

核心：

```c
nmbs->msg.fc += 0x80;

put_msg_header(nmbs, 1);

put_1(nmbs, exception);

send_msg(nmbs);
```

Broadcast 不返回 Exception。

因此：

```text
请求 FC03
    ↓
异常
    ↓
03 + 80h
    ↓
83
```

具体 ADU 封装、CRC/MBAP 仍然交给公共层。

---

# 32. Raw PDU 是库留下的扩展口

nanoMODBUS 还提供：

```c
nmbs_send_raw_pdu()
nmbs_receive_raw_pdu_response()
```

Raw Send：

```text
设置 FC
↓
构造 RTU/TCP Header
↓
把用户给的 Data 原样写入
↓
send_msg()
```

Raw Receive：

```text
recv_res_header()
↓
接收指定长度
↓
recv_msg_footer()
```



所以某个设备如果有：

```text
私有 Function Code
```

也不一定要修改 nanoMODBUS。

---

# 33. 一笔 FC03 的完整 nanoMODBUS 调用链

这条链建议作为 nanoMODBUS 的核心记忆：

```text
nmbs_read_holding_registers()
           │
           ▼
read_registers(fc = 3)
           │
           ├── 参数校验
           │
           ▼
msg_state_req()
           │
           ├── Transaction ID++
           ├── platform.flush()
           ├── reset state
           ├── Unit ID
           └── FC = 03
           │
           ▼
put_req_header()
           │
           ▼
put_2(address)
           │
           ▼
put_2(quantity)
           │
           ▼
send_msg()
           │
           ├── RTU：加 CRC
           │
           ▼
send()
           │
           ▼
platform.write()
           │
         物理链路
           │
           ▼
platform.read()
           │
           ▼
recv()
           │
           ▼
recv_res_header()
           │
           ├── Unit/TID
           ├── FC
           └── Exception
           │
           ▼
Byte Count
           │
           ▼
get_2() × quantity
           │
           ▼
recv_msg_footer()
           │
           └── RTU CRC
           │
           ▼
      nmbs_error
```

然后一进入当前项目：

```text
platform.write()
       ↓
ModbusPort::prvWrite()
       ↓
Transport

platform.read()
       ↓
ModbusPort::prvRead()
       ↓
Transport
```

这样两层就彻底连起来了。

---

# 34. nanoMODBUS 与 ModbusPort 的职责边界

这一张表最值得留下：

| nanoMODBUS               | ModbusPort                |
| ------------------------ | ------------------------- |
| Modbus FC 编解码            | 工程级 API                   |
| RTU/TCP ADU              | nanoMODBUS ↔ Transport 适配 |
| CRC / MBAP               | 总事务 Deadline              |
| Unit ID / Transaction ID | RTU 帧间静默                  |
| Response 校验              | Trace                     |
| Modbus Exception         | Fault Detail              |
| 协议参数限制                   | 工程错误归一化                   |
| Server FC dispatch       | `TransportChannel_t` 绑定   |

所以：

```text
nanoMODBUS
=
协议引擎

ModbusPort
=
协议引擎的工程适配器 + 事务控制器
```

---

# 35. 两层放到一起，整个架构就非常清楚了

```text
DeviceLibrary
     │
     │  xModbusPortReadHolding()
     ▼
┌────────────────────────────┐
│        ModbusPort          │
│                            │
│ 总事务超时                 │
│ Trace / Fault              │
│ Error Mapping              │
│ RTU Frame Silence          │
│                            │
│       ┌─────────────┐      │
│       │   nmbs_t    │      │
│       │             │      │
│       │ PDU编解码   │      │
│       │ RTU/TCP封装 │      │
│       │ CRC/MBAP    │      │
│       │ FC校验      │      │
│       │ Exception   │      │
│       └──────┬──────┘      │
│              │             │
│      platform.read/write   │
│              │             │
│       prvRead/prvWrite     │
└──────────────┼─────────────┘
               ▼
          Transport
               │
        ┌──────┴───────┐
        ▼              ▼
      UART            TCP
```

到这里，这两层你可以分别记成一句话：

> **nanoMODBUS：我知道 Modbus 报文应该怎么长、怎么解析。**

> **ModbusPort：我知道怎样让 nanoMODBUS 在这个工程的 Transport 和 RTOS 环境里正确运行。**

后面再读 `Transport` 时，我们就只需要追一条线：

```text
nanoMODBUS
platform.read/write
        ↓
ModbusPort
prvRead/prvWrite
        ↓
xTransportReceiveExact /
xTransportSend
        ↓
TransportChannel_t
```

这会自然进入第三层。
