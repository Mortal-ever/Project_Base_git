# MilkTea 项目 lwIP 协议栈深入解读

> **历史资料提示（2026-07-24）**：LwIP 原理仍有效，旧的 MilkTea Modbus TCP
> Server 产品调用链已退役。当前使用两个独立 TCP Client 任务。

---

## 一、项目整体架构概览

```
┌──────────────────────────────────────────────────────────────────┐
│                       Application Layer                          │
│  app_net_monitor.c    app_modbus_service.c   (Modbus TCP Server) │
│  (网络状态监控)       (外部系统接入 + 机器人通信)                   │
├──────────────────────────────────────────────────────────────────┤
│                     Transport Layer                              │
│  transport.c (通道调度器)  ←  transport_tcp.c (TCP 后端)          │
│  同时使用 Netconn API 和 Socket API 两种 lwIP 编程模型              │
├──────────────────────────────────────────────────────────────────┤
│                      lwIP Stack Layer                            │
│  LWIP/App/lwip.c (初始化)  ←  LWIP/Target/lwipopts.h (配置)       │
│  lwIP 内核: TCP/UDP/IP/ARP/DHCP/DNS 等                           │
├──────────────────────────────────────────────────────────────────┤
│               Ethernet Interface (ethernetif.c)                  │
│  Zero-Copy DMA 收发  |  DP83848 PHY 管理  |  中断+信号量驱动      │
├──────────────────────────────────────────────────────────────────┤
│              STM32F407 ETH HAL + DP83848 RMII                    │
└──────────────────────────────────────────────────────────────────┘
```

**硬件平台**: STM32F407 + DP83848 百兆以太网 PHY (RMII 模式)
**lwIP 版本**: 2.1.2 (由 STM32CubeMX 生成)
**RTOS**: FreeRTOS (CMSIS-RTOS v2 封装)

---

## 二、lwIP 配置详解 (`LWIP/Target/lwipopts.h`)

### 2.1 RTOS 与硬件加速

```c
#define WITH_RTOS 1               // 启用 RTOS 模式, lwIP 内部使用 tcpip_thread
#define CHECKSUM_BY_HARDWARE 1    // 由 STM32 ETH MAC 硬件计算/校验 checksum
```

**`WITH_RTOS=1`** 是关键配置，它让 lwIP 运行在 **多线程模式**：
- lwIP 创建一个独立的 `tcpip_thread` 线程，所有网络操作通过邮箱 (mbox) 投递到该线程
- 应用层通过 Netconn API / Socket API 安全地与 lwIP 核心交互
- 底层中断通过信号量唤醒接收线程

**`CHECKSUM_BY_HARDWARE=1`** + 下面关闭所有 checksum 的软件生成和校验（第117-135行全部 =0），表明 TCP/IP/UDP/ICMP 的 checksum 全部交给 STM32 的 ETH MAC 硬件处理，极大减少 CPU 开销。

### 2.2 内存池配置

| 配置项                    | 值           | 含义                              |
| ------------------------- | ------------ | --------------------------------- |
| `MEM_SIZE`                | 24576 (24KB) | lwIP 堆内存总大小                 |
| `MEM_ALIGNMENT`           | 4            | 4字节对齐                         |
| `MEMP_NUM_TCP_PCB`        | 8            | 最大 TCP PCB 数量（用户区额外设） |
| `MEMP_NUM_TCP_PCB_LISTEN` | 3            | 最大监听 PCB 数量                 |
| `MEMP_NUM_TCP_SEG`        | 24           | TCP 分段数量                      |
| `MEMP_NUM_NETBUF`         | 6            | Netbuf 结构体池                   |
| `MEMP_NUM_NETCONN`        | 8            | Netconn 连接对象池                |
| `MEMP_NUM_SYS_TIMEOUT`    | 12           | 超时事件数量                      |

**关键约束关系**（来自 `app_modbus_service.c` 的编译期检查）：
```c
#if (MEMP_NUM_NETCONN < (APP_MODBUS_EXTERNAL_MAX_CLIENTS + 2U))
#error MEMP_NUM_NETCONN cannot support listener, slots, and robot client
#endif
```
每个外部客户端 slot 使用一个 `netconn`，加上 listener 和 robot client，必须 ≤ 8。

### 2.3 数据包缓冲配置

| 配置项              | 值   | 含义                                    |
| ------------------- | ---- | --------------------------------------- |
| `PBUF_POOL_SIZE`    | 12   | Rx pbuf 池大小                          |
| `PBUF_POOL_BUFSIZE` | 1536 | 每个 pbuf buffer 大小（= 最大以太网帧） |
| `TCP_MSS`           | 1460 | TCP 最大分片大小                        |
| `TCP_SND_QUEUELEN`  | 9    | TCP 发送队列长度                        |
| `TCP_SNDQUEUELOWAT` | 5    | 发送队列低水位                          |

1536 字节的 `PBUF_POOL_BUFSIZE` 可以容纳一个完整的以太网帧（1518 字节），配合 Zero-Copy 的 DMA 接收设计。

### 2.4 协议与功能启用

| 配置项                       | 值   | 含义                                        |
| ---------------------------- | ---- | ------------------------------------------- |
| `LWIP_DNS`                   | 1    | 启用 DNS 客户端                             |
| `LWIP_ETHERNET`              | 1    | 启用以太网支持                              |
| `LWIP_DNS_SECURE`            | 7    | DNS 安全级别（随机XID+防多请求+随机源端口） |
| `LWIP_NETIF_STATUS_CALLBACK` | 1    | 启用网口状态回调                            |
| `LWIP_NETIF_LINK_CALLBACK`   | 1    | 启用链路状态回调                            |
| `LWIP_TCP_KEEPALIVE`         | 1    | 启用 TCP KeepAlive                          |
| `LWIP_SO_RCVTIMEO`           | 1    | 启用接收超时选项                            |
| `LWIP_SO_SNDTIMEO`           | 1    | 启用发送超时选项                            |
| `SO_REUSE`                   | 1    | 启用地址复用                                |
| `LWIP_SO_LINGER`             | 1    | 启用 SO_LINGER                              |
| `LWIP_SO_RCVBUF`             | 1    | 启用接收缓冲区选项（FIONREAD 依赖）         |
| `LWIP_STATS`                 | 0    | 关闭统计（减少RAM开销）                     |
| `TCP_LISTEN_BACKLOG`         | 1    | listen backlog                              |

### 2.5 线程配置

| 配置项                     | 值          | 含义                         |
| -------------------------- | ----------- | ---------------------------- |
| `TCPIP_THREAD_STACKSIZE`   | 1024 (字节) | tcpip 核心线程栈             |
| `TCPIP_THREAD_PRIO`        | 24          | tcpip 线程优先级（FreeRTOS） |
| `TCPIP_MBOX_SIZE`          | 6           | tcpip 线程消息邮箱大小       |
| `DEFAULT_THREAD_STACKSIZE` | 1024        | 默认线程栈                   |
| `DEFAULT_ACCEPTMBOX_SIZE`  | 6           | accept 邮箱大小              |
| `RECV_BUFSIZE_DEFAULT`     | 2000000000  | 接收缓冲区（约2GB，无限制）  |

---

## 三、以太网底层驱动 (`LWIP/Target/ethernetif.c`)

这是 lwIP 与 STM32 硬件之间的关键衔接层，实现了 **Zero-Copy DMA** 模式。

### 3.1 Zero-Copy 接收设计

```c
typedef struct {
  struct pbuf_custom pbuf_custom;
  uint8_t buff[(ETH_RX_BUF_SIZE + 31) & ~31] __ALIGNED(32);  // 32字节对齐
} RxBuff_t;

#define ETH_RX_BUFFER_CNT 12U
LWIP_MEMPOOL_DECLARE(RX_POOL, ETH_RX_BUFFER_CNT, sizeof(RxBuff_t), "Zero-copy RX PBUF pool");
```

**核心思想**：Rx 缓冲区不再由 ETH DMA 描述符单独管理，而是**直接从 lwIP 的 pbuf 池中分配**。

**工作流程**:
1. `HAL_ETH_RxAllocateCallback()` — 每当 ETH HAL 需要一个接收缓冲区时，从 `RX_POOL` 分配一个 `RxBuff_t`，将其 `buff` 地址直接交给 DMA 描述符
2. `HAL_ETH_RxLinkCallback()` — 当一帧数据接收完成，HAL 将多个 buffer 链成一个 pbuf 链表
3. `pbuf_free_custom()` — 应用层释放 pbuf 时，自动将 buffer 回收到 `RX_POOL`

**关键点**：`buff` 字段通过 `__ALIGNED(32)` 确保 32 字节对齐，满足 STM32F4 的 L1-Cache 行大小要求。

### 3.2 接收流程（中断驱动）

```
ETH 中断 → HAL_ETH_RxCpltCallback() → osSemaphoreRelease(RxPktSemaphore)
                                              ↓
                              ethernetif_input 线程(优先级=osPriorityRealtime)
                                              ↓
                              osSemaphoreAcquire 等待信号量
                                              ↓
                              low_level_input() → HAL_ETH_ReadData()
                                              ↓
                              获得 struct pbuf *
                                              ↓
                              netif->input(p, netif)  // 投递到 lwIP 协议栈
                                              ↓
                              tcpip_input() 处理 IP 层
```

### 3.3 发送流程

```c
static err_t low_level_output(struct netif *netif, struct pbuf *p)
{
  // 遍历 pbuf 链表，填充 ETH_BufferTypeDef 数组
  for(q = p; q != NULL; q = q->next) {
    Txbuffer[i].buffer = q->payload;
    Txbuffer[i].len = q->len;
    i++;
  }
  TxConfig.TxBuffer = Txbuffer;
  pbuf_ref(p);  // 增加引用计数防止被释放

  do {
    if(HAL_ETH_Transmit_IT(&heth, &TxConfig) == HAL_OK) {
      errval = ERR_OK;
    } else if(HAL_ETH_GetError(&heth) & HAL_ETH_ERROR_BUSY) {
      // DMA 描述符忙 → 等待前一帧发送完成
      osSemaphoreAcquire(TxPktSemaphore, ETHIF_TX_TIMEOUT);
      HAL_ETH_ReleaseTxPacket(&heth);
      errval = ERR_BUF;  // 重试
    }
  } while(errval == ERR_BUF);
}
```

**发送确认**：`HAL_ETH_TxCpltCallback()` 中释放 `TxPktSemaphore`，`HAL_ETH_TxFreeCallback()` 中调用 `pbuf_free()` 释放应用层持有的引用。

### 3.4 TX Checksum 硬件卸载配置

```c
TxConfig.Attributes = ETH_TX_PACKETS_FEATURES_CSUM | ETH_TX_PACKETS_FEATURES_CRCPAD;
TxConfig.ChecksumCtrl = ETH_CHECKSUM_IPHDR_PAYLOAD_INSERT_PHDR_CALC;
TxConfig.CRCPadCtrl = ETH_CRC_PAD_INSERT;
```

STM32 ETH MAC 自动完成：
- IP 头 checksum 插入
- TCP/UDP payload checksum 插入
- 自动填充 CRC
- 自动 padding 到最小帧长

### 3.5 PHY 管理 (`DP83848`)

- `ethernetif_init()` 调用 `low_level_init()`，后者初始化 DP83848 PHY
- PHY 链路状态轮询由独立的 `EthLink` 线程 (`ethernet_link_thread`) 每 100ms 执行
- 链路断开 → `HAL_ETH_Stop_IT()` + `netif_set_down()` + `netif_set_link_down()`
- 链路恢复 → 配置 MAC duplex/speed → `HAL_ETH_Start_IT()` + `netif_set_up()` + `netif_set_link_up()`

### 3.6 关键回调注册

```c
// ethernetif_init 中注册
netif->output      = etharp_output;  // IPv4 发送路径（带 ARP 解析）
netif->linkoutput  = low_level_output; // 实际硬件发送函数
```

`output` 负责 IP→MAC（ARP 查询），`linkoutput` 负责实际的以太网帧发送。

---

## 四、lwIP App 层初始化 (`LWIP/App/lwip.c`)

### 4.1 `MX_LWIP_Init()` 调用流程

```c
void MX_LWIP_Init(void)
{
  // 1. 设置静态 IP 地址
  IP_ADDRESS[0]=192; IP_ADDRESS[1]=168; IP_ADDRESS[2]=5; IP_ADDRESS[3]=10;     // 192.168.5.10
  NETMASK_ADDRESS: 255.255.255.0
  GATEWAY_ADDRESS: 192.168.5.1

  // 2. 启动 lwIP 核心 (WITH_RTOS 模式)
  tcpip_init(NULL, NULL);  // 创建 tcpip_thread

  // 3. 添加网络接口
  netif_add(&gnetif, &ipaddr, &netmask, &gw, NULL, &ethernetif_init, &tcpip_input);
  //   ↑ 全局 netif    ↑ 初始化函数       ↑ 输入函数(tcpip线程安全投递)

  // 4. 设为默认网口
  netif_set_default(&gnetif);

  // 5. 启用网口
  netif_set_up(&gnetif);

  // 6. 注册链路状态回调
  netif_set_link_callback(&gnetif, ethernet_link_status_updated);

  // 7. 创建 EthLink 线程(优先级 osPriorityBelowNormal，轮询 PHY 状态)
  osThreadNew(ethernet_link_thread, &gnetif, &attributes);
}
```

### 4.2 `tcpip_input` 的关键作用

`netif_add` 的最后一个参数 `&tcpip_input` 决定了数据包的输入路径：
- ETH 中断 → `ethernetif_input` → `netif->input(p, netif)` → **`tcpip_input`**
- `tcpip_input` 将 pbuf 通过 mbox 投递到 `tcpip_thread`，**实现线程安全**
- `tcpip_thread` 内部逐层解析：以太网帧 → ARP 或 IP → TCP/UDP → 应用回调

### 4.3 运行时线程结构

```
tcpip_thread         (优先级 24) — lwIP 核心协议栈
  │
EthIf 线程           (优先级 osPriorityRealtime) — 以太网帧接收
  │  等待 RxPktSemaphore → low_level_input → netif->input → tcpip_input mbox
  │
EthLink 线程          (优先级 osPriorityBelowNormal) — PHY 链路状态轮询 (100ms)
  │
ModbusTask            — Modbus 业务逻辑
MilkTeaApp            — 奶茶机主业务
DefaultTask           — 网络监控 (AppNetMonitor_Process) + LED 控制
```

---

## 五、lwIP 编程模型 — Netconn API vs Socket API

本工程**同时使用了 lwIP 的两种编程模型**：

### 5.1 Netconn API — 机器人 TCP 客户端
**文件**: `transport_tcp.c`（通过 `s_xTcpOps` 操作表）

```
连接建立:
  netconn_new(NETCONN_TCP) → netconn_set_recvtimeout/sendtimeout → netconn_connect()

数据发送:
  netconn_write_partly(conn, data, len, NETCONN_COPY, &written) // 循环直到全部发送

数据接收:
  netconn_recv(conn, &buf) → netbuf_copy_partial(buf, dest, len, offset)

连接关闭:
  netconn_close(conn) → netconn_delete(conn)
```

**Netconn 的特点**：
- lwIP 内部通过 API mbox 与 tcpip_thread 通信，**天然线程安全**
- 数据拷贝语义（`NETCONN_COPY` 会从应用 buffer 复制到发送队列）
- 适合 **连接数少、数据量适中** 的 C/S 模型

### 5.2 BSD Socket API — 外部 Modbus TCP Server
**文件**: `app_modbus_service.c` + `transport_tcp.c`（通过 `s_xTcpSocketOps` 操作表）

```
Listener 创建:
  lwip_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)
  → lwip_setsockopt(SO_REUSEADDR)
  → lwip_bind(INADDR_ANY, port)
  → lwip_listen(backlog)
  → lwip_ioctl(FIONBIO)  // 设为非阻塞

多客户端 select 轮询:
  FD_ZERO → FD_SET(listener + all active clients)
  → lwip_select(maxfd+1, &readset, NULL, NULL, &timeout)
  → FD_ISSET(listener) → lwip_accept() → 分配到固定 slot
  → FD_ISSET(client)  → lwip_recv(MSG_PEEK) 先窥探 MBAP 头

客户端 socket 配置:
  lwip_ioctl(FIONBIO)           // 非阻塞
  lwip_setsockopt(SO_KEEPALIVE) // TCP KeepAlive
  lwip_setsockopt(TCP_KEEPIDLE/TCP_KEEPINTVL/TCP_KEEPCNT)  // KeepAlive 参数
  lwip_setsockopt(SO_SNDTIMEO)  // 发送超时

连接终止(含 RST):
  lwip_setsockopt(SO_LINGER, {onoff=1, linger=0})  // 立即 RST 不等待 TIME_WAIT
  lwip_shutdown(SHUT_RDWR) → lwip_close()
```

**Socket API 的特点**：
- 与 POSIX 兼容，代码可移植性好
- 使用 `lwip_select()` 实现一个线程管理多达 6 个并发客户端
- 非阻塞模式 + select 多路复用，避免为每个连接创建独立线程
- `MSG_PEEK` + `FIONREAD` 机制：先窥探 MBAP 头获取帧长度，再检查是否有完整帧

### 5.3 两种 API 的选择原因

| 场景                         | API             | 原因                                        |
| ---------------------------- | --------------- | ------------------------------------------- |
| 机器人连接(1对1)             | Netconn         | 简单、线程安全、阻塞语义直观                |
| 外部系统(1对多, 最多6客户端) | Socket + select | 需要多路复用，select 让一个线程管理多个连接 |

---

## 六、网络状态监控 (`app_net_monitor.c`)

### 6.1 通信就绪判断

```c
static uint8_t prvIsCommunicationReady(void) {
  return netif_is_up(&gnetif)           // 网口已启用
      && netif_is_link_up(&gnetif)      // PHY 链路已连接
      && (ip4_addr_get_u32(netif_ip4_addr(&gnetif)) != 0U); // 已获得IP地址
}
```

三层检查：**接口管理状态 + 物理链路状态 + IP 配置状态**

### 6.2 LED 状态机

```
OFFLINE ──(5s 无连接)──→ RECONNECTING ──(5s 仍无连接)──→ OFFLINE
   ↑                         │  ↓ blink(1s周期)
   │                         │  ↓ PHY 硬件复位 (dp83848_hw_reset())
   │                   (连接恢复) → ONLINE
   │                                      │
   └────────────── (连接丢失) ─────────────┘
```

- **ONLINE**: LED 常亮
- **OFFLINE**: LED 熄灭
- **RECONNECTING**: LED 以 1s 周期闪烁，**同时触发 `dp83848_hw_reset()` 硬件复位 PHY**

`AppNetMonitor_Process()` 由 DefaultTask 每 100ms 调用一次。

---

## 七、完整数据流追踪

### 7.1 接收路径 (外部客户端发送 Modbus 请求)

```
1. ETH 硬件收到以太网帧
      ↓
2. ETH DMA 中断 → HAL_ETH_RxCpltCallback()
      ↓
3. osSemaphoreRelease(RxPktSemaphore) → 唤醒 EthIf 线程
      ↓
4. ethernetif_input() → low_level_input() → HAL_ETH_ReadData() → struct pbuf *
      ↓
5. netif->input(p, netif) → tcpip_input(p, netif)
                            (通过 mbox 投递到 tcpip_thread)
      ↓
6. tcpip_thread: 以太网帧解析→ARP(如需要)→IP层→TCP层
      ↓
7. TCP 数据到达 lwIP socket API 层
      ↓
8. app_modbus_service.c: lwip_select() 返回, FD_ISSET(client) = true
      ↓
9. prvProcessExternalClient():
   lwip_recv(MSG_PEEK) → 读取 MBAP 头 → 解析长度
   lwip_ioctl(FIONREAD) → 检查完整帧是否到达
   xModbusTcpServerProcess() → 解析 Modbus → 调用外部系统模型
      ↓
10. 响应通过 lwip_send(MSG_DONTWAIT) 非阻塞发送
       ↓
11. lwIP 内部: socket→netconn→tcpip_thread→tcp_output→ip_output→etharp_output
       ↓
12. netif->linkoutput = low_level_output → HAL_ETH_Transmit_IT → DMA 发送
```

### 7.2 机器人通信路径 (读取机械臂寄存器)

```
RobotTcp Task:
  xAppModbusRobotOpen() → xTransportOpen(s_xRobotChannel)
    → prvOpenClient() → netconn_new + netconn_connect
      ↓
  xAppModbusRobotReadHolding(addr, qty, buf)
    → xModbusTcpClientReadHoldingRegisters()
      → xTransportSend() → prvSend() → netconn_write_partly()  发送 FC03 请求
      ↓
  (等待响应)
      → xTransportReceive() → prvReceive() → netconn_recv() → netbuf_copy_partial()
      → 解析 Modbus 响应
```

---

## 八、关键设计要点总结

### 8.1 资源约束与编译期检查

工程在 `app_modbus_service.c` 中显式约束资源：

```c
#if (APP_MODBUS_EXTERNAL_MAX_CLIENTS > 6U)
#error APP_MODBUS_EXTERNAL_MAX_CLIENTS exceeds current LwIP resource design
#endif
#if (MEMP_NUM_NETCONN < (APP_MODBUS_EXTERNAL_MAX_CLIENTS + 2U))
#error MEMP_NUM_NETCONN cannot support listener, slots, and robot client
#endif
#if (MEMP_NUM_TCP_PCB < (APP_MODBUS_EXTERNAL_MAX_CLIENTS + 2U))
#error MEMP_NUM_TCP_PCB cannot support clients and recovery headroom
#endif
#if (LWIP_SO_RCVBUF == 0)
#error LWIP_SO_RCVBUF is required by the MilkTea FIONREAD frame guard
#endif
```

`lwipopts.h` 中 `MEMP_NUM_NETCONN=8`，对应：1个 listener + 6个外部客户端 + 1个机器人客户端 = 8。

### 8.2 安全性设计

- **任务隔离**：每个协议对象归属于创建它的任务，不跨任务直接操作
- **临界区保护**：状态快照通过 `taskENTER_CRITICAL/taskEXIT_CRITICAL` 保护
- **非阻塞 socket**：所有外部客户端 socket 设为非阻塞 + select，防止单个慢客户端阻塞整个服务
- **SO_LINGER RST**：拒绝或关闭连接时设 `linger{1,0}`，立即发送 RST 而不是进入 TIME_WAIT，释放 PCB 资源
- **多层超时**：帧超时、空闲超时、连接超时三个维度的超时保护

### 8.3 性能优化

- **Zero-Copy RX**：ETH DMA 直接写入 lwIP pbuf pool，无需额外拷贝
- **硬件 checksum**：IP/TCP/UDP checksum 由 MAC 硬件完成
- **pbuf_custom**：自定义 pbuf 释放回调，buffer 回收不需要额外拷贝
- **MSG_DONTWAIT**: socket 发送使用非阻塞模式，配合 polling loop 实现带超时的可靠发送

---

这就是 MilkTea 项目中 lwIP 的完整技术栈——从 `lwipopts.h` 的编译期配置，到 `ethernetif.c` 的 Zero-Copy DMA 驱动，到 `lwip.c` 的 tcpip 多线程初始化，再到 Transport 层的 Netconn/Socket 双 API 封装，最终服务于 Modbus TCP 的工业通信场景。
