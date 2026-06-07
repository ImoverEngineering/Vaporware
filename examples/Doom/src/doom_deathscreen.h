#ifndef DOOM_DEATHSCREEN_ASSET_H
#define DOOM_DEATHSCREEN_ASSET_H

#include <stdint.h>

#define DOOM_DEATHSCREEN_W 128
#define DOOM_DEATHSCREEN_H 160

extern const uint16_t doom_deathscreen_palette[16];
extern const uint8_t doom_deathscreen_pixels_4bpp[10240];

void doom_deathscreen_draw(void);

#endif /* DOOM_DEATHSCREEN_ASSET_H */
