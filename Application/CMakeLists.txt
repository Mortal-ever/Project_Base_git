set(APP_PUBLIC_INCLUDE_DIRS
    ${CMAKE_CURRENT_SOURCE_DIR}/Common
    ${CMAKE_CURRENT_SOURCE_DIR}/Common/Diagnostics/Inc
    ${CMAKE_CURRENT_SOURCE_DIR}/Common/Diagnostics/Config
    ${CMAKE_CURRENT_SOURCE_DIR}/Transport/Inc
    ${CMAKE_CURRENT_SOURCE_DIR}/ProtocolStack/ModbusPort/Inc
    ${CMAKE_CURRENT_SOURCE_DIR}/ProtocolStack/ModbusPort/Config
    ${CMAKE_CURRENT_SOURCE_DIR}/New_Party/nanoMODBUS/Inc
    ${CMAKE_CURRENT_SOURCE_DIR}/New_Party/nanoMODBUS/Config
    ${CMAKE_CURRENT_SOURCE_DIR}/DeviceLibrary/Inc
    ${CMAKE_CURRENT_SOURCE_DIR}/DeviceLibrary/Robot/Dobot
    ${CMAKE_CURRENT_SOURCE_DIR}/DeviceLibrary/CoffeeMachine
    ${CMAKE_CURRENT_SOURCE_DIR}/DeviceLibrary/CupLidController/ShengShu
    ${CMAKE_CURRENT_SOURCE_DIR}/DeviceLibrary/SyrupMachine/CurrentModbus
    ${CMAKE_CURRENT_SOURCE_DIR}/DeviceLibrary/IceMachine/CurrentModbus
    ${CMAKE_CURRENT_SOURCE_DIR}/DeviceLibrary/Scale/BSQ_DG_V2
    ${CMAKE_CURRENT_SOURCE_DIR}/DeviceLibrary/PowerMeter/DDSU666
    ${CMAKE_CURRENT_SOURCE_DIR}/DeviceLibrary/IoModule/ModbusDigitalIo
)

add_library(product_interfaces INTERFACE)
target_include_directories(product_interfaces INTERFACE
    ${APP_PUBLIC_INCLUDE_DIRS}
)
target_link_libraries(product_interfaces INTERFACE
    stm32cubemx
    nanomodbus
)

add_library(app_diagnostics OBJECT
    Common/Diagnostics/Src/app_crash_diag.c
    Common/Diagnostics/Src/app_crash_fault_gcc.S
)
target_link_libraries(app_diagnostics PUBLIC product_interfaces)

add_library(app_transport OBJECT
    Transport/Src/transport.c
    Transport/Src/transport_uart.c
    Transport/Src/transport_tcp.c
)
target_link_libraries(app_transport PUBLIC product_interfaces)

add_library(app_modbus_port OBJECT
    ProtocolStack/ModbusPort/Src/modbus_port.c
)
target_link_libraries(app_modbus_port PUBLIC
    product_interfaces
    app_transport
)

# Public application services.  These targets are declared independently of
# PRODUCT_NAME so every future target may opt in to the same implementation.
add_library(app_log OBJECT
    Common/Log/app_log.c
)
target_link_libraries(app_log PUBLIC product_interfaces)

add_library(app_lwip_alert OBJECT
    Common/LwipAlert/app_lwip_alert.c
)
target_link_libraries(app_lwip_alert PUBLIC product_interfaces)

add_library(app_ota OBJECT
    Common/Ota/app_ota_flash.c
    Common/Ota/app_ota_http.c
)
target_link_libraries(app_ota PUBLIC product_interfaces)

add_library(app_tcp_client_session OBJECT
    Common/TcpClientSession/tcp_client_session.c
)
target_link_libraries(app_tcp_client_session PUBLIC product_interfaces)

add_library(device_dobot OBJECT
    DeviceLibrary/Robot/Dobot/dobot_robot_device.c
)
target_link_libraries(device_dobot PUBLIC product_interfaces app_modbus_port)

add_library(device_f200 OBJECT
    DeviceLibrary/CoffeeMachine/coffee_machine_f200.c
)
target_link_libraries(device_f200 PUBLIC product_interfaces app_transport)

add_library(device_coffee_o OBJECT
    DeviceLibrary/CoffeeMachine/coffee_machine_O.c
)
target_link_libraries(device_coffee_o PUBLIC product_interfaces app_modbus_port)

add_library(device_coffee_x OBJECT
    DeviceLibrary/CoffeeMachine/coffee_machine_X.c
)
target_link_libraries(device_coffee_x PUBLIC product_interfaces app_modbus_port)

add_library(device_coffee_m50 OBJECT
    DeviceLibrary/CoffeeMachine/coffee_machine_m50.c
)
target_link_libraries(device_coffee_m50 PUBLIC product_interfaces app_modbus_port)

add_library(device_cup_lid OBJECT
    DeviceLibrary/CupLidController/ShengShu/cup_lid_shengshu.c
)
target_link_libraries(device_cup_lid PUBLIC product_interfaces app_modbus_port)

add_library(device_syrup OBJECT
    DeviceLibrary/SyrupMachine/CurrentModbus/syrup_machine_modbus.c
)
target_link_libraries(device_syrup PUBLIC product_interfaces app_modbus_port)

add_library(device_ice OBJECT
    DeviceLibrary/IceMachine/CurrentModbus/ice_machine_modbus.c
)
target_link_libraries(device_ice PUBLIC product_interfaces app_modbus_port)

add_library(device_scale OBJECT
    DeviceLibrary/Scale/BSQ_DG_V2/scale_bsq_dg_v2.c
)
target_link_libraries(device_scale PUBLIC product_interfaces app_modbus_port)

add_library(device_power_meter OBJECT
    DeviceLibrary/PowerMeter/DDSU666/power_meter_ddsu666.c
)
target_link_libraries(device_power_meter PUBLIC product_interfaces app_modbus_port)

add_library(device_io OBJECT
    DeviceLibrary/IoModule/ModbusDigitalIo/io_module_modbus_digital.c
)
target_link_libraries(device_io PUBLIC product_interfaces app_modbus_port)

set(APP_PUBLIC_TARGETS
    app_diagnostics
    app_transport
    app_modbus_port
    app_log
    app_lwip_alert
    app_ota
    app_tcp_client_session
    device_dobot
    device_f200
    device_coffee_o
    device_coffee_x
    device_coffee_m50
    device_cup_lid
    device_syrup
    device_ice
    device_scale
    device_power_meter
    device_io
)

if(PRODUCT_NAME STREQUAL "Coffee2")
    set(COFFEE2_APP_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/UserAPP/Coffee2App)

    add_library(coffee2_app OBJECT
        ${COFFEE2_APP_ROOT}/Comm_Log/coffee2_log.c
        ${COFFEE2_APP_ROOT}/Comm_Log/coffee2_crash_log_port.c
        ${COFFEE2_APP_ROOT}/Comm_Log/coffee2_retarget.c
        ${COFFEE2_APP_ROOT}/Device/coffee2_device.c
        ${COFFEE2_APP_ROOT}/Device/coffee2_device_image.c
        ${COFFEE2_APP_ROOT}/IO_State/coffee2_io.c
        ${COFFEE2_APP_ROOT}/Modbus_Rtu_Bus/coffee2_rtu_bus.c
        ${COFFEE2_APP_ROOT}/Modbus_Tcp_Server/coffee2_server.c
        ${COFFEE2_APP_ROOT}/Ota/coffee2_ota.c
        ${COFFEE2_APP_ROOT}/Robot_Tcp/coffee2_robot_tcp.c
        ${COFFEE2_APP_ROOT}/Task_Manager/coffee2_manager.c
        ${COFFEE2_APP_ROOT}/WorkFlow/coffee2_workflow.c
    )
    target_compile_definitions(coffee2_app PRIVATE USE_COFFEE2=1)
    target_include_directories(coffee2_app PRIVATE
        ${COFFEE2_APP_ROOT}/Config
        ${COFFEE2_APP_ROOT}/Comm_Log
        ${COFFEE2_APP_ROOT}/Task_Manager
        ${COFFEE2_APP_ROOT}/Device
        ${COFFEE2_APP_ROOT}/IO_State
        ${COFFEE2_APP_ROOT}/Modbus_Rtu_Bus
        ${COFFEE2_APP_ROOT}/Modbus_Tcp_Server
        ${COFFEE2_APP_ROOT}/Ota
        ${COFFEE2_APP_ROOT}/Robot_Tcp
        ${COFFEE2_APP_ROOT}/WorkFlow
    )
    target_link_libraries(coffee2_app PUBLIC
        product_interfaces
        ${APP_PUBLIC_TARGETS}
    )

    set(PRODUCT_LINK_TARGETS
        ${APP_PUBLIC_TARGETS}
        coffee2_app
        PARENT_SCOPE
    )
else()
    message(FATAL_ERROR "Unsupported PRODUCT_NAME=${PRODUCT_NAME}")
endif()
