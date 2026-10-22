# Builds the Nanostack libraries from NANOSTACK_DIR for the target.
#
# Why not add_subdirectory(${NANOSTACK_DIR})? The stack's CMakeLists.txt
# hard-links its Linux port (nanostack_port_linux), requires Threads and
# looks MbedTLS up with find_package (change D18 of GENERICITY_PLAN.md, not
# done yet; see docs/stack-patches/). Until then the firmware compiles the
# stack sources itself. To stay in sync with the stack, the source lists are
# *read from the stack's own CMakeLists.txt* (NANOSTACK_SOURCES,
# NANOSTACK_BR_SOURCES, NANOSTACK_RCP_SOURCES); only the library/port wiring
# is done here.
#
# Inputs:  NANOSTACK_DIR, NS_PORT_INCLUDE_DIR (provides common/rand.h,
#          common/hal_interrupt.h, common/utils.h), mbedtls target `mbedtls_fw`.
# Outputs: nanostack_libservice_fw, nanostack_eventloop_fw, nanostack_rcp_fw,
#          nanostack_br_fw, nanostack_rn_fw (static libraries).

if(NOT EXISTS "${NANOSTACK_DIR}/nanostack/source/6lowpan/ws/ws_bootstrap.c")
    message(FATAL_ERROR "NANOSTACK_DIR=${NANOSTACK_DIR} does not look like nanostack-generic-standalone")
endif()

file(READ "${NANOSTACK_DIR}/CMakeLists.txt" _ns_cmake)
function(_ns_extract_list var)
    string(REGEX MATCH "set\\(${var}\n([^)]*)\\)" _m "${_ns_cmake}")
    if(NOT _m)
        message(FATAL_ERROR "Could not find set(${var} ...) in ${NANOSTACK_DIR}/CMakeLists.txt")
    endif()
    string(REGEX REPLACE "#[^\n]*" "" _body "${CMAKE_MATCH_1}")
    string(REGEX REPLACE "[ \t\r\n]+" ";" _body "${_body}")
    list(FILTER _body EXCLUDE REGEX "^$")
    list(TRANSFORM _body PREPEND "${NANOSTACK_DIR}/")
    set(${var} ${_body} PARENT_SCOPE)
endfunction()
_ns_extract_list(NANOSTACK_SOURCES)
_ns_extract_list(NANOSTACK_BR_SOURCES)
_ns_extract_list(NANOSTACK_RCP_SOURCES)

# D15: rf_driver_storage.c exists in nanostack/source/mac/ and in rcp/mac/
# (same code, 10 duplicate symbols). In-process builds link only one copy:
# the nanostack/ one.
list(FILTER NANOSTACK_RCP_SOURCES EXCLUDE REGEX "rcp/mac/rf_driver_storage\\.c$")

set(NS_LIBSERVICE_SOURCES
    ${NANOSTACK_DIR}/mbed-client-libservice/source/libBits/common_functions.c
    ${NANOSTACK_DIR}/mbed-client-libservice/source/libip6string/ip6tos.c
    ${NANOSTACK_DIR}/mbed-client-libservice/source/libip6string/stoip6.c
    ${NANOSTACK_DIR}/mbed-client-libservice/source/libList/ns_list.c
    ${NANOSTACK_DIR}/mbed-client-libservice/source/libtrace/ns_trace.c
)
set(NS_EVENTLOOP_SOURCES
    ${NANOSTACK_DIR}/nanostack-event-loop/source/event.c
    ${NANOSTACK_DIR}/nanostack-event-loop/source/system_timer.c
)

# Flags for the (inherited, warning-heavy) stack code: GCC >= 14 needs
# -std=gnu17 -fpermissive (stack README). Warnings are not the gate here.
set(NS_STACK_COPTS -std=gnu17 -fpermissive -w)
# Trace level compiled in (ns_trace). 4 = INFO, 5 = DEBUG.
set(NS_TRACE_MAX_LEVEL "TRACE_LEVEL_INFO" CACHE STRING "MBED_TRACE_MAX_LEVEL for stack traces")
set(NS_STACK_DEFS MBED_TRACE_MAX_LEVEL=${NS_TRACE_MAX_LEVEL})

add_library(nanostack_libservice_fw STATIC ${NS_LIBSERVICE_SOURCES})
target_include_directories(nanostack_libservice_fw PUBLIC
    ${NANOSTACK_DIR} ${NANOSTACK_DIR}/mbed-client-libservice ${NS_PORT_INCLUDE_DIR})
target_compile_options(nanostack_libservice_fw PRIVATE ${NS_STACK_COPTS})
target_compile_definitions(nanostack_libservice_fw PUBLIC ${NS_STACK_DEFS})

add_library(nanostack_eventloop_fw STATIC ${NS_EVENTLOOP_SOURCES})
target_include_directories(nanostack_eventloop_fw PUBLIC ${NANOSTACK_DIR}/nanostack-event-loop)
target_compile_options(nanostack_eventloop_fw PRIVATE ${NS_STACK_COPTS})
target_link_libraries(nanostack_eventloop_fw PUBLIC nanostack_libservice_fw)

# 802.15.4 MAC + FHSS + CCM, in-process. rcp/ comes before nanostack/ (it has
# its own mac/rf_driver_storage.h), nanostack/source/ is not on its path.
add_library(nanostack_rcp_fw STATIC ${NANOSTACK_RCP_SOURCES})
target_include_directories(nanostack_rcp_fw PRIVATE ${NANOSTACK_DIR}/rcp ${NANOSTACK_DIR}/nanostack)
target_compile_options(nanostack_rcp_fw PRIVATE ${NS_STACK_COPTS})
target_link_libraries(nanostack_rcp_fw PUBLIC nanostack_eventloop_fw mbedtls_fw)

function(nanostack_fw_add_library name nsconfig)
    cmake_parse_arguments(PARSE_ARGV 2 ARG "BORDER_ROUTER" "" "")
    add_library(${name} STATIC ${NANOSTACK_SOURCES})
    if(ARG_BORDER_ROUTER)
        target_sources(${name} PRIVATE ${NANOSTACK_BR_SOURCES})
    endif()
    target_include_directories(${name} PUBLIC ${NANOSTACK_DIR}/nanostack/source ${NANOSTACK_DIR}/nanostack)
    target_compile_definitions(${name} PUBLIC NSCONFIG=${nsconfig})
    target_compile_options(${name} PRIVATE ${NS_STACK_COPTS})
    target_link_libraries(${name} PUBLIC nanostack_eventloop_fw nanostack_libservice_fw mbedtls_fw)
endfunction()

nanostack_fw_add_library(nanostack_br_fw ws_border_router BORDER_ROUTER)
nanostack_fw_add_library(nanostack_rn_fw ws_router)
