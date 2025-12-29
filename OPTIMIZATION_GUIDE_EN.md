# Hyperspace GBA Port - Optimization Guide

This document provides a detailed explanation of all optimizations performed during the port from the PicoSystem version (original) to the GBA version, with side-by-side code comparisons.

## Table of Contents

1. [Hardware Differences](#1-hardware-differences)
2. [Memory Layout Optimization](#2-memory-layout-optimization)
3. [Division Elimination (Reciprocal LUT)](#3-division-elimination-reciprocal-lut)
4. [ARM Assembly Acceleration](#4-arm-assembly-acceleration)
5. [DMA Screen Clearing](#5-dma-screen-clearing)
6. [Rasterizer Optimization](#6-rasterizer-optimization)
7. [Sorting Algorithm Improvement](#7-sorting-algorithm-improvement)
8. [Early Culling](#8-early-culling)
9. [Direct VRAM Rendering](#9-direct-vram-rendering)
10. [Other Optimizations](#10-other-optimizations)

---

## 1. Hardware Differences

### PicoSystem (RP2040)
- **CPU**: ARM Cortex-M0+ dual-core @ 125-250MHz
- **RAM**: 264KB SRAM
- **Display**: 240×240 (120×120 with pixel doubling)
- **Bus Width**: 32-bit
- **Features**: Hardware interpolators available

### Game Boy Advance (ARM7TDMI)
- **CPU**: ARM7TDMI @ 16.78MHz (approximately 1/10 the clock speed)
- **RAM**: IWRAM 32KB (fast) + EWRAM 256KB (slow)
- **Display**: 240×160 (Mode 5: 160×128)
- **Bus Width**: ROM 16-bit, IWRAM 32-bit
- **Features**: DMA controller available

### Challenge

Since the GBA runs at approximately 1/10 the clock frequency, significant optimizations are required to maintain equivalent frame rates.

---

## 2. Memory Layout Optimization

### Original (PicoSystem)

```c
// All data placed in regular RAM
static Trail trails[MAX_TRAILS];
static Enemy enemies[MAX_ENEMIES];
static Background bgs[MAX_BGS];
```

### GBA Version

```c
// Place large arrays in EWRAM (prevents IWRAM overflow)
#define EWRAM_DATA __attribute__((section(".ewram")))

EWRAM_DATA static Trail trails[MAX_TRAILS];
EWRAM_DATA static Enemy enemies[MAX_ENEMIES];
EWRAM_DATA static Background bgs[MAX_BGS];
EWRAM_DATA static s32 cart_data[64];
```

```c
// Place hot code/data in IWRAM
#define IWRAM_CODE __attribute__((section(".iwram"), long_call))
#define IWRAM_DATA __attribute__((section(".iwram")))

// Dither pattern requiring fast access
IWRAM_DATA static u8 dither_pattern[8][8];
```

### Explanation

| Memory Region | Size | Access Speed | Usage |
|--------------|------|--------------|-------|
| IWRAM | 32KB | 1 cycle (32-bit) | Hot functions, frequently accessed data |
| EWRAM | 256KB | 2-3 cycles (16-bit) | Large arrays, infrequently accessed data |
| ROM | - | 2-3 cycles (16-bit) | Code, const data |

IWRAM has a 32-bit bus and is the fastest, but its size is limited. Only small, frequently accessed data should be placed there.

---

## 3. Division Elimination (Reciprocal LUT)

Division on ARM7TDMI takes approximately 50+ cycles. This is reduced to about 5 cycles using a LUT.

### Original (PicoSystem)

```c
// Frequent division in rasterizer
fix16_t c = fix16_div(y1 - y0, y2 - y0);

// Scanline interpolation
fix16_t inv_span = fix16_div(fix16_one, span);
```

### GBA Version

```c
// 513-entry reciprocal LUT (1/1 to 1/512)
static const u32 recip_lut[513] = {
    0xFFFFFFFF, // 0: unused
    0x10000, 0x8000, 0x5555, 0x4000, 0x3333, 0x2AAA, 0x2492, 0x2000, // 1-8
    0x1C71, 0x1999, 0x1745, 0x1555, 0x13B1, 0x1249, 0x1111, 0x1000, // 9-16
    // ... continues to 512 entries
};

// Fast reciprocal function (linear interpolation for accuracy)
IWRAM_CODE static inline fix16_t fast_recip(fix16_t x) {
    if (x <= 0) return 0;
    // Fall back to traditional division for values < 1.0
    if (x < fix16_one) {
        return fix16_div(fix16_one, x);
    }
    // Use LUT for values >= 1.0
    int ix = x >> 16;  // Integer part
    if (ix > 511) return recip_lut[512];

    // Linear interpolation for fractional accuracy
    u32 base = recip_lut[ix];
    u32 next = recip_lut[ix + 1];
    int frac = (x >> 8) & 0xFF;  // 8-bit fractional part
    return base - (((base - next) * frac) >> 8);
}
```

### Usage Example

```c
// Original
fix16_t c = fix16_div(y1 - y0, y2 - y0);

// GBA Version
fix16_t total_dy = tv2->y - tv0->y;
fix16_t c = fix16_mul(tv1->y - tv0->y, fast_recip(total_dy));
```

### Performance Improvement

- **Division**: ~50 cycles
- **LUT lookup + interpolation**: ~5 cycles
- **Speedup**: ~10x

With 10-50 divisions per triangle, this results in significant overall speedup.

---

## 4. ARM Assembly Acceleration

### 4.1 Fixed-Point Multiplication

#### Original (PicoSystem)

```c
// Using libfixmath function
fix16_t result = fix16_mul(a, b);

// Or direct calculation
static inline fix16_t fix16_mul(fix16_t a, fix16_t b) {
    return (fix16_t)(((int64_t)a * b) >> 16);
}
```

#### GBA Version (ARM Assembly)

```asm
@ raster_arm.s
fix16_mul_arm:
    smull   r2, r1, r0, r1      @ 64-bit result in r1:r2
    mov     r0, r2, lsr #16     @ Extract middle 32 bits
    orr     r0, r0, r1, lsl #16
    bx      lr
```

```c
// Calling from C
extern fix16_t fix16_mul_arm(fix16_t a, fix16_t b);
#define fix16_mul(a, b) fix16_mul_arm(a, b)
```

### 4.2 Scanline Renderer

#### Original (PicoSystem)

```c
// Inner loop processes one pixel at a time
for (int x = xl; x <= xr; x++) {
    int tx = (u >> 16) + tex_ox;
    int ty = (v >> 16) + tex_oy;
    u8 c = SGET_FAST(tx, ty);
    if (c != 0) {
        screen[y][x] = palette_map[c];
    }
    u += du_dx;
    v += dv_dx;
}
```

#### GBA Version (ARM Assembly)

```asm
@ render_scanline_arm - approximately 12 cycles per pixel
render_scanline_arm:
    push    {r4-r11, lr}

    @ Get parameters from stack
    ldr     r4, [sp, #36]       @ v
    ldr     r5, [sp, #40]       @ dudx
    ldr     r6, [sp, #44]       @ dvdx
    ldr     r7, [sp, #48]       @ texture pointer
    ldr     r8, [sp, #52]       @ palette pointer
    ldr     r9, [sp, #56]       @ tex_ox
    ldr     r10,[sp, #60]       @ tex_oy

    @ Advance to starting pixel
    add     r0, r0, r1, lsl #1

    @ Calculate pixel count
    subs    r11, r2, r1
    blt     9f
    add     r11, r11, #1

1:  @ --- pixel loop ---
    mov     r12, r3, asr #16    @ tu = u >> 16
    add     r12, r12, r9        @ + offset
    mov     lr, r4, asr #16     @ tv = v >> 16
    add     lr, lr, r10         @ + offset
    add     r12, r12, lr, lsl #7  @ texture is 128 wide
    ldrb    r12, [r7, r12]      @ fetch texel
    mov     r12, r12, lsl #1    @ *2 for u16 lookup
    ldrh    r12, [r8, r12]      @ palette lookup
    strh    r12, [r0], #2       @ store & advance pointer
    add     r3, r3, r5          @ u += dudx
    add     r4, r4, r6          @ v += dvdx
    subs    r11, r11, #1
    bgt     1b

9:  pop     {r4-r11, pc}
```

### 4.3 4x Unrolled Version

```asm
render_scanline_arm_unrolled:
    @ ... initialization code ...

    @ Process 4 pixels at a time
4:  cmp     r11, #4
    blt     1f

    @ Pixel 0
    mov     r12, r3, asr #16
    add     r12, r12, r9
    mov     lr, r4, asr #16
    add     lr, lr, r10
    add     r12, r12, lr, lsl #7
    ldrb    r12, [r7, r12]
    mov     r12, r12, lsl #1
    ldrh    r12, [r8, r12]
    strh    r12, [r0], #2
    add     r3, r3, r5
    add     r4, r4, r6

    @ Pixels 1, 2, 3 similarly unrolled
    @ ...

    sub     r11, r11, #4
    b       4b

    @ Handle remaining pixels one at a time
1:  @ ...
```

### 4.4 Fast memset

```asm
@ fast_memset16_arm - 32 bytes per iteration using STM
fast_memset16_arm:
    push    {r4-r9, lr}

    @ Duplicate 16-bit value into both halves
    orr     r1, r1, r1, lsl #16

    @ Spread across 8 registers
    mov     r3, r1
    mov     r4, r1
    mov     r5, r1
    mov     r6, r1
    mov     r7, r1
    mov     r8, r1
    mov     r9, r1

    @ Write 16 halfwords (32 bytes) per iteration
8:  cmp     r2, #16
    blt     4f
    stmia   r0!, {r1, r3, r4, r5, r6, r7, r8, r9}
    sub     r2, r2, #16
    b       8b

    @ 4 at a time
4:  cmp     r2, #4
    blt     1f
    stmia   r0!, {r1, r3}
    sub     r2, r2, #4
    b       4b

    @ Remainder
1:  @ ...
```

---

## 5. DMA Screen Clearing

### Original (PicoSystem)

```c
static void cls(void) {
    memset(screen, 0, sizeof(screen));  // CPU loop
}
```

### GBA Version

```c
// Fast clear using DMA3
static inline void DMAFastCopy(void* source, void* dest, u32 count, u32 mode) {
    REG_DMA3SAD = (u32)source;
    REG_DMA3DAD = (u32)dest;
    REG_DMA3CNT = count | mode;
}

static u16 clear_color = 0;

static void cls(void) {
    // Fixed source mode = fill mode
    DMAFastCopy(&clear_color, (void*)vram_buffer,
                SCREEN_WIDTH * SCREEN_HEIGHT,
                DMA_SRC_FIXED | DMA_DST_INC | DMA_16BIT | DMA_ENABLE);
}
```

### Performance Comparison

| Method | Cycles (approximate) |
|--------|---------------------|
| memset (CPU) | 160×128×4 = 81,920 |
| DMA transfer | 160×128×2 = 40,960 |

DMA transfers run in parallel with the CPU, making them effectively even faster.

---

## 6. Rasterizer Optimization

### 6.1 IWRAM Placement and ARM Mode

```c
// Place important functions in IWRAM and execute in ARM mode
IWRAM_CODE __attribute__((target("arm")))
static void transform_pos(Vec3* proj, const Mat34* mat, const Vec3* pos);

IWRAM_CODE __attribute__((target("arm")))
static void rasterize_flat_tri(...);

IWRAM_CODE __attribute__((target("arm")))
static void rasterize_tri(int index, Triangle* tris, Vec3* projs);
```

### 6.2 Pre-computing Scanline Gradients

#### Original

```c
// Division on each scanline
fix16_t span = x_right - x_left;
if (span > 0) {
    fix16_t du_dx = fix16_div(u_right - u_left, span);
    fix16_t dv_dx = fix16_div(v_right - v_left, span);
    // ...
}
```

#### GBA Version

```c
// Using reciprocal LUT
fix16_t span = x_right - x_left;
if (span > F16(0.5)) {
    fix16_t inv_span = fast_recip(span);
    fix16_t du_dx = fix16_mul(u_right - u_left, inv_span);
    fix16_t dv_dx = fix16_mul(v_right - v_left, inv_span);
    // ...
}
```

### 6.3 Pre-computing Edge Gradients

```c
// Calculate once per triangle
fix16_t dy = y_bot - y_top;
fix16_t inv_dy = fast_recip(dy);

// Left and right edge gradients
fix16_t dx_left = fix16_mul(v1->x - v0->x, inv_dy);
fix16_t dx_right = fix16_mul(v2->x - v0->x, inv_dy);
fix16_t du_left = fix16_mul(uv1[0] - uv0[0], inv_dy);
// ...
```

---

## 7. Sorting Algorithm Improvement

### Original (PicoSystem)

```c
static int tri_compare(const void* a, const void* b) {
    const Triangle* ta = (const Triangle*)a;
    const Triangle* tb = (const Triangle*)b;
    if (ta->z < tb->z) return -1;
    if (ta->z > tb->z) return 1;
    return 0;
}

static void sort_tris(Triangle* tris, int num, Vec3* projs) {
    for (int i = 0; i < num; i++) {
        tris[i].z = projs[tris[i].tri[0]].z +
                    projs[tris[i].tri[1]].z +
                    projs[tris[i].tri[2]].z;
    }
    qsort(tris, num, sizeof(Triangle), tri_compare);
}
```

### GBA Version

```c
// Insertion sort - faster than qsort for small arrays (<20 elements)
static void sort_tris(Triangle* tris, int num, Vec3* projs) {
    // Calculate Z values
    for (int i = 0; i < num; i++) {
        tris[i].z = projs[tris[i].tri[0]].z +
                    projs[tris[i].tri[1]].z +
                    projs[tris[i].tri[2]].z;
    }

    // Insertion sort (stable, good for small n, no function call overhead)
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
```

### Performance Comparison

| Elements | qsort | Insertion Sort |
|----------|-------|----------------|
| 5 | Slow (function call overhead) | Fast |
| 10 | Similar | Slightly faster |
| 20 | Slightly faster | Similar |
| 50+ | Fast | Slow |

Hyperspace meshes typically have 10-20 triangles, making insertion sort optimal.

---

## 8. Early Culling

### Original

Only basic backface culling:

```c
fix16_t nz = fix16_mul(x1 - x0, y2 - y0) - fix16_mul(y1 - y0, x2 - x0);
if (nz < 0) return;
```

### GBA Version

Multiple stages of early culling added:

```c
static void rasterize_tri(int index, Triangle* tris, Vec3* projs) {
    // 1. Reject invalid triangles
    if (tri->tri[0] < 0 || tri->tri[1] < 0 || tri->tri[2] < 0 || !cur_tex) return;

    // 2. Reject triangles behind camera (Z early culling)
    if (v0->z <= 0 && v1->z <= 0 && v2->z <= 0) return;

    // 3. Reject off-screen triangles (bounding box)
    fix16_t min_x = fix16_min(v0->x, fix16_min(v1->x, v2->x));
    fix16_t max_x = fix16_max(v0->x, fix16_max(v1->x, v2->x));
    fix16_t min_y = fix16_min(v0->y, fix16_min(v1->y, v2->y));
    fix16_t max_y = fix16_max(v0->y, fix16_max(v1->y, v2->y));

    if (max_x < 0 || min_x >= F16(SCREEN_WIDTH)) return;
    if (max_y < 0 || min_y >= F16(SCREEN_HEIGHT)) return;

    // 4. Reject tiny triangles (< 1 pixel)
    if (max_x - min_x < fix16_one && max_y - min_y < fix16_one) return;

    // 5. Backface culling
    fix16_t nz = fix16_mul(v1->x - v0->x, v2->y - v0->y) -
                 fix16_mul(v1->y - v0->y, v2->x - v0->x);
    if (nz < 0) return;

    // ... rasterization processing ...
}
```

### Effect

By excluding distant enemies and off-screen objects before rasterization, unnecessary processing is significantly reduced.

---

## 9. Direct VRAM Rendering

### Original (PicoSystem)

```c
// Draw to intermediate buffer
static uint8_t screen[SCREEN_HEIGHT][SCREEN_WIDTH];

static void pset(int x, int y, int c) {
    if (/* bounds check */) {
        screen[y][x] = palette_map[c & 15];
    }
}

// Transfer to hardware at end of frame
void flip() {
    // Transfer to LCD via PIO
}
```

### GBA Version

```c
// Draw directly to VRAM (no intermediate buffer)
static volatile u16* vram_buffer;  // Points to back buffer

#define VRAM_PSET(x, y, c) do { \
    if ((unsigned)(x) < SCREEN_WIDTH && (unsigned)(y) < SCREEN_HEIGHT) \
        vram_buffer[(y) * SCREEN_WIDTH + (x)] = (c); \
} while(0)

// Double buffering
static void flip_screen(void) {
    current_page = 1 - current_page;
    REG_DISPCNT = DCNT_MODE5 | DCNT_BG2 | (current_page ? DCNT_PAGE : 0);
    vram_buffer = current_page ? VRAM_PAGE1 : VRAM_PAGE2;
}
```

### Benefits

- Complete elimination of memory copy
- Reduced cache misses
- Reduced memory usage (160×128×2 = 40KB saved)

---

## 10. Other Optimizations

### 10.1 Bitmask Modulo

```c
// Original
idx = idx % 256;

// GBA Version
idx = idx & 255;
```

### 10.2 Simplified Angle Wrapping

```c
// Original
angle = fmod(angle, TWO_PI);

// GBA Version
if (angle > FIX_TWO_PI) angle -= FIX_TWO_PI;
if (angle < 0) angle += FIX_TWO_PI;
```

### 10.3 Inline Macros

```c
// Eliminate function call overhead
#define SGET_FAST(x, y) (hyperspace_spritesheet[y][x])

#define VRAM_PSET_FAST(x, y, c) \
    (vram_buffer[(y) * SCREEN_WIDTH + (x)] = (c))
```

### 10.4 Optimized Fixed-Point Square Root

```c
// Integer square root for initial estimate
static inline u32 isqrt(u32 n) {
    u32 x = n, y = (x + 1) >> 1;
    while (y < x) { x = y; y = (x + n / x) >> 1; }
    return x;
}

static inline fix16_t fix16_sqrt(fix16_t x) {
    if (x <= 0) return 0;
    u32 root = isqrt((u32)x << 8);
    return (fix16_t)(root << 4);
}
```

---

## Performance Summary

| Optimization | Speedup | Description |
|--------------|---------|-------------|
| Reciprocal LUT | ~10x | Replace division with LUT lookup |
| IWRAM + ARM | ~2x | Fast memory + 32-bit instructions |
| ARM Assembly | ~2x | Hand-tuned inner loops |
| DMA Transfer | ~3x | Hardware screen clearing |
| Early Culling | Variable | Skip invisible triangles |
| Insertion Sort | ~1.5x | Reduced function calls for small arrays |
| Direct VRAM | ~1.3x | Eliminate memcpy |

**Overall**: Achieved approximately 5-10x speedup compared to the original, enabling smooth 3D rendering on the 16.78MHz ARM7TDMI.

---

## Backported to PicoSystem

The following optimizations developed for the GBA version have been backported to the PicoSystem version:

| Optimization | Description |
|--------------|-------------|
| Insertion Sort | Faster than qsort for small arrays |
| Scanline Gradients | Pre-compute divisions |
| Fast Macros | SGET_FAST/PSET_FAST |
| Bitmask Modulo | Optimize for powers of 2 |
| Early Culling | Skip invisible triangles |

---

## References

- [ARM7TDMI Technical Reference Manual](https://developer.arm.com/documentation/ddi0029/e)
- [GBATEK - GBA Technical Documentation](https://problemkaputt.de/gbatek.htm)
- [Tonc - GBA Programming Guide](https://www.coranac.com/tonc/text/)
