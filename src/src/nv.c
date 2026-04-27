/* vaporware/src/nv.c — Write-forward internal flash NV storage
 *
 * Design: write-forward (also called "log-structured") within each 512-byte page.
 *
 *   Each key (0–7) owns one 512-byte flash page at NV_BASE + key * 512.
 *   The page is divided into 64 slots of 8 bytes each.
 *   nv_write() appends a new 8-byte record to the first blank slot —
 *   no erase on every write.  The page is only erased when all 64 slots are
 *   consumed, then the new value is written to slot 0 of the freshly erased page.
 *
 *   Flash endurance: N32G031 flash is rated for ~10,000 erase cycles per page.
 *   With 64 slots per page, that gives ~640,000 effective writes per key before
 *   wearing out the page.  For counters that increment every puff or spin, this
 *   is effectively unlimited for a consumer device lifetime.
 *
 *   Power-fail safety: the value word is written FIRST, the magic word SECOND.
 *   If power is lost between the two writes, the slot has a valid value but no
 *   magic header.  nv_read() only accepts slots where the magic is present, so
 *   a partially-written slot is silently skipped — no corruption.
 *
 * Flash unlock:
 *   Keys 0x45670123 / 0xCDEF89AB are the ARM Cortex standard flash unlock
 *   magic values (not device-specific); the same sequence works on all devices
 *   that use the ARM flash programming interface.
 *
 * NV_KEY constants and the public API are defined in nv.h.
 *
 * See nv.h for the full memory map and slot format.
 */
#include "nv.h"
#include "n32g031.h"
#include "system.h"

/* ── Flash layout ─────────────────────────────────────────────────── */
#define NV_BASE         0x0800F000UL  /* start of NV region */
#define NV_PAGE_SIZE    512U          /* N32G031 erase page size */
#define NV_SLOTS        64U           /* 8-byte slots per 512-byte page */
#define NV_MAGIC        0xA55A0000UL  /* upper 16 bits of slot word 0 */
#define NV_BLANK        0xFFFFFFFFUL

/* ── Flash register bits ──────────────────────────────────────────── */
#define FLASH_CR_PG     (1UL << 0)
#define FLASH_CR_PER    (1UL << 1)
#define FLASH_CR_STRT   (1UL << 6)
#define FLASH_CR_LOCK   (1UL << 7)
#define FLASH_SR_BSY    (1UL << 0)

/* ── Internal helpers ─────────────────────────────────────────────── */

static uint32_t page_addr(uint8_t key) {
    return NV_BASE + (uint32_t)key * NV_PAGE_SIZE;
}

static void flash_unlock(void) {
    if (FLASH_IF->CR & FLASH_CR_LOCK) {
        /* ARM Cortex standard flash unlock sequence.
         * Both keys must be written to KEYR consecutively with no other
         * KEYR writes between them.  Writing any other value re-locks CR. */
        FLASH_IF->KEYR = 0x45670123UL;  /* FLASH_KEY1 */
        FLASH_IF->KEYR = 0xCDEF89ABUL;  /* FLASH_KEY2 */
    }
}

static void flash_lock(void) {
    FLASH_IF->CR |= FLASH_CR_LOCK;
}

static void flash_wait(void) {
    while (FLASH_IF->SR & FLASH_SR_BSY) {
        IWDG_FEED();
    }
    FLASH_IF->SR = FLASH_IF->SR; /* clear EOP/error flags */
}

/* Write one 32-bit word to a blank (0xFFFFFFFF) flash location */
static void flash_write_word(uint32_t addr, uint32_t val) {
    FLASH_IF->CR |= FLASH_CR_PG;
    *(volatile uint32_t *)addr = val;
    flash_wait();
    FLASH_IF->CR &= ~FLASH_CR_PG;
}

/* Erase one 512-byte page */
static void flash_erase_page(uint32_t addr) {
    FLASH_IF->CR |= FLASH_CR_PER;
    FLASH_IF->AR  = addr;
    FLASH_IF->CR |= FLASH_CR_STRT;
    flash_wait();
    FLASH_IF->CR &= ~FLASH_CR_PER;
}

/* ── Public API ───────────────────────────────────────────────────── */

uint32_t nv_read(uint8_t key, uint32_t default_val) {
    if (key >= 8u) return default_val;
    uint32_t base = page_addr(key);

    /* Scan backward — last written slot wins */
    for (int i = (int)NV_SLOTS - 1; i >= 0; i--) {
        uint32_t w0 = *(volatile uint32_t *)(base + (uint32_t)i * 8u);
        uint32_t w1 = *(volatile uint32_t *)(base + (uint32_t)i * 8u + 4u);
        if ((w0 & 0xFFFF0000UL) == NV_MAGIC && (uint8_t)w0 == key) {
            (void)w1; /* silence warning — value is at w1 */
            return *(volatile uint32_t *)(base + (uint32_t)i * 8u + 4u);
        }
    }
    return default_val;
}

void nv_write(uint8_t key, uint32_t val) {
    if (key >= 8u) return;
    uint32_t base = page_addr(key);

    flash_unlock();

    /* Find first blank slot */
    int blank = -1;
    for (int i = 0; i < (int)NV_SLOTS; i++) {
        uint32_t w0 = *(volatile uint32_t *)(base + (uint32_t)i * 8u);
        uint32_t w1 = *(volatile uint32_t *)(base + (uint32_t)i * 8u + 4u);
        if (w0 == NV_BLANK && w1 == NV_BLANK) { blank = i; break; }
    }

    if (blank < 0) {
        /* All 64 slots are consumed — erase the entire page and start fresh.
         * The erase takes ~30 ms; IWDG is fed inside flash_wait() during BSY.
         * After erase, all 64 slots are blank again (0xFFFFFFFF) and the new
         * value is written to slot 0. */
        flash_erase_page(base);
        blank = 0;
    }

    /* Write the value word FIRST (power-fail safety):
     * If power is lost before the magic word is written, nv_read() sees a slot
     * with no valid magic and skips it — the previous value (or default) is
     * returned rather than garbage. */
    uint32_t slot = base + (uint32_t)blank * 8u;
    flash_write_word(slot + 4u, val);          /* value word written first */
    flash_write_word(slot,      NV_MAGIC | (uint32_t)key); /* magic+key second */

    flash_lock();
}

void nv_reset(uint8_t key) {
    if (key >= 8u) return;
    flash_unlock();
    flash_erase_page(page_addr(key));
    flash_lock();
}
