#include "doom_title_letterbox.h"  /* or doom_title_crop.h */

static void draw_title(void) {
    doom_title_letterbox_draw();      /* or doom_title_crop_draw() */

    /* Optional one-button prompt. For crop version, this covers lower art. */
    display_fill_rect(35, 146, 58, 8, COL_BLACK);
    display_fill_rect(39, 148, 50, 4, COL_RGB(255, 220, 0));
}
