# Cross-compilation toolchain: Linux host -> Windows x86_64 (mingw-w64)
#
# Usage:
#   cmake -B build-win -G Ninja \
#         -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-mingw64.cmake \
#         -DCMAKE_PREFIX_PATH=$(pwd)/build-win/deps-prefix \
#         -DCMAKE_BUILD_TYPE=Release
#   cmake --build build-win

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(MINGW_TRIPLE x86_64-w64-mingw32)

# Prefer the posix-thread-model variant (needed for full std::thread/
# std::mutex/std::condition_variable support) over the default win32 variant.
find_program(CMAKE_C_COMPILER   NAMES ${MINGW_TRIPLE}-gcc-posix   ${MINGW_TRIPLE}-gcc)
find_program(CMAKE_CXX_COMPILER NAMES ${MINGW_TRIPLE}-g++-posix   ${MINGW_TRIPLE}-g++)
find_program(CMAKE_RC_COMPILER  NAMES ${MINGW_TRIPLE}-windres)

# /usr/<triple> = system mingw sysroot (headers/libs shipped by the distro package).
# deps-prefix   = where we install our own cross-built deps (fftw3f, glfw3, libiio, libad9361).
set(CMAKE_FIND_ROOT_PATH
    /usr/${MINGW_TRIPLE}
    ${CMAKE_CURRENT_LIST_DIR}/../build-win/deps-prefix
)

# Search headers/libs only in the target sysroot / prefix; keep programs on the host.
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# Static-link the mingw/gcc runtime so the resulting .exe doesn't need
# libgcc_s/libstdc++/winpthread DLLs alongside it.
set(CMAKE_EXE_LINKER_FLAGS_INIT "-static-libgcc -static-libstdc++ -static -lwinpthread")

# The mingw-w64 sysroot only ships lowercase "windows.h" — fine on Windows'
# case-insensitive filesystem, but this is a case-sensitive Linux host, so
# any vendored source that writes `#include <Windows.h>` (libiio does) fails
# to resolve. Shim it with a symlink and put it first on the include path.
set(CMAKE_C_FLAGS_INIT   "-I${CMAKE_CURRENT_LIST_DIR}/../build-win/case-shim")
set(CMAKE_CXX_FLAGS_INIT "-I${CMAKE_CURRENT_LIST_DIR}/../build-win/case-shim")
