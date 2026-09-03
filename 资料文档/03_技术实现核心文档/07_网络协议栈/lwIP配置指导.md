# lwIP 配置指导（工程维护参考指南）

> 适用基线：2026-08-27 当前 Workspace，Coffee2 Target。  
> 建议口径：**2 个 TCP Server（6001 四槽 + OTA/80）+ 3 个机器人 client**。  
> 文档定位：让“懂计算机网络、未接触过 lwIP”的工程师**看懂每一项配置、每个目标**，可直接按表格落地。  
> 数值来源：正本 `CubeMX_Base/LWIP/Target/lwipopts.h` 与 `CubeMX_Base/Middlewares/Third_Party/LwIP/src/include/lwip/opt.h`。

---

# 一、lwIP 基础认知

> 目标：让你在改任何一个宏之前，先建立一张“能进出的图”。
> 关键认知：**lwIP 不是一堆可调用库函数，而是一个“自己会跑的东西”**——它有自己的线程（`tcpip_thread`）、自己的内存管理器（`mem`/`memp`）、自己的一堆内存池。你配置的每一项，本质上是给这个系统“发配额”。

## 1. lwIP 是什么 / 用哪一版

- **版本**：lwIP 2.1.2（CubeMX 生成）。
- **模式**：`NO_SYS = 0`，即“有操作系统”模式，跑在 FreeRTOS 之上、独享一个 `tcpip_thread` 线程。
- **用途**：让 STM32 做 TCP 服务端**或**客户端，承载本工程的：上位机 Server(6001)、OTA HTTP(80)、3 个机器人 Modbus TCP client(502)。
- **为什么选它**：嵌入式里 TCP/IP 栈的“标准答案”，RAM 可控、专为 MCU 设计。

## 2. lwIP 与你已掌握的网络概念对照

| 你已经懂的概念 | 在 lwIP 里的落地 |
|---|---|
| 网卡 / 接口 | `struct netif`（本机 IP、up/down、MAC） |
| IP 地址表 | `netif` 里的 IPv4/IPv6 地址 |
| ARP 缓存表 | `ETHARP`（以太网 ARP 协议） |
| 一个 TCP 连接 | 服务端/客户端都走 `netconn` 或 `socket` 接口 |
| 承载网络数据的缓冲 | `struct pbuf`（你写代码接触最多的类型） |
| 每个 TCP 连接的控制块 | `struct tcp_pcb`（池里一个连接占一个） |
| 发送/接收窗口 | `TCP_SND_BUF`（发）、`TCP_WND`（收） |
| 一个“待发送的段” | `struct tcp_seg` |
| 线程安全接口层 | `netconn`（Socket 精华）上层，Robot 用；`socket` 上位机 Server 的用户 |

---

# 二、lwIP 的工作流程（数据包怎么走）

## 3. 一帧数据的旅程（图示）

```text
  [PHY 网口] ──(收帧)──> [ETH DMA] ──> [tcpip_thread：tcpip_input()]
        ──> 交给 IP / TCP / ARP 协议处理 ──> (Socket/Netconn 邮箱) ──> 业务任务(FreeRTOS)
  业务任务要发(如 Server 回一个 Modbus 帧)──> netconn_write / socket send ──> tcpip_thread：tcpip_msg
        ──> [ETH DMA TX] ──> [PHY 发出去]
```

- 所有发包、收包、重传、ACK、ARP、状态机，**全部在 `tcpip_thread` 里跑**。
- 你的业务任务只是“通过 Socket / Netconn 接口把数据交给这个线程”，自己不碰协议。
- 因此：`tcpip_thread` 的栈、邮箱、以及它要用的内存池，直接决定网络是否卡。

## 4. 本工程的连接拓扑（3 client + 2 server）

```text
上位机1..4 ──▶ C2Server（Socket 监听 6001，4 槽）────┐
OTA 下载工具 ─▶ OTA(HTTP 监听 80，1 会话)           ├─▶ tcpip_thread
机器人 client1..3 ─▶ robot_tcp（Netconn 连 502）────┘        └─▶ ETH DMA ─▶ PHY
```

**PCB 需求不是“连接数”，而是“连接数 + 过渡态”**：
`活动连接 + SYN_SENT(发起中) + FIN_WAIT/LAST_ACK(关闭中) + TIME_WAIT(关后冷却) + 同时重连的新旧连接 + 安全余量`。
这正是下面 `MEMP_NUM_TCP_PCB` 要按 8+ 配置、甚至走到 20 的原因。
---

# 三、lwIP 的对象体系（把这些“部件”先认识清楚）

> 这一节讲清楚 lwIP 内部“有哪些东西、各管什么”——配表时你能对着名字 1:1 找到它。

## 5. 协议与对象总览

### 5.1 网络层协议开关
| 协议 | 本工程 | 作用 | 是否需要 |
|---|---|---|---|
| `LWIP_ARP` | 1 | 局域网 IP→MAC 解析，以太网 IPv4 必需 | 必须 |
| `LWIP_ICMP` | 1 | ping/回显，现场诊断用 | 保留 |
| `LWIP_DHCP` | 0(GUI)-静态IP | 自动获取 IP；本机固定 IP 所以关 | 静态可关 |
| `LWIP_DNS` | 0 | 域名解析；本工程全用 IP，**已关** | 可关 |
| `LWIP_IPV4 / IPV6` | 1 / 0 | IPv4 开启、IPv6 关闭 | 保持 |
| `LWIP_UDP` | 0 | UDP；本工程只跑 TCP，**已关** | 可关 |

### 5.2 传输层
| 对象 | 本工程 | 说明 |
|---|---|---|
| `LWIP_TCP` | 1 | TCP 栈本体，所有业务都靠它 | 必须 |
| `LWIP_UDP` | 0 | 无 UDP 业务 | 已关 |
| `LWIP_RAW` | 0 | 原始 IP 包(无 L4 头)；OTA 用的是 TCP callback，不是 RAW | 关 |

### 5.3 lwIP 的对象（内存视角核心）
| 对象 | 是什么 | 一个占多大 | 太少会怎样 |
|---|---|---|---|
| `MEMP_NUM_PBUF` | pbuf 结构对象 | 小头 | 收发包分配失败 |
| `MEMP_NUM_TCP_PCB` | 每个 TCP 连接的“连接控制块” | 约 164B | 新建连接失败(connect/accept 失败) |
| `MEMP_NUM_TCP_PCB_LISTEN` | 监听(服务端 listen)的 PCB | 小 | 无法再监听新端口 |
| `MEMP_NUM_TCP_SEG` | 待发送 segment 描述符 | 元数据小 | 发送阶段 `ERR_MEM` |
| `MEMP_NUM_NETCONN` | Socket/Netconn 上层对象 | 约 56B+OS 对象 | socket 创建失败 |
| `MEMP_NUM_NETBUF` | Netconn 接收缓冲对象 | 小 | 并发 recv 失败 |
| `MEMP_NUM_TCPIP_MSG_API` | netconn 调用投递的消息 | 小 | API 调用发不出去 |
| `MEMP_NUM_TCPIP_MSG_INPKT` | 收包投递消息 | 小 | 收包突发时丢投递 |
| `PBUF_POOL_SIZE` | 通用 pbuf 池 | 约 1.5KB/个 | 丢包、TCP 卡 |
| `PBUF_POOL_BUFSIZE` | 每个 pbuf 载荷大小 | 好 | 太小要拆链 |

> 记忆法：**PCB=连接、SEG=待发段、NETCONN/NETBUF=上层接口、TCPIP_MSG=线程间消息、PBUF=数据缓冲**。前几个是“结构对象”，最后一个是“真正的数据内存”。

## 6. 内存模型图解（这块最关键）

```text
┌────────────────────── STM32F407 SRAM(0x20000000, 128KiB) ──────────────────────┐
│                                                                            │
│   lwIP 通用堆  mem_heap  ← 运行时可分配的“再分配池”，TCP_SND_BUF 从这里拿   │
│        ┌───────────────────────────────────────────────────────────┐       │
│        │  TCP_PCB 池 / NETCONN 池 / TCP_SEG 池 / PBUF_POOL 池 / ... │  ← 固定大小内存池        │
│        │  (一张张“房间”，各住一种对象，满了就失败)                     │       │
│        └───────────────────────────────────────────────────────────┘       │
│   ETH DMA RX_POOL / RX/TX Descriptor  ← 网卡零拷贝搬数据用                  │
└──────────────────────────────────────────────────────────────────────┘
```

**两条内存来源，你要分清楚：**
1. `mem_heap`(MEM_SIZE) —— 通用堆，动态分配，例如 TCP 发送缓冲。
2. `memp_pools`(各 MEMP_NUM_* ) — 固定大小池，每池一张表，超出就报错。

> `MEM_SIZE` 增加 → 通用堆更大（更抗瞬时，但静占 SRAM）。
> `MEMP_NUM_*` 增加 → 对应池的“座位”更多（抗并发连接）。
> **两者不是一回事**：加连接数是加 pool，别靠狂加 MEM_SIZE。
---

# 四、核心配置表（当前 vs 建议）

> “当前”= 正本 `lwipopts.h` + `opt.h` 实际生效值；  
> “建议”= 面向 **3×robot client + 2×server** 的推荐值；  
> 单位：内存结构是 SRAM 固定占用，数量关系 = 并发对象数。

## 7. 一组“必懂的分表”

### 7.1 基础协议开关
| 参数 | 当前 | 建议 | 作用 / 后果 |
|---|---|---|---|
| `LWIP_ARP` | 1 | 1 | 以太网必须，路由地址解析 |
| `LWIP_ICMP` | 1 | 1 | ping 诊断 |
| `LWIP_IPV4 / IPV6` | 1 / 0 | 1 / 0 | 只用 IPv4 |
| `LWIP_DHCP` | 0 | 0 | 静态 IP，关闭自动获取 |
| `LWIP_DNS` | 0 | 0 | 全用 IP，无需域名 |
| `LWIP_UDP` | 0 | 0 | 无 UDP 业务，已关 |
| `LWIP_RAW` | 0 | 0 | 不需要原始 IP 包 |
| `LWIP_TCP` | 1 | 1 | TCP 本体 |

### 7.2 系统/线程
| 参数 | 当前 | 建议 | 作用 |
|---|---|---|---|
| `NO_SYS` | 0 | 0 | 有 OS，用 tcpip_thread |
| `SYS_LIGHTWEIGHT_PROT` | 1 | 1 | 中断/任务保护 |
| `LWIP_TCPIP_CORE_LOCKING` | 1 | 1 | 核心锁 |
| `TCPIP_THREAD_STACKSIZE` | 2048 | 2048 | lwIP 线程栈(字节) |
| `TCPIP_MBOX_SIZE` | 6 | **12** | 线程消息邮箱，多连接时偏小 |
| `MEMP_NUM_TCPIP_MSG_API` | 8 | **12** | API 投递消息，多 client 偏紧 |
| `MEMP_NUM_TCPIP_MSG_INPKT` | 8 | **12** | 收包投递，多连接并发收包偏紧 |

### 7.3 Socket / Netconn 对象池
| 参数 | 当前 | 建议 | 理由(3 client 视角) |
|---|---|---|---|
| `LWIP_NETCONN / LWIP_SOCKET` | 1/1 | 1/1 | Robot 用 netconn，Server 用 socket |
| `MEMP_NUM_NETCONN` | 12 | **16** | 4server槽+3client+瞬态 |
| `MEMP_NUM_NETBUF` | 10 | 10 | 短报文足够 |
| `MEMP_NUM_SOCKET_SETGETSOCKOPT_DATA` | (=API) | 保持 | socket 选项用小结构 |

### 7.4 内存池（核心，最该关心）
| 参数 | 当前 | 建议 | 一个约多大 | 太少后果 |
|---|---|---|---|---|
| `MEM_SIZE` | 32768 | **24576** | 通用堆 | 留下 8KB；调低的依据见§9 |
| `MEMP_NUM_PBUF` | 16 | 16 | 小 | 收发包失败 |
| `MEMP_NUM_TCP_PCB` | 16 | **20** | ~164B | accept/connect 失败 |
| `MEMP_NUM_TCP_PCB_LISTEN` | 3 | 3 | 小 | 无法监听更多端口 |
| `MEMP_NUM_TCP_SEG` | 32 | 32 | 元数据 | 发送 ERR_MEM |
| `MEMP_NUM_NETCONN` | 12 | **16** | 56B | 创建失败 |
| `MEMP_NUM_NETBUF` | 10 | 10 | 小 | recv 失败 |
| `PBUF_POOL_SIZE` | 12 | 12 | 1.5KB | 丢包 |
| `PBUF_POOL_BUFSIZE` | 1536 | 1536 | — | 容纳整帧 |

> ★ `MEMP_NUM_TCP_PCB` 建议 20：**8 个稳态活动连接（4server+1OTA+3robot）+ 若干 SYN/FIN/TIME_WAIT 过渡 + 3 robot 同时重连旧连接回收交叠**。16 可跑，20 更稳。

### 7.5 TCP 参数（窗口/重传）
| 参数 | 当前 | 建议 | 作用 |
|---|---|---|---|
| `TCP_MSS` | 1460 | 1460 | 单段最大载荷(1500-20-20) |
| `TCP_SND_BUF` | 2920 | 2920 | 发送缓冲=2×MSS |
| `TCP_WND` | 5840 | 5840 | 接收窗口=4×MSS |
| `TCP_SND_QUEUELEN` | 9 | 9 | 发送队列项上限 |
| `TCP_SNDQUEUELOWAT` | 5 | 5 | 低水位 |
| `TCP_MAXRTX` | 12 | 12 | 数据重传上限 |
| `TCP_SYNMAXRTX` | 6 | 6 | SYN 重传上限(决定 connect 超时一段约 63s) |
| `TCP_TMR_INTERVAL` | 250 | 250 | TCP 定时器周期(默认) |

> `TCP_SYNMAXRTX=6`：SYN 超时 ≈ 1+2+4+8+16+32≈63s。若想“机器人晚于主控启动时更快重连”，可降到 3(~15s)。但它和 connect 幂等软件重连配合，通常保持 6 即可。

### 7.6 校验和/诊断
| 参数 | 当前 | 建议 | 说明 |
|---|---|---|---|
| `CHECKSUM_BY_HARDWARE` | 1 | 1 | ETH MAC 硬件校验 |
| `CHECKSUM_GEN_*` / `CHECK*` | 0 / 0 | 0 / 0 | 交给硬件 |
| `LWIP_STATS` / `MEM_STATS` | 1 / 1 | 1 / 1 | 内存统计，排障用 |
| 各种 `*_DEBUG` | OFF | OFF | 平时关，需要再开 |

---

## 8. 容量测算（3-client + 2-server 依据）

| 需要 | 数量 |
|---|---|
| Server 1(6001) 槽 | 4 |
| OTA(80) | 1 |
| Robot client | 3 |
| 静态活跃 TCP PCB | 8 |
| 过渡态/重连 | 3~4 |
| **TCP PCB 需要** | **≈11~12，建议 20** |

```text
4(server) + 1(OTA) + 3(robot) = 8 稳态
+ 3~4(过渡态: 关闭回收/重连/new SYN) = 11~12
+ 余量 => 建议 20
```
---

# 五、MEM_SIZE = 32768 是否过大 —— 综合评估

## 9. 为什么说“可以降到 24576”（你的直觉是对的）

**MEM_SIZE = lwIP 通用堆 `mem_heap`**，其用途与 lwIP 的固定池不同：
- 用途：TCP 发送缓冲、临时 PBUF_RAM、程序运行中的临时分配。
- 本工程业务 = 机器人短 Modbus 帧(~几十字节) + 上位机短报文 + OTA 大上传。

**关键判断**：
1. Robot 报文小、上位机报文短，后续 CON 大吞吐，heap 长期占用低。
2. OTA 大固件走 `ETH RX_POOL` 零拷贝 + 分块发送，**不大量吃 `mem_heap`**。
3. 3 个 robot 加的是 **MEMP 池对象(小)**，不是 heap 冲量。
4. CubeMX 对照区原始默认是 24576，正本被改成 32768——而你的场景并没有需要它高到 32768 的大缓存冲击。

**结论**：`MEM_SIZE 32768 → 24576` 可省**约 8KB SRAM**，且能抵消 3-client 新增 pool 的几 KB 增量。这比“单纯加 pool”更省 RAM，是当前对内存压力最有效的一步。

⚠ 前提：OTA 下载 + 4server + 3robot 并发实测，确认 `mem.err` 统计无增长、无 `MEM_ALLOC_FAILED`。若后续 OTA 大包把 heap 拉满，再回升到 32768。

```text
减少8KB(静态 SRAM)  ──▶ RAM 从 ~77% 降到 ~72%
新增 3-client pool   ──▶ 回升到 ~74%
净效果：仍比现状省，且能容纳 3 client
```

---

# 六、参数作用全图解（每个关键参数一图）

## 10. CPU 线程与邮箱
```text
tcpip_thread (跑 TCP/IP/ARP/重传)
    ├─ 输入邮箱 TCPIP_MBOX_SIZE=12  ← 收包/控制消息队列
    ├─ 栈 TCPIP_THREAD_STACKSIZE=2048
    └─ 连接池: PCB(20)/NETCONN(12)/SEG(32)   ← 并发座位
```

## 11. MEM_SIZE vs MEMP 池（用一张图对比）
```
MEM_SIZE (mem_heap 通用堆)      MEMP_NUM_* (固定池)
+---------------------+      +----------------------+
| 运行期动态分配      |      | PCB / SEG / NETCONN   |
| TCP_SND_BUF 等     |      | PBUF / NETBUF / MSG   |
+---------------------+      +----------------------+
  增加: 耗 RAM          增加: 抗并发
```

## 12. TCP_PCB 紧张表现
```text
connect/accept 返回 ERR_MEM / lwip_socket/fd 为负数
→ MEMP_NUM_TCP_PCB 池不够 → 加 16→20
```

---

# 七、排障速查（给现场的浓缩版）

| 现象 | 优先查 | 说明 |
|---|---|---|
| 能 ping 但 TCP 连不上 | 服务端是否监听/防火墙/IP | 非阻塞 connect 确认 ESTABLISHED |
| TCP 偶尔断开 | PHY/link/网线/重传/PCB | 看 stats 是否增长 |
| TCP 建立但协议失败 | 功能码/unitid/事务号 | Robot 首次 Modbus probe |
| 多客户端后异常 | PCB/NETCONN/NETBUF/TCPIP_MSG | 4槽≠只要4个PCB |
| 长时间连不上 | used/max/err 统计、close/delete | 查泄漏 | 
| 运行后 HardFault | 栈溢出/pbuf 泄漏/DMA 进 CCM/跨线程 | 查 ELF/MAP |

> 错误分类提醒：**不要把所有“连不上/发不出”都当协议错误**——先分清 PHY/IP/ARP/TCP/Socket/Protocol/Device 七层（见上文第四大节的参数表如何定位各层）。

---

# 八、落地与双工具链

## 13. 修改哪个文件（关键：只改一份）

> **正本** = `CubeMX_Base/LWIP/Target/lwipopts.h`（两工具链都 include 它）。  
> ⚠ `CubeMX_Base/CubeMX_Genarate/...` 是 CubeMX 临时对照区，**不参与编译**，别改错。

- 手工参数：加在 lwipopts.h 的 `USER CODE BEGIN 0` 段（CubeMX 再生会保留该段）。
- 别再单靠 CubeMX 面板：每次重新生成后务必回看 `lwipopts.h`，面板改动未必落盘。

## 14. GCC 工具链编译
```shell
cd d:\Project_Items\Project_Base\GCC-ARM
.tools\python\cmake\data\bin\cmake.exe --build build\Coffee2-Debug -j 4
```
产物：`build\Coffee2-Debug\Coffee2Target.bin/.hex/.elf`  
烧录：`scripts\flash.ps1 -Product Coffee2 -Configuration Debug -Backend OpenOCD -Program`

## 15. KEIL 工具链编译
1. 打开 `MDK-ARM\STM32F407_Base.uvprojx`，选 **Coffee2** Target。
2. **Rebuild(全部重建)**。
3. 产物：`MDK-ARM\Objects\Coffee2\Coffee2.hex`（ST-LINK 烧录）。

> 两者都引用同一份 `CubeMX_Base/LWIP/Target`，所以**改一处，两工具各自重新编译**即可，无需维护两套 lwIP 配置。

## 16. 结论

1. lwIP 采用“有 OS + tcpip_thread”模式，业务用 socket/netconn 接口。
2. **连接是加 MEMP 池，不是加 MEM_SIZE**。
3. 建议：`MEM_SIZE 24576`、`MEMP_NUM_TCP_PCB 20`、`NETCONN 16`、`TCPIP_MBOX/MSG 12`。
4. DNS/UDP 关掉正确（本工程不用），RAM 收益主要来自降 MEM_SIZE。
5. 修改只动 `CubeMX_Base/LWIP/Target/lwipopts.h`，两工具链各自用。  
6. 排序：先做“降 MEM_SIZE + 小幅抬 pool”，性价比最高。
