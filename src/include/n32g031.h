/* N32G031 peripheral register definitions
 *
 * Target device: N32G031K8Q7-1 (Raz DC25000 vape hardware)
 *   Core: ARM Cortex-M0, 8 MHz HSI (no PLL used in this firmware)
 *   Flash: 64 KB  SRAM: 8 KB  VDDA: 3.0 V (LDO, not 3.3 V)
 *
 * Addresses verified against NSING N32G031_DFP 1.0.5 official SDK:
 *   APB1 base: 0x40000000
 *   APB2 base: 0x40010000  ← GPIO and SPI1 are HERE
 *   AHB  base: 0x40018000
 *   RCC  base: 0x40021000
 *   FLASH base: 0x40022000
 *
 * GPIO (APB2):
 *   GPIOA @ 0x40010800, GPIOB @ 0x40010C00, GPIOC @ 0x40011000
 *   N32G031 GPIO base addresses are NOT the same as STM32F1 values.
 *
 * GPIO clocks in RCC->APB2ENR (offset +0x18 from RCC_BASE = 0x40021018):
 *   IOPAEN = bit 2, IOPBEN = bit 3, IOPCEN = bit 4
 *
 * SPI1: APB2 + 0x2000 = 0x40012000; SPI1EN = APB2ENR bit 9
 *   (NOT bit 12 — that is the STM32F1 position, which is wrong here)
 *
 * ADC1: base = 0x40020800; ADCEN = AHBENR bit 12
 *   (NOT 0x40012400 — that address does not respond on this device variant)
 *
 * TIM3: APB1 + 0x0400 = 0x40000400; TIM3EN = APB1ENR bit 1
 * TIM1: APB2 + 0x2C00 = 0x40012C00; TIM1EN = APB2ENR bit 11
 *
 * IWDG: base = 0x40003000 (KR=+0, PR=+4, RLR=+8)
 *   feed=0xAAAA, unlock=0x5555, start=0xCCCC
 *
 * NOTE: The hardware RE document ("xbenkozx community") claiming GPIO at
 * 0x50000000 and IOPENR at 0x40021034 was COMPLETELY WRONG.
 * The original addresses in this file (before that doc) were CORRECT.
 *
 * NOTE: SysTick is not functional on this device variant — writes to
 * SYST_CSR are silently discarded.  All timing uses TIM3 / TIM1 instead.
 */
#ifndef N32G031_H
#define N32G031_H

#include <stdint.h>
#include <stdbool.h>

/* ---- Core ---- */
#define SCB_VTOR        (*((volatile uint32_t *)0xE000ED08))

/* ---- RCC ---- */
/* RCC is on AHB at 0x40021000 (AHBPERIPH_BASE=0x40018000, RCC=+0x9000) */
#define RCC_BASE        0x40021000UL
typedef struct {
    volatile uint32_t CR;       /* +0x00  CTRL: clock control */
    volatile uint32_t CFGR;     /* +0x04  CFG:  clock config */
    volatile uint32_t CIR;      /* +0x08  CLKINT: clock interrupt */
    volatile uint32_t APB2RSTR; /* +0x0C  APB2PRST: APB2 reset */
    volatile uint32_t APB1RSTR; /* +0x10  APB1PRST: APB1 reset */
    volatile uint32_t AHBENR;   /* +0x14  AHBPCLKEN: AHB clock enable */
    volatile uint32_t APB2ENR;  /* +0x18  APB2PCLKEN: APB2 clock enable ← GPIO here */
    volatile uint32_t APB1ENR;  /* +0x1C  APB1PCLKEN: APB1 clock enable ← TIM3 here */
    volatile uint32_t BDCR;     /* +0x20  LSCTRL */
    volatile uint32_t CSR;      /* +0x24  CTRLSTS */
    volatile uint32_t AHBRSTR;  /* +0x28  AHBPRST */
    volatile uint32_t CFGR2;    /* +0x2C  CFG2 */
    volatile uint32_t CFGR3;    /* +0x30  EMCCTRL */
} RCC_TypeDef;
#define RCC             ((RCC_TypeDef *)RCC_BASE)

/* RCC_CR bits */
#define RCC_CR_HSION        (1UL << 0)
#define RCC_CR_HSIRDY       (1UL << 1)
#define RCC_CR_PLLON        (1UL << 24)
#define RCC_CR_PLLRDY       (1UL << 25)

/* RCC_CFGR bits */
#define RCC_CFGR_SW_PLL     (0x2UL << 0)
#define RCC_CFGR_SWS_PLL    (0x2UL << 2)
#define RCC_CFGR_PLLSRC_HSI (0UL << 16)
/* PLLMUL: HSI(8MHz) × 6 = 48MHz → bits[21:18]=4 → 0x00100000 */
#define RCC_CFGR_PLLMULL_6  (4UL << 18)

/* RCC_APB2ENR bits (register at RCC_BASE+0x18 = 0x40021018) */
#define RCC_APB2ENR_AFIOEN  (1UL << 0)
#define RCC_APB2ENR_IOPAEN  (1UL << 2)  /* GPIOA clock */
#define RCC_APB2ENR_IOPBEN  (1UL << 3)  /* GPIOB clock */
#define RCC_APB2ENR_IOPCEN  (1UL << 4)  /* GPIOC clock */
#define RCC_APB2ENR_SPI1EN  (1UL << 9)  /* SPI1 clock — bit 9 (NOT bit 12 like STM32F1) */
#define RCC_APB2ENR_TIM1EN  (1UL << 12) /* TIM1 clock — advanced timer, used as 1kHz wall clock (bit 12, NOT 11 — confirmed by OpenOCD register scan on Raz DC25000) */
#define RCC_APB2ENR_USART1EN (1UL << 14)

/* RCC_APB1ENR bits (register at RCC_BASE+0x1C = 0x4002101C) */
#define RCC_APB1ENR_TIM3EN  (1UL << 1)

/* ---- GPIO ---- */
typedef struct {
    volatile uint32_t MODER;    /* +0x00 PMODE */
    volatile uint32_t OTYPER;   /* +0x04 POTYPE */
    volatile uint32_t OSPEEDR;  /* +0x08 SR */
    volatile uint32_t PUPDR;    /* +0x0C PUPD */
    volatile uint32_t IDR;      /* +0x10 PID */
    volatile uint32_t ODR;      /* +0x14 POD */
    volatile uint32_t BSRR;     /* +0x18 PBSC */
    volatile uint32_t LCKR;     /* +0x1C PLOCK */
    volatile uint32_t AFRL;     /* +0x20 AFL */
    volatile uint32_t AFRH;     /* +0x24 AFH */
    volatile uint32_t BRR;      /* +0x28 PBC */
    volatile uint32_t DS;       /* +0x2C drive strength */
} GPIO_TypeDef;

#define GPIOA_BASE  0x40010800UL
#define GPIOB_BASE  0x40010C00UL
#define GPIOC_BASE  0x40011000UL
#define GPIOA       ((GPIO_TypeDef *)GPIOA_BASE)
#define GPIOB       ((GPIO_TypeDef *)GPIOB_BASE)
#define GPIOC       ((GPIO_TypeDef *)GPIOC_BASE)

/* MODER values (2 bits per pin) */
#define GPIO_MODE_INPUT     0x0U
#define GPIO_MODE_OUTPUT    0x1U
#define GPIO_MODE_AF        0x2U
#define GPIO_MODE_ANALOG    0x3U

/* OSPEEDR values */
#define GPIO_SPEED_LOW      0x0U
#define GPIO_SPEED_MEDIUM   0x1U
#define GPIO_SPEED_HIGH     0x3U

/* Pin SET/CLEAR via BSRR */
#define GPIO_SET(port, pin)    ((port)->BSRR = (1UL << (pin)))
#define GPIO_CLR(port, pin)    ((port)->BSRR = (1UL << ((pin) + 16)))
#define GPIO_READ(port, pin)   (((port)->IDR >> (pin)) & 1U)

/* ---- TIM3 (APB1 + 0x0400 = 0x40000400; TIM3EN = APB1ENR bit 1) ----
 *
 * Used as the 1 ms polling timebase for delay_ms().
 * Configuration: PSC=7 → 8MHz HSI / (7+1) = 1MHz tick
 *                ARR=999 → UIF fires every 1000 ticks = 1 ms
 * Note: TIM3 is NOT used for PWM or the backlight — PB4 backlight is
 * plain GPIO (active-LOW). TIM3 is required only for the 1ms timebase.
 */
#define TIM3_BASE   0x40000400UL
typedef struct {
    volatile uint32_t CR1;       /* +0x00 Control 1 (CEN=bit0, ARPE=bit7) */
    volatile uint32_t CR2;       /* +0x04 Control 2 */
    volatile uint32_t SMCR;      /* +0x08 Slave mode control */
    volatile uint32_t DIER;      /* +0x0C DMA/interrupt enable */
    volatile uint32_t SR;        /* +0x10 Status (UIF=bit0, clear by writing 0) */
    volatile uint32_t EGR;       /* +0x14 Event generation (UG=bit0: force reload) */
    volatile uint32_t CCMR1;     /* +0x18 Capture/compare mode 1 */
    volatile uint32_t CCMR2;     /* +0x1C Capture/compare mode 2 */
    volatile uint32_t CCER;      /* +0x20 Capture/compare enable */
    volatile uint32_t CNT;       /* +0x24 Counter value (16-bit) */
    volatile uint32_t PSC;       /* +0x28 Prescaler (divides input clock by PSC+1) */
    volatile uint32_t ARR;       /* +0x2C Auto-reload register (reloads on UIF) */
    volatile uint32_t RESERVED1; /* +0x30 (RCR on TIM1, reserved on TIM3) */
    volatile uint32_t CCR1;      /* +0x34 Capture/compare register 1 */
    volatile uint32_t CCR2;      /* +0x38 */
    volatile uint32_t CCR3;      /* +0x3C */
    volatile uint32_t CCR4;      /* +0x40 */
} TIM_TypeDef;

/* TIM CR1 bits */
#define TIM_CR1_CEN     (1UL << 0)   /* Counter enable */
#define TIM_CR1_ARPE    (1UL << 7)   /* Auto-reload preload enable: ARR changes take
                                       * effect only at UEV rather than immediately.
                                       * Set so PSC/ARR writes don't corrupt a running count. */

/* TIM EGR bits */
#define TIM_EGR_UG      (1UL << 0)   /* Update generation: force-load PSC and ARR shadow
                                       * registers immediately and reset CNT to 0.
                                       * Always clear SR after writing UG — UG sets UIF. */

/* TIM SR bits */
#define TIM_SR_UIF      (1UL << 0)   /* Update interrupt flag (1 ms tick flag for TIM3) */

#define TIM3        ((TIM_TypeDef *)TIM3_BASE)

/* TIM1 — Advanced control timer on APB2 @ 0x40012C00 (TIM1EN = APB2ENR bit 11).
 * Configured as a free-running 16-bit counter at 1 kHz (PSC=7999, ARR=0xFFFF).
 * Read with ms_now() for non-blocking elapsed time in game loops.
 * Wraps every 65535 ms (~65 s) — always use (uint16_t) subtraction for deltas.
 * TIM1 is NOT used for interrupts or PWM in this firmware. */
#define TIM1_BASE   0x40012C00UL
#define TIM1        ((TIM_TypeDef *)TIM1_BASE)

/* ---- SPI1 (APB2 + 0x2000 = 0x40012000; SPI1EN = APB2ENR bit 9) ----
 *
 * Used to drive the GC9107 LCD at up to APB2/2 = 4 MHz (8MHz HSI / 2).
 * Configured in Mode 0 (CPOL=0, CPHA=0), 8-bit, MSB-first, software NSS.
 * CS (PA15) is managed in software — the GC9107 requires CS to stay LOW
 * for the entire window/fill operation, not just per-byte.
 *
 * To write 8-bit data: cast DR address to (volatile uint8_t *) before write.
 * Writing a 32-bit value to DR triggers a 16-bit transfer on this device.
 */
#define SPI1_BASE   0x40012000UL
typedef struct {
    volatile uint32_t CR1;    /* +0x00 Control 1 */
    volatile uint32_t CR2;    /* +0x04 Control 2 */
    volatile uint32_t SR;     /* +0x08 Status */
    volatile uint32_t DR;     /* +0x0C Data register — write as uint8_t for 8-bit frames */
    volatile uint32_t CRCPR;  /* +0x10 CRC polynomial */
    volatile uint32_t RXCRCR; /* +0x14 RX CRC */
    volatile uint32_t TXCRCR; /* +0x18 TX CRC */
    volatile uint32_t I2SCFGR;/* +0x1C I2S config (not used) */
    volatile uint32_t I2SPR;  /* +0x20 I2S prescaler (not used) */
} SPI_TypeDef;
#define SPI1        ((SPI_TypeDef *)SPI1_BASE)

/* SPI_CR1 bits */
#define SPI_CR1_CPHA        (1UL << 0)   /* Clock phase: 0=first edge (Mode 0/2) */
#define SPI_CR1_CPOL        (1UL << 1)   /* Clock polarity: 0=idle LOW (Mode 0/1) */
#define SPI_CR1_MSTR        (1UL << 2)   /* Master mode select */
/* BR[2:0] = bits[5:3]: baud rate = APB2_CLK / (2 << BR) */
#define SPI_CR1_BR_DIV2     (0UL << 3)   /* APB2/2   = 4 MHz at 8 MHz HSI */
#define SPI_CR1_BR_DIV4     (1UL << 3)   /* APB2/4   = 2 MHz */
#define SPI_CR1_BR_DIV8     (2UL << 3)
#define SPI_CR1_BR_DIV16    (3UL << 3)
#define SPI_CR1_BR_DIV32    (4UL << 3)
#define SPI_CR1_BR_DIV64    (5UL << 3)
#define SPI_CR1_BR_DIV128   (6UL << 3)
#define SPI_CR1_BR_DIV256   (7UL << 3)
#define SPI_CR1_SPE         (1UL << 6)   /* SPI enable */
#define SPI_CR1_LSBFIRST    (1UL << 7)   /* 0=MSB first (GC9107 requires MSB first) */
#define SPI_CR1_SSI         (1UL << 8)   /* Internal slave select (must be 1 in master+SSM) */
#define SPI_CR1_SSM         (1UL << 9)   /* Software slave management (NSS controlled by SSI) */
#define SPI_CR1_DFF         (1UL << 11)  /* Data frame format: 0=8-bit, 1=16-bit */
#define SPI_CR1_BIDIMODE    (1UL << 15)  /* Bidirectional mode (not used) */

/* SPI_SR bits */
#define SPI_SR_RXNE         (1UL << 0)   /* RX buffer not empty */
#define SPI_SR_TXE          (1UL << 1)   /* TX buffer empty (safe to write next byte) */
#define SPI_SR_BSY          (1UL << 7)   /* Busy: transfer in progress; wait before deasserting CS */

/* ---- Flash interface (AHB + 0xA000 = 0x40022000) ----
 *
 * Flash on the N32G031 is written in 32-bit words (half-word writes do not work).
 * Page size = 512 bytes.  Erase granularity = one page.
 *
 * Unlock sequence: write 0x45670123 then 0xCDEF89AB to KEYR.
 * These are the ARM Cortex standard flash unlock keys (not device-specific).
 * After programming or erase is complete, set LOCK bit to re-lock.
 *
 * NV storage occupies the last 4 KB (8 pages) at 0x0800F000–0x0800FFFF.
 */
#define FLASH_BASE  0x40022000UL
typedef struct {
    volatile uint32_t ACR;     /* +0x00 Access control (wait states, prefetch) */
    volatile uint32_t KEYR;    /* +0x04 Unlock key register — write unlock sequence here */
    volatile uint32_t OPTKEYR; /* +0x08 Option byte key (not used) */
    volatile uint32_t SR;      /* +0x0C Status: BSY=bit0, EOP=bit5, WRPRTERR=bit4 */
    volatile uint32_t CR;      /* +0x10 Control: PG=bit0, PER=bit1, STRT=bit6, LOCK=bit7 */
    volatile uint32_t AR;      /* +0x14 Address register: set before STRT on erase */
    volatile uint32_t RESERVED;/* +0x18 */
    volatile uint32_t OBR;     /* +0x1C Option byte register */
    volatile uint32_t WRPR;    /* +0x20 Write protection register */
} FLASH_TypeDef;
#define FLASH_IF    ((FLASH_TypeDef *)FLASH_BASE)

/* FLASH_CR bit constants */
#define FLASH_CR_PG     (1UL << 0)  /* Programming enable: set before 32-bit word write */
#define FLASH_CR_PER    (1UL << 1)  /* Page erase: set AR, then set STRT */
#define FLASH_CR_STRT   (1UL << 6)  /* Start: triggers page erase when PER is set */
#define FLASH_CR_LOCK   (1UL << 7)  /* Lock: set to lock CR (cleared by unlock sequence) */

/* FLASH_SR bit constants */
#define FLASH_SR_BSY    (1UL << 0)  /* Busy: poll until clear after program/erase */

/* Flash unlock keys (ARM Cortex standard — same on all N32G031 parts) */
#define FLASH_KEY1  0x45670123UL
#define FLASH_KEY2  0xCDEF89ABUL

#define FLASH_ACR_LATENCY_1WS  (1UL << 0)
#define FLASH_ACR_PRFTBE       (1UL << 4)
#define FLASH_ACR_INIT_48MHZ   (FLASH_ACR_PRFTBE | FLASH_ACR_LATENCY_1WS)

/* ---- ADC1 (AHB; ADCEN = AHBENR bit 12; base = 0x40020800) ----
 *
 * Confirmed live via OpenOCD register reads on Raz DC25000 (2026-04-09).
 * Do NOT use 0x40012400 — that address does not respond on this device variant.
 *
 * Battery sense: PB0 / ADC channel 8.
 * Empirical voltage formula (actual divider ratio ~0.71):
 *   Vbat = raw * 1.41 * 3.0 / 4096
 * (config.h uses a 1:28 approximation for threshold derivation; the actual
 *  ratio differs slightly — use the thresholds in config.h directly rather
 *  than computing from the DIVIDER constant.)
 *
 * Register offsets (relative to VAPE_ADC_BASE):
 *   +0x00  STS   — status (EOC=bit1; clear by writing 0 before conversion)
 *   +0x08  CTRL2 — control 2 (ADON=bit0, SWSTRRCH=bit22, EXTSEL/EXTTRIG)
 *   +0x10  SMPR2 — sample time for channels 0-9 (3 bits per channel)
 *   +0x30  RSEQ1 — regular sequence length (0=1 conversion)
 *   +0x38  RSEQ3 — regular sequence register 1 (channel number for 1st conv)
 *   +0x50  DAT   — data register (12-bit result in bits[11:0])
 *
 * SMPR2 channel 8 occupies bits[26:24]; value 7 = 239.5-cycle sample time.
 * The long sample time is needed because the ~96 kΩ Thevenin source impedance
 * on the resistor divider charges the ADC sample capacitor slowly.
 *
 * SWSTRRCH = bit 22 of CTRL2: software-start regular channel conversion.
 * Write 1 to begin conversion; hardware clears it.  Poll STS bit 1 (EOC).
 */
#define VAPE_ADC_BASE  0x40020800UL

/* ---- SysTick ---- */
#define SYSTICK_BASE    0xE000E010UL
typedef struct {
    volatile uint32_t CTRL;
    volatile uint32_t LOAD;
    volatile uint32_t VAL;
    volatile uint32_t CALIB;
} SysTick_TypeDef;
#define SYSTICK     ((SysTick_TypeDef *)SYSTICK_BASE)

/* ---- IWDG (base = 0x40003000) ----
 *
 * Independent watchdog — clocked from the internal LSI oscillator (~40 kHz),
 * so it keeps running regardless of main clock state.
 *
 * Register map:
 *   KR  (+0x00): write-only key register.  Three magic values:
 *     0xAAAA — reload counter (feed/pat the dog)
 *     0x5555 — unlock PR and RLR for write
 *     0xCCCC — start the watchdog (if in software-start mode)
 *   PR  (+0x04): prescaler (0=LSI/4=~10kHz, 4=LSI/64=~625Hz, etc.)
 *   RLR (+0x08): reload value (counter reloads with this on feed)
 *
 * In this firmware the IWDG is configured by hardware option bytes (assumed
 * hardware-start mode).  IWDG_START() is called at boot defensively in case
 * the device is in software-start mode.
 *
 * delay_ms() feeds the IWDG every 1 ms, so the watchdog timeout must be set
 * longer than the longest blocking operation (LCD init ≈ 300 ms).
 * bat_read_raw() also feeds the IWDG during the conversion poll loop.
 */
#define IWDG_KR       (*(volatile uint32_t *)0x40003000UL)
#define IWDG_FEED()   (IWDG_KR = 0xAAAAUL)   /* reload counter — call at least once per timeout period */
#define IWDG_START()  (IWDG_KR = 0xCCCCUL)   /* safe to call even if already running */

/* ---- NVIC ---- */
#define NVIC_BASE       0xE000E100UL

#endif /* N32G031_H */
