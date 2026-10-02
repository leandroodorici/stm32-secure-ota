# Mbed TLS verify path, shared by the firmware and host builds
#
# Pinned 3.6 LTS submodule. Only the sources the verify path needs are built,
# against bootloader/boot_crypto_config.h, which replaces the library's default
# configuration. Both builds compile the same sources with the same
# configuration, so the host tests exercise the code that ships.
#
# Third-party code is built without the project's warning flags, and its
# headers are included as system headers so that code which includes them
# still builds with -Werror.
set(MBEDTLS_DIR ${CMAKE_SOURCE_DIR}/third_party/mbedtls)

if(NOT EXISTS ${MBEDTLS_DIR}/library/ecdsa.c)
    message(FATAL_ERROR "Mbed TLS is missing. Run: "
        "git submodule update --init third_party/mbedtls")
endif()

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
