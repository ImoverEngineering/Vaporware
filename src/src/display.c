/* vaporware/src/display.c — GC9107 LCD driver for N32G031 vape hardware
 *
 * Pin assignments come from config.h (LCD_CS_PORT/PIN, LCD_DC_*, etc.).
 * Default pinout (GV2024 boards):
 *   PB3 = SPI1_SCK   PB5 = SPI1_MOSI   PA15 = CS (software, active-low)
 *   PB7 = D/C (LOW=cmd, HIGH=data)      PB6  = RST (active-low pulse)
 *   PB4 = backlight enable (LOW=on, active-low GPIO — NOT PWM)
 *
 * GC9107 notes:
 *   - MADCTL=0x98: MY=1, ML=1, BGR=1 — R and B channels swapped in GRAM
 *   - CS must stay LOW for the entire init sequence
 *   - Sleep Out requires 120ms before Display On
 */
#include "display.h"
#include "config.h"
#include "system.h"

#define LCD_CS_LOW()    GPIO_CLR(LCD_CS_PORT,  LCD_CS_PIN)
#define LCD_CS_HIGH()   GPIO_SET(LCD_CS_PORT,  LCD_CS_PIN)
#define LCD_DC_CMD()    GPIO_CLR(LCD_DC_PORT,  LCD_DC_PIN)
#define LCD_DC_DATA()   GPIO_SET(LCD_DC_PORT,  LCD_DC_PIN)
#define LCD_RST_LOW()   GPIO_CLR(LCD_RST_PORT, LCD_RST_PIN)
#define LCD_RST_HIGH()  GPIO_SET(LCD_RST_PORT, LCD_RST_PIN)

static void spi_write_byte(uint8_t byte) {
    /* Hardware SPI1: Mode 0 (CPOL=0 CPHA=0), CR1=0x0344, PCLK/2=4MHz.
     * Confirmed from slots binary disassembly and live CR1 readback at runtime.
     * Bit-bang was tried and does not work on this hardware. */
    while (!(SPI1->SR & SPI_SR_TXE));
    *(volatile uint8_t *)&SPI1->DR = byte;
    while (SPI1->SR & SPI_SR_BSY);
}

static void lcd_write_cmd(uint8_t cmd) {
    LCD_DC_CMD();
    spi_write_byte(cmd);
}

static void lcd_write_data(uint8_t data) {
    LCD_DC_DATA();
    spi_write_byte(data);
}

static void display_gpio_init(void) {
    /* Enable GPIOA + GPIOB + SPI1 clocks */
    RCC->APB2ENR |= RCC_APB2ENR_IOPAEN | RCC_APB2ENR_IOPBEN | RCC_APB2ENR_SPI1EN;

    /* Backlight enable — active-LOW output, LOW = on at startup */
    LCD_BL_PORT->MODER  &= ~(3UL << (LCD_BL_PIN * 2));
    LCD_BL_PORT->MODER  |=  (GPIO_MODE_OUTPUT << (LCD_BL_PIN * 2));
    LCD_BL_PORT->OTYPER &= ~(1UL << LCD_BL_PIN);
    LCD_BL_PORT->BSRR   =  (1UL << (LCD_BL_PIN + 16));  /* LOW = on */

    /* SPI1_SCK (PB3) SPI1_MOSI (PB5) — AF0 (hardware SPI1) */
    LCD_SCK_PORT->MODER  &= ~((3UL << (LCD_SCK_PIN  * 2)) | (3UL << (LCD_MOSI_PIN * 2)));
    LCD_SCK_PORT->MODER  |=  ((GPIO_MODE_AF << (LCD_SCK_PIN  * 2)) |
                               (GPIO_MODE_AF << (LCD_MOSI_PIN * 2)));
    LCD_SCK_PORT->AFRL   &= ~((0xFUL << (LCD_SCK_PIN  * 4)) | (0xFUL << (LCD_MOSI_PIN * 4)));
    /* AF0 = 0: no bits to set */
    LCD_SCK_PORT->OTYPER &= ~((1UL << LCD_SCK_PIN) | (1UL << LCD_MOSI_PIN));
    LCD_SCK_PORT->OSPEEDR|=  (GPIO_SPEED_HIGH << (LCD_SCK_PIN  * 2)) |
                              (GPIO_SPEED_HIGH << (LCD_MOSI_PIN * 2));

    /* RST, D/C — outputs */
    LCD_RST_PORT->MODER  &= ~((3UL << (LCD_RST_PIN * 2)) | (3UL << (LCD_DC_PIN * 2)));
    LCD_RST_PORT->MODER  |=  ((GPIO_MODE_OUTPUT << (LCD_RST_PIN * 2)) |
                               (GPIO_MODE_OUTPUT << (LCD_DC_PIN  * 2)));
    LCD_RST_PORT->OTYPER &= ~((1UL << LCD_RST_PIN) | (1UL << LCD_DC_PIN));
    LCD_RST_PORT->OSPEEDR|=  (GPIO_SPEED_HIGH << (LCD_RST_PIN * 2)) |
                              (GPIO_SPEED_HIGH << (LCD_DC_PIN  * 2));

    /* CS — output */
    LCD_CS_PORT->MODER  &= ~(3UL << (LCD_CS_PIN * 2));
    LCD_CS_PORT->MODER  |=  (GPIO_MODE_OUTPUT << (LCD_CS_PIN * 2));
    LCD_CS_PORT->OTYPER &= ~(1UL << LCD_CS_PIN);
    LCD_CS_PORT->OSPEEDR|=  (GPIO_SPEED_HIGH << (LCD_CS_PIN * 2));

    LCD_CS_HIGH();
    LCD_DC_DATA();
    LCD_RST_HIGH();

    /* PA4, PA5, PA6 — driven LOW to match slots firmware.
     * One of these is a display module power enable (active-LOW).
     * Without this, GC9107 receives SPI commands but shows nothing.
     * Confirmed by disassembling restore_slots.bin: GPIOA_ODR=0, PA4/5/6=output. */
    GPIOA->MODER &= ~((3UL << (4*2)) | (3UL << (5*2)) | (3UL << (6*2)));
    GPIOA->MODER |=  ((GPIO_MODE_OUTPUT << (4*2)) |
                      (GPIO_MODE_OUTPUT << (5*2)) |
                      (GPIO_MODE_OUTPUT << (6*2)));
    GPIOA->OTYPER &= ~((1UL << 4) | (1UL << 5) | (1UL << 6));
    GPIOA->BSRR = (1UL << (4+16)) | (1UL << (5+16)) | (1UL << (6+16)); /* LOW */

    /* SPI1: Mode 0 (CPOL=0 CPHA=0), master, PCLK/2=4MHz, SSM+SSI, SPE.
     * CR1=0x0344 confirmed from slots binary CR1 readback at runtime. */
    SPI1->CR1 = 0x0344UL;
    SPI1->CR2 = 0;
}

void display_init(void) {
    display_gpio_init();

    LCD_RST_HIGH(); delay_ms(10);
    LCD_RST_LOW();  IWDG_FEED(); delay_ms(100); IWDG_FEED();
    LCD_RST_HIGH(); IWDG_FEED(); delay_ms(120); IWDG_FEED();

    /* CS stays LOW for the entire init sequence.
     * The GC9107 state machine requires an uninterrupted SPI session during init
     * (re-asserting CS mid-sequence can corrupt the internal register state and
     * leave the panel in an undefined mode with no visible output). */
    LCD_CS_LOW();

    lcd_write_cmd(0x11); /* Sleep Out — panel exits low-power state */
    /* GC9107 datasheet requires ≥120 ms after Sleep Out before Display ON.
     * IWDG is fed before and after to prevent watchdog reset during the wait. */
    IWDG_FEED(); delay_ms(120); IWDG_FEED();

    /*
     * ── GC9107 manufacturer init sequence ──────────────────────────────────
     * Extracted from fw_dump.bin @ 0x27C0 (factory firmware, Raz DC25000).
     *
     * Command groups:
     *   0xFF 0xA5    — Unlock extended/manufacturer command set.
     *                  Without this, commands 0x60-0xFF are ignored.
     *   0x3A 0x65    — COLMOD: pixel format = 0x65 = 16-bit RGB565.
     *                  This is NOT the standard 0x55; 0x65 is the GC9107-specific
     *                  value confirmed to enable correct 16-bit color on this panel.
     *   0x3E 0x08    — Interface mode control (restore default after unlock).
     *   0x82–0x99    — Manufacturer power/timing settings.
     *                  Values are board-tuned; do not alter without re-testing
     *                  display brightness and stability.
     *   0xB4–0xC3    — Display timing / gate scan control.
     *   0x70–0x7F    — Positive gamma correction table (16 control points).
     *   0xA0–0xAF    — Negative gamma correction table (16 control points).
     *   0xFF 0x00    — Re-lock extended commands.
     *   0x36 0x98    — MADCTL (Memory Access Control):
     *                    MY=1 (bit7): mirror Y axis
     *                    ML=1 (bit4): gate scan direction reversed
     *                    BGR=1(bit3): R and B channels are swapped in GRAM.
     *                    Combined value 0x98 = 0b10011000.
     *                    Consequence: to display visual (R,G,B), send
     *                    ((B>>3)<<11) | ((G>>2)<<5) | (R>>3) — see COL_RGB macro.
     *   0x29         — Display ON.
     * ───────────────────────────────────────────────────────────────────────
     */

    /* Unlock extended manufacturer commands */
    lcd_write_cmd(0xFF); lcd_write_data(0xA5);

    /* Interface / miscellaneous setup */
    lcd_write_cmd(0x3E); lcd_write_data(0x08); /* restore — present in working gc9107.c */
    lcd_write_cmd(0x3A); lcd_write_data(0x65); /* COLMOD: 16-bit RGB565 (GC9107-specific 0x65) */
    lcd_write_cmd(0x82); lcd_write_data(0x00);
    lcd_write_cmd(0x98); lcd_write_data(0x00);

    /* Power / timing manufacturer settings */
    lcd_write_cmd(0x63); lcd_write_data(0x0F);
    lcd_write_cmd(0x64); lcd_write_data(0x0F);
    lcd_write_cmd(0xB4); lcd_write_data(0x34);
    lcd_write_cmd(0xB5); lcd_write_data(0x30);
    lcd_write_cmd(0x83); lcd_write_data(0x13);
    lcd_write_cmd(0x86); lcd_write_data(0x04);
    lcd_write_cmd(0x87); lcd_write_data(0x19);
    lcd_write_cmd(0x88); lcd_write_data(0x2F);
    lcd_write_cmd(0x89); lcd_write_data(0x36);
    lcd_write_cmd(0x93); lcd_write_data(0x63);
    lcd_write_cmd(0x96); lcd_write_data(0x81);
    lcd_write_cmd(0xC3); lcd_write_data(0x10);
    lcd_write_cmd(0xE6); lcd_write_data(0x00);
    lcd_write_cmd(0x99); lcd_write_data(0x01);
    lcd_write_cmd(0x44); lcd_write_data(0x00);

    /* Positive gamma correction table (registers 0x70–0x7F, 16 control points) */
    lcd_write_cmd(0x70); lcd_write_data(0x07);
    lcd_write_cmd(0x71); lcd_write_data(0x19);
    lcd_write_cmd(0x72); lcd_write_data(0x1A);
    lcd_write_cmd(0x73); lcd_write_data(0x13);
    lcd_write_cmd(0x74); lcd_write_data(0x19);
    lcd_write_cmd(0x75); lcd_write_data(0x1D);
    lcd_write_cmd(0x76); lcd_write_data(0x47);
    lcd_write_cmd(0x77); lcd_write_data(0x0A);
    lcd_write_cmd(0x78); lcd_write_data(0x07);
    lcd_write_cmd(0x79); lcd_write_data(0x47);
    lcd_write_cmd(0x7A); lcd_write_data(0x05);
    lcd_write_cmd(0x7B); lcd_write_data(0x09);
    lcd_write_cmd(0x7C); lcd_write_data(0x0D);
    lcd_write_cmd(0x7D); lcd_write_data(0x0C);
    lcd_write_cmd(0x7E); lcd_write_data(0x0C);
    lcd_write_cmd(0x7F); lcd_write_data(0x08);

    /* Negative gamma correction table (registers 0xA0–0xAF, 16 control points) */
    lcd_write_cmd(0xA0); lcd_write_data(0x0B);
    lcd_write_cmd(0xA1); lcd_write_data(0x36);
    lcd_write_cmd(0xA2); lcd_write_data(0x09);
    lcd_write_cmd(0xA3); lcd_write_data(0x0D);
    lcd_write_cmd(0xA4); lcd_write_data(0x08);
    lcd_write_cmd(0xA5); lcd_write_data(0x23);
    lcd_write_cmd(0xA6); lcd_write_data(0x3B);
    lcd_write_cmd(0xA7); lcd_write_data(0x04);
    lcd_write_cmd(0xA8); lcd_write_data(0x07);
    lcd_write_cmd(0xA9); lcd_write_data(0x38);
    lcd_write_cmd(0xAA); lcd_write_data(0x0A);
    lcd_write_cmd(0xAB); lcd_write_data(0x12);
    lcd_write_cmd(0xAC); lcd_write_data(0x0C);
    lcd_write_cmd(0xAD); lcd_write_data(0x07);
    lcd_write_cmd(0xAE); lcd_write_data(0x2F);
    lcd_write_cmd(0xAF); lcd_write_data(0x07);

    /* Re-lock extended commands — prevents accidental writes to mfr registers */
    lcd_write_cmd(0xFF); lcd_write_data(0x00);

    /* MADCTL = 0x98: MY=1 (mirror Y), ML=1 (gate scan reversed), BGR=1 (R/B swap)
     * Must be set AFTER re-locking (0xFF 0x00) — 0x36 is a standard MIPI command */
    lcd_write_cmd(0x36); lcd_write_data(0x98);
    lcd_write_cmd(0x29);  /* Display ON */
    delay_ms(10);

    LCD_CS_HIGH();

    /* First-frame clear: write black to all 128×160 = 20480 pixels.
     * Required because GC9107 GRAM is not guaranteed to be zeroed after reset;
     * uninitialized GRAM can show noise or partial images before the first draw.
     * This also exercises the full SPI path and confirms the panel is alive. */
    display_fill(0x0000);
}

#define GC9107_COL_OFFSET  0
#define GC9107_ROW_OFFSET  0

void display_set_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1) {
    x0 += GC9107_COL_OFFSET; x1 += GC9107_COL_OFFSET;
    y0 += GC9107_ROW_OFFSET; y1 += GC9107_ROW_OFFSET;
    lcd_write_cmd(0x2A);
    lcd_write_data(x0 >> 8); lcd_write_data(x0 & 0xFF);
    lcd_write_data(x1 >> 8); lcd_write_data(x1 & 0xFF);
    lcd_write_cmd(0x2B);
    lcd_write_data(y0 >> 8); lcd_write_data(y0 & 0xFF);
    lcd_write_data(y1 >> 8); lcd_write_data(y1 & 0xFF);
    lcd_write_cmd(0x2C);
}

void display_fill(uint16_t color) {
    uint8_t hi = color >> 8, lo = color & 0xFF;
    LCD_CS_LOW();
    display_set_window(0, 0, LCD_WIDTH - 1, LCD_HEIGHT - 1);
    LCD_DC_DATA();
    for (uint32_t i = 0; i < (uint32_t)LCD_WIDTH * LCD_HEIGHT; i++) {
        if ((i & 0x1FF) == 0) IWDG_FEED();   /* feed every 512 pixels */
        spi_write_byte(hi);
        spi_write_byte(lo);
    }
    LCD_CS_HIGH();
}

void display_fill_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color) {
    uint8_t hi = color >> 8, lo = color & 0xFF;
    LCD_CS_LOW();
    display_set_window(x, y, x + w - 1, y + h - 1);
    LCD_DC_DATA();
    for (uint32_t i = 0; i < (uint32_t)w * h; i++) {
        if ((i & 0x1FF) == 0) IWDG_FEED();   /* feed every 512 pixels */
        spi_write_byte(hi);
        spi_write_byte(lo);
    }
    LCD_CS_HIGH();
}

void display_draw_image(const uint16_t *img, uint16_t x, uint16_t y,
                        uint16_t w, uint16_t h) {
    LCD_CS_LOW();
    display_set_window(x, y, x + w - 1, y + h - 1);
    LCD_DC_DATA();
    uint32_t npix = (uint32_t)w * h;
    for (uint32_t i = 0; i < npix; i++) {
        if ((i & 0x1FF) == 0) IWDG_FEED();   /* feed every 512 pixels */
        uint16_t px = img[i];
        spi_write_byte(px >> 8);
        spi_write_byte(px & 0xFF);
    }
    LCD_CS_HIGH();
}

void display_set_backlight(uint8_t on) {
    if (on == 0)
        LCD_BL_PORT->BSRR = (1UL << LCD_BL_PIN);           /* HIGH = off */
    else
        LCD_BL_PORT->BSRR = (1UL << (LCD_BL_PIN + 16));    /* LOW  = on  */
}

void display_sleep_in(void) {
    display_set_backlight(0);
    LCD_CS_LOW(); lcd_write_cmd(0x28); LCD_CS_HIGH(); /* Display Off */
    delay_ms(10);
    LCD_CS_LOW(); lcd_write_cmd(0x10); LCD_CS_HIGH(); /* Sleep In */
    delay_ms(5);
}

void display_sleep_out(void) {
    LCD_CS_LOW(); lcd_write_cmd(0x11); LCD_CS_HIGH(); /* Sleep Out */
    IWDG_FEED(); delay_ms(120); IWDG_FEED();
    LCD_CS_LOW(); lcd_write_cmd(0x29); LCD_CS_HIGH(); /* Display On */
    delay_ms(10);
    display_set_backlight(1);
}

void display_draw_pixel(uint16_t x, uint16_t y, uint16_t color) {
    if (x >= LCD_WIDTH || y >= LCD_HEIGHT) return;
    LCD_CS_LOW();
    display_set_window(x, y, x, y);
    LCD_DC_DATA();
    spi_write_byte(color >> 8);
    spi_write_byte(color & 0xFF);
    LCD_CS_HIGH();
}
