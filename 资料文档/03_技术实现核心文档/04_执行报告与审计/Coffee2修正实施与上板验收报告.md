# Coffee2 修正实施与上板验收报告

日期：2026-09-07。范围：当前 Coffee2；Coffee3、MilkTea、Bootloader、IOC、上位机 DOCX 均未改动。本报告不是实机验收合格证。

## 1. 本轮如何解释审批

依据《Coffee2工程锐评与Coffee3落地建议审批版》，先核查具体缺陷，再修改。审批中“两个储位、线上订单二次检查、储位满不影响线下单”属于 Coffee3，本轮不移植到 Coffee2。

Coffee2 保持：开放式、两个出餐口、F200 自有串口协议、无储位、无门、无升降机构。制作完成仍是放杯传感器确认后机器人回 Home 成功；没有新增制作 ACK、放杯 ACK 或 30 秒关门流程。

原锐评中“物理后置条件缺失”不能一概而论：源码已有落杯、落盖、重量、F200/糖浆应用状态、出餐杯感应检查。本轮保留这些检查，补上出口空闲预检、状态所有权及失败出口，不重复建设协议包装层。

## 2. 已修正的源码问题

| 问题 | 修正及验收依据 |
| --- | --- |
| Server 接受新订单时清零两个出餐口状态 | 状态迁入 `g_xCoffee2WorkflowStatus.ausOutputState[2]`；每个出餐口保留自己的订单号和结果。新订单不再清除另一出餐口的状态 |
| Server 在新单替换旧单时提前改写运行订单回显 | `vCoffee2ServerPublishOrder()` 由 Workflow 实际开始该订单时调用；旧订单执行/收尾阶段继续显示旧订单，不混合新参数 |
| 无条件接受提前或重复取餐 ACK | Server 只投递 `vCoffee2WorkflowConfirmPickup()`；只有状态 5 可挂起 ACK，由 Workflow 消费为 0x10；状态 0/2/3 的 ACK 不推进状态 |
| 下位机未检查目标出餐口是否空闲 | 开始机械动作前、最终放杯前各执行一次 `prvCheckOutputEmpty()`；先检查未消费事务，再进行成功的 IO 输入刷新和杯感应检查。离线不是空位 |
| 机器人放杯成功后，等待传感器失败仍可能保留状态 2 | 等待出餐感应取消/通信失败也将本次放杯状态置 3；已确认的状态 5 不因后续 Home 失败被清除 |
| 全局设备最近结果可能被后续刷新覆盖，错误指向错误事务 | 步骤失败取 `orderEpoch + commandId` 对应终态历史；正文包含具体设备名、动作、步骤和原始错误 |
| EventGroup 的无身份事件位可能被解释为当前事务终态 | 事件位只唤醒；成功/失败由带身份的终态记录判定。无关粘滞事件让出一个 tick，避免空转抢占 Bus owner |
| RTU 队列中已取消订单仍可能发送旧动作或重试 | 每次尝试及间隔等待后再次检查 epoch；取消时不发该动作，记录取消结果，不伪装为设备掉线 |
| safe-stop 把“停止命令被取消”当作成功 | 必须取对应停止命令终态，`valid && result == 0` 才接受；取消事件本身不证明设备停下 |
| 停止动作与普通动作共用取消判定 | 利用现有命令 `ucFlags` 指明安全停止，且只有 Robot/F200 CANCEL、制冰 OFF、单点 IO OFF 在白名单中，不能用标志绕过 ON 动作取消 |
| 取消所有设备而不管本单是否使用 | 记录实际投递过的动作设备集合；不因未参与动作的 F200/制冰设备离线误判本次停止失败。IO 危险输出关闭仍保留 |
| 糖浆未结束却可能宣称 SAFE_STOP_CONFIRMED | 已成功读到应用完成的糖浆动作清除占用标记；未结束的糖浆动作没有已确认的 STOP 协议，明确报告停止未确认并锁定，不编造停止指令 |
| 无法证明杯位/机器人位置安全却重新接单 | 出现机械动作后的订单失败，或安全停止不完整，置 `ucRecoveryRequired=1`；保留业务原始错误与独立 `lSafetyResult`；拒绝新订单和手动动作，人工检查后复位重走初始化 |
| 手动动作与自动动作没有覆盖队列生命周期的仲裁 | 手动申请在入队前取得静态计数预留，入队失败/设备终态时释放。自动接单、维护和热水启动检查该预留；并非只检查一瞬间的 `busy` |
| 清洗等待期间仍可接受订单或不响应取消 | 清洗请求接受时关闭接单，整个维护占用 Workflow；使用独立 epoch、响应取消并走失败收尾。维护不再误写普通订单制作投影 |
| 16 路输出位图拆成最多 16 条队列命令，可能只接受部分 | `IO_WRITE_MASK` 是一条 Coffee2 私有命令。Bus5 执行时先 FC01 读取，再逐点 FC05+FC01 验证；不依赖 Server 的旧缓存，也不要求设备新增 FC15 支持 |
| FC16 同时写 520/521 时先改本地再发现远端队列满 | 先完整校验本地高位和控制权限，远端请求先入队，再操作本地 GPIO；保持 FC16 数量 2 的支持 |
| IO 调试路径绕过 OTA 检查 | 调试写入统一检查 OTA、自动/维护所有权；拒绝时有可读日志 |
| IO 协议错误可能把零初始化缓冲提交成全关状态 | 位图/单点写入均要求图像含完整 16 点，才允许提交实际读回；初次 FC01 失败不会覆盖为零，也不宣称设备输出成功 |
| 清报警给 F200 提交不存在的 RESET 动作 | 移除此未支持命令；机器人清报警成功入队后再提交初始化复查请求 |

说明：取餐 ACK 没有订单号/序号，协议本身不能识别“跨订单延迟到达、恰逢下一单也进入状态 5”的旧 ACK。本轮不宣称解决协议无法表达的幂等性；上位机仍须按当前出餐口事务发 ACK。

## 3. 公共层与私有层

### 公共层

只修改 F200、糖浆现有取消回调的实现与接口说明：允许传 NULL 保持原来行为；传有效回调时，在事务发送前及多事务之间检查取消。F200 CANCEL 本身不被取消回调抑制。

回调不带 Coffee2 枚举、寄存器、任务或总线参数。取消不能撤回已经发送到设备的物理动作，也不是中止正在传输的一帧。其他 target 可以继续独立调用这些公共驱动。

### Coffee2 私有层

- `Config/coffee2_device_bindings.h`：唯一静态设备选择表，含设备 ID、route、unit、最小间隔、category、role、driver、protocol、英文诊断名；只由 `coffee2_device.c` 实例化。可用能力仍由实际选中 driver 和执行分支决定，不添加重复 capability 位图或虚假的 enabled 配置。
- `Device`：32 字节命令、队列路由、取消身份和终态历史；不新增任务、队列、事件总线或堆对象。
- `WorkFlow`：接单/维护/调试仲裁、订单身份、物理不确定标志、安全收尾、两个出餐口状态和 ACK 消费。
- `Modbus_Tcp_Server`：输入协议、寄存器投影；没有新业务 owner。
- `Modbus_Rtu_Bus`：串行执行原子命令、取消前检、IO 位图执行/读回；不决定制作步骤。

已有大文件未机械拆为 order/recovery/maintenance 多层转发文件。本轮抽出空闲检查、出餐发布、ACK 消费、手动预留等有明确职责的函数，避免为文件数量而引入抽象。Keil 在 `Application/Coffee2App` 展示新增 Config 头文件；CMake 已有该私有 include 路径，无需增加翻译单元。

## 4. 上位机协议事实表

以下是本轮 Coffee2 固件规则，不把 Coffee3 值域套用过来。地址均为零基地址；FC16 十进制功能码等于十六进制 0x10。

| 地址 | 访问/值域 | 谁维护与清除 |
| --- | --- | --- |
| 0x0000–0x001F | FC06/FC16 写订单参数，FC03 回读；订单号不取 0000/F123 | Server 冻结快照，Workflow 执行；后续改参数不修改正在执行的快照 |
| 0x0007、0x0008 | present 非零且 verified=1 触发接收；present=0 解除 Server 锁存 | 保持现有 Coffee2 握手：上位机准备下一单时先写 present=0，再下发参数/校验；本轮不新增内部自动清零握手 |
| 0x0009、0x000C | Coffee2 必须为 0 | 不支持储位和封闭式机型字段 |
| 0x000A | 目标出餐口 1/2 | Workflow 在动作前及放杯前复核；有杯、未消费放杯事件、输入状态读失败均不认为空闲 |
| 0x000B | FC06/FC16：0x0001 确认出口1；0x0010 确认出口2 | Server 消费写入后清命令寄存器；Workflow 仅接受对应状态 5 的 ACK |
| 0x0021 | 清报警请求 | 初始化残杯阻塞时，先排入机器人清报警，再触发完整初始化重查。运行期 recovery 锁定不由此绕过 |
| 0x0022 | 取消请求，非零 | Server 清触发字；Workflow 取消当前 epoch、执行安全收尾 |
| 0x0030=1 | Robot STOP | 自动订单运行时转换为 Workflow 取消请求，避免只停机器人却留下液路；空闲时保持手动 Robot STOP |
| 0x0208（520） | FC03 读取实际板载 DO；FC06/FC16 写低 8 位，0x05=Y1/Y3 输出 | 高 8 位非零异常 03；自动/维护/OTA/恢复锁占用时异常 04，日志说明原因 |
| 0x0209（521） | FC03 实际外置 DO；写完整 16 位目标位图 | 单请求单入队，正常响应表示接受，实际成功以 Bus5 读回及 IO 页面为准；失败可能已执行前几个点位，不承诺跨硬件回滚 |
| 520+521 连写 | FC16 数量 2 合法 | 先校验全部值/权限，再入队远端与执行本地；不添加“只允许数量1”的限制 |
| 0x1008 | FC03：0默认/1制作中/2完成/3失败 | Workflow 维护。实际开始的新订单才切换整套回显 |
| 0x100B、0x100C | FC03：0默认/2放杯中/3放杯失败/5放杯完成/0x10已确认取走 | 每出口独立保持。只有对应出口重新使用时开始下一次状态，不受另一单清零 |
| 0x1020 | FC03：0默认/1待机/2初始化/3忙碌/4报警 | `ucRecoveryRequired` 对应运行期人工复位要求；只看到设备恢复在线不自动解锁 |
| 0x10F0–0x10FF | FC03 只读 IO 页面，16 个寄存器均有效 | 保持之前 32 路预留布局，实际板载 8DI/8DO、第一组 16DI/16DO 实时更新，其余为 0 |

Coffee1 事实核对：`D:/Project_Items/Coffee1/coffee_close_v2.8.29_IOPage/modbus/order.c` 的 `order_make_finish()` 与 `order_take_finish()`（27、36 行）分别清制作请求和取餐请求；`ControlFlow/control_flow.c` 5539、5544 行分别调用。吸收其“制作/取餐分开”的业务原则，不复制旧架构、封闭式储位、Coffee1 全局清零函数或额外 ACK。

访问权限不是内部清除所有权的证明。本轮没有再将 FC03/FC06/FC16 标为冲突；Coffee2 的 present 清零规则与 Coffee1 不同，已在上表明确保留现状，不用一句“完全照搬 Coffee1”掩盖差异。

## 5. 异常与恢复操作

1. 参数错误、输入状态刷新失败发生在机械动作前：订单失败，执行必要安全收尾；确认安全后可再接收订单。
2. 机械动作开始后失败/取消：无法仅凭通信 ACK 推断夹爪和工位杯子位置。保持整机报警并关闭接单、调试和热水启动，人工检查、移除残杯后复位。
3. 任何安全停止未确认：同样锁定；错误日志分别保留业务失败与停止失败，不把超时/取消伪报为确认成功。
4. 等待替换的新订单在恢复锁定时不再执行；人工复位后由上位机重新下单。本轮最多保留一个替换请求，不允许第三个请求静默覆盖第二个。
5. 热水仍是既有独立后台服务，可与不冲突的订单推进共存；手动 IO 调试不能在热水占用期间改写相同输出。无新增 RTOS 任务。

## 6. 验证方法与边界

静态检查命令：

```powershell
python -m unittest discover -s tests -p test_coffee2_correction_contracts.py -v
git diff --check
```

测试覆盖状态所有权、ACK 门控、出口预检、取消白名单、终态身份、手动预留、位图单命令、FC16 完整校验、恢复锁、维护预留、公共回调边界、16 寄存器窗口、设备配置唯一性、Keil 路径存在性。**这是源码契约测试，不是运行 C 状态机的仿真，也不证明真实多任务调度和机械动作正确。**

本机无可用原生主机 C 测试编译器；不把正则/源码检查描述为硬件功能测试。实机验收由下一节闭环。

构建、资源、固件身份的最终结果见报告末尾“交付快照”。普通 GCC Ninja 调度异常与源码编译失败分开报告。备用 `GCC-ARM/scripts/build_snapshot.py` 只消费既有 CMake 生成图，全量直接编译所有翻译单元、校验源码快照不变、链接并转换新产物，不配置、不清理、不烧录。

## 7. 上板验收清单

建议先空机/安全工装，再有杯、液体、机械动作。使用本轮产物，记录固件哈希、订单号、写寄存器值、日志与传感器实测。

| 场景 | 应观察到的结果 |
| --- | --- |
| 上电正常/残杯/输入模组离线 | 正常完整初始化；残杯指出位置且不接单；离线不当空位。清报警复查不能和手动动作重叠 |
| 热/冷、带盖/不带盖 | 保留原配方/落杯/落盖/称重路线；对应真实后置条件满足才继续 |
| 果乳 A/B、四路糖浆、F200/跳过 F200 | 只执行本单必要设备；跳过 F200 的订单不因其前置 REFRESH 失败被拦截；核对实际出液量 |
| 出口1已放杯且未 ACK，给出口2下单 | 出口1状态5保持；出口2可正常执行，不清掉出口1 |
| 同一个已占出口再次下单 | 动作前拒绝；另一出口不受影响 |
| 下单时出口为空，制作中被放入杯子 | 最终放杯前复查阻止放杯，进入失败/人工恢复，不覆盖已有杯 |
| 出口杯传感器通信失败 | 不凭旧0数据判空；失败原因能看到 IoInput16、步骤、错误 |
| 放杯机器人成功但杯感应未到 | 继续等待，不报告放杯完成；取消/刷新失败则状态3 |
| 提前 ACK、重复 ACK、另一出口 ACK | 提前不推进；重复不生成新完成；只消费指定出口 |
| 放杯感应成立后 Home 失败 | 本出口已放杯状态保留；制作失败/恢复锁可单独诊断 |
| 订单每个阶段取消、通信超时后迟到响应 | 已取消 epoch 的排队旧动作不再发送；旧终态不能推进新命令；危险设备停止必须有真实成功终态 |
| 糖浆正在动作时取消 | 不打印伪造的停止已确认；显示糖浆无已验证 STOP、锁定等待人工检查 |
| 机械动作后失败，再尝试新单/520/521/手动机器人 | 接单与调试均拒绝；清报警不解锁；人工检查后复位恢复 |
| 订单正常时写520/521或机器人点位 | 拒绝并有原因日志；原订单不被调试动作偷改 |
| IO调试进行中立即下单 | 调试命令仍在队列/执行时不接受自动抢占；终态释放后可重新下单 |
| FC06：520=0x05/0x00/0x0100 | Y1/Y3 输出/全关闭/异常03；以实际 IO 页核对 |
| FC16：520起数量2（例如0x05、0x8001） | 请求合法；板载 Y1/Y3，外置第1/16点按掩码执行；实际接线安全确认后测试 |
| 快速连续521=FFFF、0000 | 各自只有一条位图命令；Bus 按顺序使用实时读回比较，不出现基于旧镜像漏掉第二次关断 |
| 远端在位图中途断线 | 日志报失败，页面不伪造全成功；承认可能部分执行，恢复后重新下发目标位图 |
| 清洗中取消/清洗中下单 | 取消可达失败收尾；订单不得抢占清洗物理资源 |
| RTU/Robot断线恢复、日志满、长期循环 | 结合现有链接恢复测试；检查任务栈水位、最小堆余量、IO反馈周期和总线负载 |

## 8. 交付快照

### 8.1 最终验证结果（2026-09-07）

| 项目 | 结果与证据 |
| --- | --- |
| Keil Coffee2 / ARMCC 5.06u7 build 960 | `PASS`，最终日志 0 Error(s)、0 Warning(s)。Code=200788、RO=7440、RW=552、ZI=143432 字节 |
| GCC Coffee2-Debug / Arm GNU 15.2.Rel1 | `PASS`，直接执行 CMake 已生成编译命令，全量 166 个翻译单元、重新归档/链接、产出 ELF/HEX/BIN/MAP；构建日志无 warning/error；起止源码哈希一致 |
| 常规 CMake/Ninja 构建入口 | `BLOCKED_TOOLING`：实际调度卡在首个编译器启动之前；dry-run、图查询和直接编译正常。没有声称常规 Run Task 已恢复，也未覆盖原 build.ps1/VSCode tasks。操作系统层面的根因未证实 |
| 回归检查 | `PASS`，20 项 Python 静态源码契约检查；`git diff --check` 通过；Keil XML 仍只有 Coffee2，引用文件存在 |
| 独立只读复核 | `ACCEPT`；复核发现的恢复锁绕过、手动冰 pending→active 空窗已修正，并复验 IO 失败图像不覆盖有效状态 |
| 实物与运行时 | `UNKNOWN`：未烧录、未验证真实负载/物理位置/STOP 效果/任务栈水位；由第 7 节上板清单验收 |

GCC 正常入口仍需后续环境排查；本轮可复现的备用构建命令（工程根执行）：

```powershell
python GCC-ARM/scripts/build_snapshot.py
```

该脚本仅适用于**已经配置且与当前源清单一致**的 Coffee2-Debug 构建目录，不替代新增源文件后的 CMake 配置。它不是新的架构/构建系统。失败时不得使用目录中可能遗留的旧产物；只有本次输出 PASS 且 manifest 对应当前源码才视为有效。

### 8.2 链接内存核对

- Keil Flash 执行区：起点 `0x0800C000`，大小 `0x32D64`，上限 `0x54000`；CCM `0xA0F0/0x10000`；IRAM1 `0x17180/0x1C000`；IRAM2 `0x2000/0x4000`，UNINIT。
- GCC：Flash `176892/344064` 字节（51.41%），RAM `102544/131072`（78.23%），CCM `41064/65536`（62.66%）。text=176716、data=168、bss=143440 字节。两工具链的区域统计口径不同，不直接将 Code 等同于 FLASH Used。
- `g_xCoffee2WorkflowStatus` 为 56 字节：Keil `0x100020B8`，GCC `0x10009EA8`；32KB FreeRTOS `ucHeap`：Keil `0x100020F0`，GCC `0x10001EA8`，均在 CCM，不能直接交给 DMA。
- Ethernet DMA 描述符位于 SRAM：Keil RX/TX `0x200050E4/0x20005184`；GCC RX/TX `0x2001104C/0x20010FAC`。本轮没有改动链接脚本、DMA 生命周期或生成代码；地址核对不代替长时运行水位测试。
- 两份正式 IOC SHA-256 与开始前一致：CMAKE `F88F869FCF75943835F8D851F37134B60287C02D27DE7E4F0538193D49570FF5`；MDK `0E1298E9C16EE3636C5C056D137CD13104846890374329DF061AA2FD4EFDAD1C`。

### 8.3 固件身份与日志

以下 SHA-256 是本轮最终产物，后续重编译可能变化；以实际待下载文件重新计算为准。本轮没有烧录。

| 文件（相对工程根） | SHA-256 |
| --- | --- |
| `MDK-ARM/Objects/Coffee2/Coffee2.hex` | `9D917847CACA3C2E880D106EE9466F9DDB032487D8712453689942F66D740B5C` |
| `MDK-ARM/Objects/Coffee2/Coffee2.axf` | `BCA6607406A9D396D5A17E0E42BE5BBC13E3A703872DB53B629012ADBCCEF61A` |
| `MDK-ARM/Objects/Coffee2/Coffee2.bin` | `1F203DFF6941B8F366D1A67AC23301E013C3C6167F2BE3228CD5590DD37219F9` |
| `GCC-ARM/build/Coffee2-Debug/Coffee2Target.elf` | `5E76F495168A43ACBA9BB3F65728962C181D52A189E094EB7D9FCB94DC04124D` |
| `GCC-ARM/build/Coffee2-Debug/Coffee2Target.hex` | `7E48BC5928F46166837BAD2FEFF2E0C81CD23F12A3FF09F3F5713A2616D8ED8F` |
| `GCC-ARM/build/Coffee2-Debug/Coffee2Target.bin` | `8CF0336AA343B12C6C6049DC27779AF11C5B8B5BC6310BA1574D4C4B417EE34F` |

验证文件：

- [Keil 本次构建日志](../../../MDK-ARM/Objects/Coffee2/Coffee2_correction_20260907.log)
- [Keil map](../../../MDK-ARM/Objects/Coffee2_Lst/Coffee2.map)
- [GCC 全量构建日志](../../../GCC-ARM/build/Coffee2-Debug/snapshot_build.log)
- [GCC 源码/产物哈希 manifest](../../../GCC-ARM/build/Coffee2-Debug/snapshot_manifest.json)
- [GCC map](../../../GCC-ARM/build/Coffee2-Debug/Coffee2Target.map)
- [20 项静态契约测试](../../../tests/test_coffee2_correction_contracts.py)

这些日志和固件位于忽略的构建目录，不会因为新增本报告而自动纳入 Git。

## 9. 源码维护入口

以下行号对应本轮快照，后续修改以符号为准。

| 文件 | 关键符号/位置 |
| --- | --- |
| [Workflow](../../../Application/UserAPP/Coffee2App/WorkFlow/coffee2_workflow.c) | `xCoffee2WorkflowAcquireManual` 202；`vCoffee2WorkflowConfirmPickup` 238；`prvServicePickup` 251；`prvCheckOutputEmpty` 282；`prvAbortDevices` 2275 |
| [Device](../../../Application/UserAPP/Coffee2App/Device/coffee2_device.c) | `ucCoffee2CommandIsCanceled` 205；`xCoffee2DeviceWaitCommand` 573；命令入队及终态释放同文件 |
| [Server](../../../Application/UserAPP/Coffee2App/Modbus_Tcp_Server/coffee2_server.c) | `vCoffee2ServerPublishOrder` 549；`prvCommitIoDebugWriteRange`；`prvEvaluateManualCommands`；寄存器定义仍在同目录头文件 |
| [Bus](../../../Application/UserAPP/Coffee2App/Modbus_Rtu_Bus/coffee2_rtu_bus.c) | `vCoffee2RtuBusTask` 的取消前检；`prvExecute` 的 `COFFEE2_ACTION_IO_WRITE_MASK` 与图像有效性判断 |
| [私有设备配置](../../../Application/UserAPP/Coffee2App/Config/coffee2_device_bindings.h) | `s_axBindings`，10 台设备；不在公共层存放产品映射 |

残留边界：设备写命令 ACK 丢失可能发生“设备已执行而软件未知”，本轮没有重做全部非幂等设备重试策略；取消回调不能撤回已发送帧，普通软件 STOP 也不等同于硬件急停。不得以本报告替代机械/电气安全联调。Coffee3 target 仍未创建。
