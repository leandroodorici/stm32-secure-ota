cmake_minimum_required(VERSION 3.16)
project(stm32_secure_ota_fw C ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

add_compile_options(
    -Wall -Wextra -Werror
    -Wconversion -Wsign-conversion
    -Wshadow -Wundef -Wpointer-arith
    -Os                # size matters: the bootloader region cannot grow later
    -g3                # debug info costs nothing in flash, it is not loaded
    -fno-common
)

# Builds one firmware image: ELF for debugging, plus a raw binary for the
# signing tool, and prints the size so the bootloader budget stays visible.
function(add_firmware name linker_script)
    add_executable(${name} ${ARGN})
    target_link_options(${name} PRIVATE
        -T${CMAKE_SOURCE_DIR}/${linker_script}
        -Wl,-Map=${name}.map,--cref      # map file: what ended up where, and why
    )
    # CMake does not track the linker script as a dependency on its own, so a
    # changed memory map would silently produce a stale binary.
    set_target_properties(${name} PROPERTIES
        SUFFIX ".elf"
        LINK_DEPENDS ${CMAKE_SOURCE_DIR}/${linker_script}
    )

    add_custom_command(TARGET ${name} POST_BUILD
        COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:${name}> ${name}.bin
        COMMAND ${CMAKE_SIZE} $<TARGET_FILE:${name}>
        COMMENT "Building ${name}.bin"
    )
endfunction()

add_firmware(bootloader bootloader/bootloader.ld
    bootloader/startup.c
    bootloader/main.c
    bootloader/flash_stm32.c
    common/boot_record.c
)

target_include_directories(bootloader PRIVATE
    ${CMAKE_SOURCE_DIR}/common/include
    ${CMAKE_SOURCE_DIR}/bootloader
)

add_firmware(app_slot_a app/app_slot_a.ld
    app/startup.c
    app/main.c
)

target_include_directories(app_slot_a PRIVATE
    ${CMAKE_SOURCE_DIR}/common/include
)

# Same sources as slot A, linked for bank 2. The two images are identical
# apart from their addresses, per the A/B design.
add_firmware(app_slot_b app/app_slot_b.ld
    app/startup.c
    app/main.c
)

target_include_directories(app_slot_b PRIVATE
    ${CMAKE_SOURCE_DIR}/common/include
)

# ---- Mbed TLS verify path ----
#
# Pinned 3.6 LTS submodule. Only the sources the verify path needs are built,
# against bootloader/boot_crypto_config.h, which replaces the library's default
# configuration. Third-party code is built without the project's warning
# flags, and its headers are included as system headers so that code which
# includes them still builds with -Werror.
set(MBEDTLS_DIR ${CMAKE_SOURCE_DIR}/third_party/mbedtls)

add_library(boot_crypto STATIC
    ${MBEDTLS_DIR}/library/asn1parse.c
    ${MBEDTLS_DIR}/library/asn1write.c
    ${MBEDTLS_DIR}/library/bignum.c
    ${MBEDTLS_DIR}/library/bignum_core.c
    ${MBEDTLS_DIR}/library/constant_time.c
    ${MBEDTLS_DIR}/library/ecdsa.c
    ${MBEDTLS_DIR}/library/ecp.c
    ${MBEDTLS_DIR}/library/ecp_curves.c
    ${MBEDTLS_DIR}/library/memory_buffer_alloc.c
    ${MBEDTLS_DIR}/library/platform.c
    ${MBEDTLS_DIR}/library/platform_util.c
    ${MBEDTLS_DIR}/library/sha256.c
)
target_include_directories(boot_crypto SYSTEM PUBLIC
    ${MBEDTLS_DIR}/include
    ${CMAKE_SOURCE_DIR}/bootloader
)
target_compile_definitions(boot_crypto PUBLIC
    MBEDTLS_CONFIG_FILE="boot_crypto_config.h"
)
target_compile_options(boot_crypto PRIVATE -w)

# Runs from 0x08000000 in place of the bootloader
add_firmware(crypto_bench bootloader/bootloader.ld
    bootloader/startup.c
    bench/crypto_bench.c
)
target_link_libraries(crypto_bench PRIVATE boot_crypto)
