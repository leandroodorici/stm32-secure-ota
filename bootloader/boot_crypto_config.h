// Mbed TLS configuration for the bootloader's verify-only path
//
// This replaces Mbed TLS's default configuration entirely: only what signature
// verification needs is enabled, since everything in the bootloader is permanent.
#ifndef BOOT_CRYPTO_CONFIG_H
#define BOOT_CRYPTO_CONFIG_H

// SHA-256 over header and payload. The smaller variant rolls up the
// compression loop: less flash, somewhat slower.
#define MBEDTLS_SHA256_C
#define MBEDTLS_SHA256_SMALLER

// ECDSA P-256 verification: bignum arithmetic, the curve, and ECDSA itself.
// ASN.1 is a build prerequisite of ECDSA. The DER functions it provides are
// never called, because signatures are stored as raw r||s.
#define MBEDTLS_BIGNUM_C
#define MBEDTLS_ECP_C
#define MBEDTLS_ECP_DP_SECP256R1_ENABLED
#define MBEDTLS_ECP_NIST_OPTIM
#define MBEDTLS_ECDSA_C
#define MBEDTLS_ASN1_PARSE_C
#define MBEDTLS_ASN1_WRITE_C

// Hand-written multiply-accumulate for Cortex-M bignum arithmetic
#define MBEDTLS_HAVE_ASM

// The bootloader has no heap. Mbed TLS allocates bignums from a static
// buffer instead, so its memory use is fixed and visible in .bss.
#define MBEDTLS_PLATFORM_C
#define MBEDTLS_PLATFORM_MEMORY
#define MBEDTLS_MEMORY_BUFFER_ALLOC_C

// Without this, the library's default allocator points at libc calloc, which
// drags in malloc and an sbrk the bootloader has no heap for.
#define MBEDTLS_PLATFORM_NO_STD_FUNCTIONS

// Verification needs no randomness, and there is no entropy source
#define MBEDTLS_NO_PLATFORM_ENTROPY

#endif // BOOT_CRYPTO_CONFIG_H