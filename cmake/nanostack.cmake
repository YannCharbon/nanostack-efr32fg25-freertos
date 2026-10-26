# Builds the Nanostack libraries from NANOSTACK_DIR for the target, through
# the stack's own CMake (GENERICITY_PLAN.md D18, step S3).
#
# Inputs:  NANOSTACK_DIR; NS_PORT_TARGET, the target implementing the porting
#          API (the stack's include/ns_port/); mbedTLS target `mbedtls_fw`.
# Outputs: nanostack_br_fw, nanostack_rn_fw (upper layers), nanostack_rcp_fw
#          (802.15.4 MAC, FHSS, CCM, in-process), plus the stack's
#          nanostack_eventloop and nanostack_libservice.

if(NOT EXISTS "${NANOSTACK_DIR}/nanostack/source/6lowpan/ws/ws_bootstrap.c")
    message(FATAL_ERROR "NANOSTACK_DIR=${NANOSTACK_DIR} does not look like nanostack-generic-standalone")
endif()

# The stack uses mbedTLS through these names; they are ours.
add_library(MbedTLS::mbedtls ALIAS mbedtls_fw)
add_library(MbedTLS::mbedcrypto ALIAS mbedtls_fw)
add_library(MbedTLS::mbedx509 ALIAS mbedtls_fw)

set(NANOSTACK_PORT ${NS_PORT_TARGET} CACHE STRING "" FORCE)
# No file system: ns_file_system.c without its stdio fallback (D9, D10).
set(NANOSTACK_FILE_NO_STDIO ON CACHE BOOL "" FORCE)
add_subdirectory(${NANOSTACK_DIR} nanostack)

nanostack_add_library(nanostack_br_fw CONFIG ws_border_router BORDER_ROUTER)
nanostack_add_library(nanostack_rn_fw CONFIG ws_router)
# The MAC runs in the same image: one rf_driver_storage.c (D15).
nanostack_add_rcp_library(nanostack_rcp_fw INPROCESS)

# Trace level compiled in (ns_trace): debug traces included (runtime level
# selects what is printed; e.g. the FHSS "synch to parent" drift trace is DEBUG).
set(NS_TRACE_MAX_LEVEL "TRACE_LEVEL_DEBUG" CACHE STRING "MBED_TRACE_MAX_LEVEL for stack traces")
target_compile_definitions(nanostack_libservice PUBLIC MBED_TRACE_MAX_LEVEL=${NS_TRACE_MAX_LEVEL})
# Flags for the (inherited, warning-heavy) stack code: GCC >= 14 needs
# -std=gnu17 -fpermissive (stack README). Warnings are not the gate here.
foreach(t nanostack_br_fw nanostack_rn_fw nanostack_rcp_fw nanostack_eventloop nanostack_libservice)
    target_compile_options(${t} PRIVATE -std=gnu17 -fpermissive -w)
endforeach()
