/* slots.h — public API for the Vaporware slot machine
 *
 * Call pattern:
 *   slots_init();                  // once at startup (inside app_init)
 *   // in app_update(frame):
 *   slots_update(frame, button_pressed());  // once per frame
 *
 * All rendering is performed inside slots_update() — callers do not need
 * to draw anything else.  spins/wins statistics persist across power cycles
 * via nv_read/nv_write (NV_KEY_SPINS, NV_KEY_WINS).
 */
#ifndef SLOTS_H
#define SLOTS_H

#include <stdint.h>

/* Initialise slot machine state and load spins/wins from NV flash.
 * Restores g_spins and g_wins from NV_KEY_SPINS / NV_KEY_WINS.
 * Forces a full redraw on the next slots_update() call. */
void slots_init(void);

/* Update state machine and render.  Call once per frame (every ~33 ms).
 *
 * frame       — monotonically increasing frame counter (from app_update)
 * btn_pressed — 1 if button is currently held, 0 otherwise (uint8_t boolean).
 *               Edge detection is done internally — pass button_pressed() directly.
 *
 * The state machine progresses: IDLE → SPINNING (60 f) → STOP_0 (20 f) →
 * STOP_1 (20 f) → STOP_2 (20 f) → RESULT (90 f, or skip on button) → IDLE.
 * Rendering is incremental — only changed reels are redrawn each frame. */
void slots_update(uint32_t frame, uint8_t btn_pressed);

/* Zero the lifetime spins and wins counters in RAM.
 * Does NOT write to NV flash — the zeroed values will be written on the next spin.
 * Forces a top-bar redraw to reflect the reset counters. */
void slots_reset_stats(void);

/* Force a complete redraw of all screen regions on the next slots_update() call.
 * Call this after waking from display sleep, because the LCD GRAM is stale and
 * the incremental renderer would otherwise skip unchanged regions. */
void slots_wake(void);

#endif /* SLOTS_H */
