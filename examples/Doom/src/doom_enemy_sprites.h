#ifndef DOOM_ENEMY_SPRITES_H
#define DOOM_ENEMY_SPRITES_H

#include <stdint.h>

typedef struct {
    uint8_t width;
    uint8_t height;
    const uint16_t *palette;
    const uint8_t *pixels_4bpp;
} doom_enemy_sprite_t;

#define DOOM_ENEMY_SPRITE_COUNT 8

extern const doom_enemy_sprite_t doom_enemy_sprites[DOOM_ENEMY_SPRITE_COUNT];

#endif /* DOOM_ENEMY_SPRITES_H */
