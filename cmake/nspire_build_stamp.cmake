# Writes nspire_build_stamp.h (see OptiCraftBuildStamp in nspire.cmake).
# Usage: cmake -DOUT=<header> -DDEBUG_LOG=<ON|OFF> -P nspire_build_stamp.cmake
#
# Minute resolution, and the file is left alone when the text is unchanged, so
# repeated builds within a minute do not recompile its includers.

string(TIMESTAMP stamp "%Y-%m-%d %H:%M")
if(DEBUG_LOG)
    set(kind "debug log")
else()
    set(kind "release")
endif()
set(content "#pragma once\n#define NSPIRE_BUILD_STAMP \"${stamp} (${kind})\"\n")

set(old "")
if(EXISTS "${OUT}")
    file(READ "${OUT}" old)
endif()
if(NOT old STREQUAL content)
    file(WRITE "${OUT}" "${content}")
endif()
