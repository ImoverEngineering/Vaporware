/* flash_video.c — Full-resolution (128×160) video from external GT25Q80A flash.
 *
 * Streams video stored in the external 1 MB SPI NOR flash (GT25Q80A) directly
 * to the GC9107 display.  A full frame (40 960 B) exceeds the 8 KB SRAM, so
 * it is never buffered: rows are read from external flash and forwarded to the
 * display SPI one row at a time.
 *
 * External flash layout (GT25Q80A, PA8=CS PA9=SCK PA10=MISO PA11=MOSI):
 *   0x000000  video header (16 bytes, same format as internal-flash version)
 *   0x000010  frame data   (n_frames × 128×160×2 bytes BGR565 LE)
 *
 * Video header (8 × u16, little-endian):
 *   [0] magic    = 0x5650 ('VP')
 *   [1] n_frames
 *   [2] fps      (informational; playback rate is hardware-limited to ~6 fps)
 *   [3] width    must equal 128
 *   [4] height   must equal 160
 *   [5..7] reserved
 *
 * Throughput at 48 MHz PLL:
 *   Bit-bang read from flash:  ~1.5 µs / byte  → ~62 ms / frame
 *   HW SPI write to display:   ~2.7 µs / byte  → ~111 ms / frame
 *   Non-pipelined (read row then send row):  ~160–180 ms / frame → ~6 fps
 *
 * Build:
 *   build_flash_video.bat         → compiles flash_video.bin + ext_flash_writer.bin
 *   python tools\prep_frames.py <input> --fps 6
 *                                 → produces build\ext_flash_video.bin
 *   python gen_flash.py           → generates build\combined_flash.tcl
 *   flash_vape.bat                → runs combined_flash.tcl via OpenOCD in WSL
 */
#include "config.h"
#include "system.h"
#include "display.h"
#include "vape.h"

/* ── Video / display constants ────────────────────────────────────────────── */
#define VIDEO_MAGIC     0x5650U
#define VIDEO_HDR_SZ    16U
#define VIDEO_HDR_ADDR  0x000000UL   /* external flash start */

/* ── External flash bit-bang SPI ─────────────────────────────────────────── */
#define FLASH_CS_PIN    8
#define FLASH_SCK_PIN   9
#define FLASH_MISO_PIN  10
#define FLASH_MOSI_PIN  11

#define FLASH_CS_HIGH()  GPIO_SET(GPIOA, FLASH_CS_PIN)
#define FLASH_CS_LOW()   GPIO_CLR(GPIOA, FLASH_CS_PIN)
#define FLASH_SCK_HIGH() GPIO_SET(GPIOA, FLASH_SCK_PIN)
#define FLASH_SCK_LOW()  GPIO_CLR(GPIOA, FLASH_SCK_PIN)
#define FLASH_MOSI_HI()  GPIO_SET(GPIOA, FLASH_MOSI_PIN)
#define FLASH_MOSI_LO()  GPIO_CLR(GPIOA, FLASH_MOSI_PIN)
#define FLASH_MISO_RD()  GPIO_READ(GPIOA, FLASH_MISO_PIN)

#define CMD_READ  0x03u

/* ── Display SPI helpers (mirrors display.c internals) ──────────────────── */
#define LCD_CS_LOW()   GPIO_CLR(LCD_CS_PORT, LCD_CS_PIN)
#define LCD_CS_HIGH()  GPIO_SET(LCD_CS_PORT, LCD_CS_PIN)
#define LCD_DC_DATA()  GPIO_SET(LCD_DC_PORT, LCD_DC_PIN)

static inline void spi_byte(uint8_t b) {
    while (!(SPI1->SR & SPI_SR_TXE));
    *(volatile uint8_t *)&SPI1->DR = b;
}

/* ── Flash bit-bang ───────────────────────────────────────────────────────── */
static void flash_gpio_init(void) {
    RCC->APB2ENR |= RCC_APB2ENR_IOPAEN;

    GPIOA->MODER &= ~((3UL << (FLASH_CS_PIN   * 2)) |
                      (3UL << (FLASH_SCK_PIN  * 2)) |
                      (3UL << (FLASH_MOSI_PIN * 2)));
    GPIOA->MODER |=  ((GPIO_MODE_OUTPUT << (FLASH_CS_PIN   * 2)) |
                      (GPIO_MODE_OUTPUT << (FLASH_SCK_PIN  * 2)) |
                      (GPIO_MODE_OUTPUT << (FLASH_MOSI_PIN * 2)));
    GPIOA->OTYPER  &= ~((1UL << FLASH_CS_PIN) |
                        (1UL << FLASH_SCK_PIN) |
                        (1UL << FLASH_MOSI_PIN));
    GPIOA->OSPEEDR |= ((GPIO_SPEED_HIGH << (FLASH_CS_PIN   * 2)) |
                       (GPIO_SPEED_HIGH << (FLASH_SCK_PIN  * 2)) |
                       (GPIO_SPEED_HIGH << (FLASH_MOSI_PIN * 2)));

    GPIOA->MODER &= ~(3UL << (FLASH_MISO_PIN * 2));
    GPIOA->PUPDR  &= ~(3UL << (FLASH_MISO_PIN * 2));
    GPIOA->PUPDR  |=  (1UL  << (FLASH_MISO_PIN * 2));

    FLASH_CS_HIGH();
    FLASH_SCK_LOW();
    FLASH_MOSI_LO();
}

/* flash_spi_xfer: bit-bang CPOL=0 CPHA=0 transfer on GPIOA PA8-PA11.
 * Sequential: SCK_LOW → set MOSI → SCK_HIGH → sample MISO.
 * 4 peripheral accesses per bit.  GT25Q80A requires MOSI to be stable
 * before SCK rises; changing MOSI simultaneously with SCK falling (via
 * combined BSRR write) violates setup time and corrupts reads.
 */
static uint8_t flash_spi_xfer(uint8_t tx) {
    uint8_t rx = 0;
    for (int i = 7; i >= 0; i--) {
        FLASH_SCK_LOW();
        if (tx & (1u << i)) FLASH_MOSI_HI(); else FLASH_MOSI_LO();
        FLASH_SCK_HIGH();
        if (FLASH_MISO_RD()) rx |= (uint8_t)(1u << i);
    }
    FLASH_SCK_LOW();
    return rx;
}

/* Read len bytes from external flash at addr into buf (used for header). */
static void flash_read(uint32_t addr, uint8_t *buf, uint32_t len) {
    FLASH_CS_LOW();
    flash_spi_xfer(CMD_READ);
    flash_spi_xfer((uint8_t)(addr >> 16));
    flash_spi_xfer((uint8_t)(addr >>  8));
    flash_spi_xfer((uint8_t)(addr));
    for (uint32_t i = 0; i < len; i++)
        buf[i] = flash_spi_xfer(0xFF);
    FLASH_CS_HIGH();
}

/* ── Frame streaming ─────────────────────────────────────────────────────── */
/* Pipelined write: issue one continuous CMD_READ for the whole frame, then
 * for every byte — write the current byte to the HW SPI DR, then immediately
 * bit-bang read the NEXT byte from flash while the SPI hardware is transmitting.
 *
 * At 48 MHz PLL:
 *   SPI at 3 MHz (48/16): 1 byte = 8/3 MHz = 2.67 µs
 *   Bit-bang: ~14 cycles/bit × 8 bits = ~112 cycles = ~2.3 µs  ← fits inside SPI window
 *
 * Net throughput: ~2.67 µs/byte (SPI-limited) × 40 960 bytes = ~109 ms/frame → ~9 fps.
 * No SRAM row buffer needed — zero copying.
 */
static void write_frame(uint32_t flash_addr, uint16_t w, uint16_t h) {
    uint32_t total = (uint32_t)w * h * 2u;   /* 40 960 bytes for 128×160 */

    /* SPE toggle clears OVR accumulated during the previous frame's TXE-only
     * burst.  On N32G031, OVR permanently deadlocks TXE after ~2-3 full-screen
     * bursts without this clear; TXE never asserts → while(!TXE) spins forever
     * → IWDG resets the chip → animation appears at ~1fps instead of ~9fps.
     * Must happen BEFORE LCD_CS_LOW to avoid glitching the GC9107 SPI bus. */
    SPI1->CR1 &= ~SPI_CR1_SPE;
    SPI1->CR1 |=  SPI_CR1_SPE;

    /* Prepare display: set window, switch to data mode */
    LCD_CS_LOW();
    display_set_window(0, 0, (uint16_t)(w - 1u), (uint16_t)(h - 1u));
    LCD_DC_DATA();

    /* Start a single continuous READ covering the entire frame */
    FLASH_CS_LOW();
    flash_spi_xfer(CMD_READ);
    flash_spi_xfer((uint8_t)(flash_addr >> 16));
    flash_spi_xfer((uint8_t)(flash_addr >>  8));
    flash_spi_xfer((uint8_t)(flash_addr));

    /* Prime the pipeline with the first byte */
    uint8_t b = flash_spi_xfer(0xFF);

    for (uint32_t i = 1u; i < total; i++) {
        /* Kick off SPI transmission of current byte */
        while (!(SPI1->SR & SPI_SR_TXE));
        *(volatile uint8_t *)&SPI1->DR = b;

        /* While SPI is sending, bit-bang read the next byte from flash.
         * This overlaps almost perfectly with the 2.67 µs SPI transmit window. */
        b = flash_spi_xfer(0xFF);

        /* Feed watchdog every 4096 bytes (~10 ms at 9 fps) */
        if ((i & 0xFFFu) == 0u) IWDG_FEED();
    }

    /* Send the last byte */
    while (!(SPI1->SR & SPI_SR_TXE));
    *(volatile uint8_t *)&SPI1->DR = b;

    FLASH_CS_HIGH();
    /* Fixed drain: BSY polling deadlocks on N32G031 after TXE-only bursts.
     * 300 cycles @ 48 MHz ≈ 6.25 µs = 2× byte-time at 3 MHz SPI — long enough
     * for both the TX-FIFO byte and the shift-register byte to clock out fully. */
    for (volatile uint32_t _d = 300u; _d--; );
    LCD_CS_HIGH();
}

/* ── Main ─────────────────────────────────────────────────────────────────── */
int main(void) {
    IWDG_START();
    IWDG_FEED();

    vape_safety_init();

    clock_init();
    clock_boost_48mhz();      /* 48 MHz → faster bit-bang and SPI */
    IWDG_FEED(); delay_ms(200); IWDG_FEED();

    flash_gpio_init();
    display_init();
    display_set_backlight(1);

    /* Read and validate 16-byte video header from external flash */
    uint8_t hdr[VIDEO_HDR_SZ];
    flash_read(VIDEO_HDR_ADDR, hdr, VIDEO_HDR_SZ);

    uint16_t magic    = (uint16_t)(hdr[0] | ((uint16_t)hdr[1] << 8));
    uint16_t n_frames = (uint16_t)(hdr[2] | ((uint16_t)hdr[3] << 8));
    uint16_t w        = (uint16_t)(hdr[6] | ((uint16_t)hdr[7] << 8));
    uint16_t h        = (uint16_t)(hdr[8] | ((uint16_t)hdr[9] << 8));

    if (magic != VIDEO_MAGIC || w != LCD_WIDTH || h != LCD_HEIGHT || n_frames == 0) {
        display_fill(COL_RGB(220, 30, 30));   /* red = invalid/missing video */
        while (1) { IWDG_FEED(); }
    }

    uint32_t frame_sz   = (uint32_t)w * h * 2u;   /* bytes per frame */
    uint32_t data_start = VIDEO_HDR_ADDR + VIDEO_HDR_SZ;

    /* Playback loop — runs as fast as hardware allows (~6 fps) */
    uint16_t f = 0;
    while (1) {
        IWDG_FEED();
        write_frame(data_start + (uint32_t)f * frame_sz, w, h);
        if (++f >= n_frames) f = 0;
    }
}
