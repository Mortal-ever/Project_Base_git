add_library(nanomodbus STATIC
    Src/nanomodbus.c
)

target_include_directories(nanomodbus PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/Inc
    ${CMAKE_CURRENT_SOURCE_DIR}/Config
)

target_link_libraries(nanomodbus PUBLIC stm32cubemx)

