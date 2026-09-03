## 一、通信基础
| 项目 | 说明 |
| --- | --- |
| 从机地址 | 0x01（固定） |
| 通信方式 | Modbus-RTU |
| 波特率 | 57600 bps（默认） |
| 数据位 | 8 |
| 停止位 | 1 |
| 校验位 | 无 |


---

## 二、寄存器地址分布
| 地址范围 | 用途 | 访问方式 |
| --- | --- | --- |
| 0x1000~0x1FFF | 状态信息 | 只读 |
| 0x2000~0x2FFF | 控制指令 | 只写 |


---

## 三、功能码
| 功能码 | 名称 | 作用 |
| --- | --- | --- |
| 0x03 | 读保持寄存器 | 读取数据（查询状态） |
| 0x06 | 写单个寄存器 | 写入数据（控制咖啡机） |


---

## 四、颜色说明
<font style="color:#e91e63;">■</font> 从机地址（固定0x01）  
<font style="color:#4caf50;">■</font> 功能码（03读/06写）  
<font style="color:#ff9800;">■</font> 寄存器地址  
<font style="color:#9c27b0;">■</font> 数量/写入值  
<font style="color:#607d8b;">■</font> CRC校验码

---

## 五、读取指令汇总
| 操作 | 功能码 | 寄存器地址 | 数量 | 指令 |
| --- | --- | --- | --- | --- |
| 机器运行状态 | <font style="color:#4caf50;">03</font> | <font style="color:#ff9800;">0x1000</font> | <font style="color:#9c27b0;">1</font> | <font style="color:#e91e63;">01</font> <font style="color:#4caf50;">03</font> <font style="color:#ff9800;">10 00</font> <font style="color:#9c27b0;">00 01</font> <font style="color:#607d8b;">80 CA</font> |
| 故障信息 | <font style="color:#4caf50;">03</font> | <font style="color:#ff9800;">0x1001</font> | <font style="color:#9c27b0;">8</font> | <font style="color:#e91e63;">01</font> <font style="color:#4caf50;">03</font> <font style="color:#ff9800;">10 01</font> <font style="color:#9c27b0;">00 08</font> <font style="color:#607d8b;">11 0C</font> |
| 告警信息 | <font style="color:#4caf50;">03</font> | <font style="color:#ff9800;">0x1009</font> | <font style="color:#9c27b0;">7</font> | <font style="color:#e91e63;">01</font> <font style="color:#4caf50;">03</font> <font style="color:#ff9800;">10 09</font> <font style="color:#9c27b0;">00 07</font> <font style="color:#607d8b;">D0 CA</font> |
| 故障+告警 | <font style="color:#4caf50;">03</font> | <font style="color:#ff9800;">0x1001</font> | <font style="color:#9c27b0;">15</font> | <font style="color:#e91e63;">01</font> <font style="color:#4caf50;">03</font> <font style="color:#ff9800;">10 01</font> <font style="color:#9c27b0;">00 0F</font> <font style="color:#607d8b;">50 CE</font> |
| 完整状态 | <font style="color:#4caf50;">03</font> | <font style="color:#ff9800;">0x1000</font> | <font style="color:#9c27b0;">16</font> | <font style="color:#e91e63;">01</font> <font style="color:#4caf50;">03</font> <font style="color:#ff9800;">10 00</font> <font style="color:#9c27b0;">00 10</font> <font style="color:#607d8b;">C5 D2</font> |
| Action数量 | <font style="color:#4caf50;">03</font> | <font style="color:#ff9800;">0x1013</font> | <font style="color:#9c27b0;">1</font> | <font style="color:#e91e63;">01</font> <font style="color:#4caf50;">03</font> <font style="color:#ff9800;">10 13</font> <font style="color:#9c27b0;">00 01</font> <font style="color:#607d8b;">71 0F</font> |
| Action1进度 | <font style="color:#4caf50;">03</font> | <font style="color:#ff9800;">0x1014</font> | <font style="color:#9c27b0;">2</font> | <font style="color:#e91e63;">01</font> <font style="color:#4caf50;">03</font> <font style="color:#ff9800;">10 14</font> <font style="color:#9c27b0;">00 01</font> <font style="color:#607d8b;">C0 CE</font> |
| Action2进度 | <font style="color:#4caf50;">03</font> | <font style="color:#ff9800;">0x1016</font> | <font style="color:#9c27b0;">2</font> | <font style="color:#e91e63;">01</font> <font style="color:#4caf50;">03</font> <font style="color:#ff9800;">10 15</font> <font style="color:#9c27b0;">00 01</font> <font style="color:#607d8b;">91 0E</font> |


---

## 六、控制指令汇总
| 操作 | 功能码 | 寄存器地址 | 值 | 指令 |
| --- | --- | --- | --- | --- |
| 制作饮品 | <font style="color:#4caf50;">06</font> | <font style="color:#ff9800;">0x2000</font> | 0x0000~0x0031 | <font style="color:#e91e63;">01</font> <font style="color:#4caf50;">06</font> <font style="color:#ff9800;">20 00</font> <font style="color:#9c27b0;">00 XX</font> <font style="color:#607d8b;">xx xx</font> |
| 暂停制作 | <font style="color:#4caf50;">06</font> | <font style="color:#ff9800;">0x2001</font> | 0x0001 | <font style="color:#e91e63;">01</font> <font style="color:#4caf50;">06</font> <font style="color:#ff9800;">20 01</font> <font style="color:#9c27b0;">00 01</font> <font style="color:#607d8b;">xx xx</font> |
| 恢复制作 | <font style="color:#4caf50;">06</font> | <font style="color:#ff9800;">0x2001</font> | 0x0002 | <font style="color:#e91e63;">01</font> <font style="color:#4caf50;">06</font> <font style="color:#ff9800;">20 01</font> <font style="color:#9c27b0;">00 02</font> <font style="color:#607d8b;">xx xx</font> |
| 关机 | <font style="color:#4caf50;">06</font> | <font style="color:#ff9800;">0x200B</font> | 0x0001 | <font style="color:#e91e63;">01</font> <font style="color:#4caf50;">06</font> <font style="color:#ff9800;">20 0B</font> <font style="color:#9c27b0;">00 01</font> <font style="color:#607d8b;">xx xx</font> |
| 重启 | <font style="color:#4caf50;">06</font> | <font style="color:#ff9800;">0x200B</font> | 0x0002 | <font style="color:#e91e63;">01</font> <font style="color:#4caf50;">06</font> <font style="color:#ff9800;">20 0B</font> <font style="color:#9c27b0;">00 02</font> <font style="color:#607d8b;">xx xx</font> |
| 冲泡器冲洗 | <font style="color:#4caf50;">06</font> | <font style="color:#ff9800;">0x200C</font> | 0x0001 | <font style="color:#e91e63;">01</font> <font style="color:#4caf50;">06</font> <font style="color:#ff9800;">20 0C</font> <font style="color:#9c27b0;">00 01</font> <font style="color:#607d8b;">xx xx</font> |
| 搅拌器冲洗 | <font style="color:#4caf50;">06</font> | <font style="color:#ff9800;">0x200C</font> | 0x0003 | <font style="color:#e91e63;">01</font> <font style="color:#4caf50;">06</font> <font style="color:#ff9800;">20 0C</font> <font style="color:#9c27b0;">00 03</font> <font style="color:#607d8b;">xx xx</font> |
| 内部奶管冲洗 | <font style="color:#4caf50;">06</font> | <font style="color:#ff9800;">0x200C</font> | 0x0004 | <font style="color:#e91e63;">01</font> <font style="color:#4caf50;">06</font> <font style="color:#ff9800;">20 0C</font> <font style="color:#9c27b0;">00 04</font> <font style="color:#607d8b;">xx xx</font> |
| 奶系统冲洗 | <font style="color:#4caf50;">06</font> | <font style="color:#ff9800;">0x200C</font> | 0x0006 | <font style="color:#e91e63;">01</font> <font style="color:#4caf50;">06</font> <font style="color:#ff9800;">20 0C</font> <font style="color:#9c27b0;">00 06</font> <font style="color:#607d8b;">xx xx</font> |
| 故障确认 | <font style="color:#4caf50;">06</font> | <font style="color:#ff9800;">0x200D</font> | 故障代码 | <font style="color:#e91e63;">01</font> <font style="color:#4caf50;">06</font> <font style="color:#ff9800;">20 0D</font> <font style="color:#9c27b0;">XX XX</font> <font style="color:#607d8b;">xx xx</font> |
| 取消制作 | <font style="color:#4caf50;">06</font> | <font style="color:#ff9800;">0x200E</font> | 0x0000 | <font style="color:#e91e63;">01</font> <font style="color:#4caf50;">06</font> <font style="color:#ff9800;">20 0E</font> <font style="color:#9c27b0;">00 00</font> <font style="color:#607d8b;">E3 C9</font> |


---

## 七、帧格式说明
### 读取指令格式（功能码 0x03）
<font style="color:#e91e63;">01</font> <font style="color:#4caf50;">03</font> <font style="color:#ff9800;">10 00</font> <font style="color:#9c27b0;">00 01</font> <font style="color:#607d8b;">80 CA</font>

```plain
01 03 10 00 00 01 80 CA
└─┘└─┘└────┘└────┘└────┘
 从机  功能  起始   寄存器  CRC
 地址  码    地址   数量
```

### 写入指令格式（功能码 0x06）
<font style="color:#e91e63;">01</font> <font style="color:#4caf50;">06</font> <font style="color:#ff9800;">20 00</font> <font style="color:#9c27b0;">00 05</font> <font style="color:#607d8b;">xx xx</font>

```plain
01 06 20 00 00 05 xx xx
└─┘└─┘└────┘└────┘└────┘
 从机  功能  寄存器 写入值  CRC
 地址  码    地址
```

### 返回值格式
<font style="color:#e91e63;">01</font> <font style="color:#4caf50;">03</font> <font style="color:#9c27b0;">02</font> <font style="color:#ff9800;">00 FF</font> <font style="color:#607d8b;">xx xx</font>

```plain
01 03 02 00 FF xx xx
└─┘└─┘└─┘└────┘└────┘
 从机  功能  字节数  数据   CRC
 地址  码
```

### 规则
+ 2字节数据：**高字节在前**（大端序）
+ CRC校验：**低字节在前**（小端序）

### 异常响应格式
当从机处理请求出错时，不会返回正常响应，而是返回**异常响应**：功能码的最高位被置 1（即 `原功能码 | 0x80`），随后跟 1 个字节的异常码。

| 请求功能码 | 异常响应功能码 |
| --- | --- |
| 0x03（读） | 0x83 |
| 0x06（写） | 0x86 |


异常帧结构（以写指令出错为例）：

<font style="color:#e91e63;">01</font><font style="color:#4caf50;">86</font><font style="color:#ff9800;">20 00</font><font style="color:#9c27b0;">00 04</font><font style="color:#607d8b;">82 17</font>

```plain
01 86 20 00 00 04 82 17
└─┘└─┘└────┘└──┘└─┘└────┘
从机 异常  寄存器 -  异常 CRC
地址 功能码 地址      码
```

注：写指令（0x86）异常响应会带回寄存器地址；读指令（0x83）异常响应格式为 `从机地址 + 0x83 + 异常码 + CRC`。

### 异常码表（Modbus 协议层）
此表为**通信协议层**的异常码，与附录一/附录二的机器故障/告警码含义不同，请勿混淆。

| 异常码 | 含义 | 典型场景 |
| --- | --- | --- |
| 0x01 | 不支持的功能码 | 使用了 0x03/0x06 以外的功能码；暂停指令参数非法 |
| 0x02 | 不合法的数据地址 | 寄存器地址超出范围；读取数量超限 |
| 0x03 | 不合法的数据值（参数非法） | 写入值超出允许范围（如制作位置、冲洗类型非法） |
| 0x04 | 从机设备故障 | 设备内部处理失败、机器状态不可用 |
| 0x05 | 确认 | 指令已受理（长耗时操作） |
| 0x06 | 从机设备忙碌 | 机器非空闲状态，拒绝制作/暂停等操作 |


---

## 八、读取完整状态示例与解析
### 请求指令
<font style="color:#e91e63;">01</font> <font style="color:#4caf50;">03</font> <font style="color:#ff9800;">10 00</font> <font style="color:#9c27b0;">00 10</font> <font style="color:#607d8b;">C5 D2</font>

```plain
01 03 10 00 00 10 C5 D2
└─┘└─┘└────┘└────┘└────┘
 从机  功能  起始   寄存器  CRC
 地址  码    地址   数量=16
```

### 返回数据
<font style="color:#e91e63;">01</font> <font style="color:#4caf50;">03</font> <font style="color:#9c27b0;">20</font> <font style="color:#ff9800;">00 FF</font> <font style="color:#ff9800;">00 0F</font> <font style="color:#ff9800;">00 10</font> <font style="color:#ff9800;">00 CC</font> <font style="color:#ff9800;">00 00</font> <font style="color:#ff9800;">00 00</font> <font style="color:#ff9800;">00 00</font> <font style="color:#ff9800;">00 00</font> <font style="color:#ff9800;">00 00</font> <font style="color:#ff9800;">00 00</font> <font style="color:#ff9800;">00 0E</font> <font style="color:#ff9800;">00 04</font> <font style="color:#ff9800;">00 00</font> <font style="color:#ff9800;">00 00</font> <font style="color:#ff9800;">00 00</font> <font style="color:#ff9800;">00 00</font> <font style="color:#ff9800;">00 00</font> <font style="color:#ff9800;">00 00</font> <font style="color:#ff9800;">00 00</font> <font style="color:#ff9800;">00 00</font> <font style="color:#607d8b;">59 40</font>

### 数据解析
| 位置 | 寄存器地址 | 数据 | 含义 |
| --- | --- | --- | --- |
| 1 | 0x1000 | <font style="color:#ff9800;">00 FF</font> | **状态：设备空闲** |
| 2 | 0x1001 | <font style="color:#ff9800;">00 0F</font> | 故障代码15 = 水箱不在位 |
| 3 | 0x1002 | <font style="color:#ff9800;">00 10</font> | 故障代码16 = 蓄水盘不在位 |
| 4 | 0x1003 | <font style="color:#ff9800;">00 CC</font> | 故障代码204 = 咖啡分向阀警告 |
| 5 | 0x1004 | <font style="color:#ff9800;">00 00</font> | 无故障 |
| 6 | 0x1005 | <font style="color:#ff9800;">00 00</font> | 无故障 |
| 7 | 0x1006 | <font style="color:#ff9800;">00 00</font> | 无故障 |
| 8 | 0x1007 | <font style="color:#ff9800;">00 00</font> | 无故障 |
| 9 | 0x1008 | <font style="color:#ff9800;">00 00</font> | 无故障 |
| 10 | 0x1009 | <font style="color:#ff9800;">00 0E</font> | 告警代码14 = 冰箱不在位 |
| 11 | 0x100A | <font style="color:#ff9800;">00 04</font> | 告警代码4 = 云服务没连接 |
| 12 | 0x100B | <font style="color:#ff9800;">00 00</font> | 无告警 |
| 13 | 0x100C | <font style="color:#ff9800;">00 00</font> | 无告警 |
| 14 | 0x100D | <font style="color:#ff9800;">00 00</font> | 无告警 |
| 15 | 0x100E | <font style="color:#ff9800;">00 00</font> | 无告警 |
| 16 | 0x100F | <font style="color:#ff9800;">00 00</font> | 无告警 |


### 解析结果
```plain
【状态】00 FF → 0x00FF → 设备空闲
【故障】3个
  - 00 0F → 15 → 水箱不在位
  - 00 10 → 16 → 蓄水盘不在位
  - 00 CC → 204 → 咖啡分向阀警告
【告警】2个
  - 00 0E → 14 → 冰箱不在位
  - 00 04 → 4 → 云服务没连接
```

### 注意事项
+ 故障/告警代码为 `00 00` 表示无故障/告警
+ 非零值表示对应的故障或告警代码
+ 故障代码为16进制，协议中故障编号是10进制

---

## 九、机器运行状态返回值
| 状态 | 十六进制值 | 返回值 |
| --- | --- | --- |
| 正在开机 | 0x0001 | <font style="color:#e91e63;">01</font> <font style="color:#4caf50;">03</font> <font style="color:#9c27b0;">02</font> <font style="color:#ff9800;">00 01</font> <font style="color:#607d8b;">79 84</font> |
| 关机冲洗 | 0x0002 | <font style="color:#e91e63;">01</font> <font style="color:#4caf50;">03</font> <font style="color:#9c27b0;">02</font> <font style="color:#ff9800;">00 02</font> <font style="color:#607d8b;">39 85</font> |
| 开机冲洗 | 0x0003 | <font style="color:#e91e63;">01</font> <font style="color:#4caf50;">03</font> <font style="color:#9c27b0;">02</font> <font style="color:#ff9800;">00 03</font> <font style="color:#607d8b;">F8 45</font> |
| 冲泡器冲洗 | 0x0004 | <font style="color:#e91e63;">01</font> <font style="color:#4caf50;">03</font> <font style="color:#9c27b0;">02</font> <font style="color:#ff9800;">00 04</font> <font style="color:#607d8b;">B9 87</font> |
| 冲泡器清洗 | 0x0005 | <font style="color:#e91e63;">01</font> <font style="color:#4caf50;">03</font> <font style="color:#9c27b0;">02</font> <font style="color:#ff9800;">00 05</font> <font style="color:#607d8b;">78 47</font> |
| 奶系统清洗 | 0x0006 | <font style="color:#e91e63;">01</font> <font style="color:#4caf50;">03</font> <font style="color:#9c27b0;">02</font> <font style="color:#ff9800;">00 06</font> <font style="color:#607d8b;">38 46</font> |
| 内部奶管冲洗 | 0x0007 | <font style="color:#e91e63;">01</font> <font style="color:#4caf50;">03</font> <font style="color:#9c27b0;">02</font> <font style="color:#ff9800;">00 07</font> <font style="color:#607d8b;">F9 86</font> |
| 除垢-蒸汽 | 0x000A | <font style="color:#e91e63;">01</font> <font style="color:#4caf50;">03</font> <font style="color:#9c27b0;">02</font> <font style="color:#ff9800;">00 0A</font> <font style="color:#607d8b;">38 43</font> |
| 除垢-咖啡 | 0x000B | <font style="color:#e91e63;">01</font> <font style="color:#4caf50;">03</font> <font style="color:#9c27b0;">02</font> <font style="color:#ff9800;">00 0B</font> <font style="color:#607d8b;">F9 83</font> |
| 冲泡器预热冲洗 | 0x000C | <font style="color:#e91e63;">01</font> <font style="color:#4caf50;">03</font> <font style="color:#9c27b0;">02</font> <font style="color:#ff9800;">00 0C</font> <font style="color:#607d8b;">B8 41</font> |
| 搅拌器冲洗 | 0x000E | <font style="color:#e91e63;">01</font> <font style="color:#4caf50;">03</font> <font style="color:#9c27b0;">02</font> <font style="color:#ff9800;">00 0E</font> <font style="color:#607d8b;">39 80</font> |
| 奶沫器冲洗 | 0x000F | <font style="color:#e91e63;">01</font> <font style="color:#4caf50;">03</font> <font style="color:#9c27b0;">02</font> <font style="color:#ff9800;">00 0F</font> <font style="color:#607d8b;">F8 40</font> |
| 清空蒸汽水路 | 0x0010 | <font style="color:#e91e63;">01</font> <font style="color:#4caf50;">03</font> <font style="color:#9c27b0;">02</font> <font style="color:#ff9800;">00 10</font> <font style="color:#607d8b;">B9 88</font> |
| 清空咖啡水路 | 0x0011 | <font style="color:#e91e63;">01</font> <font style="color:#4caf50;">03</font> <font style="color:#9c27b0;">02</font> <font style="color:#ff9800;">00 11</font> <font style="color:#607d8b;">78 48</font> |
| 组件测试 | 0x0012 | <font style="color:#e91e63;">01</font> <font style="color:#4caf50;">03</font> <font style="color:#9c27b0;">02</font> <font style="color:#ff9800;">00 12</font> <font style="color:#607d8b;">38 49</font> |
| 自动冲洗 | 0x0014 | <font style="color:#e91e63;">01</font> <font style="color:#4caf50;">03</font> <font style="color:#9c27b0;">02</font> <font style="color:#ff9800;">00 14</font> <font style="color:#607d8b;">B8 4B</font> |
| 校准 | 0x0015 | <font style="color:#e91e63;">01</font> <font style="color:#4caf50;">03</font> <font style="color:#9c27b0;">02</font> <font style="color:#ff9800;">00 15</font> <font style="color:#607d8b;">79 8B</font> |
| **设备空闲** | **0x00FF** | <font style="color:#e91e63;">01</font> <font style="color:#4caf50;">03</font> <font style="color:#9c27b0;">02</font> <font style="color:#ff9800;">00 FF</font> <font style="color:#607d8b;">F8 04</font> |
| 饮品制作中 | 0x1004 | <font style="color:#e91e63;">01</font> <font style="color:#4caf50;">03</font> <font style="color:#9c27b0;">02</font> <font style="color:#ff9800;">10 04</font> <font style="color:#607d8b;">B4 47</font> |
| 故障解除中 | 0x2043 | <font style="color:#e91e63;">01</font> <font style="color:#4caf50;">03</font> <font style="color:#9c27b0;">02</font> <font style="color:#ff9800;">20 43</font> <font style="color:#607d8b;">E0 75</font> |


---

## 九、状态码速查表（Sync ID）
| 状态码 | 含义 |
| --- | --- |
| 0x0001 | 正在开机 |
| 0x0002 | 关机冲洗 |
| 0x0003 | 开机冲洗 |
| 0x0004 | 冲泡器冲洗 |
| 0x0005 | 冲泡器清洗 |
| 0x0006 | 奶系统清洗 |
| 0x0007 | 内部奶管冲洗 |
| 0x000A | 除垢-蒸汽 |
| 0x000B | 除垢-咖啡 |
| 0x000C | 冲泡器预热冲洗 |
| 0x000E | 搅拌器冲洗 |
| 0x000F | 奶沫器冲洗 |
| 0x0010 | 清空蒸汽水路 |
| 0x0011 | 清空咖啡水路 |
| 0x0012 | 组件测试 |
| 0x0014 | 自动冲洗 |
| 0x0015 | 校准 |
| **0x00FF** | **设备空闲** |
| 0x1000~0x1FFF | 饮品制作中 |
| 0x2000~0x2FFF | 故障解除中 |
| 0xF000 | IOT锁机 |


---

## 十、Action ID 定义表
| ID | Action名称 | 类别 |
| --- | --- | --- |
| 2 | 系统初始化 | 基础 |
| 3 | 故障修复 | 基础 |
| 4 | 暂停 | 基础 |
| 5 | bypass热水 | 饮品 |
| 6 | 磨豆 | 饮品 |
| 7 | 冲泡 | 饮品 |
| 8 | 磨豆+冲泡 | 饮品 |
| 9 | 粉料 | 饮品 |
| 10 | 蒸汽（热牛奶/奶沫） | 饮品 |
| 11 | 蒸汽杆 | 饮品 |
| 12 | 冲泡器预热冲洗 | 维护 |
| 13 | 冲泡器冲洗 | 维护 |
| 15 | 冲泡器清洗 | 维护 |
| 16 | 奶沫器清洗 | 维护 |
| 18 | 奶管冲洗 | 维护 |


---

## 十一、寄存器地址分布图
```plain
0x1000 │ 状态(Sync ID)
───────┼─────────────────────
0x1001 │ Error1 │
0x1002 │ Error2 │
0x1003 │ Error3 │  故障信息
0x1004 │ Error4 │  (8个寄存器)
0x1005 │ Error5 │
0x1006 │ Error6 │
0x1007 │ Error7 │
0x1008 │ Error8 │
───────┼─────────────────────
0x1009 │ Alarm1 │
0x100A │ Alarm2 │
0x100B │ Alarm3 │  告警信息
0x100C │ Alarm4 │  (7个寄存器)
0x100D │ Alarm5 │
0x100E │ Alarm6 │
0x100F │ Alarm7 │
───────┼─────────────────────
0x1010 │ 云平台UI位置
0x1011 │ 订单号高32位
0x1012 │ 订单号低32位
───────┼─────────────────────
0x1013 │ Action数量
0x1014~0x1015 │ Action1进度
0x1016~0x1017 │ Action2进度
═════════════════════════════
0x2000 │ 制作饮品(写)
0x2001 │ 暂停/恢复(写)
0x200B │ 关机/重启(写)
0x200C │ 执行冲洗(写)
0x200D │ 故障确认(写)
0x200E │ 取消制作(写)
═════════════════════════════
```

---

## 十二、快速参考
| 操作 | 指令 |
| --- | --- |
| 读完整状态 | <font style="color:#e91e63;">01</font> <font style="color:#4caf50;">03</font> <font style="color:#ff9800;">10 00</font> <font style="color:#9c27b0;">00 10</font> <font style="color:#607d8b;">C5 D2</font> |
| 读运行状态 | <font style="color:#e91e63;">01</font> <font style="color:#4caf50;">03</font> <font style="color:#ff9800;">10 00</font> <font style="color:#9c27b0;">00 01</font> <font style="color:#607d8b;">80 CA</font> |
| 读故障信息 | <font style="color:#e91e63;">01</font> <font style="color:#4caf50;">03</font> <font style="color:#ff9800;">10 01</font> <font style="color:#9c27b0;">00 08</font> <font style="color:#607d8b;">11 0C</font> |
| 读告警信息 | <font style="color:#e91e63;">01</font> <font style="color:#4caf50;">03</font> <font style="color:#ff9800;">10 09</font> <font style="color:#9c27b0;">00 07</font> <font style="color:#607d8b;">D0 CA</font> |
| 制作饮品(位置5) | <font style="color:#e91e63;">01</font> <font style="color:#4caf50;">06</font> <font style="color:#ff9800;">20 00</font> <font style="color:#9c27b0;">00 05</font> <font style="color:#607d8b;">xx xx</font> |
| 暂停制作 | <font style="color:#e91e63;">01</font> <font style="color:#4caf50;">06</font> <font style="color:#ff9800;">20 01</font> <font style="color:#9c27b0;">00 01</font> <font style="color:#607d8b;">xx xx</font> |
| 恢复制作 | <font style="color:#e91e63;">01</font> <font style="color:#4caf50;">06</font> <font style="color:#ff9800;">20 01</font> <font style="color:#9c27b0;">00 02</font> <font style="color:#607d8b;">xx xx</font> |
| 关机 | <font style="color:#e91e63;">01</font> <font style="color:#4caf50;">06</font> <font style="color:#ff9800;">20 0B</font> <font style="color:#9c27b0;">00 01</font> <font style="color:#607d8b;">xx xx</font> |
| 重启 | <font style="color:#e91e63;">01</font> <font style="color:#4caf50;">06</font> <font style="color:#ff9800;">20 0B</font> <font style="color:#9c27b0;">00 02</font> <font style="color:#607d8b;">xx xx</font> |
| 冲泡器冲洗 | <font style="color:#e91e63;">01</font> <font style="color:#4caf50;">06</font> <font style="color:#ff9800;">20 0C</font> <font style="color:#9c27b0;">00 01</font> <font style="color:#607d8b;">xx xx</font> |
| 取消制作 | <font style="color:#e91e63;">01</font> <font style="color:#4caf50;">06</font> <font style="color:#ff9800;">20 0E</font> <font style="color:#9c27b0;">00 00</font> <font style="color:#607d8b;">E3 C9</font> |


---

## 十三、CRC计算验证
CRC生成工具：[https://www.23bei.com/tool/59.html](https://www.23bei.com/tool/59.html)

| 输入数据 | CRC16 (低字节在前) |
| --- | --- |
| `01 03 10 00 00 01` | `80 CA` |
| `01 03 10 00 00 10` | `C5 D2` |
| `01 06 20 00 00 05` | `4D A5` |
| `01 06 20 01 00 01` | `DB DA` |
| `01 06 20 01 00 02` | `9A D9` |
| `01 06 20 0B 00 01` | `4D D7` |
| `01 06 20 0C 00 01` | `1C D3` |




# 附录一：错误码表
| 错误码 | 级别 | 描述 | 备注 |
| --- | --- | --- | --- |
| 1 | ERROR | 主板故障 | **E1** |
| 2 | ERROR | 锅炉温度过高 | **E2** |
| 3 | ERROR | 蒸汽锅炉温度过高 | **E3** |
| 4 | ERROR | 锅炉温度过低 | **E4** |
| 5 | ERROR | 蒸汽锅炉温度过低 | **E5** |
| 6 | ERROR | 锅炉加热过快 | **E6** |
| 7 | ERROR | 蒸汽锅炉加热过快 | **E7** |
| 8 | ERROR | 锅炉加热过慢 | **E8** |
| 9 | ERROR | 蒸汽锅炉加热过慢 | **E9** |
| 10 | ERROR | 锅炉不继续加热 | **E10** |
| 11 | ERROR | 蒸汽锅炉不继续加热 | **E11** |
| 12 | ERROR | 水源温度检测组件异常 | **E12** |
| 13 | SENSOR | 水箱丢失 |  |
| 14 | SENSOR | 蓄水盘丢失 |  |
| 15 | SENSOR | 渣盒丢失 |  |
| 16 | SENSOR | 左豆仓丢失 |  |
| 17 | SENSOR | 右豆仓丢失 |  |
| 18 | SENSOR | 前粉仓丢失 |  |
| 19 | SENSOR | 后粉仓丢失 |  |
| 20 | SENSOR | 水箱低水位 |  |
| 21 | SENSOR | 清空蓄水盘 |  |
| 22 | ADMIN | 左磨豆组异常 | **E22** |
| 23 | ADMIN | 右磨豆组异常 | **E23** |
| 24 | SENSOR | 左豆仓无豆 |  |
| 25 | SENSOR | 右豆仓无豆 |  |
| 26 | SENSOR | 前粉仓无粉 |  |
| 27 | SENSOR | 后粉仓无粉 |  |
| 28 | SENSOR | 使用牛奶温度高 |  |
| 29 | SENSOR | 使用牛奶温度低 |  |
| 30 | SENSOR | 请安装冲泡器 |  |
| 31 | REDIRECT | 咖啡水路系统缺水 |  |
| 32 | REDIRECT | 蒸汽水路系统缺水 |  |
| 33 | REDIRECT | 冲泡器异常(步数大于阈值) |  |
| 34 | REDIRECT | 冲泡器异常(步数小于阈值) |  |
| 35 | REDIRECT | 冲泡器内咖啡粉过少 |  |
| 36 | REDIRECT | 冲泡器内咖啡粉过多 |  |
| 37 | MANUAL | 搅拌器警告 |  |
| 38 | MANUAL | 前粉仓推粉警告 |  |
| 39 | MANUAL | 后粉仓推粉警告 |  |
| 40 | IGNORE | 水源温度高 |  |
| 41 | IGNORE | 水源温度低 |  |
| 42 | IGNORE | 渣盒风扇异常 |  |
| 43 | IGNORE | 粉料风扇异常 |  |
| 44 | ERROR | 异常开机 | **E44** |
| 45 | MANUAL | 软件警告 |  |
| 46 | REDIRECT | 咖啡水路流速过低 |  |
| 47 | REDIRECT | 蒸汽水路流速过低 |  |
| 48 | MANUAL | 冲泡器未复位 |  |
| 49 | SENSOR | 请移除水箱 |  |
| 50 | MANUAL | 咖啡渣是否已清空？ |  |
| 51 | SENSOR | 请连接牛奶NTC |  |
| 52 | SENSOR | 药片盒不在位 |  |
| 53 | REDIRECT | 咖啡水路流速过高 |  |
| 54 | REDIRECT | 蒸汽水路流速过高 |  |
| 55 | SENSOR | 清空渣盒 |  |
| 56 | SENSOR | 水桶组件通讯异常 |  |
| 57 | MANUAL | 水桶组件水路警告 |  |
| 58 | ERROR | 锅炉不继续加热 | **E58** |
| 59 | SENSOR | 水箱低水位 |  |
| 60 | SENSOR | 冰箱不在位 |  |
| 61 | SENSOR | 冰箱左侧奶盒缺奶 |  |
| 62 | SENSOR | 冰箱右侧奶盒缺奶 |  |
| 63 | MANUAL | 冰箱#1泵警告 |  |
| 64 | MANUAL | 冰箱#2泵警告 |  |
| 65 | SENSOR | 糖浆机不在位 |  |
| 66 | MANUAL | 糖浆机#1泵警告 |  |
| 67 | MANUAL | 糖浆机#2泵警告 |  |
| 68 | MANUAL | 糖浆机#3泵警告 |  |
| 69 | MANUAL | 糖浆机#4泵警告 |  |
| 70 | ERROR | 蒸汽锅炉水位异常 | **E70** |
| 71 | ERROR | 出奶口NTC异常 | **E71** |
| 72 | ERROR | TDS NTC异常 | **E72** |
| 73 | MANUAL | 奶泵部件警告 |  |
| 74 | IGNORE_USE | 药盒缺药，请投放专用规格药球 |  |
| 75 | MANUAL | 落药部件警告 |  |
| 76 | ADMIN | 左磨豆粗细调节部件警告 | **E76** |
| 77 | ADMIN | 右磨豆粗细调节部件警告 | **E77** |
| 78 | REDIRECT | 奶盒缺奶警告 |  |
| 79 | REDIRECT |  |  |
| 80 | SENSOR | 请检查水箱是否缺水？加水后点击确认 |  |
| 81 | SENSOR | 搅拌器不在位 |  |
| 94 | SENSOR | 左侧搅拌器警告 |  |
| 95 | ERROR | 蒸汽杆NTC异常 | **E95** |
| 200 | MANUAL | 咖啡系统水路警告 |  |
| 201 | MANUAL | 蒸汽系统水路警告 |  |
| 202 | MANUAL | 冲泡器行程警告 |  |
| 203 | MANUAL | 冲泡器内粉量警告 |  |
| 204 | MANUAL | 自进水水路警告 |  |
| 205 | MANUAL | 奶盒缺奶警告 |  |
| 301 | SENSOR | 豆仓丢失 |  |
| 302 | SENSOR | 豆仓无豆 |  |
| 303 | SENSOR | 粉仓丢失 |  |
| 304 | SENSOR | 粉仓无粉 |  |
| 305 | ADMIN | 磨豆组异常 | **E305** |
| 306 | MANUAL | 粉仓推粉警告 |  |
| 307 | ERROR | 推粉电机异常 | **E307** |
| 308 | SENSOR | 冰箱奶盒缺奶 |  |
| 309 | SENSOR | 前粉仓不在位 |  |
| 310 | SENSOR | 后粉仓不在位 |  |
| 311 | SENSOR | 前粉仓无粉 |  |
| 312 | SENSOR | 后粉仓无粉 |  |
| 313 | MANUAL | 前粉仓推粉警告 |  |
| 314 | MANUAL | 后粉仓推粉警告 |  |
| 315 | MANUAL | 前推粉电机异常 |  |
| 316 | MANUAL | 后推粉电机异常 |  |
| 317 | MANUAL | 右侧搅拌器警告 |  |
| 318 | MANUAL | 右侧搅拌器异常 |  |
| 319 | ADMIN | 磨豆粗细调节部件警告 | **E319** |
| 400 | ERROR | 咖啡水路异常 | **E400** |
| 401 | ERROR | 蒸汽水路异常 | **E401** |
| 402 | ERROR | 冲泡器异常 | **E402** |
| 403 | ERROR | 奶泵部件异常 | **E403** |
| 404 | ERROR | 落药部件异常 | **E404** |
| 405 | ERROR | 搅拌器异常 | **E405** |
| 406 | ERROR | 前推粉电机异常 | **E406** |
| 407 | ERROR | 后推粉电机异常 | **E407** |
| 408 | ERROR | 左磨豆粗细调节部件异常 | **E408** |
| 409 | ERROR | 右磨豆粗细调节部件异常 | **E409** |
| 410 | ERROR | 自进水水路异常 | **E410** |
| 411 | ERROR | 奶盒补奶部件异常 | **E411** |
| 600 | SENSOR | 咖啡系统温度低 正在加热 |  |
| 601 | SENSOR | 蒸汽系统温度低 正在加热 |  |


---

# 附录二：错误码表
| 错误码 | 级别 | 描述 | 备注 |
| --- | --- | --- | --- |
| 1 | CTR | 通讯异常 |  |
| 2 | CTR | 通讯异常（Ⅱ） |  |
| 3 | CLOUD | 网络无连接 |  |
| 4 | CLOUD | 云服务无连接 |  |
| 5 | CLOUD | 正在维护 |  |
| 6 | MAINTAIN | 冲泡器清洗 |  |
| 7 | MAINTAIN | 奶系统清洗 |  |
| 8 | MAINTAIN | 热水锅炉除垢 |  |
| 9 | MAINTAIN | 锅炉除垢 |  |
| 10 | MAINTAIN_FORCE | 强制冲泡器清洗 |  |
| 11 | MAINTAIN_FORCE | 强制奶系统清洗 |  |
| 12 | MAINTAIN_FORCE | 强制热水锅炉除垢 |  |
| 13 | MAINTAIN_FORCE | 强制锅炉除垢 |  |
| 14 | TIPS | 冰箱未安装 |  |
| 16 | MAINTAIN_FORCE | 强制糖浆机清洗 |  |
| 18 | MAINTAIN_FORCE | 强制冰箱蠕动泵清洗 |  |
| 19 | TIPS | 请更换滤水器 |  |
| 20 | CLOUD | 奶盒缺奶 |  |
| 21 | TIPS | 风味不足（#1） |  |
| 22 | TIPS | 风味不足（#2） |  |
| 23 | TIPS | 风味不足（#3） |  |
| 24 | TIPS | 风味不足（#4） |  |
| 25 | TIPS | 风味不足（左） |  |
| 26 | TIPS | 风味不足（右） |  |
| 27 | TIPS | 打印机连接失败 |  |
| 28 | TIPS | 机器人CH34X串口工具未插入 |  |
| 29 | TIPS | 落杯机不在位 |  |
| 30 | TIPS | 请取下粉料搅拌器，手动清洗搅拌器内部 |  |
| 31 | MAINTAIN_FORCE | 强制搅拌器手动清洗 |  |
| 32 | MAINTAIN | 咖啡机一键清洗 |  |
| 33 | MAINTAIN_FORCE | 强制咖啡机一键清洗 |  |




# 附录三：饮品故障关联关系
## 一、饮品通用黑名单故障（所有饮品Action均不可做）
| Action ID | 故障来源 | 故障 ID | 故障名称 |
| --- | :--- | --- | --- |
| 通用 | CTR | 1 | 主板故障 |
| | CTR | 2 | 锅炉温度过高 |
| | CTR | 3 | 蒸汽锅炉温度过高 |
| | CTR | 4 | 锅炉温度过低 |
| | CTR | 5 | 蒸汽锅炉温度过低 |
| | CTR | 6 | 锅炉加热过快 |
| | CTR | 7 | 蒸汽锅炉加热过快 |
| | CTR | 8 | 锅炉加热过慢 |
| | CTR | 9 | 蒸汽锅炉加热过慢 |
| | CTR | 10 | 锅炉不继续加热 |
| | CTR | 11 | 蒸汽锅炉不继续加热 |
| | CTR | 12 | 水源温度检测组件异常 |
| | CTR | 44 | 异常开机 |
| | CTR | 58 | 锅炉不继续加热 |
| | CTR | 70 | 蒸汽锅炉水位异常 |
| | CTR | 71 | 出奶口NTC异常 |
| | CTR | 72 | TDS NTC异常 |
| | CTR | 13 | 水箱丢失 |
| | CTR | 14 | 蓄水盘丢失 |
| | CTR | 15 | 渣盒丢失 |
| | CTR | 16 | 左豆仓丢失 |
| | CTR | 17 | 右豆仓丢失 |
| | CTR | 18 | 前粉仓丢失 |
| | CTR | 19 | 后粉仓丢失 |
| | CTR | 20 | 水箱低水位 |
| | CTR | 21 | 清空蓄水盘 |
| | CTR | 59 | 水箱低水位 |
| | CTR | 79 | 自进水水路警告 |
| | CTR | 45 | 软件警告 |
| | HMI | 1 | 通讯异常 |
| | HMI | 12 | 强制热水锅炉除垢 |
| | HMI | 13 | 强制锅炉除垢 |
| | HMI | 16 | 强制糖浆机清洗 |
| | HMI | 18 | 强制冰箱蠕动泵清洗 |
| | HMI | 33 | 强制咖啡机一键清洗 |


## 二、饮品Action黑名单故障（当前Action不可做）
| Action ID（名称） | 条件 | 故障来源 | 故障 ID | 故障名称 |
| :--- | --- | :--- | --- | --- |
| 4（暂停） | 无 | 无 | 无 | 无 |
| 5（热水、常温水） | 高温/常温 | CTR | 31 | 咖啡系统水路警告 |
| | | CTR | 46 | |
| | | CTR | 53 | |
| | 高温 | CTR | 600 | 咖啡系统温度低正在加热 |
| 6（咖啡、滤式咖啡）-（磨豆） | 左右共用 | CTR | 30 | 冲泡器不在位 |
| | | HMI | 10 | 强制冲泡器清洗 |
| | 左侧磨豆机 | CTR | 24 | 左豆仓无豆 |
| | | CTR | 22 | 左磨豆组异常 |
| | 右侧磨豆机 | CTR | 25 | 右豆仓无豆 |
| | | CTR | 23 | 右磨豆组异常 |
| 7（咖啡、滤式咖啡）-（冲泡） | 无 | CTR | 30 | 冲泡器不在位 |
| | | CTR | 31 | 咖啡系统水路警告 |
| | | CTR | 46 | |
| | | CTR | 53 | |
| | | CTR | 33 | 冲泡器行程警告 |
| | | CTR | 34 | |
| | | CTR | 35 | 冲泡器内粉量警告 |
| | | CTR | 36 | |
| | | CTR | 600 | 咖啡系统温度低正在加热 |
| | | CTR | 48 | 冲泡器未复位 |
| | | CTR | 50 | 咖啡渣是否已清空？ |
| | | CTR | 55 | 清空渣盒 |
| 8（咖啡、滤式咖啡）-（磨豆+冲泡） | 左右共用 | CTR | 30 | 冲泡器不在位 |
| | | CTR | 31 | 咖啡系统水路警告 |
| | | CTR | 46 | |
| | | CTR | 53 | |
| | | CTR | 33 | 冲泡器行程警告 |
| | | CTR | 34 | |
| | | CTR | 35 | 冲泡器内粉量警告 |
| | | CTR | 36 | |
| | | CTR | 600 | 咖啡系统温度低正在加热 |
| | | CTR | 48 | 冲泡器未复位 |
| | | CTR | 50 | 咖啡渣是否已清空？ |
| | | CTR | 55 | 清空渣盒 |
| | | HMI | 10 | 强制冲泡器清洗 |
| | 左侧磨豆机 | CTR | 24 | 左豆仓无豆 |
| | | CTR | 22 | 左磨豆组异常 |
| | 右侧磨豆机 | CTR | 25 | 右豆仓无豆 |
| | | CTR | 23 | 右磨豆组异常 |
| 9（粉料） | 前后共用 | CTR | 31 | 咖啡系统水路警告 |
| | | CTR | 46 | |
| | | CTR | 53 | |
| | | CTR | 37 | 搅拌器异常 |
| | | CTR | 600 | 咖啡系统温度低正在加热 |
| | | CTR | 81 | 搅拌器不在位 |
| | | HMI | 31 | 强制搅拌器手动清洗   |
| | 前粉仓 | CTR | 38 | 前粉仓推粉警告 |
| | | CTR | 26 | 前粉仓无粉 |
| | 后粉仓 | CTR | 39 | 后粉仓推粉警告 |
| | | CTR | 27 | 后粉仓无粉 |
| 10（热奶、热奶沫、冷奶、冷奶沫） | 左右奶盒共用 | CTR | 78 | 奶盒缺奶告警 |
| | | CTR | 31 | 咖啡系统水路警告 |
| | | CTR | 46 | |
| | | CTR | 53 | |
| | | CTR | 32 | 蒸汽系统水路警告 |
| | | CTR | 47 | |
| | | CTR | 54 | |
| | | CTR | 51 | 请连接牛奶NTC |
| | | CTR | 600 | 咖啡系统温度低正在加热 |
| | | CTR | 601 | 蒸汽系统温度低正在加热 |
| | | CTR | 60 | 冰箱不在位 |
| | | HMI | 11 | 强制奶系统清洗   |
| | | HMI | 14 | 冰箱未安装 |
| | | HMI | 20 | 奶盒缺奶 |
| | 左侧奶盒 | CTR | 61 | 冰箱左侧奶盒缺奶 |
| | 右侧奶盒 | CTR | 62 | 冰箱右侧奶盒缺奶 |
| 11（蒸汽杆）<br/><br/><br/><br/><br/> | 无 | CTR | 32 | 蒸汽系统水路警告 |
| | | CTR | 47 | |
| | | CTR | 54 | |
| | | CTR | 601 | 蒸汽系统温度低正在加热 |
| 12（冲泡器预热）<br/><br/><br/><br/><br/><br/><br/><br/><br/><br/> | 无<br/><br/><br/><br/><br/><br/><br/><br/><br/><br/> | CTR | 30 | 冲泡器不在位 |
| | | CTR | 31 | 咖啡系统水路警告 |
| | | CTR | 46 | |
| | | CTR | 53 | |
| | | CTR | 33 | 冲泡器行程警告 |
| | | CTR | 34 | |
| | | CTR | 48 | 冲泡器未复位 |
| 26（冰箱风味） | 泵1、泵2共用 | CTR | 60 | 冰箱不在位 |
| | | CTR | 30 | 冲泡器不在位 |
| | | CTR | 32 | 蒸汽系统水路警告 |
| | | CTR | 47 | |
| | | CTR | 54 | |
| | | HMI | 14 | 冰箱未安装 |
| | 泵1 | CTR | 63 |  冰箱#1泵警告   |
| | 泵2 | CTR | 64 |  冰箱#2泵警告   |
| 27（糖浆风味） | 泵1、泵2、泵3、泵4共用 | CTR | 65 | 糖浆机不在位 |
| | | CTR | 30 | 冲泡器不在位 |
| | | CTR | 32 | 蒸汽系统水路警告 |
| | | CTR | 47 | |
| | | CTR | 54 | |
| | 泵1 | CTR | 66 | 糖浆机#1泵警告   |
| | 泵2 | CTR | 67 | 糖浆机#2泵警告     |
| | 泵3 | CTR | 68 | 糖浆机#3泵警告   |
| | 泵4 | CTR | 69 | 糖浆机#4泵警告   |
| 31（蒸汽锅炉补水） | 无 | CTR | 31 | 咖啡系统水路警告 |
| | | CTR | 46 | |
| | | CTR | 53 | |

