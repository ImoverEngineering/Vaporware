#ifndef DOOM_TITLE_LETTERBOX_H_
#define DOOM_TITLE_LETTERBOX_H_

#include <stdint.h>

#define DOOM_TITLE_LETTERBOX_W 128
#define DOOM_TITLE_LETTERBOX_H 96
#define DOOM_TITLE_LETTERBOX_Y 32

extern const uint16_t doom_title_letterbox_palette[16];
extern const uint8_t doom_title_letterbox_pixels_4bpp[6144];

void doom_title_letterbox_draw(void);

#endif
