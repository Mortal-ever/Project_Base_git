set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR arm)

get_filename_component(GCC_ARM_ROOT
    "${CMAKE_CURRENT_LIST_DIR}/../.tools/arm-gnu-toolchain" ABSOLUTE)
set(GCC_ARM_BIN "${GCC_ARM_ROOT}/bin")

set(CMAKE_C_COMPILER "${GCC_ARM_BIN}/arm-none-eabi-gcc.exe")
set(CMAKE_ASM_COMPILER "${GCC_ARM_BIN}/arm-none-eabi-gcc.exe")
set(CMAKE_CXX_COMPILER "${GCC_ARM_BIN}/arm-none-eabi-g++.exe")
set(CMAKE_OBJCOPY "${GCC_ARM_BIN}/arm-none-eabi-objcopy.exe")
set(CMAKE_SIZE "${GCC_ARM_BIN}/arm-none-eabi-size.exe")

set(CMAKE_EXECUTABLE_SUFFIX_ASM ".elf")
set(CMAKE_EXECUTABLE_SUFFIX_C ".elf")
set(CMAKE_EXECUTABLE_SUFFIX_CXX ".elf")
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

set(MCU_FLAGS
    "-mcpu=cortex-m4 -mthumb -mfpu=fpv4-sp-d16 -mfloat-abi=hard")
set(COMMON_SECTION_FLAGS "-ffunction-sections -fdata-sections")

set(CMAKE_C_FLAGS_INIT
    "${MCU_FLAGS} ${COMMON_SECTION_FLAGS} -Wall -Wextra")
set(CMAKE_ASM_FLAGS_INIT
    "${MCU_FLAGS} ${COMMON_SECTION_FLAGS} -x assembler-with-cpp")
set(CMAKE_CXX_FLAGS_INIT
    "${MCU_FLAGS} ${COMMON_SECTION_FLAGS} -Wall -Wextra -fno-rtti -fno-exceptions -fno-threadsafe-statics")

if(PRODUCT_NAME STREQUAL "Coffee2")
    set(CMAKE_C_FLAGS_DEBUG_INIT "-Og -g3")
    set(CMAKE_CXX_FLAGS_DEBUG_INIT "-Og -g3")
else()
    set(CMAKE_C_FLAGS_DEBUG_INIT "-O0 -g3")
    set(CMAKE_CXX_FLAGS_DEBUG_INIT "-O0 -g3")
endif()
set(CMAKE_C_FLAGS_RELEASE_INIT "-Os -g0")
set(CMAKE_CXX_FLAGS_RELEASE_INIT "-Os -g0")

set(CMAKE_EXE_LINKER_FLAGS_INIT
    "${MCU_FLAGS} --specs=nano.specs --specs=nosys.specs -Wl,--gc-sections -Wl,--print-memory-usage")

set(TOOLCHAIN_LINK_LIBRARIES m c gcc nosys)
