@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@
@
@  raster_arm.s - hand-tuned ARM routines for Hyperspace GBA
@
@  put the hot stuff in IWRAM for speed
@  ~12 cycles/pixel in the inner loop, not bad for ARM7
@
@  itsmeterada / takehiko.terada@gmail.com
@
@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@

    .arm
    .align  4
    .section .iwram, "ax", %progbits


@-----------------------------------------------------------------------------
@  fix16_mul_arm
@
@  Q16.16 fixed point multiply using SMULL
@  this is way faster than the C version with shifts
@
@  in:   r0, r1 = operands
@  out:  r0 = (r0 * r1) >> 16
@-----------------------------------------------------------------------------
    .global fix16_mul_arm
    .type   fix16_mul_arm, %function
fix16_mul_arm:
    smull   r2, r1, r0, r1      @ 64-bit result in r1:r2
    mov     r0, r2, lsr #16     @ grab middle 32 bits
    orr     r0, r0, r1, lsl #16
    bx      lr


@-----------------------------------------------------------------------------
@  render_scanline_arm
@
@  draws one horizontal span of textured pixels
@  the workhorse of the whole rasterizer
@
@  args (see C prototype for details):
@    r0 = vram row ptr
@    r1 = xl, r2 = xr, r3 = u_start
@    stack: v, dudx, dvdx, tex, pal, offx, offy
@-----------------------------------------------------------------------------
    .global render_scanline_arm
    .type   render_scanline_arm, %function

render_scanline_arm:
    push    {r4-r11, lr}

    @ fish out the rest of the params from stack
    @ 9 regs pushed = 36 bytes, so args start at sp+36
    ldr     r4, [sp, #36]       @ v
    ldr     r5, [sp, #40]       @ dudx
    ldr     r6, [sp, #44]       @ dvdx
    ldr     r7, [sp, #48]       @ tex ptr
    ldr     r8, [sp, #52]       @ palette
    ldr     r9, [sp, #56]       @ tex_ox
    ldr     r10,[sp, #60]       @ tex_oy

    @ advance row ptr to starting pixel
    add     r0, r0, r1, lsl #1

    @ how many pixels?
    subs    r11, r2, r1
    blt     9f                  @ bail if span is empty
    add     r11, r11, #1        @ inclusive range

    @ register map at this point:
    @   r0  = dst ptr (advances)
    @   r3  = u (16.16)
    @   r4  = v (16.16)
    @   r5  = du/dx
    @   r6  = dv/dx
    @   r7  = texture base
    @   r8  = palette base
    @   r9  = tex offset x
    @   r10 = tex offset y
    @   r11 = pixel count
    @   r12, lr = scratch

1:  @ --- pixel loop ---
    mov     r12, r3, asr #16    @ tu = u >> 16
    add     r12, r12, r9        @ + offset

    mov     lr, r4, asr #16     @ tv = v >> 16
    add     lr, lr, r10         @ + offset

    @ tex is 128 wide
    add     r12, r12, lr, lsl #7

    ldrb    r12, [r7, r12]      @ fetch texel (palette index)
    mov     r12, r12, lsl #1    @ *2 for u16 lookup
    ldrh    r12, [r8, r12]      @ palette lookup
    strh    r12, [r0], #2       @ store & advance

    add     r3, r3, r5          @ u += dudx
    add     r4, r4, r6          @ v += dvdx

    subs    r11, r11, #1
    bgt     1b

9:  @ done
    pop     {r4-r11, pc}


@-----------------------------------------------------------------------------
@  render_scanline_arm_unrolled
@
@  same thing but unrolled 4x
@  a bit faster for long spans, about the same for short ones
@-----------------------------------------------------------------------------
    .global render_scanline_arm_unrolled
    .type   render_scanline_arm_unrolled, %function

render_scanline_arm_unrolled:
    push    {r4-r11, lr}

    ldr     r4, [sp, #36]
    ldr     r5, [sp, #40]
    ldr     r6, [sp, #44]
    ldr     r7, [sp, #48]
    ldr     r8, [sp, #52]
    ldr     r9, [sp, #56]
    ldr     r10,[sp, #60]

    add     r0, r0, r1, lsl #1

    subs    r11, r2, r1
    blt     9f
    add     r11, r11, #1

    @ try to do 4 at a time
4:  cmp     r11, #4
    blt     1f

    @ pixel 0
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

    @ pixel 1
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

    @ pixel 2
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

    @ pixel 3
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

    sub     r11, r11, #4
    b       4b

    @ leftovers
1:  cmp     r11, #0
    ble     9f

2:  mov     r12, r3, asr #16
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
    subs    r11, r11, #1
    bgt     2b

9:  pop     {r4-r11, pc}


@-----------------------------------------------------------------------------
@  fast_memset16_arm
@
@  blast 16-bit value to memory using STM
@  good for clearing the framebuffer
@
@  r0 = dest (should be word-aligned for best perf)
@  r1 = 16-bit fill value
@  r2 = count (in halfwords)
@-----------------------------------------------------------------------------
    .global fast_memset16_arm
    .type   fast_memset16_arm, %function

fast_memset16_arm:
    push    {r4-r9, lr}

    @ duplicate the 16-bit val into both halves
    orr     r1, r1, r1, lsl #16

    @ spread it across all the regs we'll use for STM
    mov     r3, r1
    mov     r4, r1
    mov     r5, r1
    mov     r6, r1
    mov     r7, r1
    mov     r8, r1
    mov     r9, r1

    @ blast 16 halfwords (32 bytes) per iteration
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

    @ stragglers
1:  cmp     r2, #0
    ble     9f
    strh    r1, [r0], #2
    sub     r2, r2, #1
    b       1b

9:  pop     {r4-r9, pc}

    .end
