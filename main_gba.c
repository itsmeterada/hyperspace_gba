/*
 * Hyperspace - GBA Port
 * Original game by J-Fry for PICO-8
 * GBA port by itsmeterada
 * Mode 5: 160x128, 15-bit color, double buffered
 */

#include <string.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>

// Include sprite data early (needed for SGET_FAST macro)
#include "../hyperspace_data.h"

// GBA hardware definitions
typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef signed char s8;
typedef signed short s16;
typedef signed int s32;
typedef long long s64;
typedef unsigned long long u64;

// Place large data in EWRAM (256KB) instead of IWRAM (32KB)
#define EWRAM_DATA __attribute__((section(".ewram")))
// Place hot code/data in IWRAM (32KB, fast)
#define IWRAM_CODE __attribute__((section(".iwram"), long_call))
#define IWRAM_DATA __attribute__((section(".iwram")))

#define REG_DISPCNT     (*(volatile u16*)0x04000000)
#define REG_KEYINPUT    (*(volatile u16*)0x04000130)
#define REG_VCOUNT      (*(volatile u16*)0x04000006)

// DMA registers
#define REG_DMA3SAD     (*(volatile u32*)0x040000D4)
#define REG_DMA3DAD     (*(volatile u32*)0x040000D8)
#define REG_DMA3CNT     (*(volatile u32*)0x040000DC)
#define DMA_ENABLE      0x80000000
#define DMA_16BIT       0x00000000
#define DMA_32BIT       0x04000000
#define DMA_SRC_FIXED   0x01000000
#define DMA_DST_INC     0x00000000

// BG2 affine transformation registers
#define REG_BG2PA       (*(volatile u16*)0x04000020)
#define REG_BG2PB       (*(volatile u16*)0x04000022)
#define REG_BG2PC       (*(volatile u16*)0x04000024)
#define REG_BG2PD       (*(volatile u16*)0x04000026)
#define REG_BG2X        (*(volatile u32*)0x04000028)
#define REG_BG2Y        (*(volatile u32*)0x0400002C)

#define DCNT_MODE5      0x0005
#define DCNT_BG2        0x0400
#define DCNT_PAGE       0x0010

#define KEY_A           0x0001
#define KEY_B           0x0002
#define KEY_SELECT      0x0004
#define KEY_START       0x0008
#define KEY_RIGHT       0x0010
#define KEY_LEFT        0x0020
#define KEY_UP          0x0040
#define KEY_DOWN        0x0080
#define KEY_R           0x0100
#define KEY_L           0x0200

#define VRAM_PAGE1      ((volatile u16*)0x06000000)
#define VRAM_PAGE2      ((volatile u16*)0x0600A000)
#define SRAM            ((volatile u8*)0x0E000000)

#define RGB15(r,g,b)    (((r)&31) | (((g)&31)<<5) | (((b)&31)<<10))

// =============================================================================
// GBA Sound Registers
// =============================================================================

// Sound control registers
#define REG_SOUNDCNT_L  (*(volatile u16*)0x04000080)  // Channel L/R volume & enable
#define REG_SOUNDCNT_H  (*(volatile u16*)0x04000082)  // DMA sound control
#define REG_SOUNDCNT_X  (*(volatile u16*)0x04000084)  // Master sound enable

// Channel 1 - Square wave with sweep
#define REG_SOUND1CNT_L (*(volatile u16*)0x04000060)  // Sweep
#define REG_SOUND1CNT_H (*(volatile u16*)0x04000062)  // Duty/Length/Envelope
#define REG_SOUND1CNT_X (*(volatile u16*)0x04000064)  // Frequency/Control

// Channel 2 - Square wave
#define REG_SOUND2CNT_L (*(volatile u16*)0x04000068)  // Duty/Length/Envelope
#define REG_SOUND2CNT_H (*(volatile u16*)0x0400006C)  // Frequency/Control

// Channel 3 - Wave output (not used for now)
#define REG_SOUND3CNT_L (*(volatile u16*)0x04000070)
#define REG_SOUND3CNT_H (*(volatile u16*)0x04000072)
#define REG_SOUND3CNT_X (*(volatile u16*)0x04000074)

// Channel 4 - Noise
#define REG_SOUND4CNT_L (*(volatile u16*)0x04000078)  // Length/Envelope
#define REG_SOUND4CNT_H (*(volatile u16*)0x0400007C)  // Frequency/Control

// Sound enable flags
#define SOUND_ENABLE    0x0080  // Master sound enable
#define SOUND1_ENABLE   0x0001
#define SOUND2_ENABLE   0x0002
#define SOUND3_ENABLE   0x0004
#define SOUND4_ENABLE   0x0008
#define SOUND1_L        0x0100
#define SOUND1_R        0x1000
#define SOUND2_L        0x0200
#define SOUND2_R        0x2000
#define SOUND3_L        0x0400
#define SOUND3_R        0x4000
#define SOUND4_L        0x0800
#define SOUND4_R        0x8000

// Mode 5 framebuffer dimensions (scaled to fill 240x160 screen)
#define SCREEN_WIDTH    160
#define SCREEN_HEIGHT   128

// Fixed-point math
typedef s32 fix16_t;
#define fix16_one       0x00010000
#define fix16_pi        0x0003243F
#define F16(x)          ((fix16_t)((x) * 65536.0f))

// ARM assembly optimized functions (in raster_arm.s)
extern fix16_t fix16_mul_arm(fix16_t a, fix16_t b);
extern void render_scanline_arm(volatile u16* row, int xl, int xr, fix16_t u,
                                 fix16_t v, fix16_t du_dx, fix16_t dv_dx,
                                 const u8* spritesheet, const u16* palette,
                                 int tex_offset_x, int tex_y);
extern void fast_memset16_arm(volatile u16* dest, u16 value, u32 count);

// Use ARM assembly for critical multiply
#define fix16_mul(a, b) fix16_mul_arm(a, b)

// Optimized division - avoid when possible, use reciprocal multiply instead
static inline fix16_t fix16_div(fix16_t a, fix16_t b) {
    if (b == 0) return 0;
    // For small divisors, use direct division
    return (fix16_t)(((s64)a << 16) / b);
}

// Fast reciprocal: returns 1/x in fixed point (for x in 0.5 to 128 range)
static inline fix16_t fix16_recip(fix16_t x) {
    if (x == 0) return 0;
    if (x < 0) return -fix16_recip(-x);
    // 1.0 / x = 65536 * 65536 / x
    return (fix16_t)(((s64)0x100000000LL) / x);
}

static inline fix16_t fix16_from_int(int a) { return a << 16; }
static inline int fix16_to_int(fix16_t a) { return a >> 16; }
static inline fix16_t fix16_floor(fix16_t x) { return x & 0xFFFF0000; }
static inline fix16_t fix16_abs(fix16_t x) { return x < 0 ? -x : x; }
static inline fix16_t fix16_min(fix16_t a, fix16_t b) { return a < b ? a : b; }
static inline fix16_t fix16_max(fix16_t a, fix16_t b) { return a > b ? a : b; }

// Sin/Cos LUT (256 entries)
static const s16 sin_lut[256] = {
    0,402,804,1205,1606,2006,2404,2801,3196,3590,3981,4370,4756,5139,5520,5897,
    6270,6639,7005,7366,7723,8076,8423,8765,9102,9434,9760,10080,10394,10702,11003,11297,
    11585,11866,12140,12406,12665,12916,13160,13395,13623,13842,14053,14256,14449,14635,14811,14978,
    15137,15286,15426,15557,15679,15791,15893,15986,16069,16143,16207,16261,16305,16340,16364,16379,
    16384,16379,16364,16340,16305,16261,16207,16143,16069,15986,15893,15791,15679,15557,15426,15286,
    15137,14978,14811,14635,14449,14256,14053,13842,13623,13395,13160,12916,12665,12406,12140,11866,
    11585,11297,11003,10702,10394,10080,9760,9434,9102,8765,8423,8076,7723,7366,7005,6639,
    6270,5897,5520,5139,4756,4370,3981,3590,3196,2801,2404,2006,1606,1205,804,402,
    0,-402,-804,-1205,-1606,-2006,-2404,-2801,-3196,-3590,-3981,-4370,-4756,-5139,-5520,-5897,
    -6270,-6639,-7005,-7366,-7723,-8076,-8423,-8765,-9102,-9434,-9760,-10080,-10394,-10702,-11003,-11297,
    -11585,-11866,-12140,-12406,-12665,-12916,-13160,-13395,-13623,-13842,-14053,-14256,-14449,-14635,-14811,-14978,
    -15137,-15286,-15426,-15557,-15679,-15791,-15893,-15986,-16069,-16143,-16207,-16261,-16305,-16340,-16364,-16379,
    -16384,-16379,-16364,-16340,-16305,-16261,-16207,-16143,-16069,-15986,-15893,-15791,-15679,-15557,-15426,-15286,
    -15137,-14978,-14811,-14635,-14449,-14256,-14053,-13842,-13623,-13395,-13160,-12916,-12665,-12406,-12140,-11866,
    -11585,-11297,-11003,-10702,-10394,-10080,-9760,-9434,-9102,-8765,-8423,-8076,-7723,-7366,-7005,-6639,
    -6270,-5897,-5520,-5139,-4756,-4370,-3981,-3590,-3196,-2801,-2404,-2006,-1606,-1205,-804,-402,
};

static inline fix16_t fix16_sin(fix16_t angle) {
    int idx = ((angle * 256) / 411775) & 255;
    return (fix16_t)sin_lut[idx] << 2;  // Standard sin (same as libfixmath)
}

static inline fix16_t fix16_cos(fix16_t angle) {
    int idx = (((angle * 256) / 411775) + 64) & 255;
    return (fix16_t)sin_lut[idx] << 2;
}

// Fast integer square root (for initial guess)
static inline u32 isqrt(u32 n) {
    u32 x = n, y = (x + 1) >> 1;
    while (y < x) { x = y; y = (x + n / x) >> 1; }
    return x;
}

// Optimized fixed-point sqrt using better initial guess
static inline fix16_t fix16_sqrt(fix16_t x) {
    if (x <= 0) return 0;
    // sqrt(x) in Q16.16 = sqrt(x * 65536) / 256 * 65536 = sqrt(x) * 256
    // Use integer sqrt for speed
    u32 val = (u32)x;
    u32 root = isqrt(val << 8);  // Shift for more precision
    return (fix16_t)(root << 4);
}

// PICO-8 palette in RGB555
static const u16 PICO8_PALETTE[16] = {
    RGB15(0,0,0), RGB15(3,5,10), RGB15(15,4,10), RGB15(0,16,10),
    RGB15(21,10,6), RGB15(11,10,9), RGB15(24,24,24), RGB15(31,30,29),
    RGB15(31,0,9), RGB15(31,20,0), RGB15(31,29,4), RGB15(0,28,6),
    RGB15(5,21,31), RGB15(16,14,19), RGB15(31,14,21), RGB15(31,25,21),
};

// Render directly to VRAM in RGB555 format (no intermediate buffer)
EWRAM_DATA static u8 map_memory[0x1000];
static u16 palette_map[16];  // Now stores RGB555 colors directly
static u16 draw_color_rgb = 0x7FFF;
static u32 rnd_state = 1;
static bool btn_state[6] = {false};
static bool btn_prev[6] = {false};
EWRAM_DATA static s32 cart_data[64];
static int current_page = 0;
static volatile u16* vram_buffer;  // Points to back buffer

// Dither pattern in IWRAM for fast access
IWRAM_DATA static u8 dither_pattern[8][8];

// Spritesheet stays in ROM (const) for faster access
// Will be accessed via hyperspace_spritesheet directly

// Fast inline macros for pixel operations (no function call overhead)
#define SGET_FAST(x, y) (hyperspace_spritesheet[y][x])
#define VRAM_PSET(x, y, c) do { \
    if ((unsigned)(x) < SCREEN_WIDTH && (unsigned)(y) < SCREEN_HEIGHT) \
        vram_buffer[(y) * SCREEN_WIDTH + (x)] = (c); \
} while(0)
#define VRAM_PSET_FAST(x, y, c) (vram_buffer[(y) * SCREEN_WIDTH + (x)] = (c))

#define FIX_HALF F16(0.5)
#define FIX_TWO F16(2.0)
#define FIX_TWO_PI F16(6.28318530718)
// Screen center for 160x128
#define FIX_SCREEN_CENTER_X F16(80.0)
#define FIX_SCREEN_CENTER_Y F16(64.0)
#define FIX_PROJ_CONST F16(-75.0)

// Clipping region
static int clip_x1 = 0, clip_y1 = 0, clip_x2 = SCREEN_WIDTH - 1, clip_y2 = SCREEN_HEIGHT - 1;

// Fast DMA copy (from mode5.c)
static inline void DMAFastCopy(void* source, void* dest, u32 count, u32 mode) {
    REG_DMA3SAD = (u32)source;
    REG_DMA3DAD = (u32)dest;
    REG_DMA3CNT = count | mode;
}

// Clear color for DMA fill
static u16 clear_color = 0;

// PICO-8 API - Optimized for direct VRAM rendering
static void cls(void) {
    // Fast clear using DMA with fixed source (fill mode)
    DMAFastCopy(&clear_color, (void*)vram_buffer, SCREEN_WIDTH * SCREEN_HEIGHT,
                DMA_SRC_FIXED | DMA_DST_INC | DMA_16BIT | DMA_ENABLE);
}

static inline void pset(int x, int y, int c) {
    if ((unsigned)x < (unsigned)SCREEN_WIDTH && (unsigned)y < (unsigned)SCREEN_HEIGHT)
        vram_buffer[y * SCREEN_WIDTH + x] = palette_map[c & 15];
}

static inline u16 pget_rgb(int x, int y) {
    if ((unsigned)x < (unsigned)SCREEN_WIDTH && (unsigned)y < (unsigned)SCREEN_HEIGHT)
        return vram_buffer[y * SCREEN_WIDTH + x];
    return 0;
}

// sget returns palette index from ROM spritesheet
static inline u8 sget(int x, int y) {
    return SGET_FAST(x, y);
}

static void line(int x0, int y0, int x1, int y1, int c) {
    u16 col = palette_map[c & 15];
    int dx = x1 - x0, dy = y1 - y0;
    int sx = dx < 0 ? -1 : 1, sy = dy < 0 ? -1 : 1;
    dx = dx < 0 ? -dx : dx; dy = dy < 0 ? -dy : dy;
    int err = dx - dy;
    while (1) {
        VRAM_PSET(x0, y0, col);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 < dx) { err += dx; y0 += sy; }
    }
}

static void rectfill(int x0, int y0, int x1, int y1, int c) {
    u16 col = palette_map[c & 15];
    if (x0 > x1) { int t = x0; x0 = x1; x1 = t; }
    if (y0 > y1) { int t = y0; y0 = y1; y1 = t; }
    if (x0 < 0) x0 = 0; if (x1 >= SCREEN_WIDTH) x1 = SCREEN_WIDTH - 1;
    if (y0 < 0) y0 = 0; if (y1 >= SCREEN_HEIGHT) y1 = SCREEN_HEIGHT - 1;
    for (int y = y0; y <= y1; y++) {
        volatile u16* row = &vram_buffer[y * SCREEN_WIDTH];
        for (int x = x0; x <= x1; x++) row[x] = col;
    }
}

static void circfill(int cx, int cy, int r, int c) {
    u16 col = palette_map[c & 15];
    int r2 = r * r;
    for (int dy = -r; dy <= r; dy++) {
        int py = cy + dy;
        if ((unsigned)py >= (unsigned)SCREEN_HEIGHT) continue;
        int max_x = 0;
        while (max_x <= r && max_x * max_x + dy * dy <= r2) max_x++;
        max_x--;
        int x0 = cx - max_x, x1 = cx + max_x;
        if (x0 < 0) x0 = 0; if (x1 >= SCREEN_WIDTH) x1 = SCREEN_WIDTH - 1;
        volatile u16* row = &vram_buffer[py * SCREEN_WIDTH];
        for (int px = x0; px <= x1; px++) row[px] = col;
    }
}

static void spr(int n, int x, int y, int w, int h) {
    int sx = (n & 15) * 8, sy = (n >> 4) * 8;
    int pw = w * 8, ph = h * 8;
    for (int py = 0; py < ph; py++) {
        int dy = y + py;
        if (dy < clip_y1 || dy > clip_y2) continue;
        if ((unsigned)dy >= (unsigned)SCREEN_HEIGHT) continue;
        volatile u16* row = &vram_buffer[dy * SCREEN_WIDTH];
        for (int px = 0; px < pw; px++) {
            int dx = x + px;
            if (dx < clip_x1 || dx > clip_x2) continue;
            if ((unsigned)dx >= (unsigned)SCREEN_WIDTH) continue;
            u8 c = SGET_FAST(sx + px, sy + py);
            if (c != 0) row[dx] = palette_map[c];
        }
    }
}

static void pal_reset(void) { for (int i = 0; i < 16; i++) palette_map[i] = PICO8_PALETTE[i]; }
static void pal(int c0, int c1) { palette_map[c0 & 15] = PICO8_PALETTE[c1 & 15]; }
static void clip_set(int x, int y, int w, int h) { clip_x1 = x; clip_y1 = y; clip_x2 = x + w - 1; clip_y2 = y + h - 1; }
static void clip_reset(void) { clip_x1 = 0; clip_y1 = 0; clip_x2 = SCREEN_WIDTH - 1; clip_y2 = SCREEN_HEIGHT - 1; }
static void color(int c) { draw_color_rgb = palette_map[c & 15]; }

// Font data (3x5)
static const u8 font_data[96][5] = {
    {0,0,0,0,0},{2,2,2,0,2},{5,5,0,0,0},{5,7,5,7,5},{6,3,6,3,6},{1,4,2,1,4},{2,5,2,5,3},{2,2,0,0,0},
    {1,2,2,2,1},{4,2,2,2,4},{5,2,5,0,0},{0,2,7,2,0},{0,0,0,2,4},{0,0,7,0,0},{0,0,0,0,2},{1,1,2,4,4},
    {2,5,5,5,2},{2,6,2,2,7},{6,1,2,4,7},{6,1,2,1,6},{5,5,7,1,1},{7,4,6,1,6},{3,4,6,5,2},{7,1,2,2,2},
    {2,5,2,5,2},{2,5,3,1,6},{0,2,0,2,0},{0,2,0,2,4},{1,2,4,2,1},{0,7,0,7,0},{4,2,1,2,4},{2,5,1,0,2},
    {2,5,5,4,3},{2,5,7,5,5},{6,5,6,5,6},{3,4,4,4,3},{6,5,5,5,6},{7,4,6,4,7},{7,4,6,4,4},{3,4,5,5,3},
    {5,5,7,5,5},{7,2,2,2,7},{1,1,1,5,2},{5,5,6,5,5},{4,4,4,4,7},{5,7,5,5,5},{5,7,7,5,5},{2,5,5,5,2},
    {6,5,6,4,4},{2,5,5,6,3},{6,5,6,5,5},{3,4,2,1,6},{7,2,2,2,2},{5,5,5,5,2},{5,5,5,5,2},{5,5,5,7,5},
    {5,5,2,5,5},{5,5,2,2,2},{7,1,2,4,7},{3,2,2,2,3},{4,4,2,1,1},{6,2,2,2,6},{2,5,0,0,0},{0,0,0,0,7},
    {4,2,0,0,0},{0,3,5,5,3},{4,6,5,5,6},{0,3,4,4,3},{1,3,5,5,3},{0,2,5,6,3},{1,2,7,2,2},{0,3,5,3,6},
    {4,6,5,5,5},{2,0,2,2,2},{1,0,1,1,6},{4,5,6,5,5},{2,2,2,2,1},{0,5,7,5,5},{0,6,5,5,5},{0,2,5,5,2},
    {0,6,5,6,4},{0,3,5,3,1},{0,3,4,4,4},{0,3,6,1,6},{2,7,2,2,1},{0,5,5,5,3},{0,5,5,5,2},{0,5,5,7,5},
    {0,5,2,2,5},{0,5,5,3,6},{0,7,1,4,7},{1,2,6,2,1},{2,2,2,2,2},{4,2,3,2,4},{5,2,0,0,0},{0,0,0,0,0},
};

static void print_char(char c, int x, int y, int col) {
    if (c < 32 || c > 127) return;
    int idx = c - 32;
    for (int row = 0; row < 5; row++) {
        u8 bits = font_data[idx][row];
        for (int i = 0; i < 3; i++)
            if (bits & (0x4 >> i)) pset(x + i, y + row, col);
    }
}

static void print_str(const char* str, int x, int y, int col) {
    int cx = x;
    while (*str) {
        if (*str == '\n') { y += 6; cx = x; }
        else { print_char(*str, cx, y, col); cx += 4; }
        str++;
    }
}

static fix16_t rnd_fix(fix16_t max) {
    rnd_state = rnd_state * 1103515245 + 12345;
    u16 frac = (rnd_state >> 16) & 0xFFFF;
    return (fix16_t)(((s64)max * frac) >> 16);
}

static int flr_fix(fix16_t x) {
    if (x >= 0) return x >> 16;
    return (x >> 16) - ((x & 0xFFFF) ? 1 : 0);
}

static fix16_t mid_fix(fix16_t a, fix16_t b, fix16_t c) {
    if (a > b) { fix16_t t = a; a = b; b = t; }
    if (b > c) b = c;
    if (a > b) b = a;
    return b;
}

static fix16_t sgn_fix(fix16_t x) {
    if (x > 0) return fix16_one;
    if (x < 0) return -fix16_one;
    return 0;
}

static bool btn(int n) { return btn_state[n]; }
static bool btnp(int n) { return btn_state[n] && !btn_prev[n]; }

// SRAM save/load
#define SRAM_MAGIC 0x48595045
static void load_cart_data(void) {
    u32 magic = SRAM[0] | (SRAM[1]<<8) | (SRAM[2]<<16) | (SRAM[3]<<24);
    if (magic == SRAM_MAGIC) {
        for (int i = 0; i < 64; i++) {
            cart_data[i] = SRAM[4+i*4] | (SRAM[5+i*4]<<8) | (SRAM[6+i*4]<<16) | (SRAM[7+i*4]<<24);
        }
    }
}

static void save_cart_data(void) {
    SRAM[0] = SRAM_MAGIC & 0xFF; SRAM[1] = (SRAM_MAGIC>>8)&0xFF;
    SRAM[2] = (SRAM_MAGIC>>16)&0xFF; SRAM[3] = (SRAM_MAGIC>>24)&0xFF;
    for (int i = 0; i < 64; i++) {
        SRAM[4+i*4] = cart_data[i]&0xFF; SRAM[5+i*4] = (cart_data[i]>>8)&0xFF;
        SRAM[6+i*4] = (cart_data[i]>>16)&0xFF; SRAM[7+i*4] = (cart_data[i]>>24)&0xFF;
    }
}

static s32 dget(int n) { if (n >= 0 && n < 64) return cart_data[n]; return 0; }
static void dset(int n, s32 v) { if (n >= 0 && n < 64 && cart_data[n] != v) cart_data[n] = v; }

// =============================================================================
// GBA Sound System (PICO-8 Compatible)
// =============================================================================

// Forward declaration - sound is optional and off by default
static int sound_enabled;

// PICO-8 frequency table (Hz) for notes 0-63
static const u16 p8_freq_table[64] = {
    65, 69, 73, 78, 82, 87, 92, 98,
    104, 110, 117, 123, 131, 139, 147, 156,
    165, 175, 185, 196, 208, 220, 233, 247,
    262, 277, 294, 311, 330, 349, 370, 392,
    415, 440, 466, 494, 523, 554, 587, 622,
    659, 698, 740, 784, 831, 880, 932, 988,
    1047, 1109, 1175, 1245, 1319, 1397, 1480, 1568,
    1661, 1760, 1865, 1976, 2093, 2217, 2349, 2489
};

// Convert PICO-8 pitch to GBA frequency register value
// GBA freq register: actual_freq = 131072 / (2048 - register_value)
// So: register_value = 2048 - 131072 / actual_freq
static u16 p8_pitch_to_gba_freq(u8 pitch) {
    if (pitch >= 64) pitch = 63;
    u16 hz = p8_freq_table[pitch];
    if (hz < 64) return 0;  // Too low for GBA
    u16 gba_freq = 2048 - (131072 / hz);
    if (gba_freq > 2047) gba_freq = 2047;
    return gba_freq;
}

// PICO-8 SFX data structure (same as original)
typedef struct {
    u8 speed;
    u8 loop_start;
    u8 loop_end;
    u8 notes[32][4];  // pitch, waveform, volume, effect
} P8SFX;

// Hyperspace SFX definitions (exact PICO-8 data)
static const P8SFX hyperspace_sfx[] = {
    // SFX 0: Laser fire (descending saw wave)
    {1, 0, 13, {
        {50, 2, 3, 0}, {51, 2, 3, 0}, {51, 2, 3, 0}, {49, 2, 1, 0},
        {46, 2, 3, 0}, {41, 2, 3, 0}, {36, 2, 4, 0}, {34, 2, 3, 0},
        {32, 2, 3, 0}, {29, 2, 3, 0}, {28, 2, 3, 0}, {28, 2, 2, 0},
        {28, 2, 1, 0}, {28, 2, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0},
        {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0},
        {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0},
        {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0},
        {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}
    }},
    // SFX 1: Player damage / barrel roll (noise)
    {5, 0, 0, {
        {36, 6, 7, 0}, {36, 6, 7, 0}, {39, 6, 7, 0}, {42, 6, 7, 0},
        {49, 6, 7, 0}, {56, 6, 7, 0}, {63, 6, 7, 0}, {63, 6, 7, 0},
        {48, 6, 7, 0}, {41, 6, 7, 0}, {36, 6, 7, 0}, {32, 6, 7, 0},
        {30, 6, 6, 0}, {28, 6, 6, 0}, {27, 6, 5, 0}, {26, 6, 5, 0},
        {25, 6, 4, 0}, {25, 6, 4, 0}, {24, 6, 3, 0}, {25, 6, 3, 0},
        {26, 6, 2, 0}, {28, 6, 2, 0}, {32, 6, 1, 0}, {35, 6, 1, 0},
        {10, 6, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0},
        {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}
    }},
    // SFX 2: Hit enemy / explosion (mixed noise)
    {3, 0, 0, {
        {45, 6, 7, 0}, {41, 4, 7, 0}, {36, 4, 7, 0}, {25, 6, 7, 0},
        {30, 4, 7, 0}, {32, 6, 7, 0}, {29, 6, 7, 0}, {13, 6, 7, 0},
        {22, 6, 7, 0}, {20, 4, 7, 0}, {16, 4, 7, 0}, {15, 4, 7, 0},
        {19, 6, 7, 0}, {11, 4, 7, 0}, {9, 4, 7, 0}, {7, 6, 6, 0},
        {7, 4, 5, 0}, {5, 4, 4, 0}, {8, 6, 3, 0}, {2, 4, 2, 0},
        {1, 4, 1, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0},
        {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0},
        {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}
    }},
    // SFX 3: (unused placeholder)
    {1, 0, 0, {
        {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0},
        {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0},
        {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0},
        {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0},
        {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0},
        {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0},
        {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0},
        {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}
    }},
    // SFX 4: (unused placeholder)
    {1, 0, 0, {
        {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0},
        {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0},
        {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0},
        {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0},
        {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0},
        {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0},
        {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0},
        {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}
    }},
    // SFX 5: Bonus pickup (pulse wave descending)
    {1, 0, 0, {
        {44, 4, 7, 0}, {40, 4, 7, 0}, {35, 4, 7, 0}, {32, 4, 7, 0},
        {28, 4, 7, 0}, {26, 4, 7, 0}, {23, 4, 6, 0}, {21, 4, 4, 0},
        {21, 4, 2, 0}, {20, 4, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0},
        {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0},
        {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0},
        {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0},
        {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0},
        {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}
    }},
    // SFX 6: Boss spawn (low square)
    {24, 0, 0, {
        {0, 0, 0, 0}, {7, 3, 6, 0}, {20, 1, 4, 0}, {7, 3, 6, 0},
        {20, 1, 4, 0}, {26, 3, 7, 0}, {20, 1, 4, 0}, {27, 3, 7, 0},
        {1, 4, 4, 0}, {23, 3, 7, 0}, {23, 3, 7, 0}, {23, 3, 7, 0},
        {23, 3, 7, 0}, {23, 3, 6, 0}, {23, 3, 5, 0}, {23, 3, 0, 0},
        {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0},
        {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0},
        {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0},
        {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}
    }},
    // SFX 7: Boss damage (saw wave)
    {32, 0, 0, {
        {13, 2, 7, 0}, {13, 2, 7, 0}, {8, 2, 7, 0}, {8, 2, 7, 0},
        {4, 2, 7, 0}, {4, 2, 7, 0}, {1, 2, 7, 0}, {1, 2, 7, 0},
        {1, 2, 7, 0}, {1, 2, 7, 0}, {1, 2, 7, 0}, {1, 2, 7, 0},
        {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0},
        {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0},
        {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0},
        {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0},
        {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}
    }}
};

#define NUM_GBA_SFX (sizeof(hyperspace_sfx) / sizeof(hyperspace_sfx[0]))

// Sound channel state
typedef struct {
    const P8SFX *sfx;
    u8 note_index;
    u8 frame_count;
    bool active;
} SoundChannel;

static SoundChannel sound_channels[4];
static bool sound_initialized = false;

static void sound_init(void) {
    // Enable sound
    REG_SOUNDCNT_X = SOUND_ENABLE;

    // Set volume and enable all channels to both L/R
    REG_SOUNDCNT_L = 0x7777 |  // Max volume
                     SOUND1_L | SOUND1_R |
                     SOUND2_L | SOUND2_R |
                     SOUND4_L | SOUND4_R;

    // PSG/FIFO ratio
    REG_SOUNDCNT_H = 0x0002;  // PSG at 100%

    // Initialize channels
    for (int i = 0; i < 4; i++) {
        sound_channels[i].active = false;
        sound_channels[i].sfx = NULL;
    }

    sound_initialized = true;
}

// Silence a hardware channel
static void silence_hw_channel(int hw_channel) {
    if (hw_channel == 0) {
        REG_SOUND1CNT_H = 0;
        REG_SOUND1CNT_X = 0;
    } else if (hw_channel == 1) {
        REG_SOUND2CNT_L = 0;
        REG_SOUND2CNT_H = 0;
    } else {
        REG_SOUND4CNT_L = 0;
        REG_SOUND4CNT_H = 0;
    }
}

// Play a PICO-8 note on a hardware channel
// PICO-8 waveforms: 0=triangle, 1=tilted saw, 2=saw, 3=square, 4=pulse, 5=organ, 6=noise, 7=phaser
static void play_p8_note(int hw_channel, u8 pitch, u8 waveform, u8 volume) {
    if (volume == 0) {
        silence_hw_channel(hw_channel);
        return;
    }

    // Convert PICO-8 pitch to GBA frequency
    u16 gba_freq = p8_pitch_to_gba_freq(pitch);

    // Map PICO-8 volume (0-7) to GBA envelope (0-15)
    u16 gba_vol = volume * 2;
    if (gba_vol > 15) gba_vol = 15;

    // PICO-8 waveform 6 is noise - use GBA channel 4
    if (waveform == 6) {
        // Noise channel - convert pitch to noise frequency
        // Higher pitch = faster noise (lower divider value)
        u16 noise_div = 7 - (pitch >> 3);  // Map 0-63 to 7-0
        if (noise_div > 7) noise_div = 7;
        REG_SOUND4CNT_L = (gba_vol << 12);  // Envelope
        REG_SOUND4CNT_H = 0x8000 | (noise_div << 4);  // Frequency and restart
    } else if (hw_channel == 0) {
        // GBA Channel 1 - Square with sweep
        // Map PICO-8 waveform to duty cycle: 0-2=25%, 3-4=50%, 5-7=75%
        u16 duty;
        if (waveform <= 2) duty = 0x0000;       // 12.5% (close to triangle/saw)
        else if (waveform <= 4) duty = 0x0080;  // 50% (square/pulse)
        else duty = 0x00C0;                     // 75% (organ/phaser)

        REG_SOUND1CNT_L = 0x0000;  // No sweep
        REG_SOUND1CNT_H = duty | (gba_vol << 12);  // Duty + envelope
        REG_SOUND1CNT_X = 0x8000 | gba_freq;  // Frequency and restart
    } else if (hw_channel == 1) {
        // GBA Channel 2 - Square
        u16 duty;
        if (waveform <= 2) duty = 0x0000;       // 12.5%
        else if (waveform <= 4) duty = 0x0080;  // 50%
        else duty = 0x00C0;                     // 75%

        REG_SOUND2CNT_L = duty | (gba_vol << 12);  // Duty + envelope
        REG_SOUND2CNT_H = 0x8000 | gba_freq;  // Frequency and restart
    } else {
        // Channel 2+ uses noise channel for non-noise waveforms too
        // (fallback - shouldn't normally happen)
        u16 noise_div = 7 - (pitch >> 3);
        if (noise_div > 7) noise_div = 7;
        REG_SOUND4CNT_L = (gba_vol << 12);
        REG_SOUND4CNT_H = 0x8000 | (noise_div << 4);
    }
}

static void sfx(int n, int channel) {
    // Check if sound is enabled
    if (!sound_enabled) return;

    if (!sound_initialized) sound_init();

    if (channel < 0 || channel >= 4) return;

    // Map software channel to hardware channel
    // Channel 0 -> GBA Ch1, Channel 1 -> GBA Ch2, Channel 2/3 -> GBA Ch4 (noise)
    int hw_channel = (channel >= 2) ? 2 : channel;

    if (n == -1) {
        // Stop this channel completely
        sound_channels[channel].active = false;
        silence_hw_channel(hw_channel);
        return;
    }

    if (n == -2) {
        // Stop all channels completely
        for (int i = 0; i < 4; i++) {
            sound_channels[i].active = false;
        }
        silence_hw_channel(0);
        silence_hw_channel(1);
        silence_hw_channel(2);
        return;
    }

    if (n < 0 || n >= (int)NUM_GBA_SFX) return;

    // Start playing SFX
    SoundChannel *ch = &sound_channels[channel];
    ch->sfx = &hyperspace_sfx[n];
    ch->note_index = 0;
    ch->frame_count = 0;
    ch->active = true;

    // Play first note immediately
    const u8 *note = ch->sfx->notes[0];
    if (note[2] > 0) {  // If volume > 0
        play_p8_note(hw_channel, note[0], note[1], note[2]);
    }
}

// Call this every frame to advance SFX playback
static void sound_update(void) {
    if (!sound_enabled || !sound_initialized) return;

    for (int i = 0; i < 4; i++) {
        SoundChannel *ch = &sound_channels[i];
        if (!ch->active || !ch->sfx) continue;

        // Map software channel to hardware channel
        int hw_channel = (i >= 2) ? 2 : i;

        ch->frame_count++;

        if (ch->frame_count >= ch->sfx->speed) {
            ch->frame_count = 0;
            ch->note_index++;

            // P8SFX has 32 notes max, end when we reach 32 or volume is 0
            if (ch->note_index >= 32) {
                // SFX finished
                ch->active = false;
                silence_hw_channel(hw_channel);
            } else {
                const u8 *note = ch->sfx->notes[ch->note_index];
                if (note[2] == 0) {
                    // Volume 0 = end of sound
                    ch->active = false;
                    silence_hw_channel(hw_channel);
                } else {
                    // Play next note
                    play_p8_note(hw_channel, note[0], note[1], note[2]);
                }
            }
        }
    }
}
static fix16_t sym_random_fix(fix16_t f) { return f - rnd_fix(fix16_mul(f, FIX_TWO)); }
static int get_random_idx(int max) { return flr_fix(rnd_fix(fix16_from_int(max))); }

// 3D Types
typedef struct { fix16_t x, y, z; } Vec3;
typedef struct { fix16_t m[12]; } Mat34;
typedef struct { Vec3 pos; int tri[3]; fix16_t uv[3][2]; Vec3 normal; fix16_t z; } Triangle;
typedef struct { Vec3* vertices; Vec3* projected; Triangle* triangles; int num_vertices; int num_triangles; } Mesh;
typedef struct { int x, y, light_x; } Texture;
typedef struct { Vec3 pos0, pos1, spd; } Laser;
typedef struct { Vec3 pos0, pos1; fix16_t spd; int col; } Trail;
typedef struct { Vec3 pos; fix16_t spd; int index; } Background;
typedef struct {
    Vec3 pos; int type; Vec3* proj; int life; Vec3 light_dir; int hit_t; Vec3 hit_pos;
    fix16_t rot_x, rot_y, rot_x_spd, rot_y_spd; Vec3 spd, waypoint;
    fix16_t laser_t, stop_laser_t, next_laser_t, laser_offset_x[2], laser_offset_y[2];
} Enemy;

// Game state
static Mesh ship_mesh;
static Texture ship_tex, ship_tex_laser_lit;
static Mesh nme_meshes[4];
static Texture nme_tex[4], nme_tex_hit;
static fix16_t nme_scale[4] = {F16(1.0), F16(2.5), F16(3.0), F16(5.0)};
static int nme_life[4] = {1, 3, 10, 80};
static int nme_score[4] = {1, 10, 10, 100};
static fix16_t nme_radius[4] = {F16(3.25), F16(6.0), F16(8.0), F16(16.0)};
static fix16_t nme_bounds[3] = {F16(-50.0), F16(-50.0), F16(-100.0)};
static fix16_t nme_rot[3] = {F16(0.18), F16(0.24), F16(0.06)};
static fix16_t nme_spd[3] = {F16(1.0), F16(0.5), F16(0.6)};

#define MAX_TRAILS 32
#define MAX_BGS 32
#define MAX_LASERS 50
#define MAX_NME_LASERS 20
#define MAX_ENEMIES 16

EWRAM_DATA static Trail trails[MAX_TRAILS];
static int trail_color[5] = {7, 7, 6, 13, 1};
EWRAM_DATA static Background bgs[MAX_BGS];
static int bg_color[3] = {12, 13, 6};
EWRAM_DATA static Laser lasers[MAX_LASERS];
static int num_lasers = 0;
EWRAM_DATA static Laser nme_lasers[MAX_NME_LASERS];
static int num_nme_lasers = 0;
EWRAM_DATA static Enemy enemies[MAX_ENEMIES];
static int num_enemies = 0;

static fix16_t cam_x = 0, cam_y = 0, cam_angle_x = 0, cam_angle_z = F16(-0.4), cam_depth = F16(22.5);
static Mat34 cam_mat;
static fix16_t ship_x = 0, ship_y = 0, ship_spd_x = 0, ship_spd_y = 0;
static fix16_t roll_angle = 0, roll_spd = 0, roll_f = 0;
static fix16_t pitch_angle = 0, pitch_spd = 0, pitch_f = 0;
static Mat34 ship_mat, ship_pos_mat, inv_ship_mat, light_mat;
static Vec3 light_dir, ship_light_dir;
static int cur_mode = 0, score = 0, best_score = 0, life = 4, hit_t = -1;
static Vec3 hit_pos;
static bool laser_on = false, laser_spawned = false;
static fix16_t global_t = 0, game_spd = F16(1.0);
static int barrel_dir = 0;
static fix16_t barrel_cur_t = F16(-1.0), cur_thrust = 0, cur_nme_t = 0;
static int cur_sequencer_x = 96, cur_sequencer_y = 96;
static fix16_t next_sequencer_t = 0, aim_z = F16(-200.0);
static Vec3* tgt_pos = 0;
static Vec3 interp_tgt_pos, aim_proj, star_proj;
static bool star_visible = false;
static fix16_t aim_life_ratio = F16(-1.0);
static int nb_nme_ship = 0;
static bool waiting_nme_clear = false, spawn_asteroids = false;
static fix16_t fade_ratio = F16(-1.0);
static int manual_fire = 1, non_inverted_y = 0;
static fix16_t cur_laser_t = 0;
static int cur_laser_side = -1;
static Texture* cur_tex = 0;
static Vec3* t_light_dir = 0;
static fix16_t old_noise_roll = 0, cur_noise_roll = 0, old_noise_pitch = 0, cur_noise_pitch = 0;
static fix16_t cur_noise_t = 0, tgt_noise_t = 0;
static fix16_t src_cam_x, src_cam_y, src_cam_angle_x, src_cam_angle_z;
static fix16_t dst_cam_x, dst_cam_y, dst_cam_angle_x, dst_cam_angle_z;
static fix16_t interpolation_ratio = 0, interpolation_spd = 0, asteroid_mul_t = F16(1.0);
static int explosion_color[4] = {9, 10, 15, 7};
// PICO-8 original: ngn_colors = {13,12,7,12}
static int ngn_colors[4] = {13, 12, 7, 12};
// PICO-8 original: laser_ngn_colors = {3,11,7,11}
static int laser_ngn_colors[4] = {3, 11, 7, 11};
static fix16_t ngn_col_idx = 0, ngn_laser_col_idx = 0;
static int flare_offset = 0;
static int mem_pos = 0;

// Vector/Matrix functions
static void vec3_copy(Vec3* dst, const Vec3* src) { dst->x = src->x; dst->y = src->y; dst->z = src->z; }
static void vec3_set(Vec3* v, fix16_t x, fix16_t y, fix16_t z) { v->x = x; v->y = y; v->z = z; }
static void vec3_mul(Vec3* v, fix16_t f) { v->x = fix16_mul(v->x, f); v->y = fix16_mul(v->y, f); v->z = fix16_mul(v->z, f); }
static Vec3 vec3_minus(const Vec3* v0, const Vec3* v1) { Vec3 r = {v0->x - v1->x, v0->y - v1->y, v0->z - v1->z}; return r; }
static fix16_t vec3_dot(const Vec3* v0, const Vec3* v1) { return fix16_mul(v0->x, v1->x) + fix16_mul(v0->y, v1->y) + fix16_mul(v0->z, v1->z); }
static fix16_t vec3_length(const Vec3* v) { return fix16_sqrt(vec3_dot(v, v)); }
static void vec3_normalize(Vec3* v) { fix16_t len = vec3_length(v); if (len > 0) { fix16_t invl = fix16_div(fix16_one, len); vec3_mul(v, invl); } }

static void mat_rotx(Mat34* m, fix16_t a) {
    fix16_t angle = fix16_mul(a, FIX_TWO_PI);
    fix16_t cos_a = fix16_cos(angle), sin_a = fix16_sin(angle);
    m->m[0] = fix16_one; m->m[1] = 0; m->m[2] = 0; m->m[3] = 0;
    m->m[4] = 0; m->m[5] = cos_a; m->m[6] = -sin_a; m->m[7] = 0;
    m->m[8] = 0; m->m[9] = sin_a; m->m[10] = cos_a; m->m[11] = 0;
}

static void mat_roty(Mat34* m, fix16_t a) {
    fix16_t angle = fix16_mul(a, FIX_TWO_PI);
    fix16_t cos_a = fix16_cos(angle), sin_a = fix16_sin(angle);
    m->m[0] = cos_a; m->m[1] = 0; m->m[2] = -sin_a; m->m[3] = 0;
    m->m[4] = 0; m->m[5] = fix16_one; m->m[6] = 0; m->m[7] = 0;
    m->m[8] = sin_a; m->m[9] = 0; m->m[10] = cos_a; m->m[11] = 0;
}

static void mat_rotz(Mat34* m, fix16_t a) {
    fix16_t angle = fix16_mul(a, FIX_TWO_PI);
    fix16_t cos_a = fix16_cos(angle), sin_a = fix16_sin(angle);
    m->m[0] = cos_a; m->m[1] = -sin_a; m->m[2] = 0; m->m[3] = 0;
    m->m[4] = sin_a; m->m[5] = cos_a; m->m[6] = 0; m->m[7] = 0;
    m->m[8] = 0; m->m[9] = 0; m->m[10] = fix16_one; m->m[11] = 0;
}

static void mat_translation(Mat34* m, fix16_t x, fix16_t y, fix16_t z) {
    m->m[0] = fix16_one; m->m[1] = 0; m->m[2] = 0; m->m[3] = x;
    m->m[4] = 0; m->m[5] = fix16_one; m->m[6] = 0; m->m[7] = y;
    m->m[8] = 0; m->m[9] = 0; m->m[10] = fix16_one; m->m[11] = z;
}

static void mat_mul(Mat34* res, const Mat34* m0, const Mat34* m1) {
    fix16_t r[12];
    r[0] = fix16_mul(m0->m[0], m1->m[0]) + fix16_mul(m0->m[1], m1->m[4]) + fix16_mul(m0->m[2], m1->m[8]);
    r[1] = fix16_mul(m0->m[0], m1->m[1]) + fix16_mul(m0->m[1], m1->m[5]) + fix16_mul(m0->m[2], m1->m[9]);
    r[2] = fix16_mul(m0->m[0], m1->m[2]) + fix16_mul(m0->m[1], m1->m[6]) + fix16_mul(m0->m[2], m1->m[10]);
    r[3] = fix16_mul(m0->m[0], m1->m[3]) + fix16_mul(m0->m[1], m1->m[7]) + fix16_mul(m0->m[2], m1->m[11]) + m0->m[3];
    r[4] = fix16_mul(m0->m[4], m1->m[0]) + fix16_mul(m0->m[5], m1->m[4]) + fix16_mul(m0->m[6], m1->m[8]);
    r[5] = fix16_mul(m0->m[4], m1->m[1]) + fix16_mul(m0->m[5], m1->m[5]) + fix16_mul(m0->m[6], m1->m[9]);
    r[6] = fix16_mul(m0->m[4], m1->m[2]) + fix16_mul(m0->m[5], m1->m[6]) + fix16_mul(m0->m[6], m1->m[10]);
    r[7] = fix16_mul(m0->m[4], m1->m[3]) + fix16_mul(m0->m[5], m1->m[7]) + fix16_mul(m0->m[6], m1->m[11]) + m0->m[7];
    r[8] = fix16_mul(m0->m[8], m1->m[0]) + fix16_mul(m0->m[9], m1->m[4]) + fix16_mul(m0->m[10], m1->m[8]);
    r[9] = fix16_mul(m0->m[8], m1->m[1]) + fix16_mul(m0->m[9], m1->m[5]) + fix16_mul(m0->m[10], m1->m[9]);
    r[10] = fix16_mul(m0->m[8], m1->m[2]) + fix16_mul(m0->m[9], m1->m[6]) + fix16_mul(m0->m[10], m1->m[10]);
    r[11] = fix16_mul(m0->m[8], m1->m[3]) + fix16_mul(m0->m[9], m1->m[7]) + fix16_mul(m0->m[10], m1->m[11]) + m0->m[11];
    for (int i = 0; i < 12; i++) res->m[i] = r[i];
}

static void mat_mul_vec(Vec3* res, const Mat34* m, const Vec3* v) {
    res->x = fix16_mul(v->x, m->m[0]) + fix16_mul(v->y, m->m[1]) + fix16_mul(v->z, m->m[2]);
    res->y = fix16_mul(v->x, m->m[4]) + fix16_mul(v->y, m->m[5]) + fix16_mul(v->z, m->m[6]);
    res->z = fix16_mul(v->x, m->m[8]) + fix16_mul(v->y, m->m[9]) + fix16_mul(v->z, m->m[10]);
}

static void mat_mul_pos(Vec3* res, const Mat34* m, const Vec3* v) {
    res->x = fix16_mul(v->x, m->m[0]) + fix16_mul(v->y, m->m[1]) + fix16_mul(v->z, m->m[2]) + m->m[3];
    res->y = fix16_mul(v->x, m->m[4]) + fix16_mul(v->y, m->m[5]) + fix16_mul(v->z, m->m[6]) + m->m[7];
    res->z = fix16_mul(v->x, m->m[8]) + fix16_mul(v->y, m->m[9]) + fix16_mul(v->z, m->m[10]) + m->m[11];
}

static void mat_transpose_rot(Mat34* res, const Mat34* m) {
    res->m[0] = m->m[0]; res->m[1] = m->m[4]; res->m[2] = m->m[8]; res->m[3] = 0;
    res->m[4] = m->m[1]; res->m[5] = m->m[5]; res->m[6] = m->m[9]; res->m[7] = 0;
    res->m[8] = m->m[2]; res->m[9] = m->m[6]; res->m[10] = m->m[10]; res->m[11] = 0;
}

static fix16_t normalize_angle(fix16_t a) {
    while (a > FIX_HALF) a -= fix16_one;
    while (a < -FIX_HALF) a += fix16_one;
    return a;
}

static fix16_t smoothstep(fix16_t ratio) {
    fix16_t x2 = fix16_mul(ratio, ratio);
    return fix16_mul(F16(3.0), x2) - fix16_mul(FIX_TWO, fix16_mul(x2, ratio));
}

// Mesh decoding
static fix16_t decode_byte(void) {
    int res = map_memory[mem_pos++];
    if (res >= 128) res -= 256;
    return fix16_from_int(res) >> 1;
}

static int decode_byte_int(void) {
    int res = map_memory[mem_pos++];
    if (res >= 128) res -= 256;
    return res / 2;
}

static void decode_mesh(Mesh* mesh, fix16_t scale) {
    int nb_vert = decode_byte_int();
    if (nb_vert < 0) nb_vert = 0;
    if (nb_vert > 256) nb_vert = 256;
    mesh->num_vertices = nb_vert;
    mesh->vertices = (Vec3*)calloc(nb_vert > 0 ? nb_vert : 1, sizeof(Vec3));
    mesh->projected = (Vec3*)calloc(nb_vert > 0 ? nb_vert : 1, sizeof(Vec3));
    for (int i = 0; i < nb_vert; i++) {
        mesh->vertices[i].x = fix16_mul(decode_byte(), scale);
        mesh->vertices[i].y = fix16_mul(decode_byte(), scale);
        mesh->vertices[i].z = fix16_mul(decode_byte(), scale);
    }
    int nb_tri = decode_byte_int();
    if (nb_tri < 0) nb_tri = 0;
    if (nb_tri > 256) nb_tri = 256;
    mesh->num_triangles = nb_tri;
    mesh->triangles = (Triangle*)calloc(nb_tri > 0 ? nb_tri : 1, sizeof(Triangle));
    for (int i = 0; i < nb_tri; i++) {
        Triangle* tri = &mesh->triangles[i];
        tri->tri[0] = decode_byte_int() - 1; tri->normal.x = fix16_div(decode_byte(), F16(63.5));
        tri->uv[0][0] = decode_byte(); tri->uv[0][1] = decode_byte();
        tri->tri[1] = decode_byte_int() - 1; tri->normal.y = fix16_div(decode_byte(), F16(63.5));
        tri->uv[1][0] = decode_byte(); tri->uv[1][1] = decode_byte();
        tri->tri[2] = decode_byte_int() - 1; tri->normal.z = fix16_div(decode_byte(), F16(63.5));
        tri->uv[2][0] = decode_byte(); tri->uv[2][1] = decode_byte();
    }
}

static void transform_pos(Vec3* proj, const Mat34* mat, const Vec3* pos) {
    mat_mul_pos(proj, mat, pos);
    fix16_t c = fix16_div(FIX_PROJ_CONST, proj->z);
    proj->x = FIX_SCREEN_CENTER_X + fix16_mul(proj->x, c);
    proj->y = FIX_SCREEN_CENTER_Y - fix16_mul(proj->y, c);
    if (c > 0 && c <= F16(10.0)) proj->z = c; else proj->z = 0;
}

// Optimized rasterization using scanline interpolation (no per-pixel division)
// Uses affine texture mapping with edge walking
// ARM mode for faster execution (Thumb is slower for math-heavy code)
__attribute__((target("arm")))
static void rasterize_flat_tri(Vec3* v0, Vec3* v1, Vec3* v2, fix16_t* uv0, fix16_t* uv1, fix16_t* uv2, fix16_t light) {
    // v0 is tip vertex, v1 and v2 are base vertices at same y
    // Works for both flat-bottom (v0 at top) and flat-top (v0 at bottom) triangles
    fix16_t y0 = v0->y, y1 = v1->y;
    if (y0 == y1) return;

    // Determine direction: flat-bottom (y0 < y1) or flat-top (y0 > y1)
    fix16_t y_top, y_bot;
    int direction;
    if (y0 < y1) {
        y_top = y0; y_bot = y1;
        direction = 1;  // Top to bottom
    } else {
        y_top = y1; y_bot = y0;
        direction = -1; // Bottom to top (for edge calculation)
    }

    // Determine scanline range
    int y_start = fix16_to_int(y_top + F16(0.5));
    int y_end = fix16_to_int(y_bot - F16(0.5));
    if (y_start < 0) y_start = 0;
    if (y_end >= SCREEN_HEIGHT) y_end = SCREEN_HEIGHT - 1;
    if (y_start > y_end) return;

    // Pre-calculate edge gradients (one division per edge, not per pixel)
    fix16_t dy = y_bot - y_top;
    if (fix16_abs(dy) < F16(0.01)) return;
    fix16_t inv_dy = fix16_div(fix16_one, dy);

    fix16_t dx_left, dz_left, du_left, dv_left;
    fix16_t dx_right, dz_right, du_right, dv_right;
    fix16_t x_left, x_right, z_left, z_right, u_left, u_right, v_left_val, v_right_val;
    fix16_t prestep;

    if (direction > 0) {
        // Flat-bottom: v0 at top, v1/v2 at bottom
        // Left edge gradients (v0 to v1)
        dx_left = fix16_mul(v1->x - v0->x, inv_dy);
        dz_left = fix16_mul(v1->z - v0->z, inv_dy);
        du_left = fix16_mul(uv1[0] - uv0[0], inv_dy);
        dv_left = fix16_mul(uv1[1] - uv0[1], inv_dy);
        // Right edge gradients (v0 to v2)
        dx_right = fix16_mul(v2->x - v0->x, inv_dy);
        dz_right = fix16_mul(v2->z - v0->z, inv_dy);
        du_right = fix16_mul(uv2[0] - uv0[0], inv_dy);
        dv_right = fix16_mul(uv2[1] - uv0[1], inv_dy);
        // Starting values (prestep to first scanline from v0)
        prestep = fix16_from_int(y_start) + FIX_HALF - y_top;
        x_left = v0->x + fix16_mul(prestep, dx_left);
        x_right = v0->x + fix16_mul(prestep, dx_right);
        z_left = v0->z + fix16_mul(prestep, dz_left);
        z_right = v0->z + fix16_mul(prestep, dz_right);
        u_left = uv0[0] + fix16_mul(prestep, du_left);
        u_right = uv0[0] + fix16_mul(prestep, du_right);
        v_left_val = uv0[1] + fix16_mul(prestep, dv_left);
        v_right_val = uv0[1] + fix16_mul(prestep, dv_right);
    } else {
        // Flat-top: v0 at bottom, v1/v2 at top
        // Left edge gradients (v1 to v0)
        dx_left = fix16_mul(v0->x - v1->x, inv_dy);
        dz_left = fix16_mul(v0->z - v1->z, inv_dy);
        du_left = fix16_mul(uv0[0] - uv1[0], inv_dy);
        dv_left = fix16_mul(uv0[1] - uv1[1], inv_dy);
        // Right edge gradients (v2 to v0)
        dx_right = fix16_mul(v0->x - v2->x, inv_dy);
        dz_right = fix16_mul(v0->z - v2->z, inv_dy);
        du_right = fix16_mul(uv0[0] - uv2[0], inv_dy);
        dv_right = fix16_mul(uv0[1] - uv2[1], inv_dy);
        // Starting values (prestep to first scanline from v1/v2)
        prestep = fix16_from_int(y_start) + FIX_HALF - y_top;
        x_left = v1->x + fix16_mul(prestep, dx_left);
        x_right = v2->x + fix16_mul(prestep, dx_right);
        z_left = v1->z + fix16_mul(prestep, dz_left);
        z_right = v2->z + fix16_mul(prestep, dz_right);
        u_left = uv1[0] + fix16_mul(prestep, du_left);
        u_right = uv2[0] + fix16_mul(prestep, du_right);
        v_left_val = uv1[1] + fix16_mul(prestep, dv_left);
        v_right_val = uv2[1] + fix16_mul(prestep, dv_right);
    }

    // Texture info
    int tex_x = cur_tex->x, tex_y = cur_tex->y, tex_lit_x = cur_tex->light_x;
    // Pre-calculate light threshold (avoid per-pixel calculation)
    int use_lit = (light <= F16(11.0)) ? 1 : 0;  // Simplified dithering

    // Scanline loop
    for (int y = y_start; y <= y_end; y++) {
        int xl = fix16_to_int(x_left + FIX_HALF);
        int xr = fix16_to_int(x_right - FIX_HALF);

        if (xl < 0) xl = 0;
        if (xr >= SCREEN_WIDTH) xr = SCREEN_WIDTH - 1;

        if (xl <= xr) {
            // Calculate horizontal gradients for this scanline
            fix16_t span = x_right - x_left;
            if (span > F16(0.5)) {
                fix16_t inv_span = fix16_div(fix16_one, span);
                fix16_t du_dx = fix16_mul(u_right - u_left, inv_span);
                fix16_t dv_dx = fix16_mul(v_right_val - v_left_val, inv_span);

                // Prestep to first pixel
                fix16_t x_prestep = fix16_from_int(xl) + FIX_HALF - x_left;
                fix16_t u = u_left + fix16_mul(x_prestep, du_dx);
                fix16_t v = v_left_val + fix16_mul(x_prestep, dv_dx);

                // Dither pattern for lighting
                int offset_x = tex_x;
                if (use_lit && ((y ^ xl) & 1)) offset_x += tex_lit_x;

                // Use ARM assembly optimized scanline renderer
                volatile u16* row = &vram_buffer[y * SCREEN_WIDTH];
                render_scanline_arm(row, xl, xr, u, v, du_dx, dv_dx,
                                    (const u8*)hyperspace_spritesheet, palette_map,
                                    offset_x, tex_y);
            }
        }

        // Step to next scanline
        x_left += dx_left;
        x_right += dx_right;
        z_left += dz_left;
        z_right += dz_right;
        u_left += du_left;
        u_right += du_right;
        v_left_val += dv_left;
        v_right_val += dv_right;
    }
}

__attribute__((target("arm")))
static void rasterize_tri(int index, Triangle* tris, Vec3* projs) {
    Triangle* tri = &tris[index];
    if (tri->tri[0] < 0 || tri->tri[1] < 0 || tri->tri[2] < 0 || !cur_tex) return;
    Vec3* v0 = &projs[tri->tri[0]], *v1 = &projs[tri->tri[1]], *v2 = &projs[tri->tri[2]];

    // Early z-culling: skip if all vertices behind camera
    if (v0->z <= 0 && v1->z <= 0 && v2->z <= 0) return;

    // Early screen bounds culling
    fix16_t min_x = fix16_min(v0->x, fix16_min(v1->x, v2->x));
    fix16_t max_x = fix16_max(v0->x, fix16_max(v1->x, v2->x));
    fix16_t min_y = fix16_min(v0->y, fix16_min(v1->y, v2->y));
    fix16_t max_y = fix16_max(v0->y, fix16_max(v1->y, v2->y));
    if (max_x < 0 || min_x >= F16(SCREEN_WIDTH)) return;
    if (max_y < 0 || min_y >= F16(SCREEN_HEIGHT)) return;

    // Backface culling
    fix16_t nz = fix16_mul(v1->x - v0->x, v2->y - v0->y) - fix16_mul(v1->y - v0->y, v2->x - v0->x);
    if (nz < 0) return;
    Vec3 *tv0 = v0, *tv1 = v1, *tv2 = v2;
    fix16_t *tuv0 = tri->uv[0], *tuv1 = tri->uv[1], *tuv2 = tri->uv[2];
    if (tv1->y < tv0->y) { Vec3* t = tv1; tv1 = tv0; tv0 = t; fix16_t* tu = tuv1; tuv1 = tuv0; tuv0 = tu; }
    if (tv2->y < tv0->y) { Vec3* t = tv2; tv2 = tv0; tv0 = t; fix16_t* tu = tuv2; tuv2 = tuv0; tuv0 = tu; }
    if (tv2->y < tv1->y) { Vec3* t = tv2; tv2 = tv1; tv1 = t; fix16_t* tu = tuv2; tuv2 = tuv1; tuv1 = tu; }
    if (tv0->y == tv2->y) return;
    fix16_t light = fix16_mul(F16(15.0), vec3_dot(t_light_dir, &tri->normal));
    fix16_t c = fix16_div(tv1->y - tv0->y, tv2->y - tv0->y);
    Vec3 v3 = {tv0->x + fix16_mul(c, tv2->x - tv0->x), tv1->y, tv0->z + fix16_mul(c, tv2->z - tv0->z)};
    fix16_t b0 = fix16_mul(fix16_one - c, tv0->z), b1 = fix16_mul(c, tv2->z);
    fix16_t sum = b0 + b1, invd = (sum > F16(0.001)) ? fix16_div(fix16_one, sum) : 0;
    fix16_t uv3[2] = {fix16_mul(fix16_mul(b0, tuv0[0]) + fix16_mul(b1, tuv2[0]), invd), fix16_mul(fix16_mul(b0, tuv0[1]) + fix16_mul(b1, tuv2[1]), invd)};
    if (tv1->x <= v3.x) { rasterize_flat_tri(tv0, tv1, &v3, tuv0, tuv1, uv3, light); rasterize_flat_tri(tv2, tv1, &v3, tuv2, tuv1, uv3, light); }
    else { rasterize_flat_tri(tv0, &v3, tv1, tuv0, uv3, tuv1, light); rasterize_flat_tri(tv2, &v3, tv1, tuv2, uv3, tuv1, light); }
}

// Insertion sort - faster than qsort for small arrays (< 20 elements)
static void sort_tris(Triangle* tris, int num, Vec3* projs) {
    // Calculate z values
    for (int i = 0; i < num; i++) {
        tris[i].z = projs[tris[i].tri[0]].z + projs[tris[i].tri[1]].z + projs[tris[i].tri[2]].z;
    }
    // Insertion sort (stable, fast for small n, no function call overhead)
    for (int i = 1; i < num; i++) {
        Triangle temp = tris[i];
        int j = i - 1;
        while (j >= 0 && tris[j].z > temp.z) {
            tris[j + 1] = tris[j];
            j--;
        }
        tris[j + 1] = temp;
    }
}

// Game initialization
static void init_ship(void) {
    mem_pos = 0;
    decode_mesh(&ship_mesh, fix16_one);
    ship_tex.x = 0; ship_tex.y = 96; ship_tex.light_x = 48;
    ship_tex_laser_lit.x = 0; ship_tex_laser_lit.y = 64; ship_tex_laser_lit.light_x = 48;
}

static void init_nme(void) {
    for (int i = 0; i < 4; i++) {
        decode_mesh(&nme_meshes[i], nme_scale[i]);
        nme_tex[i].x = i * 32; nme_tex[i].y = 32; nme_tex[i].light_x = 16;
    }
    nme_tex_hit.x = 96; nme_tex_hit.y = 64; nme_tex_hit.light_x = 16;
}

static void init_single_trail(Trail* trail, fix16_t z) {
    vec3_set(&trail->pos0, sym_random_fix(F16(100.0)) + ship_x, sym_random_fix(F16(100.0)) + ship_y, z);
    trail->spd = fix16_mul(F16(2.5) + rnd_fix(F16(5.0)), game_spd);
    trail->col = flr_fix(rnd_fix(F16(4.0))) + 1;
}

static void init_trail(void) {
    for (int i = 0; i < MAX_TRAILS; i++) init_single_trail(&trails[i], sym_random_fix(F16(150.0)));
}

static void init_single_bg(Background* bg, fix16_t z) {
    fix16_t a = rnd_fix(fix16_one), r = F16(150.0) + rnd_fix(F16(150.0));
    fix16_t angle = fix16_mul(a, FIX_TWO_PI);
    vec3_set(&bg->pos, fix16_mul(r, fix16_cos(angle)), fix16_mul(r, -fix16_sin(angle)), z);
    bg->spd = F16(0.05) + rnd_fix(F16(0.05));
    if (flr_fix(rnd_fix(F16(6.0))) == 0) bg->index = 8 + (int)rnd_fix(F16(8.0));
    else bg->index = -bg_color[get_random_idx(3)];
}

static void init_bg(void) {
    for (int i = 0; i < MAX_BGS; i++) init_single_bg(&bgs[i], sym_random_fix(F16(400.0)));
}

static void init_main(void) {
    save_cart_data();
    cur_mode = 0; cam_angle_z = F16(-0.4);
    cam_angle_x = fix16_mul(fix16_from_int(flr_fix(rnd_fix(FIX_TWO)) * 2 - 1), F16(0.03) + rnd_fix(F16(0.1)));
    ship_x = ship_y = cam_x = cam_y = ship_spd_x = ship_spd_y = 0;
    life = 4; hit_t = -1; barrel_cur_t = F16(-1.0); laser_on = false;
    global_t = 0; game_spd = fix16_one; cur_nme_t = 0;
    cur_sequencer_x = cur_sequencer_y = 96; next_sequencer_t = 0;
    // Free all enemy projection arrays before resetting count
    for (int i = 0; i < num_enemies; i++) { if (enemies[i].proj) free(enemies[i].proj); enemies[i].proj = 0; }
    num_lasers = num_nme_lasers = num_enemies = nb_nme_ship = 0;
    waiting_nme_clear = spawn_asteroids = false; fade_ratio = F16(-1.0);
    roll_angle = roll_spd = roll_f = pitch_angle = pitch_spd = pitch_f = cur_thrust = 0;
    aim_z = F16(-200.0); tgt_pos = 0; aim_life_ratio = F16(-1.0);
    old_noise_roll = cur_noise_roll = old_noise_pitch = cur_noise_pitch = cur_noise_t = tgt_noise_t = 0;
    asteroid_mul_t = fix16_one; best_score = dget(0);
}

static Laser* spawn_laser(Laser* arr, int* count, Vec3 pos) {
    if (*count >= MAX_LASERS) return 0;
    Laser* l = &arr[*count]; vec3_copy(&l->pos0, &pos); vec3_copy(&l->pos1, &pos); (*count)++; return l;
}

static void remove_laser(Laser* arr, int* count, int idx) {
    if (idx < 0 || idx >= *count) return;
    for (int i = idx; i < *count - 1; i++) arr[i] = arr[i + 1];
    (*count)--;
}

static Enemy* spawn_nme(int type, Vec3 pos) {
    if (num_enemies >= MAX_ENEMIES) return 0;
    Enemy* nme = &enemies[num_enemies];
    vec3_copy(&nme->pos, &pos); nme->type = type;
    // Allocate individual projection array for each enemy (fixes disappearing bug)
    int num_verts = nme_meshes[type - 1].num_vertices;
    nme->proj = (Vec3*)calloc(num_verts > 0 ? num_verts : 1, sizeof(Vec3));
    nme->life = nme_life[type - 1]; nme->hit_t = -1;
    nme->rot_x = nme->rot_y = 0; num_enemies++; return nme;
}

static void spawn_nme_ship(int type) {
    nb_nme_ship++; next_sequencer_t = global_t + F16(0.25);
    fix16_t desc_bounds = fix16_mul(nme_bounds[type - 2], FIX_TWO);
    Vec3 pos = {mid_fix(F16(-100.0), sym_random_fix(F16(50.0)) + ship_x, F16(100.0)),
                mid_fix(F16(-100.0), sym_random_fix(F16(50.0)) + ship_y, F16(100.0)), desc_bounds - F16(200.0)};
    Enemy* nme = spawn_nme(type, pos);
    if (nme) { vec3_set(&nme->spd, 0, 0, F16(8.0)); vec3_copy(&nme->waypoint, &nme->pos); nme->waypoint.z = desc_bounds; }
    if (type == 4) sfx(6, 1);  // Boss spawn sound (square channel)
}

static void hit_ship(Vec3* pos, fix16_t sqr_size) {
    if (hit_t == -1 && barrel_cur_t < 0) {
        fix16_t dx = fix16_mul(pos->x - ship_x, F16(0.2)), dy = fix16_mul(pos->y - ship_y, F16(0.2));
        fix16_t sqrd = fix16_mul(dx, dx) + fix16_mul(dy, dy);
        if (sqrd < sqr_size) {
            fix16_t n = fix16_div(fix16_one, fix16_sqrt(sqrd + F16(0.001)));
            roll_f += fix16_mul(fix16_mul(dx, n), F16(0.05));
            pitch_f -= fix16_mul(fix16_mul(dy, n), F16(0.02));
            hit_t = 0; vec3_copy(&hit_pos, pos); life--;
            sfx(1, 0);  // Player hit sound
            if (life == 0) fade_ratio = 0;
        }
    }
}

// Update functions (abbreviated for space - full implementation)
static void update_enemies(void);
static void update_nme_lasers(void);
static void update_lasers(void);
static void update_trail(void);
static void update_collisions(void);

static void update_trail(void) {
    for (int i = 0; i < MAX_TRAILS; i++) {
        Trail* t = &trails[i];
        if (t->pos0.z >= F16(150.0)) init_single_trail(t, F16(-150.0));
        vec3_copy(&t->pos1, &t->pos0); t->pos0.z += t->spd;
    }
    for (int i = 0; i < MAX_BGS; i++) {
        Background* bg = &bgs[i];
        bg->pos.z += fix16_mul(bg->spd, game_spd);
        if (bg->pos.z >= F16(400.0)) init_single_bg(bg, F16(-400.0));
    }
}

static void update_nme_lasers(void) {
    for (int i = 0; i < num_nme_lasers; i++) {
        Laser* l = &nme_lasers[i];
        vec3_copy(&l->pos1, &l->pos0);
        l->pos0.x += l->spd.x; l->pos0.y += l->spd.y; l->pos0.z += l->spd.z;
        if (l->pos0.z >= 0) { hit_ship(&l->pos0, F16(1.5)); hit_ship(&l->pos1, F16(1.5)); remove_laser(nme_lasers, &num_nme_lasers, i); i--; }
    }
}

static void update_lasers(void) {
    cur_laser_t += fix16_one; laser_spawned = false;
    if (laser_on && cur_laser_t > FIX_TWO) {
        cur_laser_t = 0; laser_spawned = true;
        Vec3 pos = {fix16_from_int(cur_laser_side), F16(-1.5), F16(-8.0)}, world_pos;
        mat_mul_pos(&world_pos, &ship_mat, &pos);
        spawn_laser(lasers, &num_lasers, world_pos);
        cur_laser_side = -cur_laser_side;
    }
    for (int i = 0; i < num_lasers; i++) {
        Laser* l = &lasers[i]; vec3_copy(&l->pos1, &l->pos0); l->pos0.z -= F16(5.0);
        if (l->pos0.z <= F16(-200.0)) { remove_laser(lasers, &num_lasers, i); i--; }
    }
}

static void update_enemies(void) {
    for (int i = 0; i < num_enemies; i++) {
        Enemy* nme = &enemies[i];
        nme->pos.x += fix16_mul(nme->spd.x, game_spd);
        nme->pos.y += fix16_mul(nme->spd.y, game_spd);
        nme->pos.z += fix16_mul(nme->spd.z, game_spd);
        nme->rot_x += nme->rot_x_spd; nme->rot_y += nme->rot_y_spd;
        int type = nme->type;
        if (type > 1) {
            int sub_type = type - 1;
            if (sub_type >= 1 && sub_type <= 3) {
                fix16_t desc_bounds = nme_bounds[sub_type - 1], desc_spd = nme_spd[sub_type - 1];
                Vec3 dir = vec3_minus(&nme->waypoint, &nme->pos); vec3_mul(&dir, F16(0.1));
                fix16_t dist = vec3_dot(&dir, &dir);
                if (dist < fix16_mul(game_spd, game_spd) || nme->hit_t == 0)
                    vec3_set(&nme->waypoint, sym_random_fix(F16(100.0)), sym_random_fix(F16(100.0)), desc_bounds - rnd_fix(-desc_bounds));
                vec3_normalize(&dir);
                nme->spd.x += fix16_mul(fix16_mul(dir.x, desc_spd), F16(0.1));
                nme->spd.y += fix16_mul(fix16_mul(dir.y, desc_spd), F16(0.1));
                nme->spd.z += fix16_mul(fix16_mul(dir.z, desc_spd), F16(0.1));
                if (nme->pos.z >= fix16_mul(desc_bounds, FIX_TWO)) {
                    fix16_t spd_len = vec3_length(&nme->spd);
                    if (spd_len > desc_spd) vec3_mul(&nme->spd, fix16_div(desc_spd, spd_len));
                    nme->rot_x = fix16_mul(F16(-0.08), nme->spd.y);
                    nme->rot_y = fix16_mul(-nme_rot[sub_type - 1], nme->spd.x);

                    // Enemy laser spawning logic
                    int nb_lasers = type - 2;

                    if ((type == 4 || nme->hit_t == 0) && nme->laser_t < 0) {
                        nme->laser_t = 0;
                    }

                    nme->laser_t += fix16_one;

                    if (nme->laser_t > nme->stop_laser_t) {
                        nme->laser_t = -fix16_div(F16(60.0) + rnd_fix(F16(60.0)), game_spd);
                        nme->stop_laser_t = F16(60.0) + rnd_fix(F16(60.0));
                        fix16_t c = fix16_mul(F16(-0.5), fix16_div(nme->pos.z, game_spd));
                        for (int j = 0; j < nb_lasers && j < 2; j++) {
                            nme->laser_offset_x[j] = sym_random_fix(F16(30.0)) + fix16_mul(ship_spd_x, c);
                            nme->laser_offset_y[j] = sym_random_fix(F16(30.0)) + fix16_mul(ship_spd_y, c);
                        }
                    }

                    fix16_t laser_t_val = nme->laser_t;
                    fix16_t t = fix16_div(F16(6.0), game_spd);
                    if (laser_t_val > 0) {
                        nme->next_laser_t += fix16_one;

                        if (nme->next_laser_t >= t) {
                            nme->next_laser_t -= t;

                            if (type != 2) {
                                fix16_t angle = fix16_mul(fix16_div(laser_t_val, F16(120.0)), FIX_TWO_PI);
                                fix16_t ratio = fix16_cos(angle);

                                for (int j = 0; j < nb_lasers && j < 2; j++) {
                                    Vec3 laser_pos;
                                    Mesh* mesh = &nme_meshes[type - 1];
                                    if (j < mesh->num_vertices) {
                                        laser_pos.x = nme->pos.x + mesh->vertices[j].x;
                                        laser_pos.y = nme->pos.y;
                                        laser_pos.z = nme->pos.z + mesh->vertices[j].z;
                                    } else {
                                        laser_pos = nme->pos;
                                    }

                                    Laser* laser = spawn_laser(nme_lasers, &num_nme_lasers, laser_pos);
                                    if (laser) {
                                        Vec3 target = {
                                            ship_x + fix16_mul(nme->laser_offset_x[j], ratio) + sym_random_fix(F16(5.0)),
                                            ship_y + fix16_mul(nme->laser_offset_y[j], ratio) + sym_random_fix(F16(5.0)),
                                            0
                                        };
                                        Vec3 ldir = vec3_minus(&target, &laser_pos);
                                        vec3_mul(&ldir, F16(0.1));
                                        fix16_t len = vec3_length(&ldir);
                                        fix16_t v = (len > F16(0.001)) ? fix16_div(fix16_mul(FIX_TWO, game_spd), len) : fix16_mul(FIX_TWO, game_spd);
                                        laser->spd.x = fix16_mul(ldir.x, v);
                                        laser->spd.y = fix16_mul(ldir.y, v);
                                        laser->spd.z = fix16_mul(ldir.z, v);
                                    }
                                }
                            } else {
                                Vec3 laser_pos = {nme->pos.x, nme->pos.y, nme->pos.z + F16(12.0)};
                                Laser* laser = spawn_laser(nme_lasers, &num_nme_lasers, laser_pos);
                                if (laser) {
                                    laser->spd.x = sym_random_fix(F16(0.05));
                                    laser->spd.y = sym_random_fix(F16(0.05));
                                    laser->spd.z = fix16_mul(FIX_TWO, game_spd);
                                }
                            }
                        }
                    }
                }
            }
        }
        bool del = false;
        if (nme->pos.z > 0) { hit_ship(&nme->pos, F16(2.5)); del = true; }
        if (nme->life <= 0) { nme->life--; if (nme->life < -15) del = true; }
        if (nme->hit_t > -1) { nme->hit_t++; if (nme->hit_t > 5) nme->hit_t = -1; }
        if (del) {
            if (nme->type > 1) nb_nme_ship--;
            if (nme->proj) free(nme->proj);  // Free projection array before replacing
            enemies[i] = enemies[num_enemies - 1];
            num_enemies--; i--;
        }
    }
    cur_nme_t -= fix16_one;
    if (spawn_asteroids && cur_nme_t <= 0) {
        cur_nme_t = fix16_div(fix16_mul(F16(30.0) + rnd_fix(F16(60.0)), asteroid_mul_t), game_spd);
        Vec3 pos = {mid_fix(F16(-100.0), ship_x + sym_random_fix(F16(30.0)), F16(100.0)),
                    mid_fix(F16(-100.0), ship_y + sym_random_fix(F16(30.0)), F16(100.0)), F16(-50.0)};
        Enemy* nme = spawn_nme(1, pos);
        if (nme) { vec3_set(&nme->spd, sym_random_fix(F16(0.25)), sym_random_fix(F16(0.25)), F16(0.25));
            nme->rot_x_spd = sym_random_fix(F16(0.015)); nme->rot_y_spd = sym_random_fix(F16(0.015)); }
    }
}

static void update_collisions(void) {
    int laser_idx = 0, nme_idx = num_enemies - 1;
    while (laser_idx < num_lasers && nme_idx >= 0) {
        Vec3* lp0 = &lasers[laser_idx].pos0, *lp1 = &lasers[laser_idx].pos1;
        Enemy* nme = &enemies[nme_idx];
        if (nme->pos.z > lp1->z) laser_idx++;
        else {
            if (nme->life > 0 && nme->pos.z >= lp0->z) {
                fix16_t dx = fix16_mul(lp0->x - nme->pos.x, F16(0.2)), dy = fix16_mul(lp0->y - nme->pos.y, F16(0.2));
                fix16_t radius = nme_radius[nme->type - 1];
                if (fix16_mul(dx, dx) + fix16_mul(dy, dy) <= fix16_mul(fix16_mul(radius, radius), F16(0.04))) {
                    nme->life--;
                    sfx(2, 2);  // Hit/explosion sound (noise channel)
                    if (nme->life == 0) { nme->hit_t = -1; score += nme_score[nme->type - 1]; sfx(5, 1); }  // Bonus sound on kill
                    else { vec3_copy(&nme->hit_pos, lp0); nme->hit_t = 0; if (nme->type == 4) sfx(7, 2); }  // Boss damage
                    remove_laser(lasers, &num_lasers, laser_idx); continue;
                }
            }
            nme_idx--;
        }
    }
}

// Main game update
static void game_update(void) {
    fix16_t dx = 0, dy = 0;
    if (btn(0)) dx -= fix16_one; if (btn(1)) dx += fix16_one;
    if (btn(2)) dy -= fix16_one; if (btn(3)) dy += fix16_one;

    if (cur_mode == 2) {
        global_t += F16(0.033); game_spd = fix16_one + fix16_mul(global_t, F16(0.002));
        if (dx == 0 && dy == 0) cur_thrust = 0; else cur_thrust = fix16_min(FIX_HALF, cur_thrust + F16(0.1));
        fix16_t mul_spd = cur_thrust;
        if (non_inverted_y) dy = -dy;
        if (barrel_cur_t > F16(-1.0) || life <= 0) { dx = dy = 0; }
        if (btn(5) && dx != 0 && barrel_cur_t < 0) { barrel_cur_t = 0; barrel_dir = dx > 0 ? 1 : -1; sfx(4, 1); }
        if (barrel_cur_t >= 0) { barrel_cur_t += fix16_one; dx = fix16_from_int(barrel_dir * 9); dy = 0; mul_spd = F16(0.1); if (barrel_cur_t > F16(5.0)) barrel_cur_t = F16(-20.0); }
        if (fix16_abs(ship_x) > F16(100.0)) dx = fix16_mul(-sgn_fix(ship_x), F16(0.4));
        if (fix16_abs(ship_y) > F16(100.0)) dy = fix16_mul(-sgn_fix(ship_y), F16(0.4));
        ship_spd_x += fix16_mul(dx, mul_spd); ship_spd_y += fix16_mul(dy, mul_spd);
        roll_f -= fix16_mul(F16(0.003), dx); pitch_f += fix16_mul(F16(0.0008), dy);
        ship_spd_x = fix16_mul(ship_spd_x, F16(0.85)); ship_spd_y = fix16_mul(ship_spd_y, F16(0.85));
        ship_x += ship_spd_x; ship_y += ship_spd_y;
        cam_x = fix16_mul(F16(1.05), ship_x); cam_y = ship_y + F16(11.5);
        if (hit_t != -1) { cam_x += sym_random_fix(FIX_TWO); cam_y += sym_random_fix(FIX_TWO); }
        else if (life <= 0) { hit_t = 0; vec3_set(&hit_pos, ship_x, ship_y, 0); }
        cam_angle_z = fix16_mul(cam_x, F16(0.0005)); cam_angle_x = fix16_mul(cam_y, F16(0.0003));
        if (waiting_nme_clear) { if (nb_nme_ship == 0) { next_sequencer_t = 0; waiting_nme_clear = false; } else next_sequencer_t = F16(32767.0); }
        if (global_t >= next_sequencer_t) {
            int value = sget(cur_sequencer_x, cur_sequencer_y); cur_sequencer_x++;
            if (cur_sequencer_x > 127) { cur_sequencer_x = 96; cur_sequencer_y++; }
            if (value == 1) spawn_nme_ship(3); else if (value == 13) spawn_nme_ship(4);
            else if (value == 2) spawn_nme_ship(2); else if (value == 6) { spawn_asteroids = true; asteroid_mul_t = fix16_one; }
            else if (value == 7) { spawn_asteroids = true; asteroid_mul_t = FIX_HALF; } else if (value == 5) spawn_asteroids = false;
            else if (value == 10) next_sequencer_t = global_t + fix16_one; else if (value == 9) next_sequencer_t = global_t + F16(10.0);
            else if (value == 11) waiting_nme_clear = true; else { cur_sequencer_x = 96; cur_sequencer_y = 96; }
        }
    } else if (cur_mode == 0) {
        if (dx == 0 && dy == 0) dx = F16(-0.25);
        cam_angle_z += fix16_mul(dx, F16(0.007)); cam_angle_x -= fix16_mul(dy, F16(0.007));
        if (btnp(5)) { cur_mode = 3; manual_fire = dget(1); non_inverted_y = dget(2); sound_enabled = dget(3); }
    } else if (cur_mode == 3) {
        cam_angle_z -= F16(0.00175);
        if (btnp(0) || btnp(1)) { manual_fire = 1 - manual_fire; dset(1, manual_fire); }
        if (btnp(2) || btnp(3)) { non_inverted_y = 1 - non_inverted_y; dset(2, non_inverted_y); }
        if (btnp(4)) { sound_enabled = 1 - sound_enabled; dset(3, sound_enabled); }
        if (btnp(5)) {
            src_cam_angle_z = normalize_angle(cam_angle_z); src_cam_angle_x = normalize_angle(cam_angle_x);
            src_cam_x = cam_x; src_cam_y = cam_y;
            dst_cam_x = fix16_mul(F16(1.05), ship_x); dst_cam_y = ship_y + F16(11.5);
            dst_cam_angle_z = fix16_mul(dst_cam_x, F16(0.0005)); dst_cam_angle_x = fix16_mul(dst_cam_y, F16(0.0003));
            Vec3 src = {src_cam_x, src_cam_y, F16(26.0)}, dst = {dst_cam_x, dst_cam_y, F16(22.5)};
            Vec3 diff = vec3_minus(&src, &dst); fix16_t len = vec3_length(&diff);
            interpolation_spd = (len > F16(0.01)) ? fix16_div(F16(0.25), len) : fix16_one;
            interpolation_ratio = 0; cur_mode = 1;
        }
    } else {
        interpolation_ratio += interpolation_spd;
        if (interpolation_ratio >= fix16_one) { cur_mode = 2; score = 0; }
        else {
            fix16_t r = smoothstep(interpolation_ratio);
            cam_x = src_cam_x + fix16_mul(r, dst_cam_x - src_cam_x);
            cam_y = src_cam_y + fix16_mul(r, dst_cam_y - src_cam_y);
            cam_depth = F16(22.5) + fix16_mul(r, F16(3.5));
            cam_angle_z = src_cam_angle_z + fix16_mul(r, dst_cam_angle_z - src_cam_angle_z);
            cam_angle_x = src_cam_angle_x + fix16_mul(r, dst_cam_angle_x - src_cam_angle_x);
        }
    }

    bool prev_laser_on = laser_on;
    laser_on = (cur_mode != 2 && btn(4)) || ((btn(4) || (manual_fire != 1 && tgt_pos)) && barrel_cur_t < 0 && hit_t == -1);
    // Laser sound effects
    if (laser_on && !prev_laser_on) sfx(0, 0);
    else if (!laser_on && prev_laser_on) sfx(-2, 0);
    Mat34 trans, rot;
    mat_translation(&trans, 0, 0, -cam_depth); mat_rotx(&rot, cam_angle_x); mat_mul(&cam_mat, &trans, &rot);
    mat_roty(&rot, cam_angle_z); mat_mul(&cam_mat, &cam_mat, &rot);
    mat_translation(&trans, -cam_x, -cam_y, 0); mat_mul(&cam_mat, &cam_mat, &trans);
    roll_f -= fix16_mul(roll_angle, F16(0.02)); roll_spd = fix16_mul(roll_spd, F16(0.8)) + roll_f; roll_angle += roll_spd;
    pitch_f -= fix16_mul(pitch_angle, F16(0.02)); pitch_spd = fix16_mul(pitch_spd, F16(0.8)) + pitch_f; pitch_angle += pitch_spd;
    roll_f = pitch_f = 0; roll_angle = normalize_angle(roll_angle);
    mat_translation(&ship_pos_mat, ship_x, ship_y, 0); mat_rotx(&rot, normalize_angle(pitch_angle)); mat_mul(&ship_mat, &ship_pos_mat, &rot);
    mat_rotz(&rot, normalize_angle(roll_angle)); mat_mul(&ship_mat, &ship_mat, &rot);
    mat_transpose_rot(&inv_ship_mat, &ship_mat);
    update_trail(); if (cur_mode == 2) update_enemies();
    update_lasers(); update_nme_lasers(); update_collisions();
    if (hit_t != -1) { hit_t++; if (hit_t > 15) hit_t = -1; }
    mat_mul(&ship_mat, &cam_mat, &ship_mat); mat_mul(&ship_pos_mat, &cam_mat, &ship_pos_mat);
    mat_rotx(&light_mat, F16(0.14)); mat_roty(&rot, F16(0.34) + fix16_mul(global_t, F16(0.003)));
    mat_mul(&light_mat, &light_mat, &rot);
    Vec3 light_src = {0, 0, -fix16_one}; mat_mul_vec(&light_dir, &light_mat, &light_src);
    mat_mul_vec(&ship_light_dir, &inv_ship_mat, &light_dir);
    if (fade_ratio >= 0) {
        if (cur_mode == 2) fade_ratio += FIX_TWO; else fade_ratio -= FIX_TWO;
        if (fade_ratio >= F16(100.0)) { if (score > best_score) { best_score = score; dset(0, best_score); } init_main(); }
    }
}

// Drawing functions
static void transform_vert(void) {
    for (int i = 0; i < ship_mesh.num_vertices; i++) transform_pos(&ship_mesh.projected[i], &ship_mat, &ship_mesh.vertices[i]);
    Vec3 aim_pos = {ship_x, ship_y - F16(1.5), aim_z}; transform_pos(&aim_proj, &cam_mat, &aim_pos);
    fix16_t auto_aim_dist = F16(30.0); tgt_pos = 0; aim_life_ratio = F16(-1.0);
    if (cur_mode == 2) {
        for (int i = 0; i < num_enemies; i++) {
            Enemy* nme = &enemies[i]; Mat34 nme_mat, nme_rot_x, nme_rot_z, inv_nme_mat;
            mat_translation(&nme_mat, nme->pos.x, nme->pos.y, nme->pos.z);
            mat_rotx(&nme_rot_x, nme->rot_x); mat_mul(&nme_mat, &nme_mat, &nme_rot_x);
            mat_rotz(&nme_rot_z, nme->rot_y); mat_mul(&nme_mat, &nme_mat, &nme_rot_z);
            mat_transpose_rot(&inv_nme_mat, &nme_mat); mat_mul_vec(&nme->light_dir, &inv_nme_mat, &light_dir);
            Mat34 final_nme_mat; mat_mul(&final_nme_mat, &cam_mat, &nme_mat);
            Mesh* mesh = &nme_meshes[nme->type - 1];
            for (int j = 0; j < mesh->num_vertices; j++) transform_pos(&nme->proj[j], &final_nme_mat, &mesh->vertices[j]);
            if (nme->life > 0) {
                fix16_t ddx = fix16_mul(nme->proj[0].x - aim_proj.x, F16(0.1)), ddy = fix16_mul(nme->proj[0].y - aim_proj.y, F16(0.1));
                fix16_t sqr_dist = fix16_mul(ddx, ddx) + fix16_mul(ddy, ddy);
                if (sqr_dist < auto_aim_dist) { auto_aim_dist = sqr_dist; tgt_pos = &nme->pos; }
            }
        }
    }
    fix16_t tgt_z = F16(-200.0); if (tgt_pos) tgt_z = tgt_pos->z;
    aim_z += fix16_mul(tgt_z - aim_z, F16(0.2));
    Vec3 star_pos = {fix16_mul(light_mat.m[2], F16(100.0)), fix16_mul(light_mat.m[6], F16(100.0)), fix16_mul(light_mat.m[10], F16(100.0))};
    transform_pos(&star_proj, &ship_pos_mat, &star_pos);
}

static void draw_explosion(Vec3* proj, fix16_t size) {
    fix16_t invz = proj->z; int col = explosion_color[get_random_idx(4)];
    circfill(fix16_to_int(proj->x + fix16_mul(sym_random_fix(fix16_mul(size, FIX_HALF)), invz)),
             fix16_to_int(proj->y + fix16_mul(sym_random_fix(fix16_mul(size, FIX_HALF)), invz)),
             fix16_to_int(fix16_mul(invz, size + rnd_fix(size))), col);
}

static void print_3d(const char* str, int x, int y) {
    print_str(str, x + 2, y + 2, 1); print_str(str, x + 1, y + 1, 13); print_str(str, x, y, 7);
}

static void draw_lasers(Laser* arr, int count, int col) {
    Vec3 p0, p1;
    for (int i = 0; i < count; i++) {
        transform_pos(&p0, &cam_mat, &arr[i].pos0); transform_pos(&p1, &cam_mat, &arr[i].pos1);
        if (p0.z > 0 && p1.z > 0) line(fix16_to_int(p0.x), fix16_to_int(p0.y), fix16_to_int(p1.x), fix16_to_int(p1.y), col);
    }
}

static void set_ngn_pal(void) {
    // Fast wrap-around without division
    ngn_col_idx += fix16_one;
    if (ngn_col_idx >= F16(4.0)) ngn_col_idx -= F16(4.0);
    ngn_laser_col_idx += F16(0.2);
    if (ngn_laser_col_idx >= F16(4.0)) ngn_laser_col_idx -= F16(4.0);
    pal(12, ngn_colors[fix16_to_int(ngn_col_idx)]);
    int idx = fix16_to_int(ngn_laser_col_idx);
    pal(8, laser_ngn_colors[idx]); pal(14, laser_ngn_colors[(idx + 1) & 3]); pal(15, laser_ngn_colors[(idx + 2) & 3]);
}

static void game_draw(void) {
    Vec3 p0, p1; cls(); transform_vert();
    for (int i = 0; i < MAX_BGS; i++) {
        Background* bg = &bgs[i]; transform_pos(&p0, &ship_pos_mat, &bg->pos);
        if (p0.z > 0) { int idx = bg->index; if (idx > 0) spr(idx + 16 * flr_fix(rnd_fix(FIX_TWO)), fix16_to_int(p0.x), fix16_to_int(p0.y), 1, 1);
            else { int c = 7; if (rnd_fix(fix16_one) > FIX_HALF) c = -idx; pset(fix16_to_int(p0.x), fix16_to_int(p0.y), c); } }
    }
    star_visible = star_proj.z > 0 && star_proj.x >= 0 && star_proj.x < F16(SCREEN_WIDTH) && star_proj.y >= 0 && star_proj.y < F16(SCREEN_HEIGHT);
    if (star_visible) { int idx = 32 + flr_fix(rnd_fix(F16(4.0))) * 2; spr(idx, fix16_to_int(star_proj.x) - 7, fix16_to_int(star_proj.y) - 7, 2, 2); }
    fix16_t trail_coef = F16(2.25);
    for (int i = 0; i < MAX_TRAILS; i++) {
        Trail* t = &trails[i]; transform_pos(&p0, &cam_mat, &t->pos0); transform_pos(&p1, &cam_mat, &t->pos1);
        if (p0.z > 0 && p1.z > 0) { int idx = fix16_to_int(mid_fix(fix16_from_int(t->col), fix16_div(trail_coef, p0.z) + fix16_one, F16(5.0))) - 1;
            if (idx < 0) idx = 0; if (idx > 4) idx = 4; line(fix16_to_int(p0.x), fix16_to_int(p0.y), fix16_to_int(p1.x), fix16_to_int(p1.y), trail_color[idx]); }
    }
    if (cur_mode == 2) {
        for (int i = num_enemies - 1; i >= 0; i--) {
            Enemy* nme = &enemies[i]; Mesh* mesh = &nme_meshes[nme->type - 1]; cur_tex = &nme_tex[nme->type - 1];
            if (nme->life < 0 || nme->hit_t > -1) {
                if (nme->life < 0) { fix16_t r = FIX_HALF + fix16_div(F16(15.0) + fix16_from_int(nme->life), F16(30.0));
                    fix16_t sz = fix16_mul(fix16_mul(r, nme_radius[nme->type - 1]), F16(0.8));
                    if (((-nme->life) & 1) == 0) cur_tex = &nme_tex_hit;
                    for (int j = 0; j < 3; j++) draw_explosion(&nme->proj[get_random_idx(mesh->num_vertices)], sz);
                } else { fix16_t r = FIX_HALF + fix16_div(F16(6.0) - fix16_from_int(nme->hit_t), F16(12.0));
                    if ((nme->hit_t & 1) == 0) cur_tex = &nme_tex_hit; transform_pos(&p0, &cam_mat, &nme->hit_pos); draw_explosion(&p0, fix16_mul(r, F16(3.0))); }
            }
            if (cur_tex) { t_light_dir = &nme->light_dir; for (int j = 0; j < mesh->num_triangles; j++) rasterize_tri(j, mesh->triangles, nme->proj); }
        }
    }
    draw_lasers(nme_lasers, num_nme_lasers, 8); draw_lasers(lasers, num_lasers, 11);
    if (cur_mode == 2) { int idx = 97; if (tgt_pos) idx = 98; spr(idx, fix16_to_int(aim_proj.x) - 3, fix16_to_int(aim_proj.y) - 3, 1, 1); }
    if (laser_spawned) cur_tex = &ship_tex_laser_lit; else cur_tex = &ship_tex;
    sort_tris(ship_mesh.triangles, ship_mesh.num_triangles, ship_mesh.projected);
    if (hit_t != -1) { transform_pos(&p0, &cam_mat, &hit_pos); draw_explosion(&p0, F16(3.0));
        if ((hit_t & 1) == 0) { pal(0, 2); pal(1, 8); pal(6, 14); pal(9, 8); pal(10, 14); pal(13, 14); } }
    t_light_dir = &ship_light_dir; set_ngn_pal();
    for (int i = 0; i < ship_mesh.num_triangles; i++) rasterize_tri(i, ship_mesh.triangles, ship_mesh.projected);
    pal_reset();
    if (cur_mode == 2) { char buf[32]; snprintf(buf, 32, "SCORE %d", score); print_3d(buf, 1, 1);
        spr(16, 99, 1, 8, 1); clip_set(99, 1, life * 15, 7); spr(0, 99, 1, 8, 1); clip_reset();
    } else if (cur_mode != 1) { print_3d("HYPERSPACE", 58, 1); print_3d("GBA Port by itsmeterada", 34, 8);
        if (cur_mode == 0) { print_3d("PRESS L/R", 58, 100); char buf[32]; snprintf(buf, 32, "BEST %d", best_score); print_3d(buf, 1, 120); }
        else { print_3d("PRESS L/R", 50, 55); print_3d("DPAD:OPT A:SND", 38, 65);
            const char* opt[] = {"AUTO", "MANUAL", "INV Y", "NORM Y", "SND OFF", "SND ON"};
            print_3d(opt[manual_fire], 9, 100); print_3d(opt[non_inverted_y + 2], 9, 110); print_3d(opt[sound_enabled + 4], 9, 120); } }
    if (fade_ratio > 0) { Vec3 center = {FIX_SCREEN_CENTER_X, FIX_SCREEN_CENTER_Y, fix16_one}; draw_explosion(&center, fade_ratio); }
}

// GBA-specific - optimized
static void vsync(void) { while (REG_VCOUNT >= 160); while (REG_VCOUNT < 160); }

static void flip_screen(void) {
    // Swap display page
    // When DCNT_PAGE is set: display page 2 (0x0600A000), draw to page 1
    // When DCNT_PAGE is clear: display page 1 (0x06000000), draw to page 2
    current_page = 1 - current_page;
    REG_DISPCNT = DCNT_MODE5 | DCNT_BG2 | (current_page ? DCNT_PAGE : 0);
    // Point vram_buffer to the back buffer (opposite of displayed page)
    vram_buffer = current_page ? VRAM_PAGE1 : VRAM_PAGE2;
}

static void update_input(void) {
    u16 keys = ~REG_KEYINPUT;
    for (int i = 0; i < 6; i++) btn_prev[i] = btn_state[i];
    btn_state[0] = (keys & KEY_LEFT) != 0; btn_state[1] = (keys & KEY_RIGHT) != 0;
    btn_state[2] = (keys & KEY_UP) != 0; btn_state[3] = (keys & KEY_DOWN) != 0;
    btn_state[4] = (keys & (KEY_A | KEY_B)) != 0; btn_state[5] = (keys & (KEY_L | KEY_R)) != 0;
}

static void load_embedded_data(void) {
    // Spritesheet is accessed directly from ROM (hyperspace_spritesheet)
    // Only copy map data to RAM
    memcpy(map_memory, hyperspace_map, sizeof(map_memory));

    // Initialize dither pattern in IWRAM for fast access
    for (int y = 0; y < 8; y++) {
        for (int x = 0; x < 8; x++) {
            dither_pattern[y][x] = SGET_FAST(x, 56 + y);
        }
    }
}

static void clear_vram(void) {
    // Clear both VRAM pages at startup using DMA
    u16 zero = 0;
    DMAFastCopy(&zero, (void*)VRAM_PAGE1, SCREEN_WIDTH * SCREEN_HEIGHT,
                DMA_SRC_FIXED | DMA_DST_INC | DMA_16BIT | DMA_ENABLE);
    DMAFastCopy(&zero, (void*)VRAM_PAGE2, SCREEN_WIDTH * SCREEN_HEIGHT,
                DMA_SRC_FIXED | DMA_DST_INC | DMA_16BIT | DMA_ENABLE);
}

static void game_init(void) {
    // Initialize: display page 1, draw to page 2
    current_page = 0;
    vram_buffer = VRAM_PAGE2;

    clear_vram();
    pal_reset();
    load_cart_data();
    load_embedded_data();
    init_main(); init_ship(); init_nme(); init_trail(); init_bg();
}

int main(void) {
    REG_DISPCNT = DCNT_MODE5 | DCNT_BG2;

    // Set up BG2 affine transformation for scaling 160x128 → 240x160
    // Scale factors: X = 160/240 = 0.6667, Y = 128/160 = 0.8
    // In 8.8 fixed point: PA = 171 (0.6667*256), PD = 205 (0.8*256)
    REG_BG2PA = 171;        // X scale: 160/240 in 8.8 fixed point
    REG_BG2PB = 0;          // No rotation/shear
    REG_BG2PC = 0;          // No rotation/shear
    REG_BG2PD = 205;        // Y scale: 128/160 in 8.8 fixed point
    REG_BG2X = 0;           // Start from left edge
    REG_BG2Y = 0;           // Start from top edge

    game_init();
    sound_init();  // Initialize sound system
    while (1) {
        update_input();
        game_update();
        sound_update();  // Update sound playback
        game_draw();
        vsync();        // Wait for VBlank
        flip_screen();  // Flip during VBlank to avoid tearing
    }
    return 0;
}
