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