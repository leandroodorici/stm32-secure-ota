cmake_minimum_required(VERSION 3.16)

# Two build modes from one tree, selected by whether a toolchain file was
# passed. Firmware targets need a different compiler, different flags and a
# linker script, and CMake fixes the compiler once per build tree, so they
# cannot share a configuration.
#
#   host:     cmake -S . -B build
#   firmware: cmake -S . -B build-fw -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi.cmake

if(CMAKE_TOOLCHAIN_FILE)
    include(${CMAKE_CURRENT_SOURCE_DIR}/firmware.cmake)
    return()
endif()

project(stm32_secure_ota_host C)

# This project builds ONLY the host-testable core. Firmware targets
# (bootloader/, app/, gateway/) cross-compile separately with their own
# toolchain files. Keeping them out of this tree lets CI run the full logic
# suite on a plain Linux runner with no embedded toolchain installed.

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

add_compile_options(
    -Wall -Wextra -Werror
    -Wconversion -Wsign-conversion
    -Wshadow -Wundef -Wpointer-arith
)

option(ENABLE_SANITIZERS "Build host tests with ASan/UBSan" ON)
if(ENABLE_SANITIZERS)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=address,undefined)
endif()

enable_testing()
add_subdirectory(tests/host)