# Coffee3 存储位调试拒绝与非 OTA 持久化规划（审批稿）

> 方案部分已由《Coffee1持久化全量调研与分类存储落地方案V2.md》替代；本文件保留为首轮调查记录，不再作为最新实施依据。

日期：2026-09-11。本轮只调研和输出方案，未修改固件、未编译、未烧录。

## 一、结论：不是机器人拒绝，也不是 Flash 掩码导致本次拒绝

日志 `MANUAL_ROBOT_POSITION_UNSUPPORTED result=-1 position=21/22` 来自 Coffee3 Server 的 `prvSubmitRobotPosition()`。该 switch 只映射 0x0000～0x000F 中的位置，缺少 0x0015、0x0016，所以进入 default 后直接返回，尚未向机器人任务投递命令。

注意数字含义：日志 21、22 是十进制，分别为 0x0015、0x0016；它们是写入位置寄存器 0x0031 的命令值，不是存储位配置寄存器地址，也不是机器人线圈地址。

残杯流程走另一条链：Workflow → `COFFEE3_ACTION_ROBOT_PUT_STORAGE`，参数 1/2 → Robot owner。现有 `prvExecute()` 已验证参数范围 1～2，并调用 `vCoffee3ServerSelectStorage()` 把位置选择发布到 0x0032，然后执行放存储位协议动作。因此它可以执行，而手动位置入口缺映射。

当前 `prvSelectStorage()` 只检查 X1/X2 的空位状态，循环上限为 2，没有加载持久化开放掩码。不能把本次错误解释成“Flash 没有开放两个存储位”；这是下一项配置功能尚未接入，不是这条日志的直接原因。

证据：

- `Application/UserAPP/Coffee3CloseApp/Modbus_Tcp_Server/coffee3_server.c`：`prvSubmitRobotPosition`、`vCoffee3ServerSelectStorage`。
- `Application/UserAPP/Coffee3CloseApp/Robot_Tcp/coffee3_robot_tcp.c`：`prvExecute` 的 PUT_STORAGE/TAKE_STORAGE 分支。
- `Application/UserAPP/Coffee3CloseApp/WorkFlow/coffee3_workflow.c`：`prvSelectStorage`、残杯初始化和订单放存储位调用。

## 二、与 Coffee1 对比：开放掩码、位置选择、动作要分开

参考工程为 `D:/Project_Items/Coffee1/coffee_close_v2.8.29_IOPage`。

| 项目 | Coffee1 事实 | Coffee3 现状/建议 |
| --- | --- | --- |
| 手动放存储位 | `modbus/robot.c:1852` 起处理 0x15～0x20，位置号为 control−0x14 | 增加 0x15→PUT_STORAGE(1)、0x16→PUT_STORAGE(2)，不开放不存在的 3～12 |
| 选择具体位置 | 更新 cup_storage、store_cup_pos_to_robot，再执行 robot_put_store_start | 复用现有选择 0x0032 + 动作链，不新增另一条 Modbus 通路 |
| 默认开放配置 | `machine_type.c:88` 设置 `store_cup_valid=0x00FF`，bit0～7 开放 | 默认 0x0003，bit0/bit1 对应存储位 1/2 |
| 上位机配置 | `modbus/order.h` 的 0x001A；`tcp_server.c:1492` 附近单寄存器写入并保存，批量写路径也保存 | 接入 0x001A 的读取、合法性检查、持久化及生效，不是只允许寄存器 RAM 写入 |
| 自动找空位 | `modbus/io_input.c:241` 起同时检查物理空位和开放掩码，最多检查 12 位 | 同时满足已安装、已开放、空位、IO 数据有效；只遍历 1/2 |
| 上电纠正掩码 | Coffee1 把 0/0xFFFF 改回 0x00FF | 不照搬：建议允许 0 表示禁止新增存储；非法记录才恢复默认 0x0003 |

Coffee1 的手动位置分支本身没有读取 `store_cup_valid`；掩码用于初始化配置、自动选位及状态组合。因此“有配置掩码”和“手动命令能否识别”不可混为一谈。默认 0x00FF 是下位机参数，不等于初始化时逐个向机器人写了八条开放命令。

### 建议的操作语义

1. 配置采用掩码，不是数量。0x0001=仅位置1，0x0002=仅位置2，0x0003=两个位置。数量可由掩码计算，不重复存储。
2. Coffee3 物理能力掩码固定 0x0003；上位机写高位非零直接返回非法值，不静默截断。
3. 自动新放杯必须遵守开放掩码。已停用但实际有杯的位置仍保留原始传感器显示，不能伪装成无杯；清理取杯不应仅因停用而被禁止。
4. 手动调试沿用已确认的调试边界：仅支持物理存在的 1/2，保留协议运行就绪条件，不因订单用开放掩码额外拒绝物理位置调试。若希望停用同时禁止手动放杯，应作为单独审批规则，不能悄悄加入。
5. 订单已选定位置时冻结本单配置快照；期间收到配置更新建议拒绝为忙，让上位机稍后重试，避免放杯目标中途改变。此限制针对配置，不是一次性 IO/机器人调试命令。
6. 残杯清理应在初始化前加载配置；按已开放空位选存储目标，出餐口回退保留。所有物理传感器仍参与占用观察。若全部存储被禁用且出餐口无法容纳残杯，应明确报无可用位置，由人工处理。

0x001A 是命令/参数区地址；当前 `s_ausStatusRegisters[0x001A]` 的能耗字段是另一段状态区的数组偏移，实施时必须结合状态区基址，不得据偏移相同误删能耗映射。本文地址依据源码，落地前还需与最新上位机协议文档核对。

## 三、Coffee1 非 OTA Flash 数据清单

对整个参考工程 C/H 搜索写入/擦除、直接地址、EEPROM/W25/文件写入及备份寄存器入口，追踪应用调用者。应用非 OTA 写入集中在 `ControlFlow/machine_type.c`，结构位于 `machine_type.h:23` 起，地址 0x08008000（sector 2）。底层借用 ota/flash_if.c 不代表其业务数据属于 OTA。

Keil `MDK-ARM/coffee.uvprojx` 包含 `ControlFlow/machine_type.c`、`modbus/tcp_client.c`。`review/tcp_client_new.c` 也有旧写入示例，但未在该工程清单发现，不作为运行依据。没有在所查应用源码发现另一套非 OTA Flash/外部 EEPROM 持久化业务；这不是对二进制、外接设备内部存储的保证。

| 字段 | Coffee1 证据/含义 | 当前工程建议 |
| --- | --- | --- |
| head_info[2]、info_size | W/N 标记、结构长度 | 换成 magic、格式版本、长度、target/schema 标识、序号、CRC、提交标记 |
| tcp_client_port | tcp_client.c:892 起连接成功后安排保存下一个端口，:1146 后调用 machine_type_write | 必须保留；当前 Coffee3 是连接前持久化预留端口，保留这个更严格的顺序 |
| store_cup_valid | 默认 0x00FF；0x001A 可配置 | 本次必须新增；默认 0x0003 |
| robot_type | 默认越疆；参数结构有保存和 Server 更新路径 | 当前按 Target 编译选型，不建议本次改成运行时切换 |
| ice_type | 初始化及 0x007F 配置 | 同上；设备库是否编译进入必须匹配，不能只存枚举就声称支持切换 |
| coffee_type、cup_type、lid_type、sugar_type、print_type | 结构存在，但未在所查初始化/Server 中发现这些持久字段的完整赋值配置闭环 | 视为遗留/待核实字段，不照搬；订单 coffee_type 不是这个设备选型字段 |
| machine_select | 0x001F，选择不同整机模式 | 当前由 Coffee2Open/Coffee3Close Target 区分，不持久化成可跨架构切换的开关 |
| fruit_milk_time_coef_ms | 默认100，初始化校验到10～1000 | 当前 COFFEE3_FRUIT_MILK_MS_PER_ML=100；如需要现场校准再接入，不改变单位含义 |
| fruit_milk_coef_chan1～6 | 默认100，0x00AA～0x00AF，Server 可更新保存 | Coffee3 实际通道按私有配置定义，只接真实使用的通道，未用通道不造业务 |
| ice_compensation_coef | 默认10，0x007E，校验1～50 | 后续按实际称重/出冰算法接入；不能仅保存而算法不使用 |
| hot_cup_height_offset、cold_cup_height_offset | 默认0，0x003D/0x003E，按 int16 解读，校验−100～100 | 确认机器人参数读取协议后按需接入；注意持久层有符号表示 |

不建议持久化：实时 IO、3100 就绪、机器人正在执行的动作、手动输出值、临时 ACK、门运动状态。当前订单/存储位订单号也不能仅落盘就当成可断点续做：上电必须重新残杯检查和状态核对。

### Coffee1 实现不宜原样复制的地方

- 每次配置变化先擦整个 sector 2，再写整个结构；断电时可能丢失唯一有效副本。
- 结构校验只有标记/长度，没有 CRC 和提交记录。
- 结构长度变化分支按 Flash 内 `info_size` 直接 memcpy，需补边界检查，不能将损坏长度作为可信值。
- 默认初始化分支未先清空局部结构，部分设备类型/端口字段未显式初始化；不可照搬。
- 0 掩码上电变回 0x00FF，会覆盖“主动禁用所有位置”的意图。
- 端口与低频配置共用整扇区擦写，使频繁重连牵连配置擦写。可共用存储服务，但不能每次预留端口都擦扇区。

## 四、当前 Flash 布局与推荐地址

以下依据当前 Coffee3 OTA 常量、Keil Scatter、GCC linker 和 `coffee_bootloader_v4.0/bootloader/bootloader.c/.h` 的 1MB 布局；使用前仍须确认实机为 1MB，不能用于 512KB 芯片。

| 地址范围（含末地址） | 容量 | 当前用途 | 本方案 |
| --- | --- | --- | --- |
| 0x08000000～0x08003FFF | 16KB | sector0 Bootloader | 不动 |
| 0x08004000～0x08007FFF | 16KB | sector1 OTA元数据 | 不混入业务配置 |
| 0x08008000～0x0800BFFF | 16KB | sector2 保留/旧机器配置 | 保留，不作为唯一新参数副本 |
| 0x0800C000～0x0805FFFF | 336KB | sector3～6 应用 | 不动 |
| 0x08060000～0x080BFFFF | 384KB | sector7～9 OTA暂存 | 不动 |
| 0x080C0000～0x080DFFFF | 128KB | sector10 Coffee3端口日志A | 改为统一参数日志A |
| 0x080E0000～0x080FFFFF | 128KB | sector11 Coffee3端口日志B | 改为统一参数日志B |

注意：“A/B”是软件日志副本，不意味着芯片具备并行读写的硬件双 Bank。

推荐复用 sector10/11，而不是另找一个地址把存储位塞进去。当前 `prvReserveRobotPort()` 会扫描两个完整扇区并在轮转时擦除其中一个；若新增模块直接占这两个扇区的某个小范围，迟早会被旧端口代码擦掉。也不能将一个物理擦除扇区切成两个逻辑区域就声称具备独立擦除和掉电保护。

替代方案是 sector2 保存配置、10/11继续端口日志，但 sector2单副本必须另做备份；最终会引入跨区协议，反而不如统一管理简单。本轮推荐不增加分区、不改变 Bootloader/OTA 边界。

## 五、公共/私有边界与结构体方案

### 1. 最小实现

- 公共 `Application/Common/PersistentStore/`：固定大小记录的扫描、CRC、提交、A/B轮转、HAL错误处理。只认识字节负载及 schema，不认识 Coffee3、存储位、订单或机器人。
- 私有 `Coffee3CloseApp/Config/coffee3_persistent_config.c/.h`：业务结构、默认值、范围校验、上位机配置映射、端口预留算法。
- 公共 Transport 保留现有端口预留回调；由 Target 注入，不直接依赖 Coffee3 参数结构。
- Coffee2 将来可以调用同一存储实现，但有自己的 schema/defaults；不同 Target 固件切换不自动解释彼此的参数。

建议首版私有负载只有当前明确需要的字段：

```c
typedef struct {
    uint32_t robot_port_reservation_sequence;
    uint16_t storage_enabled_mask; /* default 0x0003 */
    uint16_t reserved;             /* explicitly initialized */
} Coffee3PersistentDataV1;
```

记录外壳另含 magic、schema/version、length、record_generation、CRC32、commit。建议固定64字节记录，余量固定填充值；不要将裸结构 padding、指针、enum/位域布局作为 Keil/GCC 共用持久格式。定义精确偏移/字节序并验证长度。

`record_generation` 和端口序号必须分开：保存存储位配置只增加记录代次，不能改变下次端口选择。端口继续使用现有 49152～65535 的算法和独立预留序号。

64字节全量小快照每个128KB扇区可容纳2048条（若加扇区头则相应减少），无需每写一次就擦除。比当前16字节端口记录容量少，但配置与端口一致性和恢复规则更简单；正式实现需根据现场重连频度评估磨损，不在缺少数据时宣称无限寿命。

### 2. 写入、掉电和并发

1. 启动扫描一次，选 CRC/长度/schema/commit 都有效的最新记录，建立静态 RAM 快照和下一写位置；避免每次重连遍历256KB。
2. 保存时从当前快照修改对应字段，先写正文与 CRC，commit 最后写；读回验证后才更新 RAM 和向调用方报告成功。
3. 预留端口必须提交成功后才能 connect。允许掉电跳过一个端口，不允许为了连接成功忽略保存失败。
4. 扇区满时擦另一扇区，写入新完整快照并验证成功后才允许回收旧有效副本。掉电恢复选择最后有效记录。
5. 配置与端口读改写必须在同一个 owner/锁下完成，不能分别持有旧快照互相覆盖。OTA虽然分区不同，仍共享 Flash 控制器，所有写入者必须统一串行化；OTA期间返回忙。
6. 不在 ISR 或网络底层回调擦写。优先复用现有任务上下文，不为几个参数新建 RTOS 任务。同步写入的阻塞时长、擦除时中断/网络响应和看门狗需上板测量；不能把长时间挂起调度器当作零成本方案。
7. 同值配置不重复写。FC06/FC16都应有完整成功/失败闭环；批量配置先整体验证，再一次提交，失败不留下部分RAM生效状态。普通Modbus写成功究竟表示持久化成功还是仅受理，需明确；本方案推荐前者，不虚报保存成功。

### 3. 旧端口日志迁移

不能直接把旧16字节记录当新64字节结构。首次升级时识别旧 magic 0x43535054，按旧校验规则提取最新预留序号。保留含最新旧记录的扇区，在另一扇区写新版本快照（存储位默认0x0003），读回成功后再启用新服务；任何阶段掉电仍应有旧或新有效副本。

新固件中必须删除/停用旧 `prvReserveRobotPort` 的直接擦写实现，改调公共服务。旧固件回刷可能不识别新记录并擦除参数，因此降级、整片擦除、恢复出厂需有明确策略和告警。恢复业务默认值不要顺便把端口序号归零。

## 六、实施顺序及验收项

1. 先补 Server 两个手动位置映射，复用已有存储选择和机器人动作；区分不支持、未就绪、队列失败，不再误导。
2. 按上述地址统一端口/配置存储，完成旧记录迁移；公共层不绑定 Target。
3. 接通 0x001A 默认、读、写、保存及自动选位掩码；占用状态仍实时来自 IO，不写Flash。
4. 再按审批决定是否加入果乳校准、出冰补偿、杯高参数，必须每项完成“配置→保存→算法使用→读回”闭环，不一次复制全部Coffee1遗留字段。
5. Keil/GCC校验字段编码、链接边界、OTA擦除范围；测试空白Flash、损坏记录、写失败、A/B轮转每个阶段掉电、旧记录迁移、配置保存与重连并发、OTA互斥、重复同值写。
6. 上板：0x0031写0x0015/0x0016，确认0x0032先发布1/2再触发动作；配置0/1/2/3分别验证重启保持、自动找位；验证已有杯不会因位置禁用而从页面消失；确认调试不额外触发机器人完整启动。

需审批的主要选择：采用 sector10/11 统一双副本参数日志；默认开放掩码0x0003；0允许禁止新增存储；手动位置调试不额外受业务开放掩码限制。本轮尚未实现这些建议。
