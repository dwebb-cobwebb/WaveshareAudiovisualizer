# == DO NOT EDIT THE FOLLOWING LINES for the Raspberry Pi Pico VS Code Extension to work ==
if(WIN32)
    set(USERHOME $ENV{USERPROFILE})
else()
    set(USERHOME $ENV{HOME})
endif()
set(sdkVersion 2.2.0)
set(toolchainVersion 14_2_Rel1)
set(picotoolVersion 2.2.0-a4)
set(picoVscode ${USERHOME}/.pico-sdk/cmake/pico-vscode.cmake)
if (EXISTS ${picoVscode})
    include(${picoVscode})
endif()
# ====================================================================================
cmake_minimum_required(VERSION 3.13)

# --- Target board / platform -------------------------------------------------
# The RP2350-Touch-LCD-3.49 uses the RP2350B (the QFN-80 part with extra GPIO).
# The stock "pico2" board is the RP2350A. If pin numbering is off, supply a
# custom board header and set PICO_BOARD to it. Platform must be an rp2350 variant.
if (NOT DEFINED PICO_BOARD)
    set(PICO_BOARD pico2)
endif ()
if (NOT DEFINED PICO_PLATFORM)
    set(PICO_PLATFORM rp2350-arm-s)   # ARM Cortex-M33 secure; matches CMSIS-DSP build
endif ()

include(pico_sdk_import.cmake)

project(waveshare_audiovisualiser C CXX ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

# --- CMSIS-DSP ---------------------------------------------------------------
# Cloned into lib/cmsis-dsp (ARM-software/CMSIS-DSP). Provides target CMSISDSP.
# CMSIS-DSP needs CMSIS-Core headers (cmsis_compiler.h etc.) which the Pico SDK
# ships at src/rp2_common/cmsis/stub/CMSIS/Core.
set(CMSISDSP_DIR "${CMAKE_CURRENT_LIST_DIR}/lib/cmsis-dsp")
set(CMSISCORE "${PICO_SDK_PATH}/src/rp2_common/cmsis/stub/CMSIS/Core" CACHE PATH "CMSIS Core dir")
if (EXISTS "${CMSISDSP_DIR}/Source/CMakeLists.txt")
    add_subdirectory("${CMSISDSP_DIR}/Source" cmsis-dsp-build)
else ()
    message(WARNING "CMSIS-DSP not found at ${CMSISDSP_DIR}. Run scripts to fetch deps.")
endif ()

# --- LVGL --------------------------------------------------------------------
# Cloned into lib/lvgl (LVGL release/v8.x to match Waveshare's 8.1). Provides target lvgl.
set(LVGL_DIR "${CMAKE_CURRENT_LIST_DIR}/lib/lvgl")
add_compile_definitions(LV_CONF_INCLUDE_SIMPLE)        # lv_conf.h found via include path
if (EXISTS "${LVGL_DIR}/CMakeLists.txt")
    add_subdirectory("${LVGL_DIR}" lvgl-build)
    target_include_directories(lvgl PUBLIC "${CMAKE_CURRENT_LIST_DIR}")   # for lv_conf.h
else ()
    message(WARNING "LVGL not found at ${LVGL_DIR}. Run scripts to fetch deps.")
endif ()

# --- Application -------------------------------------------------------------
add_executable(${PROJECT_NAME}
    src/main.c
    src/usb/usb_descriptors.c
    src/usb/usb_audio.c
    src/dsp/ringbuffer.c
    src/dsp/analyzer.c
    src/dsp/loudness.c
    src/display/qspi_pio.c
    src/display/axs15231b.c
    src/display/touch_axs.c
    src/display/lv_port_disp.c
    src/display/lv_port_indev.c
    src/ui/ui_common.c
    src/ui/mode_producer.c
    src/ui/mode_vibe.c
    src/ui/mode_lufs.c
    src/ui/mode_clock.c
    src/util/wallclock.c
    src/ui/assets/vu_face.c
    src/ui/assets/vu_needle.c
    src/ui/assets/vu_hub.c
)

# Generate qspi.pio.h from the PIO assembly source
pico_generate_pio_header(${PROJECT_NAME} ${CMAKE_CURRENT_LIST_DIR}/src/display/qspi.pio)

target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
    ${CMAKE_CURRENT_LIST_DIR}/src
    ${CMAKE_CURRENT_LIST_DIR}/src/usb
)

target_link_libraries(${PROJECT_NAME}
    pico_stdlib
    pico_multicore
    hardware_dma
    hardware_spi
    hardware_i2c
    hardware_pwm
    hardware_pio
    tinyusb_device
    tinyusb_board
)

if (TARGET CMSISDSP)
    target_link_libraries(${PROJECT_NAME} CMSISDSP)
endif ()
if (TARGET lvgl)
    target_link_libraries(${PROJECT_NAME} lvgl)
endif ()

# UART stdio for boot logs; USB stdio is disabled (USB is owned by UAC2).
pico_enable_stdio_uart(${PROJECT_NAME} 1)
pico_enable_stdio_usb(${PROJECT_NAME} 0)

pico_add_extra_outputs(${PROJECT_NAME})   # .uf2 / .bin / .hex / .map
