# 02 Keil与GCC构建图逐项对照

日期：2026-09-08；以下为当前配置静态审查，不是编译成功记录。

源码入口：[Application/CMakeLists.txt](../../../../Application/CMakeLists.txt)、[GCC入口](../../../../GCC-ARM/CMakeLists.txt)、[presets](../../../../GCC-ARM/CMakePresets.json)、[Keil工程](../../../../MDK-ARM/STM32F407_Base.uvprojx)。逐文件状态见[覆盖矩阵](源码带读覆盖矩阵.md)。

## CMake从外到内读

1. PRODUCT_NAME默认Coffee2Open，只接受Coffee2Open/Coffee3Close。分支设FIRMWARE_TARGET和USE_COFFEE2/3。
2. 声明C/ASM可执行目标，加入GCC平台app_ccm.c，再加入生成层cmake/stm32cubemx。
3. nanoMODBUS单独add_subdirectory；随后Application创建公共OBJECT和产品OBJECT。
4. product_interfaces是INTERFACE目标：传播include和依赖，不是运行时接口函数。PUBLIC/PRIVATE描述构建使用要求，不能自动证明代码业务边界。
5. app_transport是3个.c，app_modbus_port依赖transport；device_f200依赖transport；其他Modbus设备目标依赖app_modbus_port。
6. Coffee2分支建立coffee2_app，包含12个私有.c；Coffee3建立coffee3_app，同样12个。所选target的Config和Task_Manager等路径以PRIVATE方式加入。
7. PRODUCT_LINK_TARGETS通过PARENT_SCOPE返回上层，主固件target_link_libraries消费。

## 当前实际选择矩阵

| 模块 | GCC Coffee2 | Keil Coffee2 | GCC Coffee3 | Keil Coffee3 |
| --- | --- | --- | --- | --- |
| 公共Transport/ModbusPort/日志/OTA/诊断/会话 | 纳入 | 纳入 | 纳入 | 纳入 |
| Dobot/CupLid/Syrup/Ice/Scale/Power/IO | 纳入 | 纳入 | 纳入 | 纳入 |
| F200 | 纳入且业务调用 | 纳入 | 未链接 | 排除 |
| M50 | 聚合列表纳入，Coffee2业务不调用 | 排除 | 纳入且业务调用 | 纳入 |
| O/X | 聚合列表纳入，Coffee2业务不调用 | 排除 | 未链接 | 排除 |
| Coffee2Open私有.c | 纳入 | 纳入 | 未选 | 组排除 |
| Coffee3Close私有.c | 未选 | 组排除 | 纳入 | 纳入 |
| MilkTea | 无产品分支 | 无Target | 无产品分支 | 无Target |

Coffee2使用完整APP_PUBLIC_TARGETS，包含F200/O/X/M50；Coffee3显式列出所需公共目标。这说明两个工具链的源码编译选择不完全相同。不要写“公共咖啡机设备均在Keil/GCC统一排除”。

是否增加最终Flash/RAM还取决于节回收和引用：OBJECT加入图与函数实际保留两回事。应看本次map和段大小；本轮没有编译，所以不提供推测的固件占用数字。

## Keil XML怎么读

TargetName是产品；pArmCC/pCCUsed指定V5.06 update7 build960，uAC6=0。Group是虚拟目录，FilePath才是磁盘路径。IncludeInBuild可在GroupOption或FileOption设置，组禁用不能被简单“文件无禁用项”判断为启用。

逐文件矩阵按当前XML同时考虑组与文件禁用项。头文件出现在Group只为IDE管理，不产生独立目标文件。历史TargetStatus Error=0不是现在重新构建的证据。

## 宏、链接和启动

GCC入口当前C_STANDARD=11、C_EXTENSIONS=ON；维护代码要兼容ARMCC5不意味着GCC当前配置为C99。教材描述实际配置，不能按规范愿望写错。

GCC对主目标及编译system_stm32f4xx.c的STM32_Drivers都设置USER_VECT_TAB_ADDRESS和VECT_TAB_OFFSET=0xC000，防止只在主文件定义而驱动目标未看到宏。当前两个产品都用STM32F407XX_COFFEE2_OTA_FLASH.ld；文件名带Coffee2不意味着Coffee3没用OTA布局。

app_ccm负责GCC段初始化，Keil走scatter/运行库；CCM heap/data必须分别解释。最终固件地址要联合链接脚本和SystemInit向量地址检查，不能只看HEX下载成功。

## 阅读练习

找M50在add_library、APP_PUBLIC_TARGETS、Coffee3 target_link_libraries三处；再对照Keil两个Target下的IncludeInBuild。说明为何Coffee2能编译M50对象而Coffee2RTU仍只走F200。

新增Target需自己的私有选源、宏、manager和配置，并选择公共OBJECT；不必创建新的公共协议层。本教材不修改工程配置。
