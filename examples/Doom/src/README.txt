VapeDOOM title assets generated from uploaded image.

Use only ONE option in the firmware build unless you intentionally want both.

Option A: doom_title_letterbox
- Preserves the entire original 320x240 art.
- Resized to 128x96 and drawn at y=32 on the 128x160 display.
- Flash for pixels: 6144 bytes + 32 bytes palette.
- SRAM during draw: 256 bytes for one RGB565 row.

Option B: doom_title_crop
- Crops the source to portrait aspect ratio and fills the full 128x160 screen.
- Loses left/right edge detail but uses the whole display.
- Flash for pixels: 10240 bytes + 32 bytes palette.
- SRAM during draw: 256 bytes for one RGB565 row.

Integration:
1. Keep the chosen .c and .h in examples/Doom/src/.
2. Add the .c file to your build command.
3. Include the header and call the draw function from app_init() or title state:
   #include "doom_title_letterbox.h"
   ...
   doom_title_letterbox_draw();

The image is stored as 4bpp indexed color. The draw function expands one row at a time
to RGB565/BGR-swapped values expected by Vaporware's display_draw_image().
