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
    /* button_raw() reads GPIO directly every frame — catches presses that
     * occur during long renders when button_update() hasn't run yet.
     * slots_update() does its own edge detection via g_last_btn. */
    slots_update(frame, button_raw());
}

void app_wake(void) {
    slots_wake();
}
