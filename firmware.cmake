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
    # __FILE__ in assertions records paths relative to the repository, so the
    # firmware, and the hash it is signed over, does not depend on where the
    # tree was checked out
    -fmacro-prefix-map=${CMAKE_SOURCE_DIR}/=
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
    drivers/flash_stm32.c
    common/boot_record.c
)

target_include_directories(bootloader PRIVATE
    ${CMAKE_SOURCE_DIR}/common/include
    ${CMAKE_SOURCE_DIR}/drivers
    ${CMAKE_SOURCE_DIR}/bootloader
)

# ---- FreeRTOS kernel ----
#
# Only the application uses it. The bootloader has no RTOS.
# Built from the core sources and the GCC Cortex-M4F port against
# app/FreeRTOSConfig.h. Like Mbed TLS, it is third-party code: built without
# the project's warning flags, its headers included as system headers.
set(FREERTOS_DIR ${CMAKE_SOURCE_DIR}/third_party/freertos)

if(NOT EXISTS ${FREERTOS_DIR}/tasks.c)
    message(FATAL_ERROR "FreeRTOS is missing. Run: "
        "git submodule update --init third_party/freertos")
endif()

add_library(freertos_kernel STATIC
    ${FREERTOS_DIR}/list.c
    ${FREERTOS_DIR}/queue.c
    ${FREERTOS_DIR}/stream_buffer.c
    ${FREERTOS_DIR}/tasks.c
    ${FREERTOS_DIR}/portable/GCC/ARM_CM4F/port.c
)
target_include_directories(freertos_kernel SYSTEM PUBLIC
    ${FREERTOS_DIR}/include
    ${FREERTOS_DIR}/portable/GCC/ARM_CM4F
    ${CMAKE_SOURCE_DIR}/app
)
target_compile_options(freertos_kernel PRIVATE -w)

# ---- Application ----
#
# Linked once per slot. Both images come from the same sources
# and differ only in their linker script.
set(APP_SOURCES
    app/startup.c
    app/main.c
    app/supervisor.c
    app/boot_info.c
    app/board.c
    app/status.c
    app/fault.c
    app/comms.c
    drivers/flash_stm32.c
    drivers/lpuart1.c
    common/boot_record.c
    common/frame.c
    common/protocol.c
    common/image_verify.c
    common/progress.c
    common/update_agent.c
    $<TARGET_OBJECTS:boot_pubkey>
)

foreach(slot a b)
    add_firmware(app_slot_${slot} app/app_slot_${slot}.ld ${APP_SOURCES})
    target_include_directories(app_slot_${slot} PRIVATE
        ${CMAKE_SOURCE_DIR}/common/include
         ${CMAKE_SOURCE_DIR}/drivers
        ${CMAKE_SOURCE_DIR}/app
    )
    target_link_libraries(app_slot_${slot} PRIVATE freertos_kernel boot_crypto)
endforeach()

include(${CMAKE_SOURCE_DIR}/cmake/boot_crypto.cmake)

# Runs from 0x08000000 in place of the bootloader
add_firmware(crypto_bench bootloader/bootloader.ld
    bootloader/startup.c
    bench/crypto_bench.c
)
target_link_libraries(crypto_bench PRIVATE boot_crypto)

# ---- Image signing ----
#
# Each application binary is wrapped by tools/imgtool.py into a signed image:
# header, payload, trailer. The image, not the raw binary, is what gets flashed 
# at the slot base. Signing needs the private key, which exists only on the 
# development machine. Without it the build still succeeds and simply produces no images.
set(SIGNING_KEY ${CMAKE_SOURCE_DIR}/keys/dev_private.pem CACHE FILEPATH
    "Private key used to sign application images")

include(${CMAKE_SOURCE_DIR}/cmake/imgtool.cmake)

# Runs after the .bin is produced: POST_BUILD commands on one target run in
# the order they were added, and add_firmware adds the objcopy step first.
function(add_signed_image target load_addr)
    if(NOT EXISTS ${SIGNING_KEY})
        message(STATUS "${target}: no key at ${SIGNING_KEY}, image not signed")
        return()
    endif()
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${IMGTOOL_PYTHON} ${IMGTOOL} sign
            --key ${SIGNING_KEY}
            --in ${target}.bin
            --out ${target}.img
            --load-addr ${load_addr}
        COMMENT "Signing ${target}.img"
    )
endfunction()

add_signed_image(app_slot_a 0x08010000)
add_signed_image(app_slot_b 0x08040000)

# ---- Bootloader verification ----
#
# The bootloader trusts exactly one key. Its public half is converted to C 
# from the .pem at build time, so the key file stays the single source of 
# truth and nothing generated is committed. A bootloader without a key could 
# never boot anything, so its absence stops the build.
set(BOOT_PUBKEY ${CMAKE_SOURCE_DIR}/keys/dev_public.pem CACHE FILEPATH
    "Public key the bootloader verifies images against")
if(NOT EXISTS ${BOOT_PUBKEY})
    message(FATAL_ERROR "No public key at ${BOOT_PUBKEY}. Generate a key pair "
        "with: .venv/bin/python tools/imgtool.py keygen")
endif()

add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/boot_pubkey.c
    COMMAND ${IMGTOOL_PYTHON} ${IMGTOOL} pubkey-c
        --key ${BOOT_PUBKEY} --out ${CMAKE_CURRENT_BINARY_DIR}/boot_pubkey.c
    DEPENDS ${BOOT_PUBKEY} ${IMGTOOL}
    COMMENT "Embedding public key from ${BOOT_PUBKEY}"
)

# A library rather than a source of each firmware, so the command that
# generates it belongs to one target and a parallel build runs it once. The
# application checks received images against the same key.
add_library(boot_pubkey OBJECT ${CMAKE_CURRENT_BINARY_DIR}/boot_pubkey.c)

target_sources(bootloader PRIVATE
    common/image_verify.c
    $<TARGET_OBJECTS:boot_pubkey>
)
target_link_libraries(bootloader PRIVATE boot_crypto)
