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
    COMMAND ${CMAKE_OBJCOPY} -I binary -O elf32-littlearm -B arm handler.bin handler.o
    DEPENDS handler.bin
)

# Convert handler_setzi.bin to object file
add_custom_command(
    OUTPUT handler_setzi.o
    COMMAND ${CMAKE_OBJCOPY} -I binary -O elf32-littlearm -B arm handler_setzi.bin handler_setzi.o
    DEPENDS handler_setzi.bin
)

# Dummy target to manage handler.o
add_custom_target(generate_handler_obj DEPENDS handler.o handler_setzi.o)

if (TARGET tinyusb_device)

    add_executable(k_line_kwp
        kline.c
        kwp2000.c
        main.c
        ring_buffer.c
        dashboard_state_machine.c
        ecu_state_machine.c
        config.h
    )

    pico_generate_pio_header(k_line_kwp ${CMAKE_CURRENT_LIST_DIR}/uart_rx.pio)

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
