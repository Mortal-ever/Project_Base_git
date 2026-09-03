cmake_minimum_required(VERSION 3.22)
# Enable CMake support for ASM and C languages
enable_language(C ASM)
# STM32CubeMX generated symbols (macros)
set(MX_Defines_Syms 
	USE_HAL_DRIVER 
	STM32F407xx
    $<$<CONFIG:Debug>:DEBUG>
)

# STM32CubeMX generated include paths
set(MX_Include_Dirs
    ${CUBEMX_BASE_ROOT}/Core/Inc
    ${CUBEMX_BASE_ROOT}/LWIP/App
    ${CUBEMX_BASE_ROOT}/LWIP/Target
    ${CUBEMX_BASE_ROOT}/Middlewares/Third_Party/LwIP/src/include
    ${CUBEMX_BASE_ROOT}/Middlewares/Third_Party/LwIP/system
    ${CUBEMX_BASE_ROOT}/Drivers/STM32F4xx_HAL_Driver/Inc
    ${CUBEMX_BASE_ROOT}/Drivers/STM32F4xx_HAL_Driver/Inc/Legacy
    ${CUBEMX_BASE_ROOT}/Middlewares/Third_Party/FreeRTOS/Source/include
    ${CUBEMX_BASE_ROOT}/Middlewares/Third_Party/FreeRTOS/Source/CMSIS_RTOS_V2
    ${CUBEMX_BASE_ROOT}/Middlewares/Third_Party/FreeRTOS/Source/portable/GCC/ARM_CM4F
    ${CUBEMX_BASE_ROOT}/Drivers/BSP/Components/dp83848
    ${CUBEMX_BASE_ROOT}/Middlewares/Third_Party/LwIP/src/include/netif/ppp
    ${CUBEMX_BASE_ROOT}/Drivers/CMSIS/Device/ST/STM32F4xx/Include
    ${CUBEMX_BASE_ROOT}/Middlewares/Third_Party/LwIP/src/include/lwip
    ${CUBEMX_BASE_ROOT}/Middlewares/Third_Party/LwIP/src/include/lwip/apps
    ${CUBEMX_BASE_ROOT}/Middlewares/Third_Party/LwIP/src/include/lwip/priv
    ${CUBEMX_BASE_ROOT}/Middlewares/Third_Party/LwIP/src/include/lwip/prot
    ${CUBEMX_BASE_ROOT}/Middlewares/Third_Party/LwIP/src/include/netif
    ${CUBEMX_BASE_ROOT}/Middlewares/Third_Party/LwIP/src/include/compat/posix
    ${CUBEMX_BASE_ROOT}/Middlewares/Third_Party/LwIP/src/include/compat/posix/arpa
    ${CUBEMX_BASE_ROOT}/Middlewares/Third_Party/LwIP/src/include/compat/posix/net
    ${CUBEMX_BASE_ROOT}/Middlewares/Third_Party/LwIP/src/include/compat/posix/sys
    ${CUBEMX_BASE_ROOT}/Middlewares/Third_Party/LwIP/src/include/compat/stdc
    ${CUBEMX_BASE_ROOT}/Middlewares/Third_Party/LwIP/system/arch
    ${CUBEMX_BASE_ROOT}/Drivers/CMSIS/Include
)

# STM32CubeMX generated application sources
set(MX_Application_Src
    ${CUBEMX_BASE_ROOT}/LWIP/App/lwip.c
    ${CUBEMX_BASE_ROOT}/LWIP/Target/ethernetif.c
    ${CUBEMX_BASE_ROOT}/Core/Src/main.c
    ${CUBEMX_BASE_ROOT}/Core/Src/gpio.c
    ${CUBEMX_BASE_ROOT}/Core/Src/freertos.c
    ${CUBEMX_BASE_ROOT}/Core/Src/crc.c
    ${CUBEMX_BASE_ROOT}/Core/Src/dma.c
    ${CUBEMX_BASE_ROOT}/Core/Src/rtc.c
    ${CUBEMX_BASE_ROOT}/Core/Src/spi.c
    ${CUBEMX_BASE_ROOT}/Core/Src/usart.c
    ${CUBEMX_BASE_ROOT}/Core/Src/stm32f4xx_it.c
    ${CUBEMX_BASE_ROOT}/Core/Src/stm32f4xx_hal_msp.c
    ${CUBEMX_BASE_ROOT}/Core/Src/stm32f4xx_hal_timebase_tim.c
    ${CUBEMX_BASE_ROOT}/Core/Src/sysmem.c
    ${CUBEMX_BASE_ROOT}/Core/Src/syscalls.c
    ${CMAKE_SOURCE_DIR}/startup_stm32f407xx.s
)

# STM32 HAL/LL Drivers
set(STM32_Drivers_Src
    ${CUBEMX_BASE_ROOT}/Core/Src/system_stm32f4xx.c
    ${CUBEMX_BASE_ROOT}/Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_tim.c
    ${CUBEMX_BASE_ROOT}/Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_tim_ex.c
    ${CUBEMX_BASE_ROOT}/Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_rcc.c
    ${CUBEMX_BASE_ROOT}/Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_rcc_ex.c
    ${CUBEMX_BASE_ROOT}/Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_flash.c
    ${CUBEMX_BASE_ROOT}/Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_flash_ex.c
    ${CUBEMX_BASE_ROOT}/Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_flash_ramfunc.c
    ${CUBEMX_BASE_ROOT}/Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_gpio.c
    ${CUBEMX_BASE_ROOT}/Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_dma_ex.c
    ${CUBEMX_BASE_ROOT}/Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_dma.c
    ${CUBEMX_BASE_ROOT}/Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_pwr.c
    ${CUBEMX_BASE_ROOT}/Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_pwr_ex.c
    ${CUBEMX_BASE_ROOT}/Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_cortex.c
    ${CUBEMX_BASE_ROOT}/Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal.c
    ${CUBEMX_BASE_ROOT}/Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_exti.c
    ${CUBEMX_BASE_ROOT}/Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_eth.c
    ${CUBEMX_BASE_ROOT}/Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_crc.c
    ${CUBEMX_BASE_ROOT}/Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_rtc.c
    ${CUBEMX_BASE_ROOT}/Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_rtc_ex.c
    ${CUBEMX_BASE_ROOT}/Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_spi.c
    ${CUBEMX_BASE_ROOT}/Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_uart.c
    ${CUBEMX_BASE_ROOT}/Drivers/BSP/Components/dp83848/dp83848.c
)

# Drivers Midllewares


set(FreeRTOS_Src
    ${CUBEMX_BASE_ROOT}/Middlewares/Third_Party/FreeRTOS/Source/croutine.c
    ${CUBEMX_BASE_ROOT}/Middlewares/Third_Party/FreeRTOS/Source/event_groups.c
    ${CUBEMX_BASE_ROOT}/Middlewares/Third_Party/FreeRTOS/Source/list.c
    ${CUBEMX_BASE_ROOT}/Middlewares/Third_Party/FreeRTOS/Source/queue.c
    ${CUBEMX_BASE_ROOT}/Middlewares/Third_Party/FreeRTOS/Source/stream_buffer.c
    ${CUBEMX_BASE_ROOT}/Middlewares/Third_Party/FreeRTOS/Source/tasks.c
    ${CUBEMX_BASE_ROOT}/Middlewares/Third_Party/FreeRTOS/Source/timers.c
    ${CUBEMX_BASE_ROOT}/Middlewares/Third_Party/FreeRTOS/Source/CMSIS_RTOS_V2/cmsis_os2.c
    ${CUBEMX_BASE_ROOT}/Middlewares/Third_Party/FreeRTOS/Source/portable/MemMang/heap_4.c
    ${CUBEMX_BASE_ROOT}/Middlewares/Third_Party/FreeRTOS/Source/portable/GCC/ARM_CM4F/port.c
)
set(LwIP_Src
    ${CUBEMX_BASE_ROOT}/Middlewares/Third_Party/LwIP/src/netif/ppp/auth.c
    ${CUBEMX_BASE_ROOT}/Middlewares/Third_Party/LwIP/src/netif/ppp/ccp.c
    ${CUBEMX_BASE_ROOT}/Middlewares/Third_Party/LwIP/src/netif/ppp/chap_ms.c
    ${CUBEMX_BASE_ROOT}/Middlewares/Third_Party/LwIP/src/netif/ppp/chap-md5.c
    ${CUBEMX_BASE_ROOT}/Middlewares/Third_Party/LwIP/src/netif/ppp/chap-new.c
    ${CUBEMX_BASE_ROOT}/Middlewares/Third_Party/LwIP/src/netif/ppp/demand.c
    ${CUBEMX_BASE_ROOT}/Middlewares/Third_Party/LwIP/src/netif/ppp/eap.c
    ${CUBEMX_BASE_ROOT}/Middlewares/Third_Party/LwIP/src/netif/ppp/eui64.c
    ${CUBEMX_BASE_ROOT}/Middlewares/Third_Party/LwIP/src/netif/ppp/fsm.c
    ${CUBEMX_BASE_ROOT}/Middlewares/Third_Party/LwIP/src/netif/ppp/ipcp.c
    ${CUBEMX_BASE_ROOT}/Middlewares/Third_Party/LwIP/src/netif/ppp/ipv6cp.c
    ${CUBEMX_BASE_ROOT}/Middlewares/Third_Party/LwIP/src/netif/ppp/lcp.c
    ${CUBEMX_BASE_ROOT}/Middlewares/Third_Party/LwIP/src/netif/ppp/magic.c
    ${CUBEMX_BASE_ROOT}/Middlewares/Third_Party/LwIP/src/netif/ppp/mppe.c
    ${CUBEMX_BASE_ROOT}/Middlewares/Third_Party/LwIP/src/netif/ppp/multilink.c
    ${CUBEMX_BASE_ROOT}/Middlewares/Third_Party/LwIP/src/netif/ppp/ppp.c
    ${CUBEMX_BASE_ROOT}/Middlewares/Third_Party/LwIP/src/netif/ppp/pppapi.c
    ${CUBEMX_BASE_ROOT}/Middlewares/Third_Party/LwIP/src/netif/ppp/pppcrypt.c
    ${CUBEMX_BASE_ROOT}/Middlewares/Third_Party/LwIP/src/netif/ppp/pppoe.c
    ${CUBEMX_BASE_ROOT}/Middlewares/Third_Party/LwIP/src/netif/ppp/pppol2tp.c
    ${CUBEMX_BASE_ROOT}/Middlewares/Third_Party/LwIP/src/netif/ppp/pppos.c
    ${CUBEMX_BASE_ROOT}/Middlewares/Third_Party/LwIP/src/netif/ppp/upap.c
    ${CUBEMX_BASE_ROOT}/Middlewares/Third_Party/LwIP/src/netif/ppp/utils.c
    ${CUBEMX_BASE_ROOT}/Middlewares/Third_Party/LwIP/src/netif/ppp/vj.c
    ${CUBEMX_BASE_ROOT}/Middlewares/Third_Party/LwIP/src/netif/bridgeif.c
    ${CUBEMX_BASE_ROOT}/Middlewares/Third_Party/LwIP/src/netif/bridgeif_fdb.c
    ${CUBEMX_BASE_ROOT}/Middlewares/Third_Party/LwIP/src/netif/ethernet.c
    ${CUBEMX_BASE_ROOT}/Middlewares/Third_Party/LwIP/src/netif/lowpan6.c
    ${CUBEMX_BASE_ROOT}/Middlewares/Third_Party/LwIP/src/netif/lowpan6_ble.c
    ${CUBEMX_BASE_ROOT}/Middlewares/Third_Party/LwIP/src/netif/lowpan6_common.c
    ${CUBEMX_BASE_ROOT}/Middlewares/Third_Party/LwIP/src/netif/slipif.c
    ${CUBEMX_BASE_ROOT}/Middlewares/Third_Party/LwIP/src/netif/zepif.c
    ${CUBEMX_BASE_ROOT}/Middlewares/Third_Party/LwIP/src/netif/ppp/ecp.c
    ${CUBEMX_BASE_ROOT}/Middlewares/Third_Party/LwIP/src/api/api_lib.c
    ${CUBEMX_BASE_ROOT}/Middlewares/Third_Party/LwIP/src/api/api_msg.c
    ${CUBEMX_BASE_ROOT}/Middlewares/Third_Party/LwIP/src/api/err.c
    ${CUBEMX_BASE_ROOT}/Middlewares/Third_Party/LwIP/src/api/if_api.c
    ${CUBEMX_BASE_ROOT}/Middlewares/Third_Party/LwIP/src/api/netbuf.c
    ${CUBEMX_BASE_ROOT}/Middlewares/Third_Party/LwIP/src/api/netdb.c
    ${CUBEMX_BASE_ROOT}/Middlewares/Third_Party/LwIP/src/api/netifapi.c
    ${CUBEMX_BASE_ROOT}/Middlewares/Third_Party/LwIP/src/api/sockets.c
    ${CUBEMX_BASE_ROOT}/Middlewares/Third_Party/LwIP/src/api/tcpip.c
    ${CUBEMX_BASE_ROOT}/Middlewares/Third_Party/LwIP/src/core/altcp.c
    ${CUBEMX_BASE_ROOT}/Middlewares/Third_Party/LwIP/src/core/altcp_alloc.c
    ${CUBEMX_BASE_ROOT}/Middlewares/Third_Party/LwIP/src/core/altcp_tcp.c
    ${CUBEMX_BASE_ROOT}/Middlewares/Third_Party/LwIP/src/core/def.c
    ${CUBEMX_BASE_ROOT}/Middlewares/Third_Party/LwIP/src/core/dns.c
    ${CUBEMX_BASE_ROOT}/Middlewares/Third_Party/LwIP/src/core/inet_chksum.c
    ${CUBEMX_BASE_ROOT}/Middlewares/Third_Party/LwIP/src/core/init.c
    ${CUBEMX_BASE_ROOT}/Middlewares/Third_Party/LwIP/src/core/ip.c
    ${CUBEMX_BASE_ROOT}/Middlewares/Third_Party/LwIP/src/core/mem.c
    ${CUBEMX_BASE_ROOT}/Middlewares/Third_Party/LwIP/src/core/memp.c
    ${CUBEMX_BASE_ROOT}/Middlewares/Third_Party/LwIP/src/core/netif.c
    ${CUBEMX_BASE_ROOT}/Middlewares/Third_Party/LwIP/src/core/pbuf.c
    ${CUBEMX_BASE_ROOT}/Middlewares/Third_Party/LwIP/src/core/raw.c
    ${CUBEMX_BASE_ROOT}/Middlewares/Third_Party/LwIP/src/core/stats.c
    ${CUBEMX_BASE_ROOT}/Middlewares/Third_Party/LwIP/src/core/sys.c
    ${CUBEMX_BASE_ROOT}/Middlewares/Third_Party/LwIP/src/core/tcp.c
    ${CUBEMX_BASE_ROOT}/Middlewares/Third_Party/LwIP/src/core/tcp_in.c
    ${CUBEMX_BASE_ROOT}/Middlewares/Third_Party/LwIP/src/core/tcp_out.c
    ${CUBEMX_BASE_ROOT}/Middlewares/Third_Party/LwIP/src/core/timeouts.c
    ${CUBEMX_BASE_ROOT}/Middlewares/Third_Party/LwIP/src/core/udp.c
    ${CUBEMX_BASE_ROOT}/Middlewares/Third_Party/LwIP/src/core/ipv4/autoip.c
    ${CUBEMX_BASE_ROOT}/Middlewares/Third_Party/LwIP/src/core/ipv4/dhcp.c
    ${CUBEMX_BASE_ROOT}/Middlewares/Third_Party/LwIP/src/core/ipv4/etharp.c
    ${CUBEMX_BASE_ROOT}/Middlewares/Third_Party/LwIP/src/core/ipv4/icmp.c
    ${CUBEMX_BASE_ROOT}/Middlewares/Third_Party/LwIP/src/core/ipv4/igmp.c
    ${CUBEMX_BASE_ROOT}/Middlewares/Third_Party/LwIP/src/core/ipv4/ip4.c
    ${CUBEMX_BASE_ROOT}/Middlewares/Third_Party/LwIP/src/core/ipv4/ip4_addr.c
    ${CUBEMX_BASE_ROOT}/Middlewares/Third_Party/LwIP/src/core/ipv4/ip4_frag.c
    ${CUBEMX_BASE_ROOT}/Middlewares/Third_Party/LwIP/src/core/ipv6/dhcp6.c
    ${CUBEMX_BASE_ROOT}/Middlewares/Third_Party/LwIP/src/core/ipv6/ethip6.c
    ${CUBEMX_BASE_ROOT}/Middlewares/Third_Party/LwIP/src/core/ipv6/icmp6.c
    ${CUBEMX_BASE_ROOT}/Middlewares/Third_Party/LwIP/src/core/ipv6/inet6.c
    ${CUBEMX_BASE_ROOT}/Middlewares/Third_Party/LwIP/src/core/ipv6/ip6.c
    ${CUBEMX_BASE_ROOT}/Middlewares/Third_Party/LwIP/src/core/ipv6/ip6_addr.c
    ${CUBEMX_BASE_ROOT}/Middlewares/Third_Party/LwIP/src/core/ipv6/ip6_frag.c
    ${CUBEMX_BASE_ROOT}/Middlewares/Third_Party/LwIP/src/core/ipv6/mld6.c
    ${CUBEMX_BASE_ROOT}/Middlewares/Third_Party/LwIP/src/core/ipv6/nd6.c
    ${CUBEMX_BASE_ROOT}/Middlewares/Third_Party/LwIP/system/OS/sys_arch.c
    ${CUBEMX_BASE_ROOT}/Middlewares/Third_Party/LwIP/src/apps/mqtt/mqtt.c
)

# Link directories setup
set(MX_LINK_DIRS

)
# Project static libraries
set(MX_LINK_LIBS 
    STM32_Drivers
    ${TOOLCHAIN_LINK_LIBRARIES}
    FreeRTOS	LwIP	
)
# Interface library for includes and symbols
add_library(stm32cubemx INTERFACE)
target_include_directories(stm32cubemx INTERFACE ${MX_Include_Dirs})
target_compile_definitions(stm32cubemx INTERFACE ${MX_Defines_Syms})

# Create STM32_Drivers static library
add_library(STM32_Drivers OBJECT)
target_sources(STM32_Drivers PRIVATE ${STM32_Drivers_Src})
target_link_libraries(STM32_Drivers PUBLIC stm32cubemx)


# Create FreeRTOS static library
add_library(FreeRTOS OBJECT)
target_sources(FreeRTOS PRIVATE ${FreeRTOS_Src})
target_link_libraries(FreeRTOS PUBLIC stm32cubemx)

# Create LwIP static library
add_library(LwIP OBJECT)
target_sources(LwIP PRIVATE ${LwIP_Src})
target_link_libraries(LwIP PUBLIC stm32cubemx)

# Add STM32CubeMX generated application sources to the project
target_sources(${CMAKE_PROJECT_NAME} PRIVATE ${MX_Application_Src})

# Link directories setup
target_link_directories(${CMAKE_PROJECT_NAME} PRIVATE ${MX_LINK_DIRS})

# Add libraries to the project
target_link_libraries(${CMAKE_PROJECT_NAME} ${MX_LINK_LIBS})

# Add the map file to the list of files to be removed with 'clean' target
set_target_properties(${CMAKE_PROJECT_NAME} PROPERTIES ADDITIONAL_CLEAN_FILES ${CMAKE_PROJECT_NAME}.map)

# Validate that STM32CubeMX code is compatible with C standard
if((CMAKE_C_STANDARD EQUAL 90) OR (CMAKE_C_STANDARD EQUAL 99))
    message(ERROR "Generated code requires C11 or higher")
endif()
