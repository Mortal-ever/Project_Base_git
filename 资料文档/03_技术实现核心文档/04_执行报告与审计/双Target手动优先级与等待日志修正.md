# 双 Target 手动优先级与等待日志修正

日期：2026-09-07。适用：Coffee2Open、Coffee3Close。

## 当前行为

- 输出调试地址不变：十进制 520/521，即 0x0208/0x0209。
- 私有 Device 的 `prvSubmit` 按 `ucSource` 和 `usAction` 判定优先级：
  Server 手动动作从所在设备路由的队首入队；REFRESH 正常队尾入队。
  板载 520 仍由 Server 直接执行，远端 521 交 Bus5 执行。
- `ROBOT_ACTION_ACCEPT_WAITING` 使用系统日志标识 0000；不修改实际命令的
  orderId、epoch、commandId、source，也不把等待状态重新提交为调试动作。
- F123 是日志调试标识，不是优先级字段。Robot 与各 RTU Bus 有独立路由/所有者；
  Robot 的状态轮询没有占用 Bus5，也不因打印 F123 自动锁住其他设备。

## 明确的限制

这是“已准入命令的队列优先级”调整，不是取消一切资源互锁。活动自动订单、
维护、OTA、Coffee3 门动作及安全恢复锁仍可拒绝调试，以免并发改变机械输出。
手动 Robot 等待不会单凭其手动预留阻止另一条已满足准入条件的 IO 手动命令。
自动新单仍不能在手动动作物理状态未完成时启动。

队首插入不能打断正在传输的 RTU 帧，也不扩充满队列。满队列返回失败；连续手动
写入可能延迟正常轮询，同一路由的多个紧急命令仍沿用现有队首插入顺序。
本次没有提升 RTOS 任务优先级、增加队列/任务或改动公共驱动。

## 源码与验证

- 两 Target 的 `Device/coffee{2,3}_device.c::prvSubmit`：手动入队规则。
- 两 Target 的 `Robot_Tcp/coffee{2,3}_robot_tcp.c`：等待状态日志。
- `tests/test_manual_priority_contracts.py`：来源与优先级分离、REFRESH、系统日志、
  地址不变及预留释放静态契约。
- Keil Coffee2Open：0 错误/0 警告，Code 200796、RO 7440、RW 552、ZI 143432。
- Keil Coffee3Close：0 错误/0 警告，Code 202748、RO 8272、RW 584、ZI 144160。
- 24 项静态契约测试通过；未执行本次 GCC 构建，既有 Ninja 环境问题仍待排查。
- 未烧录，未验证现场动作。上板验收：Robot 手动命令等待时写 520 和 521，确认
  准入条件满足时分别执行；观察 0000 等待日志、新手动 Robot 命令处理、Bus5 读回。
