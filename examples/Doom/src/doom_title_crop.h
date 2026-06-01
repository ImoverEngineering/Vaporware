#ifndef DOOM_TITLE_CROP_H_
#define DOOM_TITLE_CROP_H_

#include <stdint.h>

#define DOOM_TITLE_CROP_W 128
#define DOOM_TITLE_CROP_H 160
#define DOOM_TITLE_CROP_Y 0

extern const uint16_t doom_title_crop_palette[16];
extern const uint8_t doom_title_crop_pixels_4bpp[10240];

void doom_title_crop_draw(void);

#endif
