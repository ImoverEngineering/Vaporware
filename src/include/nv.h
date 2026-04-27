/* vaporware/include/nv.h — Non-volatile storage (internal flash)
 *
 * Simple key/value store backed by the top 4 KB of internal flash.
 *
 * DESIGN — write-forward per page:
 *   Each key (0–7) owns one dedicated 512-byte flash page.
 *   The page is divided into 64 slots of 8 bytes each.
 *   nv_write() finds the first blank slot (0xFFFFFFFF / 0xFFFFFFFF) and
 *   writes the value there — no erase required for the first 64 writes.
 *   When all 64 slots are consumed, the page is erased automatically and
 *   writing resumes from slot 0.  This gives ~640,000 effective writes per
 *   key before flash endurance (~10,000 erases × 64 slots) is exhausted.
 *
 * POWER-FAIL SAFETY:
 *   The value word is written FIRST, the magic header SECOND.
 *   A partially-written slot (value without magic) is ignored by nv_read().
 *   Only complete slots (magic present) are returned.
 *
 * SLOT FORMAT (8 bytes per slot, 64 slots per 512-byte page):
 *   Offset +0 (word 0): 0xA55A0000 | key  — magic+key (written LAST)
 *   Offset +4 (word 1): uint32_t value     — payload   (written FIRST)
 *   Blank slot:         0xFFFFFFFF / 0xFFFFFFFF
 *
 * FLASH LIFETIME NOTE:
 *   N32G031 flash is rated ~10,000 erase cycles per page.
 *   With 64 slots per page: ~640,000 writes per key before wear-out.
 *   A counter that increments every puff at 1000 puffs/day would take
 *   ~640 days to wear out the page — effectively unlimited for this device.
 *
 * FLASH LAYOUT (last 4 KB of 64 KB flash, reserved in linker script):
 *   0x0800F000  key 0 — NV_KEY_PUFF_COUNT   (tamagotchi puff counter)
 *   0x0800F200  key 1 — NV_KEY_VAPE_TIMER   (tamagotchi total vape time)
 *   0x0800F400  key 2 — NV_KEY_HIGH_SCORE   (FlappyVape best score)
 *   0x0800F600  key 3 — NV_KEY_SPINS        (slot machine total spins)
 *   0x0800F800  key 4 — NV_KEY_WINS         (slot machine total wins)
 *   0x0800FA00  key 5 — NV_KEY_APP_0        (free for app-specific use)
 *   0x0800FC00  key 6 — NV_KEY_APP_1
 *   0x0800FE00  key 7 — NV_KEY_APP_2
 *
 * WARNING: Application code must stay under ~60 KB to avoid overwriting NV pages.
 *   The linker script (n32g031.ld) caps code at 60 KB.  Current apps are <10 KB.
 *
 * VALID KEY RANGE: 0–7 (8 keys total). Keys >= 8 are silently ignored.
 */
#ifndef NV_H
#define NV_H

#include <stdint.h>

/* Pre-defined keys — always use these names, not raw numeric literals.
 * Key numbers correspond directly to page addresses (key N → 0x0800F000 + N*512). */
#define NV_KEY_PUFF_COUNT   0u   /* tamagotchi: lifetime puff counter (uint32_t count) */
#define NV_KEY_VAPE_TIMER   1u   /* tamagotchi: total vape time in 0.01 s units        */
#define NV_KEY_HIGH_SCORE   2u   /* FlappyVape: best score achieved (uint32_t)         */
#define NV_KEY_SPINS        3u   /* slot machine: lifetime spin count (uint32_t)       */
#define NV_KEY_WINS         4u   /* slot machine: lifetime win count (uint32_t)        */
#define NV_KEY_APP_0        5u   /* free for app-specific persistent data              */
#define NV_KEY_APP_1        6u
#define NV_KEY_APP_2        7u

/* Read the stored value for key.
 *   key         — NV_KEY_* constant (0–7)
 *   default_val — value to return if no write has ever occurred for this key
 * Returns: stored uint32_t value, or default_val if the page is blank.
 * Read-only; never modifies flash. */
uint32_t nv_read(uint8_t key, uint32_t default_val);

/* Write val for key.
 *   key — NV_KEY_* constant (0–7); silently ignored if >= 8
 *   val — uint32_t value to store
 * Side effects:
 *   - Unlocks flash, programs one 8-byte slot, re-locks flash.
 *   - If the page is full (all 64 slots used), erases the page first (~30 ms).
 *     Page erase feeds IWDG via flash_wait() — safe for the watchdog.
 *   - Does NOT save previous values on page erase (last write wins). */
void nv_write(uint8_t key, uint32_t val);

/* Erase all stored values for key (reset page to factory-blank state).
 * key — NV_KEY_* constant (0–7); silently ignored if >= 8.
 * After nv_reset(), nv_read() will return default_val until the next write. */
void nv_reset(uint8_t key);

#endif /* NV_H */
