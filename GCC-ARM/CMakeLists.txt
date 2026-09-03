cmake_minimum_required(VERSION 3.22)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Debug")
endif()

set(PRODUCT_NAME "Coffee2" CACHE STRING "Product target")
set_property(CACHE PRODUCT_NAME PROPERTY STRINGS Coffee2)

if(PRODUCT_NAME STREQUAL "Coffee2")
    set(FIRMWARE_TARGET Coffee2Target)
    set(PRODUCT_DEFINE USE_COFFEE2=1)
else()
    message(FATAL_ERROR "Unsupported PRODUCT_NAME=${PRODUCT_NAME}")
endif()

get_filename_component(PROJECT_BASE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
set(PROJECT_BASE_ROOT "${PROJECT_BASE_ROOT}" CACHE INTERNAL
    "Shared Project_Base source root")
set(CUBEMX_BASE_ROOT "${PROJECT_BASE_ROOT}/CubeMX_Base" CACHE INTERNAL
    "Checked-in STM32CubeMX generated source root consumed by GCC")
set(CUBEMX_MAINT_ROOT "${PROJECT_BASE_ROOT}/CubeMX_Base/CubeMX_Genarate" CACHE INTERNAL
    "CubeMX maintenance input root; do not edit generated output here")

project(${FIRMWARE_TARGET} C ASM)
message(STATUS "Product target: ${PRODUCT_NAME}")
message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")
message(STATUS "Shared source root: ${PROJECT_BASE_ROOT}")
message(STATUS "CubeMX source root: ${CUBEMX_BASE_ROOT}")
message(STATUS "CubeMX maintenance root: ${CUBEMX_MAINT_ROOT}")

add_executable(${FIRMWARE_TARGET})

target_sources(${FIRMWARE_TARGET} PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/Platform/app_ccm.c"
)

add_subdirectory(cmake/stm32cubemx)
target_compile_definitions(LwIP PRIVATE ${PRODUCT_DEFINE})
target_compile_options(STM32_Drivers PRIVATE -Wno-unused-parameter)
set_source_files_properties(
    "${CUBEMX_BASE_ROOT}/LWIP/Target/ethernetif.c"
    TARGET_DIRECTORY ${FIRMWARE_TARGET}
    PROPERTIES COMPILE_OPTIONS "-Wno-unused-parameter"
)
target_include_directories(stm32cubemx INTERFACE
    "${PROJECT_BASE_ROOT}/Application/Common"
    "${PROJECT_BASE_ROOT}/Application/Common/Diagnostics/Inc"
    "${PROJECT_BASE_ROOT}/Application/Common/Diagnostics/Config"
    "${CMAKE_CURRENT_SOURCE_DIR}/Platform"
)
# freertos.c is the sole startup composition root and is the only generated
# source allowed to include the selected target manager through CommonTargets.h.
# Keep this include PRIVATE so it never enters public target usage requirements.
# The generated sources are compiled as part of the product executable rather
# than an object target, so keep this composition-root include private to it.
target_include_directories(${FIRMWARE_TARGET} PRIVATE
    "${PROJECT_BASE_ROOT}/Application/UserAPP/Coffee2App/Task_Manager"
)
add_subdirectory(
    "${PROJECT_BASE_ROOT}/Application/New_Party/nanoMODBUS"
    "${CMAKE_CURRENT_BINARY_DIR}/nanomodbus"
)
add_subdirectory(
    "${PROJECT_BASE_ROOT}/Application"
    "${CMAKE_CURRENT_BINARY_DIR}/application"
)

target_compile_definitions(${FIRMWARE_TARGET} PRIVATE ${PRODUCT_DEFINE})
if(PRODUCT_NAME STREQUAL "Coffee2")
    target_compile_definitions(${FIRMWARE_TARGET} PRIVATE
        USER_VECT_TAB_ADDRESS
        VECT_TAB_OFFSET=0x0000C000U
    )

    # system_stm32f4xx.c is compiled by STM32_Drivers, so the relocation
    # definitions must also be applied to that object target.
    target_compile_definitions(STM32_Drivers PRIVATE
        USER_VECT_TAB_ADDRESS
        VECT_TAB_OFFSET=0x0000C000U
    )
endif()
target_link_libraries(${FIRMWARE_TARGET}
    stm32cubemx
    nanomodbus
    ${PRODUCT_LINK_TARGETS}
)

list(REMOVE_ITEM CMAKE_C_IMPLICIT_LINK_LIBRARIES ob)

set_target_properties(${FIRMWARE_TARGET} PROPERTIES
    OUTPUT_NAME "${FIRMWARE_TARGET}"
    ADDITIONAL_CLEAN_FILES "${FIRMWARE_TARGET}.map"
)

target_link_options(${FIRMWARE_TARGET} PRIVATE
    "-Wl,-Map=${CMAKE_CURRENT_BINARY_DIR}/${FIRMWARE_TARGET}.map"
)

if(PRODUCT_NAME STREQUAL "Coffee2")
    get_filename_component(FIRMWARE_LINKER_SCRIPT
        "${CMAKE_CURRENT_SOURCE_DIR}/linker/STM32F407XX_COFFEE2_OTA_FLASH.ld" ABSOLUTE)
else()
    get_filename_component(FIRMWARE_LINKER_SCRIPT
        "${CMAKE_CURRENT_SOURCE_DIR}/linker/STM32F407XX_FLASH.ld" ABSOLUTE)
endif()
target_link_options(${FIRMWARE_TARGET} PRIVATE
    "-T${FIRMWARE_LINKER_SCRIPT}"
)

add_custom_command(TARGET ${FIRMWARE_TARGET} POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -O ihex
        $<TARGET_FILE:${FIRMWARE_TARGET}>
        "${CMAKE_CURRENT_BINARY_DIR}/${FIRMWARE_TARGET}.hex"
    COMMAND ${CMAKE_OBJCOPY} -O binary
        $<TARGET_FILE:${FIRMWARE_TARGET}>
        "${CMAKE_CURRENT_BINARY_DIR}/${FIRMWARE_TARGET}.bin"
    COMMAND ${CMAKE_SIZE} $<TARGET_FILE:${FIRMWARE_TARGET}>
    COMMENT "Generating ${FIRMWARE_TARGET} HEX/BIN and memory summary"
)
