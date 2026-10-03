# CMake toolchain for the TI-Nspire (Ndless SDK).
#
# Point NDLESS_SDK at an ndless-sdk directory (the SDK's bin/ and
# toolchain/install/bin/ are searched), e.g. the prebuilt one from
# https://github.com/MaxSignal/build-toolchain/releases or a local
# Ndless/ndless-sdk after toolchain/build_toolchain.sh.
set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR arm)
set(NSPIRE TRUE)

if(NOT DEFINED NDLESS_SDK)
    if(DEFINED ENV{NDLESS_SDK})
        set(NDLESS_SDK "$ENV{NDLESS_SDK}")
    else()
        message(FATAL_ERROR "Set NDLESS_SDK (cache or environment) to the ndless-sdk directory")
    endif()
endif()
set(NDLESS_SDK "${NDLESS_SDK}" CACHE PATH "ndless-sdk directory")

set(CMAKE_C_COMPILER "${NDLESS_SDK}/bin/nspire-gcc")
set(CMAKE_CXX_COMPILER "${NDLESS_SDK}/bin/nspire-g++")
set(CMAKE_AR "${NDLESS_SDK}/toolchain/install/bin/arm-none-eabi-ar")
set(CMAKE_RANLIB "${NDLESS_SDK}/toolchain/install/bin/arm-none-eabi-ranlib")
set(CMAKE_STRIP "${NDLESS_SDK}/toolchain/install/bin/arm-none-eabi-strip")
set(CMAKE_PROGRAM_PATH "${NDLESS_SDK}/bin" "${NDLESS_SDK}/toolchain/install/bin")

set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "")

set(CMAKE_FIND_ROOT_PATH "${NDLESS_SDK}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

set(CMAKE_C_FLAGS_INIT "-I${NDLESS_SDK}/include")
set(CMAKE_CXX_FLAGS_INIT "-I${NDLESS_SDK}/include")
set(CMAKE_EXE_LINKER_FLAGS_INIT "-L${NDLESS_SDK}/lib")

# nspire-g++ wraps arm-none-eabi-g++ and finds it on PATH.
set(ENV{PATH} "${NDLESS_SDK}/toolchain/install/bin:${NDLESS_SDK}/bin:$ENV{PATH}")
