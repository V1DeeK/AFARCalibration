# Предупреждения как ошибки для целевого MinGW (как в корневом CMakeLists.txt).
function(afar_enable_warnings target)
    if(MINGW)
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Werror)
    endif()
endfunction()
