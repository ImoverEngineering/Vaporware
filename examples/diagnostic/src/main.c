/* diagnostic/src/main.c — ADC channel scanner (v5)
 *
 * Just reads four ADC channels and shows raw values with min/max.
 * No puff detection logic — let's just see which pin moves on a draw.
 *
 * Channels:
 *   CH1  / PA1 — primary suspect (Ghidra: puff ch1)
 *   CH2  / PA2 — secondary suspect
 *   CH0  / PA0 — read as ADC (not digital this time)
 *   CH13       — N32G031 internal VBG reference (~1.2V → ~1638 counts on 3V)
 *                Ghidra formula: intensity = (CH1 × CH13) / denominator
 *
 * Instructions:
 *   1. Flash this, then UNPLUG ST-LINK completely.
 *   2. Run on battery only.
 *   3. Draw on the vape and watch which row moves.
 *   4. Button resets all min/max.
 *
 * Display: 128×160 GC9107
 */
#include "app.h"
#include "display.h"
#include "button.h"
#include "system.h"
#include "config.h"

/* ── ADC registers ────────────────────────────────────────────────── */
#define ADC_BASE  VAPE_ADC_BASE
#define ADC_STS   (*(volatile uint32_t *)(ADC_BASE + 0x00))
#define ADC_CTRL2 (*(volatile uint32_t *)(ADC_BASE + 0x08))
#define ADC_SMPR1 (*(volatile uint32_t *)(ADC_BASE + 0x0C))
#define ADC_SMPR2 (*(volatile uint32_t *)(ADC_BASE + 0x10))
#define ADC_RSEQ3 (*(volatile uint32_t *)(ADC_BASE + 0x38))
#define ADC_DAT   (*(volatile uint32_t *)(ADC_BASE + 0x50))

#define GPIOA_BASE 0x40010800UL
#define GPIOB_BASE 0x40010C00UL

/* ── Colours ──────────────────────────────────────────────────────── */
#define C_BG       COL_RGB(  4,   4,  12)
#define C_HDR_BG   COL_RGB( 18,  12,  45)
#define C_HDR      COL_RGB(200, 200, 255)
#define C_LABEL    COL_RGB(100, 100, 150)
#define C_VAL      COL_RGB(240, 240, 255)
#define C_BAR_BG   COL_RGB( 18,  18,  40)
#define C_LO       COL_RGB(255, 180,  30)
#define C_HI       COL_RGB( 80, 255, 120)
#define C_HINT     COL_RGB( 45,  45,  75)
#define C_MOVED    COL_RGB(255, 255,  60)   /* highlight channel that moved */

static const uint16_t BAR_COL[4] = {
    COL_RGB( 80, 255, 120),   /* CH1/PA1 — green  */
    COL_RGB(100, 180, 255),   /* CH2/PA2 — blue   */
    COL_RGB(255, 160,  60),   /* CH0/PA0 — orange */
    COL_RGB(200,  80, 255),   /* CH13    — purple */
};

/* ── Minimal font ─────────────────────────────────────────────────── */
static const uint8_t F_DIG[][5] = {
    {0x3E,0x51,0x49,0x45,0x3E},{0x00,0x42,0x7F,0x40,0x00},
    {0x42,0x61,0x51,0x49,0x46},{0x21,0x41,0x45,0x4B,0x31},
    {0x18,0x14,0x12,0x7F,0x10},{0x27,0x45,0x45,0x45,0x39},
    {0x3C,0x4A,0x49,0x49,0x30},{0x01,0x71,0x09,0x05,0x03},
    {0x36,0x49,0x49,0x49,0x36},{0x06,0x49,0x49,0x29,0x1E},
};
static const uint8_t F_AZ[][5] = {
    {0x7E,0x11,0x11,0x11,0x7E},{0x7F,0x49,0x49,0x49,0x36},
    {0x3E,0x41,0x41,0x41,0x22},{0x7F,0x41,0x41,0x22,0x1C},
    {0x7F,0x49,0x49,0x49,0x41},{0x7F,0x09,0x09,0x09,0x01},
    {0x3E,0x41,0x49,0x49,0x7A},{0x7F,0x08,0x08,0x08,0x7F},
    {0x00,0x41,0x7F,0x41,0x00},{0x20,0x40,0x41,0x3F,0x01},
    {0x7F,0x08,0x14,0x22,0x41},{0x7F,0x40,0x40,0x40,0x40},
    {0x7F,0x02,0x0C,0x02,0x7F},{0x7F,0x04,0x08,0x10,0x7F},
    {0x3E,0x41,0x41,0x41,0x3E},{0x7F,0x09,0x09,0x09,0x06},
    {0x3E,0x41,0x51,0x21,0x5E},{0x7F,0x09,0x19,0x29,0x46},
    {0x46,0x49,0x49,0x49,0x31},{0x01,0x01,0x7F,0x01,0x01},
    {0x3F,0x40,0x40,0x40,0x3F},{0x1F,0x20,0x40,0x20,0x1F},
    {0x3F,0x40,0x38,0x40,0x3F},{0x63,0x14,0x08,0x14,0x63},
    {0x07,0x08,0x70,0x08,0x07},{0x61,0x51,0x49,0x45,0x43},
};
static const uint8_t F_MISC[][5] = {
    /* 0 = slash '/' */ {0x00,0x60,0x18,0x06,0x01},
    /* 1 = colon ':' */ {0x00,0x00,0x36,0x36,0x00},
    /* 2 = dot   '.' */ {0x00,0x00,0x60,0x60,0x00},
    /* 3 = dash  '-' */ {0x08,0x08,0x08,0x08,0x08},
};

static void px(int x, int y, uint16_t c) {
    display_draw_pixel((uint16_t)x,(uint16_t)y,c);
}
static void draw_char(int x, int y, char c, uint16_t col) {
    const uint8_t *g = 0;
    if (c>='0'&&c<='9')      g = F_DIG[(uint8_t)(c-'0')];
    else if (c>='A'&&c<='Z') g = F_AZ[(uint8_t)(c-'A')];
    else if (c>='a'&&c<='z') g = F_AZ[(uint8_t)(c-'a')];
    else if (c=='/')  g = F_MISC[0];
    else if (c==':')  g = F_MISC[1];
    else if (c=='.')  g = F_MISC[2];
    else if (c=='-')  g = F_MISC[3];
    if (!g) return;
    for (int i=0;i<5;i++){uint8_t b=g[i];for(int r=0;r<7;r++)if(b&(1u<<r))px(x+i,y+r,col);}
}
static void draw_str(int x, int y, const char *s, uint16_t col) {
    while(*s){draw_char(x,y,*s++,col);x+=6;}
}
static void draw_val4(int right_x, int y, uint16_t n, uint16_t col) {
    char buf[5]; int i=0;
    if(!n){buf[i++]='0';}
    else{uint16_t t=n;while(t){buf[i++]=(char)('0'+t%10);t/=10;}}
    for(int l=0,r=i-1;l<r;l++,r--){char t=buf[l];buf[l]=buf[r];buf[r]=t;}
    buf[i]=0;
    int w=i*6;
    display_fill_rect((uint16_t)(right_x-w),(uint16_t)y,(uint16_t)w,8,C_BG);
    draw_str(right_x-w,y,buf,col);
}

/* ── ADC ──────────────────────────────────────────────────────────── */
static uint16_t adc_read_once(uint8_t ch) {
    if (ch<=9) ADC_SMPR2 |= (7UL<<(ch*3));
    else        ADC_SMPR1 |= (7UL<<((ch-10)*3));
    ADC_RSEQ3 = ch;
    ADC_STS   = 0;
    ADC_CTRL2|= (1UL<<22);
    for(uint32_t i=0;i<2000u&&!(ADC_STS&2U);i++) IWDG_FEED();
    return (uint16_t)(ADC_DAT & 0xFFFu);
}

#define OVERSAMPLE 16
static uint16_t adc_read(uint8_t ch) {
    uint32_t acc = 0;
    for (uint8_t i = 0; i < OVERSAMPLE; i++)
        acc += adc_read_once(ch);
    return (uint16_t)(acc / OVERSAMPLE);
}

/* ── Row layout ───────────────────────────────────────────────────── */
/* 128×160 display
 * Header:  y=0  h=12
 * Row 0:   y=13 h=34  (label+bar+minmax)
 * Row 1:   y=48 h=34
 * Row 2:   y=83 h=34
 * Row 3:   y=118 h=34
 * Footer:  y=153 h=7
 */
#define ROW_Y(i) (13 + (i)*35)
#define BAR_X    4
#define BAR_W    120
#define BAR_H    8

static uint16_t g_prev_fill[4];
static uint16_t g_min[4];
static uint16_t g_max[4];
static uint16_t g_prev_val[4];
static uint16_t g_prev_min[4];
static uint16_t g_prev_max[4];

static void init_row(int y, const char *label) {
    draw_str(BAR_X, y, label, C_LABEL);
    display_fill_rect(BAR_X, (uint16_t)(y+9), BAR_W, BAR_H, C_BAR_BG);
    draw_str(BAR_X, y+20, "LO:", C_LABEL);
    draw_str(66,    y+20, "HI:", C_LABEL);
}

static void update_row(int idx, int y, uint16_t val) {
    if (val < g_min[idx]) g_min[idx] = val;
    if (val > g_max[idx]) g_max[idx] = val;

    /* Bar */
    uint16_t fill = (uint16_t)((uint32_t)val * BAR_W / 4095u);
    if (fill > BAR_W) fill = BAR_W;
    uint16_t prev = g_prev_fill[idx];
    if (fill > prev)
        display_fill_rect((uint16_t)(BAR_X+prev),(uint16_t)(y+9),
                          (uint16_t)(fill-prev),BAR_H,BAR_COL[idx]);
    else if (fill < prev)
        display_fill_rect((uint16_t)(BAR_X+fill),(uint16_t)(y+9),
                          (uint16_t)(prev-fill),BAR_H,C_BAR_BG);
    g_prev_fill[idx] = fill;

    /* Current value — right side of label row */
    if (val != g_prev_val[idx]) {
        draw_val4(127, y, val, C_VAL);
        g_prev_val[idx] = val;
    }

    /* Min */
    if (g_min[idx] != g_prev_min[idx]) {
        draw_val4(60, y+20, g_min[idx], C_LO);
        g_prev_min[idx] = g_min[idx];
    }
    /* Max */
    if (g_max[idx] != g_prev_max[idx]) {
        draw_val4(127, y+20, g_max[idx], C_HI);
        g_prev_max[idx] = g_max[idx];
    }
}

/* ── App ──────────────────────────────────────────────────────────── */
void app_init(void) {
    app_set_sleep_timeout(0);

    /* Set PA0, PA1, PA2 to analog mode */
    volatile uint32_t *pmode_a = (volatile uint32_t *)(GPIOA_BASE + 0x00);
    *pmode_a |= (3UL<<(0*2)) | (3UL<<(1*2)) | (3UL<<(2*2));

    /* Enable internal VBG/Vref for CH13 reads (TSVREFE = bit 23 of CTRL2) */
    ADC_CTRL2 |= (1UL << 23);

    /* Init trackers */
    for (int i=0;i<4;i++){
        g_min[i]=0xFFFF; g_max[i]=0;
        g_prev_fill[i]=0; g_prev_val[i]=0xFFFF;
        g_prev_min[i]=0xFFFF; g_prev_max[i]=0;
    }

    /* Static chrome */
    display_fill(C_BG);
    display_fill_rect(0, 0, 128, 12, C_HDR_BG);
    draw_str(4, 2, "ADC SCANNER", C_HDR);
    draw_str(82, 2, "BATT ONLY", COL_RGB(255,140,40));

    init_row(ROW_Y(0), "CH1 PA1");
    init_row(ROW_Y(1), "CH2 PA2");
    init_row(ROW_Y(2), "CH0 PA0");
    init_row(ROW_Y(3), "CH13 VBG");

    display_fill_rect(0, 153, 128, 7, C_HDR_BG);
    draw_str(2, 154, "BTN=RESET  DRAW=?", C_HINT);
}

void app_update(uint32_t frame) {
    (void)frame;

    uint16_t ch1  = adc_read(1);    /* PA1 */
    uint16_t ch2  = adc_read(2);    /* PA2 */
    uint16_t ch0  = adc_read(0);    /* PA0 */
    uint16_t ch13 = adc_read(13);   /* VBG internal reference ~1638 on 3V */

    update_row(0, ROW_Y(0), ch1);
    update_row(1, ROW_Y(1), ch2);
    update_row(2, ROW_Y(2), ch0);
    update_row(3, ROW_Y(3), ch13);

    /* Highlight in header: show which channel has widest spread (most movement) */
    {
        uint8_t most = 0;
        uint16_t spread = 0;
        for (int i=0;i<4;i++) {
            uint16_t s = (g_max[i] > g_min[i]) ? (g_max[i] - g_min[i]) : 0;
            if (s > spread) { spread = s; most = (uint8_t)i; }
        }
        if (spread > 10) {
            /* show which channel is moving most */
            static uint8_t prev_most = 0xFF;
            if (most != prev_most || spread > 10) {
                display_fill_rect(0, 0, 128, 12, C_HDR_BG);
                draw_str(4, 2, "ADC SCANNER", C_HDR);
                const char *names[4] = {"CH1","CH2","CH0","C13"};
                draw_str(78, 2, names[most], C_MOVED);
                draw_str(100, 2, "MOVES", C_MOVED);
                prev_most = most;
            }
        }
    }

    if (button_just_pressed()) {
        for (int i=0;i<4;i++){
            g_min[i]=0xFFFF; g_max[i]=0;
            g_prev_val[i]=0xFFFF;
            g_prev_min[i]=0xFFFF; g_prev_max[i]=0;
        }
        /* Redraw static chrome for all rows */
        display_fill(C_BG);
        display_fill_rect(0, 0, 128, 12, C_HDR_BG);
        draw_str(4, 2, "ADC SCANNER", C_HDR);
        draw_str(82, 2, "BATT ONLY", COL_RGB(255,140,40));
        init_row(ROW_Y(0), "CH1 PA1");
        init_row(ROW_Y(1), "CH2 PA2");
        init_row(ROW_Y(2), "CH0 PA0");
        init_row(ROW_Y(3), "CH13 VBG");
        display_fill_rect(0, 153, 128, 7, C_HDR_BG);
        draw_str(2, 154, "BTN=RESET  DRAW=?", C_HINT);
        for (int i=0;i<4;i++) g_prev_fill[i]=0;
    }
}

void app_wake(void) { app_init(); }
