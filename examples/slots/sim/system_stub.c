/* system_stub.c — timing stubs for slot machine sim */
#include <SDL2/SDL.h>
#include <stdint.h>

void clock_init(void) {}
void tim1_init(void)  {}
void delay_ms(uint32_t ms) { SDL_Delay(ms); }
uint32_t millis(void) { return SDL_GetTicks(); }
uint16_t ms_now(void) { return (uint16_t)SDL_GetTicks(); }
