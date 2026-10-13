# Предупреждения для своих целей. На целевом MinGW они ошибки по умолчанию;
# на Linux/macOS — ошибки по AFAR_WARNINGS_AS_ERRORS=ON (так собирает CI).
if(MINGW)
    set(_afar_werror_default ON)
else()
    set(_afar_werror_default OFF)
endif()
option(AFAR_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" ${_afar_werror_default})
unset(_afar_werror_default)

set(AFAR_WARNING_FLAGS "")
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    set(AFAR_WARNING_FLAGS -Wall -Wextra -Wpedantic)
    if(AFAR_WARNINGS_AS_ERRORS)
        list(APPEND AFAR_WARNING_FLAGS -Werror)
    endif()
endif()

function(afar_enable_warnings target)
    if(AFAR_WARNING_FLAGS)
        target_compile_options(${target} PRIVATE ${AFAR_WARNING_FLAGS})
    endif()
endfunction()
