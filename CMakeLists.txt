cmake_minimum_required(VERSION 3.22)

project(AfarRxCalibrationStudio VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Целевой стенд: Qt 6 и MinGW/UCRT64.
# Префикс Qt и прочие CMAKE_PREFIX_PATH задаются снаружи, например:
#   cmake -S . -B build -DCMAKE_PREFIX_PATH=C:/Qt/6.8.3/mingw_64
# Здесь CMAKE_PREFIX_PATH не фиксируется и не обязателен.

if(MINGW)
    add_compile_options(-Wall -Wextra -Wpedantic -Werror)
    # GCC 13 MinGW: -O3 иногда даёт ICE, оптимизацию выше -O2 не поднимаем.
    foreach(_flags_var IN ITEMS
            CMAKE_CXX_FLAGS_RELEASE
            CMAKE_CXX_FLAGS_RELWITHDEBINFO
            CMAKE_C_FLAGS_RELEASE
            CMAKE_C_FLAGS_RELWITHDEBINFO)
        if(DEFINED ${_flags_var})
            string(REPLACE "-O3" "-O2" ${_flags_var} "${${_flags_var}}")
        endif()
    endforeach()
    unset(_flags_var)
endif()

enable_testing()

find_package(OpenGL REQUIRED)
add_subdirectory(modules/spectrum)
add_subdirectory(src)
add_subdirectory(tests)

# Архив поставки: cpack --config build/CPackConfig.cmake
# (Windows — ZIP, macOS — DMG, Linux — TGZ). Боевая Windows-поставка с SBOM —
# tools/package-windows.ps1.
set(CPACK_PACKAGE_NAME "AfarRxCalibrationStudio")
set(CPACK_PACKAGE_VENDOR "AFAR")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "AFAR RX Calibration Studio")
if(WIN32)
    set(CPACK_GENERATOR "ZIP")
elseif(APPLE)
    set(CPACK_GENERATOR "DragNDrop")
else()
    set(CPACK_GENERATOR "TGZ")
endif()
include(CPack)
