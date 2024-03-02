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

if (TARGET tinyusb_device)

    add_executable(k_line_kwp kline.c kwp2000.c main.c)

    pico_generate_pio_header(k_line_kwp ${CMAKE_CURRENT_LIST_DIR}/uart_rx.pio)

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
        -Wno-format          # int != int32_t as far as the compiler is concerned because gcc has int32_t as long int
        -Wno-unused-function # we have some for the docs that aren't called
        )
if (CMAKE_C_COMPILER_ID STREQUAL "GNU")
    add_compile_options(-Wno-maybe-uninitialized)
endif()