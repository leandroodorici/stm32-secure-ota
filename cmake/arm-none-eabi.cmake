# Cross-compilation toolchain for the STM32G474 (Cortex-M4F)
#
# Used with -DCMAKE_TOOLCHAIN_FILE when configuring the firmware build tree.
# The host test build does not use this file at all.

set(CMAKE_SYSTEM_NAME Generic)      # Generic means bare metal, no OS
set(CMAKE_SYSTEM_PROCESSOR arm)

set(CMAKE_C_COMPILER arm-none-eabi-gcc)
set(CMAKE_ASM_COMPILER arm-none-eabi-gcc)
set(CMAKE_OBJCOPY arm-none-eabi-objcopy)
set(CMAKE_SIZE arm-none-eabi-size)

# CMake normally verifies a compiler by building and RUNNING a test program.
# There is no way to run a Cortex-M binary on the build machine, so tell it to
# settle for producing a static library.
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

# -mcpu    target core, so the compiler emits the right instruction set
# -mthumb  Cortex-M executes Thumb only
# The FPU flags matter for ABI compatibility: every object linked together must
# agree, or the linker refuses. hard-float means FPU registers carry arguments.
set(ARCH_FLAGS "-mcpu=cortex-m4 -mthumb -mfpu=fpv4-sp-d16 -mfloat-abi=hard")

# -ffunction-sections / -fdata-sections put each function and variable in its
# own section, so --gc-sections can drop anything unreferenced. On a bootloader
# with a size budget this matters.
# --specs=nosys.specs satisfies references to system calls (write, sbrk) that
# newlib pulls in, with stubs that do nothing. There is no OS underneath.
set(CMAKE_C_FLAGS_INIT "${ARCH_FLAGS} -ffunction-sections -fdata-sections")
set(CMAKE_EXE_LINKER_FLAGS_INIT "${ARCH_FLAGS} --specs=nosys.specs -Wl,--gc-sections")

# Look for programs on the host, but never search the host for headers or
# libraries: /usr/include holds x86 glibc headers that would silently poison a
# cross build.
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)