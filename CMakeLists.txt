# == DO NOT EDIT THE FOLLOWING LINES for the Raspberry Pi Pico VS Code Extension to work ==
if(WIN32)
    set(USERHOME $ENV{USERPROFILE})
else()
    set(USERHOME $ENV{HOME})
endif()
set(sdkVersion 2.1.1)
set(toolchainVersion 14_2_Rel1)
set(picotoolVersion 2.1.1)
set(picoVscode ${USERHOME}/.pico-sdk/cmake/pico-vscode.cmake)
if (EXISTS ${picoVscode})
    include(${picoVscode})
endif()
# ====================================================================================
cmake_minimum_required(VERSION 3.12)

# Pull in PICO SDK (must be before project)
include(pico_sdk_import.cmake)
include(hacky_cmake_helper.cmake)

project(k_line_kwp C CXX)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

# set(PICO_BOARD pico_w)
set(PICO_BOARD pico)

pico_sdk_init()

# Convert handler.bin to object file
add_custom_command(
    OUTPUT handler.o
    COMMAND ${CMAKE_COMMAND} -E chdir ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_OBJCOPY} -I binary -O elf32-littlearm -B arm
        handler.bin ${CMAKE_CURRENT_BINARY_DIR}/handler.o
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/handler.bin
)

# Convert handler_setzi.bin to object file
add_custom_command(
    OUTPUT handler_setzi.o
    COMMAND ${CMAKE_COMMAND} -E chdir ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_OBJCOPY} -I binary -O elf32-littlearm -B arm
        handler_setzi.bin ${CMAKE_CURRENT_BINARY_DIR}/handler_setzi.o
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/handler_setzi.bin
)

# Dummy target to manage handler.o
add_custom_target(generate_handler_obj DEPENDS handler.o handler_setzi.o)

if (TARGET tinyusb_device)

    add_executable(k_line_kwp
        src/main.c
        src/platform/uart_pio.c
        src/kline/kline.c
        src/kline/kwp2000.c
        src/ecu/ecu_state_machine.c
        src/host/command.c
        src/host/console.c
        src/ipc/ring_buffer.c
        src/ipc/log.c
    )

    target_include_directories(k_line_kwp PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/src/platform
        ${CMAKE_CURRENT_SOURCE_DIR}/src/kline
        ${CMAKE_CURRENT_SOURCE_DIR}/src/ecu
        ${CMAKE_CURRENT_SOURCE_DIR}/src/host
        ${CMAKE_CURRENT_SOURCE_DIR}/src/ipc
    )

    pico_generate_pio_header(k_line_kwp ${CMAKE_CURRENT_SOURCE_DIR}/src/platform/uart_rx.pio)

    # Add dependency on the object file generator
    add_dependencies(k_line_kwp generate_handler_obj)

    # Include the binary object file directly
    target_sources(k_line_kwp PRIVATE handler.o handler_setzi.o)

    target_link_libraries(k_line_kwp PRIVATE
        pico_stdlib
        pico_multicore
        hardware_pio
    )

    pico_enable_stdio_usb(k_line_kwp 1)
    pico_enable_stdio_uart(k_line_kwp 0)

    pico_add_extra_outputs(k_line_kwp)

elseif(PICO_ON_DEVICE)
    message(WARNING "not building hello_usb because TinyUSB submodule is not initialized in the SDK")
endif()

add_compile_options(-Wall
    -Wno-format
    -Wno-unused-function
)
if (CMAKE_C_COMPILER_ID STREQUAL "GNU")
    add_compile_options(-Wno-maybe-uninitialized)
endif()
