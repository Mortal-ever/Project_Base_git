# Coffee3CloseV3.0.0 创建与验证报告

日期：2026-09-07

## 结论

Coffee3Close 已建立为独立 Target。它复用公共 Transport、协议栈、设备库
和 RTOS 基础设施，业务、设备绑定、Server 寄存器语义、IO 映射、Workflow、日志、
OTA 和崩溃适配均位于 `Application/UserAPP/Coffee3CloseApp`。咖啡机绑定为公共
`coffee_machine_m50`，不是 Coffee2 的 F200。

本次没有烧录，没有编辑 `.ioc`，没有修改 Coffee2 业务实现。

## 已落地范围

- Keil Target：`Coffee3Close`，ARM Compiler V5.06u7，使用
  `Coffee3Close_CCM.sct`，输出文件名为 `Coffee3CloseV3_0_0`。
- GCC Target：`Coffee3CloseTarget`，提供 Debug/Release CMake preset，
  使用 `USE_COFFEE3=1` 和 `Coffee3CloseApp` 私有 include/source。
- 物理绑定：Robot、Coffee M50、Cup/Lid、Syrup、Ice、Scale、Power Meter、
  16 路输入和 16 路输出；M50 在 Bus2、Modbus unit 1。
- 业务闭环：两个储位残杯检查、线上储位二次拦截、线下单出口放杯、X3 空杯
  30 秒后升门、客户取餐 ACK、订单身份和 epoch、失败收尾、M50 清洗完成等待。
- 资源仲裁：自动订单、维护、IO 调试和 OTA 不能同时拥有输出；板载门上下输出
  互锁；IO 调试值按位图写入，先关闭门方向再设置目标输出。
- IO：X4/X5/X6/X7 的订单准入检查，M50 水箱低位触发 DO4 补水；高位或 20 秒
  停止，超时产生一次性告警，待有效状态和 ACK 后恢复。
- 设备轮询：周期 IO 刷新有未完成命令抑制，不因离线而无限堆积同类 refresh。

## 验证证据

### Keil

命令：

```text
UV4.exe -b STM32F407_Base.uvprojx -t Coffee3CloseV3.0.0 -j0
```

结果：`0 Error(s), 0 Warning(s)`。

Program Size：Code 202740、RO-data 8272、RW-data 584、ZI-data 144160 bytes。
AXF、HEX、BIN 输出为 `Coffee3CloseV3_0_0`。

### 静态契约测试

命令：

```text
python -X utf8 -m unittest tests.test_coffee2_correction_contracts
```

结果：22 项通过。测试覆盖原有 Coffee2 修正契约和 Coffee3 Target、M50、私有
Workflow、门/水箱/OTA 关键入口。

### GCC

GCC Release 在本轮源码最后修改前曾有通过快照；最后修改后的普通 Ninja 调度在
当前环境仍会在编译器启动前无输出挂起，已主动中止，不能把旧 Release 产物冒充
当前源码验证。Keil 构建和静态测试通过不等价于 GCC 当前源码已通过，需在本地
使用 VS Code/CMake 重新构建确认。

## 上板前必须确认

1. M50 实机寄存器返回的 16 个状态字、故障码和清洗状态值与公共驱动配置一致。
2. DO4 的真实电气点和 X4/X5/X6/X7 极性；确认 20 秒是现场允许的保护上限。
3. 机器人储位选择寄存器 `0x0032` 的实际契约，以及线上/线下订单编码。
4. 出餐门上下限位、X3 出餐杯检测和 30 秒空杯保持的实机边沿。
5. 断线、RTU 超时、取消、门限位冲突、M50 超时后的安全停机和恢复复位。
6. RAM/CCM/任务栈水位及所有 DMA 缓冲地址；本报告没有硬件和烧录证据。
