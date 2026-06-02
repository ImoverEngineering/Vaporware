/* ext_flash_writer.c — Program GT25Q80A external flash via SRAM handshake.
 *
 * Used by gen_flash.py / combined_flash.tcl to write the video blob to
 * external flash before flashing the playback firmware.
 *
 * Hardware: N32G031K8Q7-1, GT25Q80A on PA8=CS PA9=SCK PA10=MISO PA11=MOSI.
 *
 * Calls clock_boost_48mhz() so the host can use adapter speed 4000 for
 * faster SWD data transfer during the SRAM handshake write loop.
 *
 * SRAM handshake (fixed addresses):
 *   0x20000200  cmd:    0 = idle/wait, 1 = erase 4 KB sector, 2 = program page
 *   0x20000204  addr:   target flash address
 *   0x20000208  status: 0 = busy, 1 = ready (host may issue next command)
 *   0x20000400  buf[256]: page data for cmd=2 (256 bytes = one flash page)
 *
 * Host sequence per sector (4 KB = 16 pages):
 *   1. Wait status=1
 *   2. Write addr = sector_base, cmd = 1 (erase)
 *   3. Wait status=1  (sector erase takes ~100 ms)
 *   4. For each of 16 pages:
 *        Write 256 bytes of data to buf (64 × mww)
 *        Write addr = page_addr, cmd = 2
 *        Wait status=1  (~3 ms)
 *
 * End: write cmd = 0xDEADDEAD to exit loop.
 */
#include <stdint.h>
#include "n32g031.h"
#include "system.h"

/* Handshake registers */
#define CMD_REG    (*(volatile uint32_t *)0x20000200UL)
#define ADDR_REG   (*(volatile uint32_t *)0x20000204UL)
#define STATUS_REG (*(volatile uint32_t *)0x20000208UL)

static volatile uint8_t * const g_buf = (volatile uint8_t *)0x20000400UL;

/* ── GT25Q80A bit-bang SPI ────────────────────────────────────────────────── */
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

/* GT25Q80A commands */
#define CMD_WREN  0x06u
#define CMD_RDSR  0x05u
#define CMD_SE    0x20u   /* sector erase 4 KB */
#define CMD_PP    0x02u   /* page program 256 B */

static void gpio_init(void) {
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

static uint8_t spi_xfer(uint8_t tx) {
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

static void flash_wait_ready(void) {
    uint32_t timeout = 2000000UL;
    FLASH_CS_LOW();
    spi_xfer(CMD_RDSR);
    while ((spi_xfer(0xFF) & 0x01u) && timeout--)
        IWDG_FEED();   /* sector erase can take up to 400 ms — keep watchdog happy */
    FLASH_CS_HIGH();
}

static void flash_write_enable(void) {
    FLASH_CS_LOW();
    spi_xfer(CMD_WREN);
    FLASH_CS_HIGH();
}

static void flash_erase_sector(uint32_t addr) {
    flash_write_enable();
    FLASH_CS_LOW();
    spi_xfer(CMD_SE);
    spi_xfer((uint8_t)(addr >> 16));
    spi_xfer((uint8_t)(addr >>  8));
    spi_xfer((uint8_t)(addr      ));
    FLASH_CS_HIGH();
    flash_wait_ready();
}

static void flash_page_program(uint32_t addr, volatile const uint8_t *buf, uint32_t len) {
    flash_write_enable();
    FLASH_CS_LOW();
    spi_xfer(CMD_PP);
    spi_xfer((uint8_t)(addr >> 16));
    spi_xfer((uint8_t)(addr >>  8));
    spi_xfer((uint8_t)(addr      ));
    for (uint32_t i = 0; i < len; i++)
        spi_xfer(buf[i]);
    FLASH_CS_HIGH();
    flash_wait_ready();
}

int main(void) {
    clock_init();
    clock_boost_48mhz();   /* 48 MHz → host may use adapter speed 4000 */
    gpio_init();

    STATUS_REG = 1;        /* signal ready */
    CMD_REG    = 0;

    while (1) {
        IWDG_FEED();       /* keep watchdog happy while idle-waiting for host */
        uint32_t cmd = CMD_REG;
        if (cmd == 0)
            continue;
        if (cmd == 0xDEADDEADUL)
            break;

        STATUS_REG = 0;    /* busy */
        uint32_t addr = ADDR_REG;

        if (cmd == 1) {
            flash_erase_sector(addr);
        } else if (cmd == 2) {
            flash_page_program(addr, g_buf, 256);
        }

        CMD_REG    = 0;
        STATUS_REG = 1;    /* ready for next command */
    }

    while (1) { }
}
