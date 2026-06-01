/* dump_ext_flash.c — Read all 1 MB of external GT25Q80A flash via SRAM handshake.
 *
 * Hardware: N32G031K8Q7-1, GT25Q80A on PA8=CS PA9=SCK PA10=MISO PA11=MOSI (bit-bang).
 * No IWDG — OpenOCD may halt/resume freely for DAP memory access.
 *
 * SRAM handshake (fixed addresses, safely above BSS for this tiny firmware):
 *   0x20000200  status:     0 = filling, 1 = chunk ready, 0xDEADDEAD = all done
 *   0x20000204  chunk_addr: flash start address of current chunk
 *   0x20000400  buf[2048]:  current 2 KB chunk
 *
 * Host (dump_ext_flash.py) polls status; when 1, reads buf and chunk_addr via
 * AHB-AP (non-halting), then writes 0 to status to trigger the next read.
 * Firmware advances addr by CHUNK_SIZE and reads the next chunk.
 *
 * 512 chunks × 2 KB = 1 MB total.
 */
#include <stdint.h>
#include "n32g031.h"

/* Handshake registers at fixed SRAM addresses */
#define STATUS      (*(volatile uint32_t *)0x20000200UL)
#define CHUNK_ADDR  (*(volatile uint32_t *)0x20000204UL)

/* 2 KB data buffer */
static volatile uint8_t * const g_buf = (volatile uint8_t *)0x20000400UL;

#define FLASH_SIZE  0x100000UL   /* 1 MB */
#define CHUNK_SIZE  2048UL       /* bytes per read */
#define NUM_CHUNKS  (FLASH_SIZE / CHUNK_SIZE)   /* 512 */

/* ── GT25Q80A bit-bang SPI (PA8=CS PA9=SCK PA10=MISO PA11=MOSI) ──────────── */
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

static void gpio_init(void) {
    RCC->APB2ENR |= RCC_APB2ENR_IOPAEN;

    /* PA8, PA9, PA11: push-pull output, high speed */
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

    /* PA10: input with pull-up */
    GPIOA->MODER &= ~(3UL << (FLASH_MISO_PIN * 2));
    GPIOA->PUPDR  &= ~(3UL << (FLASH_MISO_PIN * 2));
    GPIOA->PUPDR  |=  (1UL << (FLASH_MISO_PIN * 2));

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

static void read_chunk(uint32_t addr, volatile uint8_t *buf, uint32_t len) {
    FLASH_CS_LOW();
    spi_xfer(CMD_READ);
    spi_xfer((uint8_t)(addr >> 16));
    spi_xfer((uint8_t)(addr >>  8));
    spi_xfer((uint8_t)(addr      ));
    for (uint32_t i = 0; i < len; i++)
        buf[i] = spi_xfer(0xFF);
    FLASH_CS_HIGH();
}

int main(void) {
    gpio_init();
    STATUS = 0;

    for (uint32_t addr = 0; addr < FLASH_SIZE; addr += CHUNK_SIZE) {
        read_chunk(addr, g_buf, CHUNK_SIZE);
        CHUNK_ADDR = addr;
        STATUS = 1;                         /* signal chunk ready */
        while (STATUS != 0) { }            /* wait for host to clear */
    }

    STATUS = 0xDEADDEADUL;                 /* all done */
    while (1) { }
}
