# Toolchain file: arm-none-eabi-gcc for the EFR32FG25 (Cortex-M33, FPv5-SP).
#
# The compiler is taken from PATH, or from ARM_GCC_DIR (environment or cache)
# if set, e.g. ARM_GCC_DIR=/opt/arm-gnu-toolchain-12.2.rel1-x86_64-arm-none-eabi
set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR arm)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

if(NOT ARM_GCC_DIR AND DEFINED ENV{ARM_GCC_DIR})
    set(ARM_GCC_DIR "$ENV{ARM_GCC_DIR}")
endif()
if(ARM_GCC_DIR)
    set(_tc "${ARM_GCC_DIR}/bin/")
else()
    set(_tc "")
endif()

set(CMAKE_C_COMPILER   ${_tc}arm-none-eabi-gcc)
set(CMAKE_CXX_COMPILER ${_tc}arm-none-eabi-g++)
set(CMAKE_ASM_COMPILER ${_tc}arm-none-eabi-gcc)
set(CMAKE_OBJCOPY      ${_tc}arm-none-eabi-objcopy CACHE FILEPATH "")
set(CMAKE_SIZE         ${_tc}arm-none-eabi-size    CACHE FILEPATH "")
set(CMAKE_NM           ${_tc}arm-none-eabi-nm      CACHE FILEPATH "")

set(CPU_FLAGS "-mcpu=cortex-m33 -mthumb -mfloat-abi=hard -mfpu=fpv5-sp-d16")
# Full newlib, NOT newlib-nano (--specs=nano.specs): nano's printf has no
# 64-bit (%lld / PRIi64 / PRIu64) support, and the stack traces 64-bit values
# with those formats (e.g. ws_pae_key_storage_trace()). nano then takes the
# wrong arguments for the following %s and dereferences NULL: a BusFault on
# the border router ~20 s after an RN authenticates (found on hardware).
set(CMAKE_C_FLAGS_INIT   "${CPU_FLAGS} -ffunction-sections -fdata-sections -fomit-frame-pointer")
set(CMAKE_ASM_FLAGS_INIT "${CPU_FLAGS} -x assembler-with-cpp")
set(CMAKE_EXE_LINKER_FLAGS_INIT "-Wl,--gc-sections")

# -Os -g for every build type: this is firmware, size matters and the debug
# info does not end up in the image.
set(CMAKE_C_FLAGS_RELEASE "-Os -g" CACHE STRING "")
set(CMAKE_C_FLAGS_MINSIZEREL "-Os -g" CACHE STRING "")
set(CMAKE_C_FLAGS_DEBUG "-Og -g" CACHE STRING "")

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
