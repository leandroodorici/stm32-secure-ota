#include "sim_flash.h"

#include <string.h>

static bool in_range(uint32_t addr, uint32_t len)
{
    if (addr < SIM_BASE) {
        return false;
    }
    uint32_t off = addr - SIM_BASE;
    return (off + len) <= SIM_SIZE;
}

static uint32_t dw_index(uint32_t addr)
{
    return (addr - SIM_BASE) / FLASH_DW_SIZE;
}

void sim_flash_init(sim_flash_t *sf)
{
    memset(sf->mem, FLASH_ERASED_BYTE, sizeof(sf->mem));
    memset(sf->ecc_bad, 0, sizeof(sf->ecc_bad));
    sf->ops = 0;
    sf->cut_after = -1;
    sf->tear_on_cut = false;
    sf->powered = true;
    sf->erases = 0;
    sf->programs = 0;
}

void sim_flash_reboot(sim_flash_t *sf)
{
    sf->ops = 0;
    sf->cut_after = -1;
    sf->tear_on_cut = false;
    sf->powered = true;
    // mem and ecc_bad are deliberately untouched: power loss does not clear
    // flash, and a model that forgot this would make every recovery test pass
    // vacuously.
}

void sim_flash_arm_cut(sim_flash_t *sf, long n, bool tear)
{
    sf->ops = 0;
    sf->cut_after = n;
    sf->tear_on_cut = tear;
    sf->powered = true;
}

// Decides whether this operation runs, and whether it runs only partway.
//
// Returns false when the device is already unpowered, in which case the caller
// must not touch memory at all. Sets *partial for the single operation that is
// interrupted mid-flight.
//
// The >= rather than == matters: once the cut happens every later operation is
// dead too. Without that, a test could "recover" by performing writes a
// powerless device could never have performed.
static bool gate(sim_flash_t *sf, bool *partial)
{
    *partial = false;
    if (!sf->powered) {
        return false;
    }
    sf->ops++;
    if (sf->cut_after >= 0 && sf->ops >= sf->cut_after) {
        sf->powered = false;
        if (sf->ops == sf->cut_after && sf->tear_on_cut) {
            *partial = true;
            return true; // proceed, but only halfway
        }
        return false; // clean cut: nothing happens
    }
    return true;
}

static flash_status_t sim_erase(void *ctx, uint32_t addr)
{
    sim_flash_t *sf = (sim_flash_t *)ctx;
    if (addr % FLASH_PAGE_SIZE != 0) {
        return FLASH_ERR_ALIGN;
    }
    if (!in_range(addr, FLASH_PAGE_SIZE)) {
        return FLASH_ERR_RANGE;
    }

    bool partial;
    if (!gate(sf, &partial)) {
        return FLASH_ERR_IO;
    }
    sf->erases++;

    // A partially completed erase leaves some of the page erased and the rest
    // untouched. Real erase does not progress linearly through a page, but
    // "partly erased, partly not" is the property the boot logic has to
    // survive, and modelling it as a prefix is enough to test that.
    uint32_t n = partial ? (FLASH_PAGE_SIZE / 2) : FLASH_PAGE_SIZE;
    memset(sf->mem + (addr - SIM_BASE), FLASH_ERASED_BYTE, n);
    for (uint32_t i = 0; i < n / FLASH_DW_SIZE; i++) {
        sf->ecc_bad[dw_index(addr) + i] = false;
    }
    return FLASH_OK;
}

static flash_status_t sim_program(void *ctx, uint32_t addr, uint64_t data)
{
    sim_flash_t *sf = (sim_flash_t *)ctx;
    if (addr % FLASH_DW_SIZE != 0) {
        return FLASH_ERR_ALIGN;
    }
    if (!in_range(addr, FLASH_DW_SIZE)) {
        return FLASH_ERR_RANGE;
    }

    uint32_t idx = dw_index(addr);
    uint32_t off = addr - SIM_BASE;

    // Checked before the power gate. This is a static property of the
    // controller, not a timing effect: the G4 refuses to reprogram a
    // double-word whose ECC syndrome is already written. Putting it first means
    // any code path that tries to rewrite fails even mid-cut, so the
    // append-only design is enforced rather than merely intended.
    uint64_t existing;
    memcpy(&existing, sf->mem + off, FLASH_DW_SIZE);
    if (existing != FLASH_ERASED_DW || sf->ecc_bad[idx]) {
        return FLASH_ERR_NOT_ERASED;
    }

    bool partial;
    if (!gate(sf, &partial)) {
        return FLASH_ERR_IO;
    }
    sf->programs++;

    if (partial) {
        // Torn write. Some bits landed and the stored ECC syndrome no longer
        // matches. The low half is written so a naive reader would see bytes
        // that look plausible, which is exactly the trap the ECC flag exists
        // to catch.
        memcpy(sf->mem + off, &data, FLASH_DW_SIZE / 2);
        sf->ecc_bad[idx] = true;
        return FLASH_OK;
    }

    memcpy(sf->mem + off, &data, FLASH_DW_SIZE);
    return FLASH_OK;
}

static flash_status_t sim_read(void *ctx, uint32_t addr, void *dst, size_t len)
{
    sim_flash_t *sf = (sim_flash_t *)ctx;
    if (!in_range(addr, (uint32_t)len)) {
        return FLASH_ERR_RANGE;
    }

    // Reads are not gated by the power model. On hardware a read only happens
    // while the core is running, so "read after power loss" is not a case that
    // exists. Gating them would model a situation the device never sees.
    uint32_t first = dw_index(addr);
    uint32_t last = dw_index(addr + (uint32_t)len - 1);
    for (uint32_t i = first; i <= last; i++) {
        if (sf->ecc_bad[i]) {
            return FLASH_ERR_ECC;
        }
    }

    memcpy(dst, sf->mem + (addr - SIM_BASE), len);
    return FLASH_OK;
}

void sim_flash_bind(sim_flash_t *sf, flash_if_t *out)
{
    out->erase_page = sim_erase;
    out->program_dw = sim_program;
    out->read = sim_read;
    out->ctx = sf;
}