cmake_minimum_required(VERSION 3.25)

# Nanostack (Wi-SUN FAN 1.0) on FreeRTOS for the Silicon Labs EFR32FG25.
# Two images: wisun_br (border router) and wisun_rn (router node).
# Configure with the toolchain file (see CMakePresets.json):
#   cmake --preset fw && cmake --build --preset br   (or rn / all)

project(nanostack-efr32fg25-freertos C ASM)

set(NANOSTACK_DIR "${CMAKE_SOURCE_DIR}/../nanostack-generic-standalone" CACHE PATH
    "nanostack-generic-standalone checkout (later: a git submodule)")
set(SISDK_DIR "${CMAKE_SOURCE_DIR}/../.deps/simplicity_sdk" CACHE PATH
    "Simplicity SDK v2025.6.3 checkout, with the xG25 RAIL library LFS object pulled")
set(MBEDTLS_DIR "${CMAKE_SOURCE_DIR}/../.deps/mbedtls-3.0.0" CACHE PATH
    "Mbed TLS v3.0.0 source tree")
set(APP_EXTRA_DEFS "" CACHE STRING "Extra compile definitions for the apps (app_config.h overrides)")
option(FW_FLASH_FS "Keys, frame counters and network information in internal flash (ns_file_system): a reset node rejoins without a full EAP-TLS; CLI nvm erase forgets them" ON)
option(FW_PTI "Packet trace (PTI) on the debug channel for silink captures (black-box phases 2-3); test builds only" OFF)
option(FW_BOARD_FLASH_DPD "Put the radio board's MX25R SPI flash (unused) in deep power-down at boot: a few µA less in EM2" ON)
set(FW_TEST_EM4_AFTER_S "" CACHE STRING "Test only: enter EM4 this many seconds after boot, before the stack starts (board residual current with the AEM); empty: off")
set(FW_TEST_EM2_IDLE_S "" CACHE STRING "Test only: stop this many seconds after boot, before the stack starts, and stay in EM2 with nothing running (the kit's bare EM2 floor); empty: off")
option(FW_TEST_EM2_VCOM_OFF "Test only, with FW_TEST_EM2_IDLE_S: switch the VCOM pins and the board's VCOM enable off first" OFF)
option(FW_POWER_STATS "Power/activity statistics for AEM measurements (HW5): EM times, cycles per wake, radio states, GPIO PC1" OFF)
option(FW_LFN_PARENT "Border router: Wi-SUN FAN 1.1 with LFN parenting (GS8 stage 6); the wisun_lfn image needs it" OFF)
option(FW_RN_LFN_PARENT "Router (wisun_rn): Wi-SUN FAN 1.1 with LFN parenting; its border router needs FW_LFN_PARENT" OFF)
set(FW_TEST_MAC_ALLOW "" CACHE STRING "Test only, wisun_lfn: accept frames only from these EUI-64s (16 hex digits each, comma-separated, at most 4), e.g. to force a router as the LFN's parent; empty: off")
set(FW_TX_POWER_DDBM "" CACHE STRING "Fixed TX power, deci-dBm (default 100: +10 dBm); the CLI 'txpower' changes it at run time")
set(FW_LFN_MC_GROUP "" CACHE STRING "wisun_lfn: join this IPv6 multicast group on the UDP test socket (registered with the parent, RFC 9685); empty: none")
set(FW_LFN_PARENT_CHECK_S "" CACHE STRING "wisun_lfn: better-parent check interval, s (both lfn parent_check_s and parent_check_weak_s); empty: the stack's defaults (6 h, 1 h while weak)")
set(FW_RUNLOOP_SAFETY_MS "" CACHE STRING
    "Longest run-loop wait without a notification, ms (0: none). Empty: the default, 0 (validated on the boards with idle-tick skipping)")
set(FW_BOARD "brd4271a" CACHE STRING "Radio board: brd4271a (863-870 MHz, Wi-SUN EU) or brd4270b (902-928 MHz, Wi-SUN NA)")
set_property(CACHE FW_BOARD PROPERTY STRINGS brd4271a brd4270b)
if(FW_BOARD STREQUAL "brd4271a")
    set(_region_default EU)
elseif(FW_BOARD STREQUAL "brd4270b")
    set(_region_default NA)
else()
    message(FATAL_ERROR "FW_BOARD must be brd4271a or brd4270b")
endif()
set(FW_REGION "${_region_default}" CACHE STRING "Wi-SUN regulatory domain: EU (class 1, mode 1a) or NA (class 1, mode 1b)")
set_property(CACHE FW_REGION PROPERTY STRINGS EU NA)
if(NOT FW_REGION STREQUAL _region_default)
    # The radio config generated for a board is single-band (sdk/gen/<board>/).
    message(FATAL_ERROR "FW_REGION=${FW_REGION} does not match FW_BOARD=${FW_BOARD} (${_region_default} only)")
endif()
add_compile_definitions(FW_REGION_${FW_REGION}=1)
# Wi-SUN PHY mode. EU: 1a (default; operating class 1, 69 channels at 100 kHz,
# 50 kbit/s) or 3 (operating class 2, 35 channels at 200 kHz, 150 kbit/s, radio
# config sdk/gen/<board>-mode3). NA: 1b only.
if(FW_REGION STREQUAL "EU")
    set(_mode_default 1a)
    set(_modes 1a 3)
else()
    set(_mode_default 1b)
    set(_modes 1b)
endif()
set(FW_PHY_MODE "${_mode_default}" CACHE STRING "Wi-SUN PHY mode: EU 1a (default) or 3; NA 1b")
set_property(CACHE FW_PHY_MODE PROPERTY STRINGS 1a 1b 3)
if(NOT FW_PHY_MODE IN_LIST _modes)
    message(FATAL_ERROR "FW_PHY_MODE=${FW_PHY_MODE} is not available for FW_REGION=${FW_REGION} (${_modes})")
endif()
if(FW_PHY_MODE STREQUAL "3")
    set(FW_SDK_GEN "${FW_BOARD}-mode3")
    add_compile_definitions(FW_PHY_MODE_3=1)
else()
    set(FW_SDK_GEN "${FW_BOARD}")
endif()
message(STATUS "Firmware variant: board ${FW_BOARD}, Wi-SUN region ${FW_REGION}, PHY mode ${FW_PHY_MODE} (sdk/gen/${FW_SDK_GEN})")

foreach(d NANOSTACK_DIR SISDK_DIR MBEDTLS_DIR)
    get_filename_component(${d} "${${d}}" ABSOLUTE)
    if(NOT IS_DIRECTORY "${${d}}")
        message(FATAL_ERROR "${d}=${${d}} does not exist (see README.md, Dependencies)")
    endif()
endforeach()
if(NOT EXISTS "${SISDK_DIR}/platform/radio/rail_lib/autogen/librail_release/librail_efr32xg25_gcc_release.a")
    message(FATAL_ERROR "RAIL library missing in ${SISDK_DIR}")
endif()
file(SIZE "${SISDK_DIR}/platform/radio/rail_lib/autogen/librail_release/librail_efr32xg25_gcc_release.a" _rail_size)
if(_rail_size LESS 10000)
    message(FATAL_ERROR "The xG25 RAIL library in ${SISDK_DIR} is a git-lfs pointer: run "
            "git lfs pull --include platform/radio/rail_lib/autogen/librail_release/librail_efr32xg25_gcc_release.a")
endif()

# ------------------------------------------------------------------ SDK part
# Generated once by tools/sdk_generate.sh (slc-cli) and committed: defines the
# OBJECT library `slc` (startup, system, emlib/peripherals, clocks, FreeRTOS +
# heap_4, sleeptimer, power manager, SE manager, iostream VCOM, RAIL PA util,
# rail_config.c) plus its include paths, defines, CPU flags and linker script.
set(SDK_PATH "${SISDK_DIR}")
include(sdk/gen/${FW_SDK_GEN}/cmake_gcc/efr32fg25_sdk.cmake)
# The SDK sources are not ours: keep their warnings out of the way.
target_compile_options(slc PRIVATE -w)
# The generated target propagates --specs=nano.specs; the firmware links the
# full newlib instead (64-bit printf formats, see cmake/arm-none-eabi.cmake),
# so drop it.
foreach(prop COMPILE_OPTIONS INTERFACE_COMPILE_OPTIONS INTERFACE_LINK_OPTIONS)
    get_target_property(_opts slc ${prop})
    list(FILTER _opts EXCLUDE REGEX "nano\\.specs")
    set_target_properties(slc PROPERTIES ${prop} "${_opts}")
endforeach()

# ------------------------------------------------------------------ mbedTLS
file(GLOB MBEDTLS_SOURCES "${MBEDTLS_DIR}/library/*.c")
add_library(mbedtls_fw STATIC ${MBEDTLS_SOURCES})
target_include_directories(mbedtls_fw PUBLIC "${MBEDTLS_DIR}/include" "${CMAKE_SOURCE_DIR}/config")
target_include_directories(mbedtls_fw PRIVATE "${MBEDTLS_DIR}/library")
target_compile_definitions(mbedtls_fw PUBLIC MBEDTLS_CONFIG_FILE="mbedtls_config_target.h")
target_compile_options(mbedtls_fw PRIVATE -std=gnu17 -w)

# ---------------------------------------------------------------- Nanostack
set(NS_PORT_TARGET ns_port_freertos)
include(cmake/nanostack.cmake)

# --------------------------------------------- port (FreeRTOS/EFR32) + radio
add_library(ns_port_freertos STATIC
    port/freertos/critical.c
    port/freertos/port_core.c
    port/freertos/random.c
    port/freertos/heap_wrap.c
    port/freertos/trace_uart.c
)
if(NOT FW_RUNLOOP_SAFETY_MS STREQUAL "")
    add_compile_definitions(FW_RUNLOOP_SAFETY_MS=${FW_RUNLOOP_SAFETY_MS})
endif()
if(FW_FLASH_FS)
    target_sources(ns_port_freertos PRIVATE port/freertos/flash_fs.c ${SISDK_DIR}/platform/emlib/src/em_msc.c)
    add_compile_definitions(FW_FLASH_FS=1)
    message(STATUS "FW_FLASH_FS=ON: keys and frame counters in flash")
endif()
target_sources(ns_port_freertos PRIVATE port/freertos/board_power.c)
if(FW_BOARD_FLASH_DPD)
    add_compile_definitions(FW_BOARD_FLASH_DPD=1)
endif()
if(NOT FW_TEST_EM4_AFTER_S STREQUAL "")
    add_compile_definitions(FW_TEST_EM4_AFTER_S=${FW_TEST_EM4_AFTER_S})
    message(WARNING "FW_TEST_EM4_AFTER_S=${FW_TEST_EM4_AFTER_S}: TEST image, enters EM4 for good after ${FW_TEST_EM4_AFTER_S} s")
endif()
if(NOT FW_TEST_EM2_IDLE_S STREQUAL "")
    add_compile_definitions(FW_TEST_EM2_IDLE_S=${FW_TEST_EM2_IDLE_S} FW_TEST_EM2_VCOM_OFF=$<BOOL:${FW_TEST_EM2_VCOM_OFF}>)
    message(WARNING "FW_TEST_EM2_IDLE_S=${FW_TEST_EM2_IDLE_S}: TEST image, stops in EM2 after ${FW_TEST_EM2_IDLE_S} s")
endif()
if(FW_PTI)
    add_compile_definitions(FW_PTI=1)
    message(STATUS "FW_PTI=ON: packet trace on the debug channel")
endif()
if(FW_POWER_STATS)
    target_sources(ns_port_freertos PRIVATE port/freertos/power_stats.c)
    target_link_libraries(ns_port_freertos PRIVATE mbedtls_fw)
    add_compile_definitions(FW_POWER_STATS=1)
    message(STATUS "FW_POWER_STATS=ON: power/activity statistics built in (pstats CLI command)")
endif()
target_include_directories(ns_port_freertos PUBLIC port/freertos/include)
# eventOS_callback_timer_start_usec() exists (ns_port/os_timer.h)
target_compile_definitions(ns_port_freertos PUBLIC NS_PORT_TIMER_USEC)
target_link_libraries(ns_port_freertos PUBLIC nanostack_port_api)
target_link_libraries(ns_port_freertos PRIVATE slc nanostack_eventloop)
target_include_directories(ns_port_freertos PRIVATE ${NANOSTACK_DIR}/nanostack)

add_library(rail_phy STATIC drivers/rail/rail_phy.c)
target_include_directories(rail_phy PUBLIC drivers/rail)
target_include_directories(rail_phy PRIVATE ${NANOSTACK_DIR}/nanostack)
target_link_libraries(rail_phy PRIVATE slc ns_port_freertos nanostack_libservice)

foreach(t ns_port_freertos rail_phy)
    target_compile_options(${t} PRIVATE -std=gnu17 -Wall -Wextra -Wno-unused-parameter)
endforeach()

# --------------------------------------------------------------------- apps
set(APP_COMMON_SOURCES
    apps/common/node.c
    apps/common/cli.c
    apps/common/net_test.c
    apps/common/certs.c
    apps/common/fault.c
)

# --start-group/--end-group feature for $<LINK_GROUP> (RESCAN is only
# predefined for hosted GNU platforms).
set(CMAKE_C_LINK_GROUP_USING_nsgroup_SUPPORTED TRUE)
set(CMAKE_C_LINK_GROUP_USING_nsgroup "LINKER:--start-group" "LINKER:--end-group")

function(add_wisun_image name role nslib)
    add_executable(${name} ${APP_COMMON_SOURCES} apps/${role}/${role}.c)
    set_target_properties(${name} PROPERTIES SUFFIX ".elf" LINKER_LANGUAGE C)
    target_include_directories(${name} PRIVATE apps/common)
    target_compile_definitions(${name} PRIVATE ${APP_EXTRA_DEFS})
    target_compile_options(${name} PRIVATE -std=gnu17 -Wall -Wextra -Wno-unused-parameter
                           -Wno-missing-field-initializers)
    # The stack libraries reference each other (stack <-> MAC <-> event loop
    # <-> port) and the port calls back into the event loop: link them as one
    # group.
    target_link_libraries(${name} PRIVATE
        slc
        "$<LINK_GROUP:nsgroup,${nslib},nanostack_rcp_fw,nanostack_eventloop,nanostack_libservice,ns_port_freertos,rail_phy,mbedtls_fw>"
    )
    target_link_options(${name} PRIVATE
        -Wl,--wrap=malloc,--wrap=free,--wrap=calloc,--wrap=realloc
        -Wl,--wrap=_malloc_r,--wrap=_free_r,--wrap=_calloc_r,--wrap=_realloc_r
        $<$<BOOL:${FW_POWER_STATS}>:-Wl,--wrap=mbedtls_aes_crypt_ecb,--wrap=mbedtls_ecp_mul_restartable,--wrap=mbedtls_ecp_muladd_restartable>
        -Wl,-Map=$<TARGET_FILE_DIR:${name}>/${name}.map
        -Wl,--print-memory-usage
    )
    add_custom_command(TARGET ${name} POST_BUILD
        COMMAND ${CMAKE_OBJCOPY} -O ihex $<TARGET_FILE:${name}> $<TARGET_FILE_DIR:${name}>/${name}.hex
        COMMAND ${CMAKE_OBJCOPY} -O srec $<TARGET_FILE:${name}> $<TARGET_FILE_DIR:${name}>/${name}.s37
        COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:${name}> $<TARGET_FILE_DIR:${name}>/${name}.bin
        COMMAND python3 ${CMAKE_SOURCE_DIR}/tools/size_report.py $<TARGET_FILE:${name}>
                --size ${CMAKE_SIZE} --nm ${CMAKE_NM}
        VERBATIM)
endfunction()

add_wisun_image(wisun_br br nanostack_br_fw)
add_wisun_image(wisun_rn rn nanostack_rn_fw)
# FAN 1.1 LFN (GS8 stage 6, HW8): the router program as an LFN, sleeping in
# EM2 between its windows
add_wisun_image(wisun_lfn rn nanostack_rn_fw)
target_compile_definitions(wisun_lfn PRIVATE APP_ROLE_LFN=1)
if(FW_LFN_PARENT)
    target_compile_definitions(wisun_br PRIVATE APP_LFN_PARENT=1)
    message(STATUS "FW_LFN_PARENT=ON: the border router runs FAN 1.1 with LFN parenting")
endif()
if(FW_RN_LFN_PARENT)
    target_compile_definitions(wisun_rn PRIVATE APP_LFN_PARENT=1)
    message(STATUS "FW_RN_LFN_PARENT=ON: the router runs FAN 1.1 with LFN parenting")
endif()
if(FW_TEST_MAC_ALLOW)
    string(REPLACE "," ";" _eui_list "${FW_TEST_MAC_ALLOW}")
    list(LENGTH _eui_list _n)
    if(_n GREATER 4)
        message(FATAL_ERROR "FW_TEST_MAC_ALLOW=${FW_TEST_MAC_ALLOW}: at most 4 EUI-64s")
    endif()
    foreach(_eui IN LISTS _eui_list)
        string(LENGTH "${_eui}" _len)
        if(NOT _eui MATCHES "^[0-9a-fA-F]+$" OR NOT _len EQUAL 16)
            message(FATAL_ERROR "FW_TEST_MAC_ALLOW=${FW_TEST_MAC_ALLOW}: EUI-64s of 16 hex digits, comma-separated")
        endif()
    endforeach()
    target_compile_definitions(wisun_lfn PRIVATE APP_TEST_MAC_ALLOW="${FW_TEST_MAC_ALLOW}")
    message(WARNING "FW_TEST_MAC_ALLOW=${FW_TEST_MAC_ALLOW}: TEST ONLY, wisun_lfn drops every frame from other sources")
endif()
if(FW_TX_POWER_DDBM)
    target_compile_definitions(rail_phy PRIVATE RAIL_PHY_TX_POWER_FIXED_DDBM=${FW_TX_POWER_DDBM})
    message(STATUS "FW_TX_POWER_DDBM=${FW_TX_POWER_DDBM}")
endif()
if(FW_LFN_MC_GROUP)
    target_compile_definitions(wisun_lfn PRIVATE APP_LFN_MC_GROUP="${FW_LFN_MC_GROUP}")
    message(STATUS "FW_LFN_MC_GROUP=${FW_LFN_MC_GROUP}: wisun_lfn joins it")
endif()
if(FW_LFN_PARENT_CHECK_S)
    target_compile_definitions(wisun_lfn PRIVATE APP_LFN_PARENT_CHECK_S=${FW_LFN_PARENT_CHECK_S})
    message(STATUS "FW_LFN_PARENT_CHECK_S=${FW_LFN_PARENT_CHECK_S}")
endif()
