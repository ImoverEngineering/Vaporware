/* sim_main.c — SDL2 event loop for the slot machine simulator
 *
 * Keys:
 *   SPACE / ENTER   spin (= button press)
 *   Q / Esc         quit
 */
#include "slots.h"
#include <SDL2/SDL.h>
#include <stdio.h>
#include <stdint.h>

extern void sim_present(void);
extern void display_init(void);

int main(void) {
    display_init();

    printf("Vaporware Slot Machine Sim\n");
    printf("  SPACE / ENTER  = spin\n");
    printf("  Q / Esc        = quit\n\n");

    slots_init();

    uint32_t frame   = 0;
    uint32_t last_ms = SDL_GetTicks();
    uint8_t  btn     = 0;      /* button state this frame */
    uint8_t  btn_req = 0;      /* keydown sets this, cleared after one frame */

    while (1) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) return 0;
            if (ev.type == SDL_KEYDOWN) {
                switch (ev.key.keysym.sym) {
                    case SDLK_q:
                    case SDLK_ESCAPE: return 0;
                    case SDLK_SPACE:
                    case SDLK_RETURN: btn_req = 1; break;
                    default: break;
                }
            }
        }

        uint32_t now = SDL_GetTicks();
        if (now - last_ms < 33u) { SDL_Delay(1); continue; }
        last_ms = now;

        btn = btn_req;
        btn_req = 0;   /* single-frame pulse */

        FireAction fire = slots_update(frame, btn);

        if (fire != FIRE_NONE) {
            static const char *FIRE_NAME[] = {
                "NONE", "SHORT(150ms)", "NORM(250ms)", "LONG(500ms)", "MEGA(jackpot!)"
            };
            printf(">>> FIRE: %s\n", FIRE_NAME[(int)fire]);
        }

        sim_present();
        frame++;
    }
}
