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
include(sdk/gen/cmake_gcc/efr32fg25_sdk.cmake)
# The SDK sources are not ours: keep their warnings out of the way.
target_compile_options(slc PRIVATE -w)
# CPU flags and --specs=nano.specs already come from the toolchain file for
# every target; drop the duplicates the generated target would propagate
# (gcc refuses nano.specs twice).
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
set(NS_PORT_INCLUDE_DIR "${CMAKE_SOURCE_DIR}/port/freertos/include")
include(cmake/nanostack.cmake)

# --------------------------------------------- port (FreeRTOS/EFR32) + radio
add_library(ns_port_freertos STATIC
    port/freertos/critical.c
    port/freertos/port_core.c
    port/freertos/random.c
    port/freertos/heap_wrap.c
    port/freertos/trace_uart.c
    port/freertos/no_stdio_fs.c
)
target_include_directories(ns_port_freertos PUBLIC ${NS_PORT_INCLUDE_DIR})
target_link_libraries(ns_port_freertos PRIVATE slc nanostack_eventloop_fw)
target_include_directories(ns_port_freertos PRIVATE ${NANOSTACK_DIR}/nanostack)

add_library(rail_phy STATIC drivers/rail/rail_phy.c)
target_include_directories(rail_phy PUBLIC drivers/rail)
target_include_directories(rail_phy PRIVATE ${NANOSTACK_DIR}/nanostack)
target_link_libraries(rail_phy PRIVATE slc ns_port_freertos nanostack_libservice_fw)

foreach(t ns_port_freertos rail_phy)
    target_compile_options(${t} PRIVATE -std=gnu17 -Wall -Wextra -Wno-unused-parameter)
endforeach()

# --------------------------------------------------------------------- apps
set(APP_COMMON_SOURCES
    apps/common/node.c
    apps/common/cli.c
    apps/common/net_test.c
    apps/common/certs.c
    apps/common/freertos_hooks.c
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
        "$<LINK_GROUP:nsgroup,${nslib},nanostack_rcp_fw,nanostack_eventloop_fw,nanostack_libservice_fw,ns_port_freertos,rail_phy,mbedtls_fw>"
    )
    target_link_options(${name} PRIVATE
        -Wl,--wrap=malloc,--wrap=free,--wrap=calloc,--wrap=realloc
        -Wl,--wrap=_malloc_r,--wrap=_free_r,--wrap=_calloc_r,--wrap=_realloc_r
        -Wl,--wrap=fopen,--wrap=fclose,--wrap=fread,--wrap=fwrite,--wrap=fseek,--wrap=ftell,--wrap=remove,--wrap=rewind
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
