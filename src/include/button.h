/* vaporware/include/button.h — Hardware button abstraction
 *
 * Hardware: PA7, active-LOW with internal pull-up.
 *   IDR reads 0 when pressed (button connects pin to GND).
 *   IDR reads 1 when released (pull-up holds pin HIGH).
 *   All button_*() accessors invert this so 1=pressed, 0=not pressed.
 *
 * Debounce: none.  The app framework calls button_update() once per frame
 * at ~30 fps (every 33 ms).  Mechanical bounce typically lasts <10 ms,
 * so the frame period acts as a natural debounce filter.
 *
 * Usage:
 *   button_init()   — call once at startup (app framework does this automatically)
 *   button_update() — call once per frame (app framework does this automatically)
 *   button_*()      — query functions, valid after button_update()
 *
 * Apps using the app framework (app.h / app.c) do NOT need to call
 * button_init() or button_update() — the framework handles both.
 */
#ifndef BUTTON_H
#define BUTTON_H

#include <stdint.h>

/* Initialise BTN_PORT/BTN_PIN (PA7) as input with internal pull-up.
 * Called automatically by the app framework before app_init(). */
void button_init(void);

/* Snapshot current GPIO state and update all edge-detect flags.
 * Must be called exactly once per frame.  Called by app framework before app_update(). */
void button_update(void);

/* Returns 1 if the button is currently held down, 0 otherwise.
 * Type: uint8_t boolean (0 or 1). */
uint8_t  button_pressed(void);

/* Returns 1 on the single frame when the button first goes down (rising edge).
 * Type: uint8_t boolean (0 or 1).
 * Automatically clears on the next call to button_update(). */
uint8_t  button_just_pressed(void);

/* Returns 1 on the single frame when the button is released (falling edge).
 * Type: uint8_t boolean (0 or 1).
 * Automatically clears on the next call to button_update(). */
uint8_t  button_just_released(void);

/* Milliseconds the button has been continuously held since the last press.
 * Returns 0 if the button is not currently held.
 * Resets to 0 on release and restarts from 0 on the next press.
 * Uses (uint16_t) subtraction against ms_now() — safe for up to 65535 ms.
 * Saturates naturally at 65535 ms (TIM1 is 16-bit; no explicit cap needed). */
uint16_t button_held_ms(void);

/* Direct GPIO read — returns 1 if pressed, 0 if not.
 * Does not update any state or edge-detect flags.
 * Use in sleep/polling loops where button_update() is not called
 * (e.g., the device_sleep() busy-wait in app.c). */
uint8_t  button_raw(void);

#endif /* BUTTON_H */
