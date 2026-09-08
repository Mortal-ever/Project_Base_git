# Bootloader 与 Coffee3 启动日志补充说明

## 结论

本次日志是 Bootloader 与应用层的连续输出，不是同一个任务的异常堆栈。

```text
BL: flash_size=0KB
BL: f_size=0KB
BL: no layout claimed
BL: update failure
BL: jump to application
```

这表示 Bootloader 没有识别出有效的 Flash 容量，因此没有选出布局，也没有执行固件写入；随后按回退路径跳转到已有应用。它不能证明本次下载的镜像已经写入。应用随后输出 `FW_VERSION:Coffee3CloseV3.0.0`，只能证明跳转到的应用版本字符串为该版本。

## 应用日志中的已知正常状态

`ROBOT_ACTION_ACCEPT_WAITING` 是机器人动作握手阶段。当前机器人 Server 为模拟状态，没有清除命令线圈，因此主控保持等待接取，不应改成“成功”或“设备故障”。

`RESULT=-4` 在当前 Modbus 结果枚举中是 `MODBUS_PORT_RESULT_TIMEOUT`。Coffee3 RTU 失败日志已补充 `RESULT_NAME`、`BUS`、`UNIT`、`DEVICE_ID`、`ACTION`、`SOURCE`、`STEP` 字段，后续可直接定位失败设备和业务步骤。

## 调试命令拒绝的判定

IO 输出调试和机器人手动动作属于 Debug 命令，使用 `COFFEE*_COMMAND_FLAG_DEBUG`，不再调用工作流的 Manual/OTA 所有权门禁；系统刷新、订单、维护、清洗和 OTA 仍走正常所有权保护。日志中仍出现：

```text
IO debug rejected: automatic/maintenance/OTA owns outputs
```

优先判断为旧固件输出，因为当前源码已移除该拒绝分支。必须用包含最新 `coffee2_server.c`/`coffee3_server.c` 与 `coffee*_device.c` 的完整镜像重新构建并下载后再验证；不能用旧日志判断新代码未生效。

## Bootloader 后续应补的最小日志

Bootloader 工程位于工作区外，本次未直接修改。下一次构建 Bootloader 时应在一次启动中打印：

```text
BL: chip_id=0x........
BL: flash_size source=<register|fallback> raw=...... result=......KB
BL: layout app=0x........ staging=0x........ metadata=0x........
BL: update decision=<no-layout|invalid|accepted|written|verified>
BL: jump app=0x........ sp=0x........ reset=0x........ vtor=0x........ valid=0|1
```

重点先排查 `detect_flash_size_kb()` 的芯片型号/Flash 容量寄存器读取；在 `flash_size=0KB` 未修复前，应用层 OTA 日志无法证明镜像已更新。
