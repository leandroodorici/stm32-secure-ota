#include <string.h>

#include "flash_if.h"

// Real flash controller driver. Implements the same flash_if_t that the host
// simulator does, so common/boot_record.c runs unchanged on both.
//
// Register layout from RM0440, embedded flash memory chapter

#define FLASH_BASE 0x40022000u
#define FLASH_ACR (*(volatile uint32_t *)(FLASH_BASE + 0x00u))
#define FLASH_KEYR (*(volatile uint32_t *)(FLASH_BASE + 0x08u))
#define FLASH_SR (*(volatile uint32_t *)(FLASH_BASE + 0x10u))
#define FLASH_CR (*(volatile uint32_t *)(FLASH_BASE + 0x14u))
#define FLASH_ECCR (*(volatile uint32_t *)(FLASH_BASE + 0x18u))

#define FLASH_KEY1 0x45670123u
#define FLASH_KEY2 0xCDEF89ABu

#define SR_BSY (1u << 16)
#define SR_ERROR_MASK                                                                    \
    0x0000C3FAu // OPERR, PROGERR, WRPERR, PGAERR, SIZERR,
                // PGSERR, MISERR, FASTERR, RDERR, OPTVERR
#define SR_PROGERR (1u << 3)

#define CR_PG (1u << 0)
#define CR_PER (1u << 1)
#define CR_PNB_SHIFT 3u
#define CR_PNB_MASK (0x7Fu << CR_PNB_SHIFT)
#define CR_BKER (1u << 11)
#define CR_STRT (1u << 16)
#define CR_LOCK (1u << 31)

#define ECCR_ECCD (1u << 31)

#define ACR_ICEN (1u << 9)
#define ACR_DCEN (1u << 10)
#define ACR_ICRST (1u << 11)
#define ACR_DCRST (1u << 12)

#define FLASH_ORIGIN 0x08000000u
#define BANK2_ORIGIN 0x08040000u
#define FLASH_TOTAL (512u * 1024u)

// Set by the NMI handler when an uncorrectable ECC error is detected. A torn
// double-word raises NMI on read rather than returning partial data, so the
// only way to notice is to clear this, perform the read, and check it after.
static volatile int g_ecc_fault;

// Overrides the weak Default_Handler in startup.c
void NMI_Handler(void) {
    if (FLASH_ECCR & ECCR_ECCD) {
        g_ecc_fault = 1;
        FLASH_ECCR = ECCR_ECCD; // write 1 to clear
    }
}

static flash_status_t wait_idle(void) {
    while (FLASH_SR & SR_BSY) {
    }
    uint32_t sr = FLASH_SR;
    if (sr & SR_ERROR_MASK) {
        FLASH_SR = sr & SR_ERROR_MASK; // write 1 to clear
        // PROGERR means a write targeted a double-word that was not erased.
        // That is the constraint the append-only boot record design exists
        // for, so it gets its own status rather than a generic failure.
        return (sr & SR_PROGERR) ? FLASH_ERR_NOT_ERASED : FLASH_ERR_IO;
    }
    return FLASH_OK;
}

static void unlock(void) {
    if (FLASH_CR & CR_LOCK) {
        // Two keys in this order. A wrong value locks the register until the
        // next reset, so this sequence is not retried on failure.
        FLASH_KEYR = FLASH_KEY1;
        FLASH_KEYR = FLASH_KEY2;
    }
}

static void lock(void) {
    FLASH_CR |= CR_LOCK;
}

// The data cache can be corrupted when it is filled from one bank while the
// other is being programmed or erased, so it stays off for the whole operation.
// Seen on target as constants loaded from bank 1 turning into wild addresses while
// the update agent programmed bank 2. Returns the ACR value to restore.
static uint32_t dcache_off(void) {
    uint32_t acr = FLASH_ACR;
    FLASH_ACR = acr & ~ACR_DCEN;
    return acr;
}

// Resets the data cache, which may hold corrupted lines, then restores acr.
// RM0440 only allows a cache to be reset while it is disabled.
static void dcache_restore(uint32_t acr) {
    FLASH_ACR = (acr & ~ACR_DCEN) | ACR_DCRST;
    FLASH_ACR = acr & ~ACR_DCRST;
}

// The instruction and data caches are enabled at reset and an erase does not
// update them, so a read after erasing can return the page's old contents.
// Resets both, then restores acr.
static void flush_caches(uint32_t acr) {
    FLASH_ACR = acr & ~(ACR_ICEN | ACR_DCEN);
    FLASH_ACR = (acr & ~(ACR_ICEN | ACR_DCEN)) | ACR_ICRST | ACR_DCRST;
    FLASH_ACR = acr & ~(ACR_ICRST | ACR_DCRST);
}

static int in_range(uint32_t addr, uint32_t len) {
    if (addr < FLASH_ORIGIN) {
        return 0;
    }
    return (addr - FLASH_ORIGIN) + len <= FLASH_TOTAL;
}

static flash_status_t stm32_erase_page(void *ctx, uint32_t addr) {
    (void)ctx;
    if (addr % FLASH_PAGE_SIZE != 0u) {
        return FLASH_ERR_ALIGN;
    }
    if (!in_range(addr, FLASH_PAGE_SIZE)) {
        return FLASH_ERR_RANGE;
    }

    // In dual-bank mode the page number is relative to its own bank, and
    // BKER selects which bank. Page 0 of bank 2 is not page 128.
    uint32_t bank_off =
        (addr >= BANK2_ORIGIN) ? (addr - BANK2_ORIGIN) : (addr - FLASH_ORIGIN);
    uint32_t page = bank_off / FLASH_PAGE_SIZE;

    flash_status_t st = wait_idle();
    if (st != FLASH_OK) {
        return st;
    }
    unlock();

    uint32_t cr = FLASH_CR & ~(CR_PNB_MASK | CR_BKER);
    cr |= CR_PER | ((page << CR_PNB_SHIFT) & CR_PNB_MASK);
    if (addr >= BANK2_ORIGIN) {
        cr |= CR_BKER;
    }
    FLASH_CR = cr;
    uint32_t acr = dcache_off();
    FLASH_CR = cr | CR_STRT;

    st = wait_idle();
    FLASH_CR &= ~CR_PER;
    flush_caches(acr);
    lock();
    return st;
}

static flash_status_t stm32_program_dw(void *ctx, uint32_t addr, uint64_t data) {
    (void)ctx;
    if (addr % FLASH_DW_SIZE != 0u) {
        return FLASH_ERR_ALIGN;
    }
    if (!in_range(addr, FLASH_DW_SIZE)) {
        return FLASH_ERR_RANGE;
    }

    flash_status_t st = wait_idle();
    if (st != FLASH_OK) {
        return st;
    }
    unlock();
    uint32_t acr = dcache_off();
    FLASH_CR |= CR_PG;

    // A double-word program is two 32-bit writes to consecutive addresses.
    // The controller latches the first and starts the operation on the second,
    // which is why this cannot be a single 64-bit store.
    volatile uint32_t *p = (volatile uint32_t *)addr;
    p[0] = (uint32_t)(data & 0xFFFFFFFFu);
    __asm volatile("dsb" ::: "memory");
    p[1] = (uint32_t)(data >> 32);

    st = wait_idle();
    FLASH_CR &= ~CR_PG;
    dcache_restore(acr);
    lock();
    return st;
}

static flash_status_t stm32_read(void *ctx, uint32_t addr, void *dst, size_t len) {
    (void)ctx;
    if (!in_range(addr, (uint32_t)len)) {
        return FLASH_ERR_RANGE;
    }

    // Flash is memory mapped, so the read itself is a memcpy. The ECC check
    // wraps it: a double-word damaged by an interrupted program raises NMI
    // during the copy, and the handler sets the flag.
    //
    // UNVERIFIED: this behaviour is taken from RM0440 and has not been observed
    g_ecc_fault = 0;
    __asm volatile("dsb" ::: "memory");

    memcpy(dst, (const void *)addr, len);

    __asm volatile("dsb" ::: "memory");
    if (g_ecc_fault) {
        return FLASH_ERR_ECC;
    }
    return FLASH_OK;
}

void flash_stm32_bind(flash_if_t *out) {
    out->erase_page = stm32_erase_page;
    out->program_dw = stm32_program_dw;
    out->read = stm32_read;
    out->ctx = 0;
}