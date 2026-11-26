#
# CalcIDE firmware for the ClockworkPi PicoCalc.
#
#   export PICO_SDK_PATH=/path/to/pico-sdk        (2.1 or newer)
#   cmake -B build-pico2 -DPICO_BOARD=pico2       (RP2350: Pico 2 / Pico 2 W)
#   cmake -B build-pico  -DPICO_BOARD=pico        (RP2040: Pico / Pico W)
#   cmake --build build-pico2 -j
#
cmake_minimum_required(VERSION 3.13)

set(PICO_BOARD pico2 CACHE STRING "Target board")
include(pico_sdk_import.cmake)

project(calcide C CXX ASM)
set(CMAKE_C_STANDARD 11)
pico_sdk_init()

find_package(Python3 REQUIRED COMPONENTS Interpreter)

# Examples and the compiler's own source go into flash for 'install'.
file(GLOB EXAMPLES ${CMAKE_CURRENT_SOURCE_DIR}/examples/*.c)
set(EMBED_ARGS)
foreach(f ${EXAMPLES})
    get_filename_component(n ${f} NAME)
    list(APPEND EMBED_ARGS /calcide/examples/${n}=${f})
endforeach()
file(GLOB SONGS ${CMAKE_CURRENT_SOURCE_DIR}/songs/*.song)
foreach(f ${SONGS})
    get_filename_component(n ${f} NAME)
    list(APPEND EMBED_ARGS /calcide/songs/${n}=${f})
endforeach()
foreach(n cc.c ops.h prelude.h)
    list(APPEND EMBED_ARGS /calcide/cc/${n}=${CMAKE_CURRENT_SOURCE_DIR}/cc/${n})
endforeach()
add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/embedded.c
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/embed.py
            ${CMAKE_CURRENT_BINARY_DIR}/embedded.c ${EMBED_ARGS}
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tools/embed.py ${EXAMPLES} ${SONGS}
            ${CMAKE_CURRENT_SOURCE_DIR}/cc/cc.c ${CMAKE_CURRENT_SOURCE_DIR}/cc/ops.h
            ${CMAKE_CURRENT_SOURCE_DIR}/cc/prelude.h
    COMMENT "Embedding examples and compiler source")

add_executable(calcide
    src/main.c
    src/shell.c
    src/editor.c
    src/tracker.c
    src/xfer.c
    src/console.c
    src/lineedit.c
    src/config.c
    src/vfs_common.c
    src/pf.c
    src/vm.c
    src/splash.c
    src/sound.c
    src/selftest.c
    src/ccport.c
    cc/cc.c
    platform/picocalc/hal_picocalc.c
    platform/picocalc/lcd.c
    platform/picocalc/keyboard.c
    platform/picocalc/sd_spi.c
    platform/picocalc/diskio.c
    platform/picocalc/vfs_fatfs.c
    platform/picocalc/fonts.c
    lib/fatfs/ff.c
    lib/fatfs/option/ffunicode.c
    ${CMAKE_CURRENT_BINARY_DIR}/embedded.c
)

target_include_directories(calcide PRIVATE src cc platform/picocalc lib/fatfs/include)

# cc.c is written in the Tiny-C dialect; ccport.h maps its library calls.
set_source_files_properties(cc/cc.c PROPERTIES COMPILE_OPTIONS
    "-fsigned-char;-Wno-unused-variable;-Wno-unused-but-set-variable;-Wno-parentheses;-Wno-sign-compare;-Wno-unused-parameter")

target_compile_definitions(calcide PRIVATE
    PICO_MALLOC_PANIC=0          # malloc returns NULL instead of halting
    PICO_STDIO_USB_CONNECT_WAIT_TIMEOUT_MS=0
    PICO_STDIO_DEFAULT_CRLF=0
)

target_compile_options(calcide PRIVATE -O2 -Wall -Wno-unused-parameter -ffunction-sections -fdata-sections)

target_link_libraries(calcide
    pico_stdlib
    hardware_spi
    hardware_i2c
    hardware_pwm
    hardware_uart
    hardware_adc
    hardware_flash
    hardware_sync
    hardware_clocks
    hardware_vreg
    hardware_watchdog
)

# -DCALCIDE_SDBOOT=ON: a .bin for the PicoCalc SD boot launcher
# (clockworkpi/PicoCalc Code/pico_multi_booter), which runs applications
# linked 200 KB into flash. 'exit' then restarts back into the launcher.
option(CALCIDE_SDBOOT "Build for the PicoCalc SD boot launcher" OFF)
if (CALCIDE_SDBOOT)
    if (PICO_PLATFORM MATCHES "rp2350")
        set(_ld ${PICO_SDK_PATH}/src/rp2_common/pico_crt0/rp2350/memmap_default.ld)
        set(_flash 4096k)
    else()
        set(_ld ${PICO_SDK_PATH}/src/rp2_common/pico_crt0/rp2040/memmap_default.ld)
        set(_flash 2048k)
    endif()
    file(READ ${_ld} _ldtext)
    string(REPLACE "INCLUDE \"pico_flash_region.ld\""
           "FLASH(rx) : ORIGIN = 0x10000000 + 200k, LENGTH = ${_flash} - 200k" _ldtext "${_ldtext}")
    file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/memmap_sdboot.ld "${_ldtext}")
    pico_set_linker_script(calcide ${CMAKE_CURRENT_BINARY_DIR}/memmap_sdboot.ld)
    target_compile_definitions(calcide PRIVATE CALCIDE_SDBOOT=1)
endif()

pico_enable_stdio_usb(calcide 1)
pico_enable_stdio_uart(calcide 0)
pico_set_program_name(calcide "CalcIDE")
pico_set_program_version(calcide "0.33")
pico_set_program_description(calcide "On-device C IDE for the ClockworkPi PicoCalc")
pico_add_extra_outputs(calcide)
