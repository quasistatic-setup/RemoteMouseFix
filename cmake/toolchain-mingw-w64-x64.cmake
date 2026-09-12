# Cross-compile toolchain: WSL / Linux host -> native Windows x64 PE executable.
#
# This does NOT produce a Linux binary. The output is a genuine PE32+ (pei-x86-64)
# Windows executable. Use it when no MSVC installation is available on the Windows side.
# The canonical/preferred path stays plain MSVC + CMake on Windows (see README).
#
# Usage:
#   cmake -S . -B build-mingw -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-mingw-w64-x64.cmake
#   cmake --build build-mingw -j
#
# Override the compiler location with -DRMF_MINGW_PREFIX=/path/to/prefix/usr if your
# toolchain does not live on PATH (for example an unpacked, non-root install).

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(RMF_MINGW_TRIPLE "x86_64-w64-mingw32")

# Optional prefix for a relocated (non-root) mingw-w64 install.
if(NOT DEFINED RMF_MINGW_PREFIX AND DEFINED ENV{RMF_MINGW_PREFIX})
    set(RMF_MINGW_PREFIX "$ENV{RMF_MINGW_PREFIX}")
endif()

if(RMF_MINGW_PREFIX)
    set(_rmf_hint "${RMF_MINGW_PREFIX}/bin")
else()
    set(_rmf_hint "")
endif()

# CMake re-includes this file inside its compiler-probe sub-projects, which start with
# an empty cache. Without this the prefix would be lost there and the probe would fail.
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES RMF_MINGW_PREFIX)

find_program(CMAKE_C_COMPILER   NAMES ${RMF_MINGW_TRIPLE}-gcc ${RMF_MINGW_TRIPLE}-gcc-posix HINTS ${_rmf_hint})
find_program(CMAKE_CXX_COMPILER NAMES ${RMF_MINGW_TRIPLE}-g++ ${RMF_MINGW_TRIPLE}-g++-posix HINTS ${_rmf_hint})
find_program(CMAKE_RC_COMPILER  NAMES ${RMF_MINGW_TRIPLE}-windres HINTS ${_rmf_hint})
find_program(CMAKE_AR           NAMES ${RMF_MINGW_TRIPLE}-ar     HINTS ${_rmf_hint})
find_program(CMAKE_RANLIB       NAMES ${RMF_MINGW_TRIPLE}-ranlib HINTS ${_rmf_hint})

if(NOT CMAKE_CXX_COMPILER)
    message(FATAL_ERROR
        "${RMF_MINGW_TRIPLE}-g++ not found.\n"
        "Install it (Debian/Ubuntu: apt install g++-mingw-w64-x86-64) or point "
        "-DRMF_MINGW_PREFIX=<prefix>/usr at an existing install.")
endif()

if(RMF_MINGW_PREFIX)
    set(CMAKE_FIND_ROOT_PATH "${RMF_MINGW_PREFIX}/${RMF_MINGW_TRIPLE}")
else()
    set(CMAKE_FIND_ROOT_PATH "/usr/${RMF_MINGW_TRIPLE}")
endif()

# Look for headers/libraries in the target sysroot, but keep host programs usable.
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
