# Coffee3Close 机器人自动回 HOME 与冗余 HOME 动作调研报告

## 1. 结论

当前 Coffee3Close 存在明确的冗余 HOME 动作，不是机器人速度慢，也不是 HOME 超时参数过短。

`ROBOT_PUT_STORAGE` 和 `ROBOT_PUT_OUTPUT` 属于最终放置动作，机器人协议程序在动作内部完成回 HOME。其对应完成线圈置位时，整个“放置+回位”动作已结束。工作流不应再发送一次独立 HOME 命令。

当机器人已在 HOME 点时，再发送独立 HOME 命令，机器人不执行位移，也不会再置位独立 HOME 完成线圈。STM32 因此会永久等待一个不会到来的完成信号。现场日志中的：

```text
[0000WARN][C3Workflow:Workflow] Step overdue: device=Robot action=robot home step=820; waiting
```

正是这个问题：取餐的放出餐口动作已经完成并自动回 HOME，但 Coffee3 又发送了 step 820 的独立 HOME。

## 2. Coffee1 与 Coffee3 寄存器映射差异

两个工程的动作语义可以对比，但不能直接照搬地址差值。

| 工程 | 完成区 | 命令区 | 配对方式 |
| --- | --- | --- | --- |
| Coffee1 | 3100～3119 | 3120～3139 | 20 位完成区对 20 位命令区 |
| Coffee3 | 3100～3109 / 3120～3129 | 3110～3119 / 3130～3139 | 两组 10 位完成区对 10 位命令区 |

关键动作对照：

| 动作 | Coffee1 | Coffee3 |
| --- | --- | --- |
| 独立 HOME | 命令 3121，完成 3101 | 命令 3111，完成 3101 |
| 放存储位 | 命令 3136，完成 3116 | 命令 3136，完成 3126 |
| 放出餐口 | 命令 3138，完成 3118 | 命令 3138，完成 3128 |

所以，Coffee3 中 `3138 -> 3128` 完成是正确的 Coffee3 协议映射；不应用 Coffee1 的 `3138 -> 3118` 去判断 Coffee3。

## 3. Coffee1 已落地流程证据

### 3.1 正常订单开始不发 HOME

Coffee1 `control_flow.c` 中，订单在 `E_FLOW_START` 完成设备检查后，直接转入 `E_FLOW_TO_CUP_POS`，没有先进入 `E_FLOW_TO_HOME_START`。

这证明 Coffee1 正常制作流程不依赖“每单先强制 HOME”。

### 3.2 放存储位完成后不追加 HOME

Coffee1 正常订单的放存储位流程为：

```text
robot_put_store_start()
    -> robot_put_store_finish()
    -> 设置生产状态/订单完成
    -> E_FLOW_IDLE
```

`robot_put_store_finish()` 读到放存储位完成位并清除后，正常分支直接进入 `E_FLOW_IDLE`，没有转入独立 HOME。

Coffee1 只有“异常取消后将机器人手上餐品放回原储位”的恢复分支，才在放回后转入 `E_FLOW_ERR_CANCEL_TO_HOME_START`。这是异常恢复策略，不是正常放置动作的必要收尾。

### 3.3 取餐放出餐口完成后不追加 HOME

Coffee1 取餐主链为：

```text
校验储位状态
    -> robot_take_store_start/finish
    -> 等出餐口可用
    -> robot_order_rdy_start/finish
    -> 出餐口机构与客户取餐流程
    -> E_FLOW_IDLE，或回到被插入的订单制作节点
```

`robot_order_rdy_finish()` 完成后，Coffee1 直接进入 `E_FLOW_FOOD_PICKUP_WAIT`，之后执行出餐口机构流程。取餐结束后，若它是插入制作中的取餐，则回到咖啡机前继续制作；否则进入空闲。该正常链路没有发送独立 HOME。

### 3.4 Coffee1 的独立 HOME 用途

Coffee1 仍保留 `E_FLOW_TO_HOME_START/FINISH` 和 `E_FLOW_ERR_CANCEL_TO_HOME_*`，但实际主要用于：

- 异常取消后的位置恢复；
- 机器人位置不确定时的人工/恢复流程；
- 非正常制作与取餐主链的收尾。

因此，Coffee1 的事实依据支持“正常放置动作完成后不再发 HOME”，不支持 Coffee3 当前的无条件追加 HOME。

## 4. Coffee3 当前的三个风险点

### 4.1 取餐流程 step 820：已确认的缺陷

Coffee3 `prvRunStoragePickup()` 当前流程：

```text
step 800  ROBOT_TAKE_STORAGE
step 810  ROBOT_PUT_OUTPUT
step 815  确认出餐口有杯
step 816  确认原储位无杯
启动出餐门
step 820  ROBOT_HOME       <- 冗余
```

step 810 的 `3138 -> 3128` 已表示放出餐口动作完成，而该动作内部已回 HOME。step 820 再发 `3111`，就会等待不再置位的 `3101`。

判定：**step 820 应删除。** `ROBOT_PUT_OUTPUT` 完成位清除且出餐口/储位 IO 确认通过后，应直接释放 Robot 所有权。

### 4.2 订单结尾 step 190：同类冗余动作

Coffee3 订单在线分支最后执行 `ROBOT_PUT_STORAGE`，离线分支最后执行 `ROBOT_PUT_OUTPUT`，但两个分支汇合后又无条件执行 step 190 `ROBOT_HOME`。

这与取餐 step 820 属于同一类错误：

```text
ROBOT_PUT_STORAGE / ROBOT_PUT_OUTPUT 内部回 HOME
    -> 完成线圈置位
    -> Coffee3 又发 ROBOT_HOME
    -> 机器人已在 HOME，不产生新的 HOME 完成沿
    -> 订单流程卡在 step 190
```

判定：**step 190 应删除。** 订单应以最后放置动作及对应 IO/业务确认作为 Robot 阶段结束条件。

### 4.3 订单开始 step 30：无条件 HOME 同样不安全

Coffee3 在每个订单开始前无条件执行 step 30 `ROBOT_HOME`。Coffee1 正常订单并没有这一步，而 Coffee3 的上一个正常放置动作本就会将机器人带回 HOME。

如果前一动作已回 HOME，step 30 同样可能等不到新的 HOME 完成信号。

判定：**正常订单不应无条件发 HOME。** 订单应在机器人服务就绪且 Robot 所有权可用后，直接进入首个业务动作。

## 5. 取物动作与放物动作的边界

不应在每个 3130～3139 动作后机械插入 HOME。机器人协议程序已定义动作内部轨迹。

- 取物类动作是中间动作，之后还要执行放置或加工动作；不在取物和下一业务动作之间额外插入 HOME。
- 最终放置类动作，当前已确认包括 `ROBOT_PUT_STORAGE` 和 `ROBOT_PUT_OUTPUT`，其动作内部完成回 HOME；完成后不追加 HOME。
- 独立 HOME 只是恢复/手动命令，不是每段业务的通用收尾动作。

对其他 3130～3139 动作是否以回 HOME 为动作内部终点，应以机器人程序的动作定义为准；STM32 只等待该动作自身的完成线圈，不引入额外 HOME 来“保险”。

## 6. 建议落地规则

1. 删除 Coffee3 取餐流程 step 820 的 `ROBOT_HOME`。
2. 删除 Coffee3 订单在线/离线放置完成后的 step 190 `ROBOT_HOME`。
3. 删除正常订单开始前 step 30 的无条件 `ROBOT_HOME`；正常流程直接进入首个业务动作。
4. 保留上位机人工 HOME 调试命令，但它仍属于工程师手动操作，不应被正常订单/取餐流程隐式调用。
5. 异常恢复若确实需要独立 HOME，必须与正常业务收尾分开，日志明确输出 `reason=recovery`。
6. Robot 所有权的释放点应是“最终放置动作完成+必要物理 IO 确认完成”，不再依赖额外 HOME 步骤。

## 7. 实现后验收点

### 7.1 取餐

```text
TAKE_STORAGE complete
PUT_OUTPUT complete
出餐口 X3 有杯
原储位 X1/X2 无杯
启动出餐门流程
释放 Robot 所有权
```

日志中不应再出现 step 820，也不应在 `PUT_OUTPUT complete` 后出现新的 `ROBOT_ACTION_START=ROBOT_HOME`。

### 7.2 在线订单

`PUT_STORAGE` 完成并确认储位有杯后，订单 Robot 阶段结束，不发 HOME，不卡 step 190。

### 7.3 离线订单

`PUT_OUTPUT` 完成并确认出餐口有杯后，进入出餐门/客户取餐流程，不发 HOME，不卡 step 190。

### 7.4 连续订单

前一单的最终放置动作完成后，下一单直接进入首个业务动作，不先发无条件 HOME。

## 8. 最终判定

本报告对当前方案的审批结果为：**REJECT 当前 Coffee3 的无条件 HOME 收尾方案，接受以动作自身完成位为终点的方案。**

Coffee1 的正常订单与取餐主链、Coffee3 的实际日志、以及机器人动作内部回 HOME 的现场语义三者一致：正常 `PUT_STORAGE` / `PUT_OUTPUT` 完成后再发 HOME 是多余且会卡流程的。

## 9. 2026-09-23 落实结果

已按本报告落实：

- 删除正常订单开始前的 step 30 `ROBOT_HOME`；
- 删除在线/离线订单最终放置后的 step 190 `ROBOT_HOME`；
- 删除独立取餐放出餐口后的 step 820 `ROBOT_HOME`；
- 保留上位机人工 HOME 调试入口和机器人协议映射；
- 新增正常订单与独立取餐不追加 HOME 的契约测试。

验证结果：Coffee3Close GCC Debug 构建通过，Keil ARMCC
V5.06u7 构建 0 error / 0 warning，85 项契约测试全部通过。
本次未执行烧录和实机动作测试。
