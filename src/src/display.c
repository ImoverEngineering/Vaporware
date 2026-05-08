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
    RCC->APB2ENR |= RCC_APB2ENR_IOPAEN | RCC_APB2ENR_IOPBEN | RCC_APB2ENR_SPI1EN;

    /* Backlight enable — active-LOW output, LOW = on at startup */
    LCD_BL_PORT->MODER  &= ~(3UL << (LCD_BL_PIN * 2));
    LCD_BL_PORT->MODER  |=  (GPIO_MODE_OUTPUT << (LCD_BL_PIN * 2));
    LCD_BL_PORT->OTYPER &= ~(1UL << LCD_BL_PIN);
    LCD_BL_PORT->BSRR   =  (1UL << (LCD_BL_PIN + 16));  /* LOW = on */

    /* SPI1_SCK, SPI1_MOSI — AF0 */
    LCD_SCK_PORT->MODER  &= ~((3UL << (LCD_SCK_PIN  * 2)) | (3UL << (LCD_MOSI_PIN * 2)));
    LCD_SCK_PORT->MODER  |=  ((GPIO_MODE_AF << (LCD_SCK_PIN  * 2)) |
                               (GPIO_MODE_AF << (LCD_MOSI_PIN * 2)));
    LCD_SCK_PORT->AFRL   &= ~((0xFUL << (LCD_SCK_PIN  * 4)) | (0xFUL << (LCD_MOSI_PIN * 4)));
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

    /* SPI1: full-duplex master, mode 0, 8-bit, software NSS.
     *
     * Full-duplex is used (no BIDIMODE) so RXNE asserts reliably after every
     * spi_write_byte() call.  This lets the OVR clear sequence (read DR then
     * read SR, both while RXNE=1) work correctly before each large pixel burst.
     *
     * N32G031 OVR behaviour: after a large TXE-only burst, the 1-byte RX FIFO
     * overflows on every received echo byte.  OVR accumulates across multiple
     * calls without clearing.  After ~2–3 full-screen bursts the OVR state
     * permanently deadlocks TXE.  Clearing OVR (DR+SR) before each burst resets
     * this and allows unlimited sequential transfers.
     *
     * BIDIMODE was tried but found NOT to suppress OVR on N32G031 (TXE still
     * deadlocks), and BIDIMODE does suppress RXNE, making DR+SR OVR-clear
     * impossible (RXNE stays 0, so reading DR never properly clears OVR).
     *
     * Target SPI clock regardless of SYSCLK:
     *   At  8 MHz PCLK: BR_DIV2  =  4 MHz (proven baseline)
     *   At 48 MHz PCLK: BR_DIV16 =  3 MHz (safe for GC9107) */
    uint32_t br = (RCC->CFGR & RCC_CFGR_SWS_PLL)  /* PLL is SYSCLK */
                  ? SPI_CR1_BR_DIV16 : SPI_CR1_BR_DIV2;
    SPI1->CR1 = SPI_CR1_MSTR | SPI_CR1_SSM | SPI_CR1_SSI | br | SPI_CR1_SPE;
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
    /* Debug sentinels — read via OpenOCD mrw to locate deadlock.
     * 0x20000090 = 0xEE110000: entered display_fill
     * 0x20000094 = 0xEE220000: past SPE toggle, entering display_set_window
     * 0x20000098 = 0xEE330000: display_set_window done, entering pixel loop
     * 0x2000009C = 0xEE440000: pixel loop done, in while(BSY)             */
    *(volatile uint32_t *)0x20000090UL = 0xEE110000UL;
    SPI1->CR1 &= ~SPI_CR1_SPE;   /* SPE=0: reset SPI — clears OVR, RXNE, all flags */
    SPI1->CR1 |=  SPI_CR1_SPE;   /* SPE=1: fresh start, TXE=1, BSY=0, OVR=0        */
    *(volatile uint32_t *)0x20000094UL = 0xEE220000UL;
    display_set_window(0, 0, LCD_WIDTH - 1, LCD_HEIGHT - 1);
    LCD_DC_DATA();
    *(volatile uint32_t *)0x20000098UL = 0xEE330000UL;
    /* Per-row IWDG feed prevents watchdog reset across consecutive fills.
     * IWDG default timeout = ~273ms (LSI=60kHz, PR=0, RLR=0xFFF).
     * Each row takes ~1.36ms at 3MHz; feeding every row keeps the gap <2ms. */
    for (uint16_t row = 0; row < LCD_HEIGHT; row++) {
        IWDG_FEED();
        for (uint16_t col = 0; col < LCD_WIDTH; col++) {
            spi_write_byte(hi);
            spi_write_byte(lo);
        }
    }
    *(volatile uint32_t *)0x2000009CUL = 0xEE440000UL;
    LCD_CS_HIGH();
}

void display_fill_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color) {
    uint8_t hi = color >> 8, lo = color & 0xFF;
    LCD_CS_LOW();
    SPI1->CR1 &= ~SPI_CR1_SPE;
    SPI1->CR1 |=  SPI_CR1_SPE;
    display_set_window(x, y, x + w - 1, y + h - 1);
    LCD_DC_DATA();
    for (uint16_t row = 0; row < h; row++) {
        IWDG_FEED();
        for (uint16_t col = 0; col < w; col++) {
            spi_write_byte(hi);
            spi_write_byte(lo);
        }
    }
    LCD_CS_HIGH();
}

void display_draw_image(const uint16_t *img, uint16_t x, uint16_t y,
                        uint16_t w, uint16_t h) {
    LCD_CS_LOW();
    SPI1->CR1 &= ~SPI_CR1_SPE;
    SPI1->CR1 |=  SPI_CR1_SPE;
    display_set_window(x, y, x + w - 1, y + h - 1);
    LCD_DC_DATA();
    for (uint16_t row = 0; row < h; row++) {
        IWDG_FEED();
        const uint16_t *rowp = img + (uint32_t)row * w;
        for (uint16_t col = 0; col < w; col++) {
            uint16_t px = rowp[col];
            spi_write_byte((uint8_t)(px >> 8));
            spi_write_byte((uint8_t)(px & 0xFF));
        }
    }
    LCD_CS_HIGH();
}

void display_draw_chunk_2x(const uint16_t *src, uint16_t log_row,
                           uint16_t log_w, uint16_t log_h)
{
    /* Blit a log_w × log_h logical-pixel chunk to the physical display,
     * scaling 2× in both axes (each logical pixel → 2×2 physical pixels).
     * Physical output region: (0, log_row*2) to (127, log_row*2 + log_h*2 - 1).
     *
     * Used by the 64×80 SWD streamer for half-resolution Doom streaming:
     *   log_w=64, log_h=16  →  128×32 physical pixels per chunk.
     *
     * Inner loop sends each logical pixel four times (hi,lo,hi,lo) to
     * produce two horizontally-adjacent physical pixels; the outer pass
     * loop repeats each logical row twice for vertical doubling.
     * Total SPI output: log_w * log_h * 4 * 2 bytes = same as 128×(log_h*2). */
    IWDG_FEED();
    uint16_t pr = (uint16_t)(log_row * 2u);
    LCD_CS_LOW();
    SPI1->CR1 &= ~SPI_CR1_SPE;
    SPI1->CR1 |=  SPI_CR1_SPE;
    display_set_window(0u, pr, 127u, (uint16_t)(pr + log_h * 2u - 1u));
    LCD_DC_DATA();
    for (uint16_t lr = 0u; lr < log_h; lr++) {
        const uint16_t *row = src + (uint32_t)lr * log_w;
        for (uint8_t pass = 0u; pass < 2u; pass++) {   /* vertical × 2 */
            for (uint16_t lc = 0u; lc < log_w; lc++) { /* horizontal × 2 */
                uint16_t px = row[lc];
                uint8_t  hi = (uint8_t)(px >> 8);
                uint8_t  lo = (uint8_t)(px & 0xFFu);
                while (!(SPI1->SR & SPI_SR_TXE)); *(volatile uint8_t *)&SPI1->DR = hi;
                while (!(SPI1->SR & SPI_SR_TXE)); *(volatile uint8_t *)&SPI1->DR = lo;
                while (!(SPI1->SR & SPI_SR_TXE)); *(volatile uint8_t *)&SPI1->DR = hi;
                while (!(SPI1->SR & SPI_SR_TXE)); *(volatile uint8_t *)&SPI1->DR = lo;
            }
        }
    }
    for (volatile uint32_t _d = 300u; _d--; );
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

void display_draw_chunk_cpu(const uint16_t *buf, uint16_t row_start, uint16_t nrows)
{
    /* CPU-polled SPI blit: identical to display_draw_image but fixed full-width.
     * Simpler than DMA — avoids DMA channel-mapping uncertainty on N32G031.
     * At 24 MHz SPI (PLL mode) each 128×16 chunk takes ~1.4 ms.             */
    IWDG_FEED();
    LCD_CS_LOW();
    SPI1->CR1 &= ~SPI_CR1_SPE;
    SPI1->CR1 |=  SPI_CR1_SPE;
    display_set_window(0u, row_start,
                       (uint16_t)(LCD_WIDTH - 1u),
                       (uint16_t)(row_start + nrows - 1u));
    LCD_DC_DATA();
    uint32_t npix = (uint32_t)LCD_WIDTH * nrows;
    for (uint32_t i = 0; i < npix; i++) {
        uint16_t px = buf[i];
        while (!(SPI1->SR & SPI_SR_TXE)); *(volatile uint8_t *)&SPI1->DR = (uint8_t)(px >> 8);
        while (!(SPI1->SR & SPI_SR_TXE)); *(volatile uint8_t *)&SPI1->DR = (uint8_t)(px & 0xFF);
    }
    /* Fixed drain: BSY/TXE polling deadlocks on N32G031 after TXE-only bursts.
     * 300 cycles @ 48 MHz ≈ 6.25 µs = 2× byte-time at 3 MHz SPI — enough for
     * both the last TX-FIFO byte and the shift register byte to clock out fully
     * before CS is raised. */
    for (volatile uint32_t _d = 300u; _d--; );
    LCD_CS_HIGH();
}

void display_draw_chunk_dma(const uint16_t *buf, uint16_t row_start, uint16_t nrows)
{
    /* Blit a 128×nrows chunk from buf to the display using DMA-driven SPI.
     *
     * Unlike display_draw_image(), the CPU does NOT poll SPI_SR_TXE per byte.
     * DMA1 Channel 3 owns the AHB bus for the SPI transfer, so the CPU (and
     * therefore the SWD master) can write to SRAM without contention.
     * This is what makes PLL useful: at 48 MHz the CPU would otherwise hammer
     * the bus in a tight polling loop, adding wait-states to every SWD write.
     *
     * Sequence:
     *   1. Set GRAM window and issue RAMWR (display_set_window leaves CS LOW).
     *   2. Point DMA1_CH3 at SPI1->DR (peripheral) and buf (memory).
     *   3. Enable SPI TX DMA request (SPI_CR2_TXDMAEN).
     *   4. Poll DMA ISR TCIF3 — CPU is idle, AHB free for SWD.
     *   5. Wait for SPI BSY to drain the shift register before raising CS.
     */
    LCD_CS_LOW();
    display_set_window(0u, row_start,
                       (uint16_t)(LCD_WIDTH - 1u),
                       (uint16_t)(row_start + nrows - 1u));
    LCD_DC_DATA();

    /* Configure DMA1 Channel 3 for SPI1 TX (memory → peripheral, 8-bit). */
    DMA1_CH3->CCR   = 0;                               /* disable first      */
    DMA1_CH3->CPAR  = (uint32_t)&SPI1->DR;             /* fixed: SPI data reg */
    DMA1_CH3->CMAR  = (uint32_t)buf;                   /* source: chunk buf  */
    DMA1_CH3->CNDTR = (uint32_t)LCD_WIDTH * nrows * 2u;/* bytes (2 per pixel)*/
    DMA1_CH3->CCR   = DMA_CCR_DIR    /* mem→periph */
                    | DMA_CCR_MINC   /* memory increments */
                    | DMA_CCR_PL_HIGH/* high priority */
                    | DMA_CCR_EN;    /* go */

    SPI1->CR2 |= SPI_CR2_TXDMAEN;   /* arm SPI to request DMA on TXE */

    /* CPU idles here — DMA owns AHB, SWD writes go uncontested. */
    while (!(DMA1->ISR & DMA_ISR_TCIF3)) { IWDG_FEED(); }
    DMA1->IFCR = DMA_IFCR_CTCIF3;   /* clear TC flag */

    while (SPI1->SR & SPI_SR_BSY);           /* clear OVR and wait for shift reg drain */
    SPI1->CR2 &= ~SPI_CR2_TXDMAEN;
    LCD_CS_HIGH();
}

void display_draw_pixel(uint16_t x, uint16_t y, uint16_t color) {
    if (x >= LCD_WIDTH || y >= LCD_HEIGHT) return;
    LCD_CS_LOW();
    display_set_window(x, y, x, y);
    LCD_DC_DATA();
    spi_write_byte(color >> 8);
    spi_write_byte(color & 0xFF);
    while (SPI1->SR & SPI_SR_BSY);
    LCD_CS_HIGH();
}
