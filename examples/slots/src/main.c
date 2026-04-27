/* slotmachine/src/main.c — Slot machine app using Vaporware framework
 *
 * The framework (app.c) provides main(), hardware init, IWDG, frame timing,
 * sleep management, and button handling.  This file only contains game logic.
 *
 * Controls:
 *   Press        → spin
 *   Hold 10 s    → reset spins/wins counters (also clears NV)
 *   Idle 20 s    → display sleeps; any press wakes it
 */
#include "app.h"
#include "button.h"
#include "nv.h"
#include "slots.h"
#include "system.h"

static void on_reset(void) {
    slots_reset_stats();
    nv_write(NV_KEY_SPINS, 0);
    nv_write(NV_KEY_WINS,  0);
}

void app_init(void) {
    /* No sleep — display stays on permanently */
    app_set_hold_reset(10000, on_reset);
    slots_init();
}

void app_update(uint32_t frame) {
    slots_update(frame, button_just_pressed());
}

void app_wake(void) {
    slots_wake();
}
