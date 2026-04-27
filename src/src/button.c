/* vaporware/src/button.c — Button driver (active-LOW with internal pull-up)
 *
 * Pin assignment is read from config.h (BTN_PORT / BTN_PIN).
 * Default: PA7 on GV2024 boards.
 *
 * No-debounce design:
 *   button_update() is called once per frame at ~30 fps (every 33 ms).
 *   The 33 ms inter-sample period is longer than typical mechanical bounce
 *   duration (~5–10 ms), so the frame rate acts as a natural debounce.
 *   No software timer or state-machine debounce is needed.
 *
 * State variables:
 *   g_pressed       — uint8_t boolean: 1=button held, 0=not held
 *   g_just_pressed  — uint8_t boolean: 1=fell this frame (rising edge), cleared next frame
 *   g_just_released — uint8_t boolean: 1=released this frame (falling edge), cleared next frame
 *   g_holding       — uint8_t boolean: 1=button has been held since g_held_since
 *   g_held_since    — uint16_t ms_now() snapshot when hold started
 *
 * button_held_ms() returns the number of milliseconds the button has been
 * continuously held.  Uses (uint16_t) subtraction against ms_now() for
 * safe wrap-around arithmetic.  The result saturates naturally at 65535 ms
 * because ms_now() is 16-bit — there is no explicit saturation cap.
 */
#include "button.h"
#include "system.h"
#include "config.h"

static uint8_t  g_pressed;
static uint8_t  g_prev;
static uint8_t  g_just_pressed;
static uint8_t  g_just_released;
static uint16_t g_held_since;
static uint8_t  g_holding;

void button_init(void) {
    RCC->APB2ENR |= BTN_RCC_EN;
    BTN_PORT->MODER &= ~(3UL << (BTN_PIN * 2));  /* input mode */
    BTN_PORT->PUPDR &= ~(3UL << (BTN_PIN * 2));
    BTN_PORT->PUPDR |=  (1UL << (BTN_PIN * 2));  /* pull-up */
    g_pressed       = 0;
    g_prev          = 0;
    g_just_pressed  = 0;
    g_just_released = 0;
    g_holding       = 0;
    g_held_since    = 0;
}

void button_update(void) {
    g_prev = g_pressed;
    /* Active-low: IDR bit = 0 when pressed → XOR 1 → logical 1 = pressed */
    g_pressed = (uint8_t)(((BTN_PORT->IDR >> BTN_PIN) & 1U) ^ 1U);

    g_just_pressed  = (g_pressed & ~g_prev)  ? 1u : 0u;
    g_just_released = (~g_pressed & g_prev)  ? 1u : 0u;

    if (g_just_pressed) {
        g_held_since = ms_now();
        g_holding    = 1;
    } else if (!g_pressed) {
        g_holding = 0;
    }
}

uint8_t button_pressed(void)       { return g_pressed; }
uint8_t button_just_pressed(void)  { return g_just_pressed; }
uint8_t button_just_released(void) { return g_just_released; }

uint16_t button_held_ms(void) {
    if (!g_holding) return 0;
    return (uint16_t)(ms_now() - g_held_since);
}

uint8_t button_raw(void) {
    return (uint8_t)(((BTN_PORT->IDR >> BTN_PIN) & 1U) ^ 1U);
}
