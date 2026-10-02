// flash_if.h - hardware abstraction for flash access
#ifndef FLASH_IF_H
#define FLASH_IF_H

#include <stddef.h>
#include <stdint.h>

// Geometry of the STM32G474 in dual-bank mode
#define FLASH_PAGE_SIZE 2048u
#define FLASH_DW_SIZE 8u
#define FLASH_ERASED_BYTE 0xFFu
#define FLASH_ERASED_DW 0xFFFFFFFFFFFFFFFFull

typedef enum {
    FLASH_OK = 0,
    FLASH_ERR_ALIGN = -1,      // not double-word or page aligned
    FLASH_ERR_RANGE = -2,      // outside the managed region
    FLASH_ERR_NOT_ERASED = -3, // target double-word already programmed
    FLASH_ERR_ECC = -4,        // read hit an uncorrectable ECC error
    FLASH_ERR_IO = -5          // controller reported a failure
} flash_status_t;

typedef struct {
    flash_status_t (*erase_page)(void *ctx, uint32_t addr);
    flash_status_t (*program_dw)(void *ctx, uint32_t addr, uint64_t data);
    flash_status_t (*read)(void *ctx, uint32_t addr, void *dst, size_t len);
    void *ctx;
} flash_if_t;

static inline flash_status_t flash_erase_page(const flash_if_t *f, uint32_t addr) {
    return f->erase_page(f->ctx, addr);
}

static inline flash_status_t flash_program_dw(const flash_if_t *f, uint32_t addr,
                                              uint64_t data) {
    return f->program_dw(f->ctx, addr, data);
}

static inline flash_status_t flash_read(const flash_if_t *f, uint32_t addr, void *dst,
                                        size_t len) {
    return f->read(f->ctx, addr, dst, len);
}

#endif // FLASH_IF_H