cmake_minimum_required(VERSION 3.20)
project(LA64M68
    VERSION 0.1.0
    DESCRIPTION "Hybrid M68k engine for Linux ARM64 with Amiga/Atari plugins"
    LANGUAGES C
)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# KISS: strict warnings, no hidden surprises.
add_compile_options(-Wall -Wextra -Wpedantic -Werror=implicit-function-declaration)

option(LA64M68_ENABLE_TESTS "Build LA64M68 unit tests" ON)
# Debug/trace capability is always compiled in and gated at runtime by
# LA64M68_MAX_DEBUG (0-POOL/RULES.md) -- there is deliberately no build
# switch that could compile the diagnostics out.

add_subdirectory(src)
add_subdirectory(plugins)

if(LA64M68_ENABLE_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
