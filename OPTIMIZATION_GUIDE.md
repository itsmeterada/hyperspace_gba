# Hyperspace GBA版 最適化ガイド

このドキュメントでは、PicoSystem版（オリジナル）からGBA版への移植で行ったすべての最適化について、コード比較を交えながら詳細に解説します。

## 目次

1. [ハードウェアの違い](#1-ハードウェアの違い)
2. [メモリ配置の最適化](#2-メモリ配置の最適化)
3. [除算の排除（逆数LUT）](#3-除算の排除逆数lut)
4. [ARM アセンブリによる高速化](#4-arm-アセンブリによる高速化)
5. [DMAによる画面クリア](#5-dmaによる画面クリア)
6. [ラスタライザの最適化](#6-ラスタライザの最適化)
7. [ソートアルゴリズムの改善](#7-ソートアルゴリズムの改善)
8. [早期カリング](#8-早期カリング)
9. [直接VRAMレンダリング](#9-直接vramレンダリング)
10. [その他の最適化](#10-その他の最適化)

---

## 1. ハードウェアの違い

### PicoSystem (RP2040)
- **CPU**: ARM Cortex-M0+ デュアルコア @ 125-250MHz
- **RAM**: 264KB SRAM
- **画面**: 240×240 (120×120 ピクセルダブル)
- **バス幅**: 32ビット
- **特徴**: ハードウェア補間器（interpolator）あり

### Game Boy Advance (ARM7TDMI)
- **CPU**: ARM7TDMI @ 16.78MHz（約1/10のクロック）
- **RAM**: IWRAM 32KB (高速) + EWRAM 256KB (低速)
- **画面**: 240×160 (Mode 5: 160×128)
- **バス幅**: ROM 16ビット、IWRAM 32ビット
- **特徴**: DMAコントローラあり

### 課題

GBAはクロック周波数が約1/10であるため、同等のフレームレートを維持するには大幅な最適化が必要です。

---

## 2. メモリ配置の最適化

### オリジナル（PicoSystem）

```c
// すべてのデータが通常のRAMに配置
static Trail trails[MAX_TRAILS];
static Enemy enemies[MAX_ENEMIES];
static Background bgs[MAX_BGS];
```

### GBA版

```c
// EWRAMに大きな配列を配置（IWRAMのオーバーフロー防止）
#define EWRAM_DATA __attribute__((section(".ewram")))

EWRAM_DATA static Trail trails[MAX_TRAILS];
EWRAM_DATA static Enemy enemies[MAX_ENEMIES];
EWRAM_DATA static Background bgs[MAX_BGS];
EWRAM_DATA static s32 cart_data[64];
```

```c
// IWRAMにホットコード/データを配置
#define IWRAM_CODE __attribute__((section(".iwram"), long_call))
#define IWRAM_DATA __attribute__((section(".iwram")))

// 高速アクセスが必要なディザパターン
IWRAM_DATA static u8 dither_pattern[8][8];
```

### 解説

| メモリ領域 | サイズ | アクセス速度 | 用途 |
|-----------|-------|-------------|------|
| IWRAM | 32KB | 1サイクル（32ビット） | ホット関数、頻繁にアクセスするデータ |
| EWRAM | 256KB | 2-3サイクル（16ビット） | 大きな配列、あまりアクセスしないデータ |
| ROM | - | 2-3サイクル（16ビット） | コード、const データ |

IWRAMは32ビットバスで最速ですがサイズが限られるため、頻繁にアクセスする小さなデータのみを配置します。

---

## 3. 除算の排除（逆数LUT）

ARM7TDMIでの除算は約50サイクル以上かかります。これをLUTで5サイクル程度に削減します。

### オリジナル（PicoSystem）

```c
// ラスタライザ内で頻繁に除算を使用
fix16_t c = fix16_div(y1 - y0, y2 - y0);

// スキャンライン補間
fix16_t inv_span = fix16_div(fix16_one, span);
```

### GBA版

```c
// 513エントリの逆数LUT（1/1 〜 1/512）
static const u32 recip_lut[513] = {
    0xFFFFFFFF, // 0: 未使用
    0x10000, 0x8000, 0x5555, 0x4000, 0x3333, 0x2AAA, 0x2492, 0x2000, // 1-8
    0x1C71, 0x1999, 0x1745, 0x1555, 0x13B1, 0x1249, 0x1111, 0x1000, // 9-16
    // ... 512エントリまで続く
};

// 高速逆数関数（線形補間で精度向上）
IWRAM_CODE static inline fix16_t fast_recip(fix16_t x) {
    if (x <= 0) return 0;
    // 1.0未満は従来の除算にフォールバック
    if (x < fix16_one) {
        return fix16_div(fix16_one, x);
    }
    // 1.0以上はLUTを使用
    int ix = x >> 16;  // 整数部
    if (ix > 511) return recip_lut[512];

    // 線形補間で小数部の精度を確保
    u32 base = recip_lut[ix];
    u32 next = recip_lut[ix + 1];
    int frac = (x >> 8) & 0xFF;  // 8ビット小数部
    return base - (((base - next) * frac) >> 8);
}
```

### 使用例

```c
// オリジナル
fix16_t c = fix16_div(y1 - y0, y2 - y0);

// GBA版
fix16_t total_dy = tv2->y - tv0->y;
fix16_t c = fix16_mul(tv1->y - tv0->y, fast_recip(total_dy));
```

### 性能向上

- **除算**: 約50サイクル
- **LUT参照+補間**: 約5サイクル
- **向上率**: 約10倍

三角形1つあたり10-50回の除算が発生するため、全体で大幅な高速化が得られます。

---

## 4. ARM アセンブリによる高速化

### 4.1 固定小数点乗算

#### オリジナル（PicoSystem）

```c
// libfixmathの関数を使用
fix16_t result = fix16_mul(a, b);

// または直接計算
static inline fix16_t fix16_mul(fix16_t a, fix16_t b) {
    return (fix16_t)(((int64_t)a * b) >> 16);
}
```

#### GBA版（ARMアセンブリ）

```asm
@ raster_arm.s
fix16_mul_arm:
    smull   r2, r1, r0, r1      @ 64ビット結果を r1:r2 に
    mov     r0, r2, lsr #16     @ 中央32ビットを取得
    orr     r0, r0, r1, lsl #16
    bx      lr
```

```c
// Cからの呼び出し
extern fix16_t fix16_mul_arm(fix16_t a, fix16_t b);
#define fix16_mul(a, b) fix16_mul_arm(a, b)
```

### 4.2 スキャンラインレンダラ

#### オリジナル（PicoSystem）

```c
// 内部ループで1ピクセルずつ処理
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

#### GBA版（ARMアセンブリ）

```asm
@ render_scanline_arm - 1ピクセルあたり約12サイクル
render_scanline_arm:
    push    {r4-r11, lr}

    @ スタックからパラメータ取得
    ldr     r4, [sp, #36]       @ v
    ldr     r5, [sp, #40]       @ dudx
    ldr     r6, [sp, #44]       @ dvdx
    ldr     r7, [sp, #48]       @ テクスチャポインタ
    ldr     r8, [sp, #52]       @ パレットポインタ
    ldr     r9, [sp, #56]       @ tex_ox
    ldr     r10,[sp, #60]       @ tex_oy

    @ 開始ピクセルに進む
    add     r0, r0, r1, lsl #1

    @ ピクセル数計算
    subs    r11, r2, r1
    blt     9f
    add     r11, r11, #1

1:  @ --- ピクセルループ ---
    mov     r12, r3, asr #16    @ tu = u >> 16
    add     r12, r12, r9        @ + オフセット
    mov     lr, r4, asr #16     @ tv = v >> 16
    add     lr, lr, r10         @ + オフセット
    add     r12, r12, lr, lsl #7  @ テクスチャは128幅
    ldrb    r12, [r7, r12]      @ テクセルフェッチ
    mov     r12, r12, lsl #1    @ *2 for u16 lookup
    ldrh    r12, [r8, r12]      @ パレット参照
    strh    r12, [r0], #2       @ 格納＆ポインタ進行
    add     r3, r3, r5          @ u += dudx
    add     r4, r4, r6          @ v += dvdx
    subs    r11, r11, #1
    bgt     1b

9:  pop     {r4-r11, pc}
```

### 4.3 4倍アンロール版

```asm
render_scanline_arm_unrolled:
    @ ... 初期化コード ...

    @ 4ピクセル単位で処理
4:  cmp     r11, #4
    blt     1f

    @ ピクセル 0
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

    @ ピクセル 1, 2, 3 も同様に展開
    @ ...

    sub     r11, r11, #4
    b       4b

    @ 残りのピクセルを1つずつ処理
1:  @ ...
```

### 4.4 高速memset

```asm
@ fast_memset16_arm - STMで32バイト/イテレーション
fast_memset16_arm:
    push    {r4-r9, lr}

    @ 16ビット値を両半分に複製
    orr     r1, r1, r1, lsl #16

    @ 8レジスタに展開
    mov     r3, r1
    mov     r4, r1
    mov     r5, r1
    mov     r6, r1
    mov     r7, r1
    mov     r8, r1
    mov     r9, r1

    @ 16ハーフワード（32バイト）単位で書き込み
8:  cmp     r2, #16
    blt     4f
    stmia   r0!, {r1, r3, r4, r5, r6, r7, r8, r9}
    sub     r2, r2, #16
    b       8b

    @ 4単位
4:  cmp     r2, #4
    blt     1f
    stmia   r0!, {r1, r3}
    sub     r2, r2, #4
    b       4b

    @ 残り
1:  @ ...
```

---

## 5. DMAによる画面クリア

### オリジナル（PicoSystem）

```c
static void cls(void) {
    memset(screen, 0, sizeof(screen));  // CPU でループ
}
```

### GBA版

```c
// DMA3を使用した高速クリア
static inline void DMAFastCopy(void* source, void* dest, u32 count, u32 mode) {
    REG_DMA3SAD = (u32)source;
    REG_DMA3DAD = (u32)dest;
    REG_DMA3CNT = count | mode;
}

static u16 clear_color = 0;

static void cls(void) {
    // 固定ソースモード = フィルモード
    DMAFastCopy(&clear_color, (void*)vram_buffer,
                SCREEN_WIDTH * SCREEN_HEIGHT,
                DMA_SRC_FIXED | DMA_DST_INC | DMA_16BIT | DMA_ENABLE);
}
```

### 性能比較

| 方式 | サイクル数（概算） |
|-----|------------------|
| memset (CPU) | 160×128×4 = 81,920 |
| DMA転送 | 160×128×2 = 40,960 |

DMA転送はCPUと並行して動作するため、実質的にはさらに高速です。

---

## 6. ラスタライザの最適化

### 6.1 IWRAM配置とARMモード

```c
// 重要な関数をIWRAMに配置し、ARMモードで実行
IWRAM_CODE __attribute__((target("arm")))
static void transform_pos(Vec3* proj, const Mat34* mat, const Vec3* pos);

IWRAM_CODE __attribute__((target("arm")))
static void rasterize_flat_tri(...);

IWRAM_CODE __attribute__((target("arm")))
static void rasterize_tri(int index, Triangle* tris, Vec3* projs);
```

### 6.2 スキャンライン勾配の事前計算

#### オリジナル

```c
// 各スキャンラインで除算
fix16_t span = x_right - x_left;
if (span > 0) {
    fix16_t du_dx = fix16_div(u_right - u_left, span);
    fix16_t dv_dx = fix16_div(v_right - v_left, span);
    // ...
}
```

#### GBA版

```c
// 逆数LUTを使用
fix16_t span = x_right - x_left;
if (span > F16(0.5)) {
    fix16_t inv_span = fast_recip(span);
    fix16_t du_dx = fix16_mul(u_right - u_left, inv_span);
    fix16_t dv_dx = fix16_mul(v_right - v_left, inv_span);
    // ...
}
```

### 6.3 エッジ勾配の事前計算

```c
// 三角形全体で1回だけ計算
fix16_t dy = y_bot - y_top;
fix16_t inv_dy = fast_recip(dy);

// 左右エッジの勾配
fix16_t dx_left = fix16_mul(v1->x - v0->x, inv_dy);
fix16_t dx_right = fix16_mul(v2->x - v0->x, inv_dy);
fix16_t du_left = fix16_mul(uv1[0] - uv0[0], inv_dy);
// ...
```

---

## 7. ソートアルゴリズムの改善

### オリジナル（PicoSystem）

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

### GBA版

```c
// 挿入ソート - 小配列（<20要素）でqsortより高速
static void sort_tris(Triangle* tris, int num, Vec3* projs) {
    // Z値計算
    for (int i = 0; i < num; i++) {
        tris[i].z = projs[tris[i].tri[0]].z +
                    projs[tris[i].tri[1]].z +
                    projs[tris[i].tri[2]].z;
    }

    // 挿入ソート（安定、小n向け、関数呼び出しオーバーヘッドなし）
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

### 性能比較

| 要素数 | qsort | 挿入ソート |
|-------|-------|-----------|
| 5 | 遅い（関数呼び出しオーバーヘッド） | 高速 |
| 10 | 同等 | やや高速 |
| 20 | やや高速 | 同等 |
| 50+ | 高速 | 遅い |

Hyperspaceのメッシュは通常10-20三角形なので、挿入ソートが最適です。

---

## 8. 早期カリング

### オリジナル

基本的なバックフェースカリングのみ：

```c
fix16_t nz = fix16_mul(x1 - x0, y2 - y0) - fix16_mul(y1 - y0, x2 - x0);
if (nz < 0) return;
```

### GBA版

複数段階の早期カリングを追加：

```c
static void rasterize_tri(int index, Triangle* tris, Vec3* projs) {
    // 1. 無効な三角形を除外
    if (tri->tri[0] < 0 || tri->tri[1] < 0 || tri->tri[2] < 0 || !cur_tex) return;

    // 2. カメラ後方の三角形を除外（Z早期カリング）
    if (v0->z <= 0 && v1->z <= 0 && v2->z <= 0) return;

    // 3. 画面外の三角形を除外（バウンディングボックス）
    fix16_t min_x = fix16_min(v0->x, fix16_min(v1->x, v2->x));
    fix16_t max_x = fix16_max(v0->x, fix16_max(v1->x, v2->x));
    fix16_t min_y = fix16_min(v0->y, fix16_min(v1->y, v2->y));
    fix16_t max_y = fix16_max(v0->y, fix16_max(v1->y, v2->y));

    if (max_x < 0 || min_x >= F16(SCREEN_WIDTH)) return;
    if (max_y < 0 || min_y >= F16(SCREEN_HEIGHT)) return;

    // 4. 1ピクセル未満の小さな三角形を除外
    if (max_x - min_x < fix16_one && max_y - min_y < fix16_one) return;

    // 5. バックフェースカリング
    fix16_t nz = fix16_mul(v1->x - v0->x, v2->y - v0->y) -
                 fix16_mul(v1->y - v0->y, v2->x - v0->x);
    if (nz < 0) return;

    // ... ラスタライズ処理 ...
}
```

### 効果

遠くの敵や画面外のオブジェクトをラスタライズ前に除外することで、不要な処理を大幅に削減します。

---

## 9. 直接VRAMレンダリング

### オリジナル（PicoSystem）

```c
// 中間バッファに描画
static uint8_t screen[SCREEN_HEIGHT][SCREEN_WIDTH];

static void pset(int x, int y, int c) {
    if (/* 境界チェック */) {
        screen[y][x] = palette_map[c & 15];
    }
}

// フレーム終了時にハードウェアに転送
void flip() {
    // PIOでLCDに転送
}
```

### GBA版

```c
// VRAMに直接描画（中間バッファなし）
static volatile u16* vram_buffer;  // バックバッファを指す

#define VRAM_PSET(x, y, c) do { \
    if ((unsigned)(x) < SCREEN_WIDTH && (unsigned)(y) < SCREEN_HEIGHT) \
        vram_buffer[(y) * SCREEN_WIDTH + (x)] = (c); \
} while(0)

// ダブルバッファリング
static void flip_screen(void) {
    current_page = 1 - current_page;
    REG_DISPCNT = DCNT_MODE5 | DCNT_BG2 | (current_page ? DCNT_PAGE : 0);
    vram_buffer = current_page ? VRAM_PAGE1 : VRAM_PAGE2;
}
```

### 利点

- メモリコピーの完全な排除
- キャッシュミスの削減
- メモリ使用量の削減（160×128×2 = 40KB節約）

---

## 10. その他の最適化

### 10.1 ビットマスクによる剰余演算

```c
// オリジナル
idx = idx % 256;

// GBA版
idx = idx & 255;
```

### 10.2 角度ラッピングの簡略化

```c
// オリジナル
angle = fmod(angle, TWO_PI);

// GBA版
if (angle > FIX_TWO_PI) angle -= FIX_TWO_PI;
if (angle < 0) angle += FIX_TWO_PI;
```

### 10.3 インラインマクロ

```c
// 関数呼び出しオーバーヘッドを排除
#define SGET_FAST(x, y) (hyperspace_spritesheet[y][x])

#define VRAM_PSET_FAST(x, y, c) \
    (vram_buffer[(y) * SCREEN_WIDTH + (x)] = (c))
```

### 10.4 固定小数点平方根の最適化

```c
// 整数平方根で初期推定
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

## 性能まとめ

| 最適化 | 高速化率 | 説明 |
|--------|---------|------|
| 逆数LUT | ~10x | 除算をLUT参照に置換 |
| IWRAM + ARM | ~2x | 高速メモリ + 32ビット命令 |
| ARMアセンブリ | ~2x | 手動チューニングされた内部ループ |
| DMA転送 | ~3x | ハードウェア画面クリア |
| 早期カリング | 可変 | 不可視三角形をスキップ |
| 挿入ソート | ~1.5x | 小配列で関数呼び出し削減 |
| 直接VRAM | ~1.3x | memcpy排除 |

**総合**: オリジナルと比較して約5-10倍の高速化を達成し、16.78MHzのARM7TDMIでスムーズな3Dレンダリングを実現しています。

---

## PicoSystem版へのバックポート

GBA版で開発された以下の最適化はPicoSystem版にもバックポートされています：

| 最適化 | 説明 |
|--------|------|
| 挿入ソート | 小配列でqsortより高速 |
| スキャンライン勾配 | 除算を事前計算 |
| 高速マクロ | SGET_FAST/PSET_FAST |
| ビットマスク剰余 | 2のべき乗で最適化 |
| 早期カリング | 不可視三角形をスキップ |

---

## 参考資料

- [ARM7TDMI Technical Reference Manual](https://developer.arm.com/documentation/ddi0029/e)
- [GBATEK - GBA Technical Documentation](https://problemkaputt.de/gbatek.htm)
- [Tonc - GBA Programming Guide](https://www.coranac.com/tonc/text/)
