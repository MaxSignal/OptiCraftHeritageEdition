# nspire.cmake -- TI-Nspire (Ndless) build branch for OptiCraft.
#
# Included from the top of CMakeLists.txt when cmake/nspire_toolchain.cmake is
# active (it sets NSPIRE) or -DPLATFORM=NSPIRE is passed; it builds its own
# target and the caller return()s, like the PS2 and Wii branches.
#
# Two shapes from the same sources:
#
#   nspire-release        nspire-g++ from the Ndless SDK -> bin/nspire/opticraft.tns
#   nspire-hostsim        the host compiler, -DPLATFORM=NSPIRE without the
#                         toolchain file -> bin/nspire-hostsim/opticraft-hostsim,
#                         a headless desktop executable of the exact port (same
#                         PlatformConfig, tuning, nGL renderer and storage) that
#                         dumps frames as PPM and reads the keypad from a script.
#                         See src/nspire/NspireSystem_hostsim.cpp.
#
# Rendering is software: nGL (external/nGL, GPLv3) rasterises into a 320x240
# RGB565 buffer. Expect a handful of frames per second at best on hardware.

cmake_minimum_required(VERSION 3.21)

include(${CMAKE_SOURCE_DIR}/cmake/SourceSelection.cmake)

set(MC_LOG_LEVEL "0" CACHE STRING "Unified diagnostic verbosity: 0=off, 1=info, 2=debug, 3=trace")

if(NSPIRE)
    set(NSPIRE_HOST_SIM OFF)
    message(STATUS "Nspire build: Ndless (calculator)")
else()
    set(NSPIRE_HOST_SIM ON)
    message(STATUS "Nspire build: host simulator")
endif()

# The Ndless linker wrapper (ndless-sdk/bin/arm-none-eabi-ld.gold) swaps the
# toolchain's crt0/crti/crtn and -lc/-lgcc for Ndless' own by filtering its
# argument list. Objects passed through a response file reach ld unfiltered and
# drag newlib's crt0 in next to Ndless'. ~1200 objects fit a Linux command line.
set(CMAKE_CXX_USE_RESPONSE_FILE_FOR_OBJECTS OFF)
set(CMAKE_C_USE_RESPONSE_FILE_FOR_OBJECTS OFF)
set(CMAKE_CXX_USE_RESPONSE_FILE_FOR_LIBRARIES OFF)

# --- Source selection ---------------------------------------------------------
mcbeta_collect_platform_sources(NSPIRE_SOURCES nspire)

# The Wii's section-build state machine and its CPU-only helpers are shared
# through PLATFORM_HANDLE_TERRAIN (see PlatformConfig.h).
list(APPEND NSPIRE_SOURCES
    "${CMAKE_SOURCE_DIR}/src/wii/minecraft/WorldRendererWii.cpp"
    "${CMAKE_SOURCE_DIR}/src/wii/minecraft/RenderList.cpp"
    "${CMAKE_SOURCE_DIR}/src/wii/minecraft/RenderGlobalWii.cpp"
    "${CMAKE_SOURCE_DIR}/src/wii/render/WiiBlockRenderInfo.cpp"
    "${CMAKE_SOURCE_DIR}/src/wii/render/WiiMeshSort.cpp"
)

# nGL (GPLv3), patched -- see external/nGL/OPTICRAFT.md.
set(NSPIRE_NGL_SOURCES
    "${CMAKE_SOURCE_DIR}/external/nGL/gl.cpp"
    "${CMAKE_SOURCE_DIR}/external/nGL/fastmath.cpp"
)
list(APPEND NSPIRE_SOURCES ${NSPIRE_NGL_SOURCES})

set(NSPIRE_MINIZIP_SOURCES
    "${CMAKE_SOURCE_DIR}/external/zlib/contrib/minizip/ioapi.c"
    "${CMAKE_SOURCE_DIR}/external/zlib/contrib/minizip/unzip.c"
)
list(APPEND NSPIRE_SOURCES ${NSPIRE_MINIZIP_SOURCES})
set_source_files_properties(${NSPIRE_MINIZIP_SOURCES} PROPERTIES COMPILE_DEFINITIONS USE_FILE32API)

# Same as the consoles: local stats only, and no network stack (NO_NETWORK keeps
# the shared JavaNetwork.cpp on its offline stubs).
mcbeta_exclude_remote_stats_sources(NSPIRE_SOURCES)
mcbeta_select_platform_backends(NSPIRE_SOURCES NSPIRE NGL NSPIRE)

# --- Target -------------------------------------------------------------------
add_executable(OptiCraft ${NSPIRE_SOURCES})
set_target_properties(OptiCraft PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO
)

target_compile_definitions(OptiCraft PRIVATE
    NSPIRE_PLATFORM
    NO_NETWORK
    NO_SOUND
    MC_LOG_LEVEL=${MC_LOG_LEVEL}
)

target_include_directories(OptiCraft PRIVATE
    "${CMAKE_SOURCE_DIR}/src"
    "${CMAKE_SOURCE_DIR}/src/pc"
    "${CMAKE_SOURCE_DIR}/src/net/minecraft/src"   # src/mods includes bare names, as on PS2
    "${CMAKE_SOURCE_DIR}/src/mods"
    "${CMAKE_SOURCE_DIR}/src/nspire/render"   # glconfig.h for nGL
    "${CMAKE_SOURCE_DIR}/external/nGL"
    "${CMAKE_SOURCE_DIR}/external/stb"
    "${CMAKE_SOURCE_DIR}/external/zlib/contrib/minizip"
)

target_compile_options(OptiCraft PRIVATE
    -fno-math-errno
    -fno-trapping-math
    -Wno-psabi
    $<$<COMPILE_LANGUAGE:CXX>:-frtti>
    # Single-threaded std::mutex/condition_variable for libstdc++ without
    # gthreads (Ndless); inert on the host simulator. See the header.
    "$<$<COMPILE_LANGUAGE:CXX>:SHELL:-include ${CMAKE_SOURCE_DIR}/src/nspire/compat/NspireThreadsCompat.h>"
)

if(NSPIRE_HOST_SIM)
    # zlib from the bundled sources; the host may not have development headers.
    set(ZLIB_BUILD_EXAMPLES OFF CACHE BOOL "Build zlib examples" FORCE)
    add_subdirectory(external/zlib EXCLUDE_FROM_ALL)
    target_link_libraries(OptiCraft PRIVATE zlibstatic)
    target_include_directories(OptiCraft PRIVATE
        "${CMAKE_SOURCE_DIR}/external/zlib"
        "${CMAKE_BINARY_DIR}/external/zlib"
    )
    target_compile_options(OptiCraft PRIVATE $<$<CONFIG:Release>:-O2>)
    set_target_properties(OptiCraft PROPERTIES
        OUTPUT_NAME "opticraft-hostsim"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/bin/nspire-hostsim"
    )
else()
    # The SDK's zlib (ndless-sdk/lib/libz.a, headers in ndless-sdk/include).
    target_link_libraries(OptiCraft PRIVATE z m)
    target_compile_options(OptiCraft PRIVATE
        -marm
        $<$<CONFIG:Release>:-O2>
        $<$<CONFIG:MinSizeRel>:-Os>
        -ffunction-sections
        -fdata-sections
    )
    target_link_options(OptiCraft PRIVATE
        "-Wl,--gc-sections"
        # newlib's clock comes from the RTC in whole seconds under Ndless;
        # NspireSystem_device.cpp supplies a 32 kHz hardware-timer clock.
        "-Wl,--wrap=_gettimeofday"
        # Heap accounting (NspireLibcShims_device.cpp).
        "-Wl,--wrap=malloc,--wrap=free,--wrap=realloc,--wrap=calloc"
        "-Wl,-Map,${CMAKE_BINARY_DIR}/OptiCraft.map"
    )
    set(NSPIRE_BIN_DIR "${CMAKE_SOURCE_DIR}/bin/nspire")
    set_target_properties(OptiCraft PROPERTIES
        SUFFIX ".elf"
        OUTPUT_NAME "opticraft"
        RUNTIME_OUTPUT_DIRECTORY "${NSPIRE_BIN_DIR}"
    )

    # ELF -> Zehn -> .tns (the format Ndless' loader runs).
    find_program(NSPIRE_GENZEHN genzehn REQUIRED)
    find_program(NSPIRE_MAKE_PRG make-prg REQUIRED)
    add_custom_command(TARGET OptiCraft POST_BUILD
        COMMAND "${NSPIRE_GENZEHN}" --input "$<TARGET_FILE:OptiCraft>"
                --output "${NSPIRE_BIN_DIR}/opticraft.zehn"
                --name "OptiCraft Heritage" --author "OptiJuegos (Nspire port)"
                --notice "Minecraft Beta clone, nGL software renderer"
                --version 173 --uses-lcd-blit true --240x320-support true --compress
        COMMAND "${NSPIRE_MAKE_PRG}" "${NSPIRE_BIN_DIR}/opticraft.zehn" "${NSPIRE_BIN_DIR}/opticraft.tns"
        COMMAND ${CMAKE_COMMAND} -E remove "${NSPIRE_BIN_DIR}/opticraft.zehn"
        COMMENT "genzehn + make-prg: ${NSPIRE_BIN_DIR}/opticraft.tns"
        VERBATIM
    )
endif()
