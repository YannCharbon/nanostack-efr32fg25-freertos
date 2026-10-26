cmake_minimum_required(VERSION 3.25)

# Host twin of the firmware: the SAME apps/, port/freertos/ and drivers/rail/
# sources, the same Nanostack + mbedTLS build (cmake/nanostack.cmake), linked
# on Linux against a pthread "FreeRTOS", a fake RAIL radio (UNIX-socket
# medium between processes) and stub SDK services. Only the RAIL/FreeRTOS/SDK
# implementations differ from the target; the RAIL headers are the real SDK
# ones. Purpose: exercise the init sequence, the port layer and the radio
# driver state machine end to end (join, EAP-TLS, DHCPv6, RPL, ping, UDP)
# before hardware.
#
#   cmake -S tests/host_twin -B build/host -G Ninja && ninja -C build/host
#   tests/host_twin/run_twin.sh build/host

project(nanostack-host-twin C)

set(FW_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../..")
set(NANOSTACK_DIR "${FW_DIR}/../nanostack-generic-standalone" CACHE PATH "")
set(SISDK_DIR "${FW_DIR}/../.deps/simplicity_sdk" CACHE PATH "")
set(MBEDTLS_DIR "${FW_DIR}/../.deps/mbedtls-3.0.0" CACHE PATH "")
foreach(d NANOSTACK_DIR SISDK_DIR MBEDTLS_DIR)
    get_filename_component(${d} "${${d}}" ABSOLUTE)
endforeach()

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -g -O1")
# The fake RAIL models the NA/1b channel plan (fake_rail.c host_channels).
add_compile_definitions(FW_REGION_NA=1)
set(APP_EXTRA_DEFS "" CACHE STRING "Extra app compile definitions, as in the firmware build")

# mbedTLS 3.0.0 with the target configuration
file(GLOB MBEDTLS_SOURCES "${MBEDTLS_DIR}/library/*.c")
add_library(mbedtls_fw STATIC ${MBEDTLS_SOURCES})
target_include_directories(mbedtls_fw PUBLIC "${MBEDTLS_DIR}/include" "${FW_DIR}/config")
target_include_directories(mbedtls_fw PRIVATE "${MBEDTLS_DIR}/library")
target_compile_definitions(mbedtls_fw PUBLIC MBEDTLS_CONFIG_FILE="mbedtls_config_target.h")
target_compile_options(mbedtls_fw PRIVATE -std=gnu17 -w)

# The port's sources are compiled into each twin executable (below); this
# target only carries the porting API headers to the stack libraries.
add_library(ns_port_twin INTERFACE)
target_link_libraries(ns_port_twin INTERFACE nanostack_port_api)
target_include_directories(ns_port_twin INTERFACE ${FW_DIR}/port/freertos/include)
set(NS_PORT_TARGET ns_port_twin)
set(NS_TRACE_MAX_LEVEL "TRACE_LEVEL_DEBUG" CACHE STRING "")
include(${FW_DIR}/cmake/nanostack.cmake)

set(RAIL_DIR "${SISDK_DIR}/platform/radio/rail_lib")
add_library(host_sdk INTERFACE)
target_include_directories(host_sdk INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/stub
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${RAIL_DIR}/common ${RAIL_DIR}/chip/efr32/efr32xg2x ${RAIL_DIR}/protocol/ieee802154
    ${SISDK_DIR}/platform/common/inc
    ${FW_DIR}/sdk/gen/brd4270b/autogen
    ${FW_DIR}/drivers/rail
    ${FW_DIR}/port/freertos/include
    ${NANOSTACK_DIR}/nanostack
)

function(add_twin name role nslib)
    add_executable(${name}
        ${FW_DIR}/apps/common/node.c
        ${FW_DIR}/apps/common/cli.c
        ${FW_DIR}/apps/common/net_test.c
        ${FW_DIR}/apps/common/certs.c
        ${FW_DIR}/apps/${role}/${role}.c
        ${FW_DIR}/port/freertos/critical.c
        ${FW_DIR}/port/freertos/port_core.c
        ${FW_DIR}/port/freertos/random.c
        ${FW_DIR}/port/freertos/heap_wrap.c
        ${FW_DIR}/port/freertos/trace_uart.c
        ${FW_DIR}/drivers/rail/rail_phy.c
        fake_rtos.c
        fake_rail.c
        fake_sdk.c
    )
    target_include_directories(${name} PRIVATE ${FW_DIR}/apps/common)
    target_compile_definitions(${name} PRIVATE ${APP_EXTRA_DEFS})
    target_compile_options(${name} PRIVATE -std=gnu17 -Wall -Wno-unused-parameter -Wno-missing-field-initializers)
    target_link_libraries(${name} PRIVATE host_sdk
        -Wl,--start-group ${nslib} nanostack_rcp_fw nanostack_eventloop nanostack_libservice mbedtls_fw -Wl,--end-group
        pthread)
    # Same wraps as the firmware link, plus main (like sl_main does).
    target_link_options(${name} PRIVATE
        -Wl,--wrap=malloc,--wrap=free,--wrap=calloc,--wrap=realloc
        -Wl,--wrap=main)
endfunction()

add_twin(twin_br br nanostack_br_fw)
add_twin(twin_rn rn nanostack_rn_fw)
