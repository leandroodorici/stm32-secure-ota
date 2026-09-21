cmake_minimum_required(VERSION 3.16)
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