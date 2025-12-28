@ Hyperspace GBA - ARM Assembly Optimizations
@ Optimized rasterizer and fixed-point math routines

    .arm
    .align 4
    .section .iwram, "ax", %progbits

@ ============================================================================
@ fix16_mul_arm - Optimized 16.16 fixed-point multiply
@ Input:  r0 = a (fix16_t), r1 = b (fix16_t)
@ Output: r0 = (a * b) >> 16
@ Clobbers: r1, r2
@ ============================================================================
    .global fix16_mul_arm
    .type fix16_mul_arm, %function
fix16_mul_arm:
    smull   r2, r1, r0, r1      @ r1:r2 = a * b (64-bit result)
    mov     r0, r2, lsr #16     @ r0 = low >> 16
    orr     r0, r0, r1, lsl #16 @ r0 |= high << 16
    bx      lr

@ ============================================================================
@ render_scanline_arm - Optimized scanline rendering
@ Renders a horizontal span of textured pixels
@
@ Input:
@   r0 = row pointer (volatile u16*)
@   r1 = xl (start x)
@   r2 = xr (end x)
@   r3 = u (fix16_t, initial U coordinate)
@   [sp+0] = v (fix16_t, initial V coordinate)
@   [sp+4] = du_dx (fix16_t)
@   [sp+8] = dv_dx (fix16_t)
@   [sp+12] = spritesheet pointer (u8*)
@   [sp+16] = palette_map pointer (u16*)
@   [sp+20] = tex_offset_x
@   [sp+24] = tex_y
@
@ This function draws pixels from xl to xr inclusive
@ ============================================================================
    .global render_scanline_arm
    .type render_scanline_arm, %function
render_scanline_arm:
    push    {r4-r11, lr}

    @ Load parameters from stack
    ldr     r4, [sp, #36]       @ r4 = v
    ldr     r5, [sp, #40]       @ r5 = du_dx
    ldr     r6, [sp, #44]       @ r6 = dv_dx
    ldr     r7, [sp, #48]       @ r7 = spritesheet
    ldr     r8, [sp, #52]       @ r8 = palette_map
    ldr     r9, [sp, #56]       @ r9 = tex_offset_x
    ldr     r10, [sp, #60]      @ r10 = tex_y

    @ r0 = row pointer
    @ r1 = xl (current x)
    @ r2 = xr (end x)
    @ r3 = u
    @ r4 = v
    @ r5 = du_dx
    @ r6 = dv_dx
    @ r7 = spritesheet
    @ r8 = palette_map
    @ r9 = tex_offset_x
    @ r10 = tex_y

    @ Calculate row pointer offset: row + xl*2
    add     r0, r0, r1, lsl #1

    @ Calculate pixel count
    subs    r11, r2, r1         @ r11 = xr - xl
    blt     .Lscanline_done     @ Exit if xl > xr
    add     r11, r11, #1        @ r11 = pixel count (xr - xl + 1)

.Lscanline_loop:
    @ Calculate texture coordinates
    mov     r12, r3, asr #16    @ tu = u >> 16
    add     r12, r12, r9        @ tu += tex_offset_x

    mov     lr, r4, asr #16     @ tv = v >> 16
    add     lr, lr, r10         @ tv += tex_y

    @ Calculate spritesheet offset: tv * 128 + tu (assuming 128-wide spritesheet)
    add     r12, r12, lr, lsl #7

    @ Load palette index from spritesheet
    ldrb    r12, [r7, r12]      @ c = spritesheet[tv * 128 + tu]

    @ Load color from palette and store to VRAM
    mov     r12, r12, lsl #1    @ c * 2 (for u16 index)
    ldrh    r12, [r8, r12]      @ color = palette_map[c]
    strh    r12, [r0], #2       @ *row++ = color

    @ Update UV coordinates
    add     r3, r3, r5          @ u += du_dx
    add     r4, r4, r6          @ v += dv_dx

    @ Loop counter
    subs    r11, r11, #1
    bgt     .Lscanline_loop

.Lscanline_done:
    pop     {r4-r11, pc}

@ ============================================================================
@ render_scanline_arm_unrolled - Unrolled version (4 pixels at a time)
@ Same parameters as render_scanline_arm
@ ============================================================================
    .global render_scanline_arm_unrolled
    .type render_scanline_arm_unrolled, %function
render_scanline_arm_unrolled:
    push    {r4-r11, lr}

    @ Load parameters from stack
    ldr     r4, [sp, #36]       @ r4 = v
    ldr     r5, [sp, #40]       @ r5 = du_dx
    ldr     r6, [sp, #44]       @ r6 = dv_dx
    ldr     r7, [sp, #48]       @ r7 = spritesheet
    ldr     r8, [sp, #52]       @ r8 = palette_map
    ldr     r9, [sp, #56]       @ r9 = tex_offset_x
    ldr     r10, [sp, #60]      @ r10 = tex_y

    @ Calculate row pointer offset
    add     r0, r0, r1, lsl #1

    @ Calculate pixel count
    subs    r11, r2, r1
    blt     .Lunroll_done
    add     r11, r11, #1

    @ Process 4 pixels at a time
.Lunroll_loop4:
    cmp     r11, #4
    blt     .Lunroll_remainder

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

    @ Pixel 1
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

    @ Pixel 2
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

    @ Pixel 3
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
    b       .Lunroll_loop4

.Lunroll_remainder:
    cmp     r11, #0
    ble     .Lunroll_done

.Lunroll_loop1:
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
    subs    r11, r11, #1
    bgt     .Lunroll_loop1

.Lunroll_done:
    pop     {r4-r11, pc}

@ ============================================================================
@ fast_memset16_arm - Fast 16-bit memset using STM
@ Input:
@   r0 = destination pointer (must be 4-byte aligned)
@   r1 = 16-bit value to fill
@   r2 = count (number of 16-bit values)
@ ============================================================================
    .global fast_memset16_arm
    .type fast_memset16_arm, %function
fast_memset16_arm:
    push    {r4-r9, lr}

    @ Create 32-bit pattern from 16-bit value
    orr     r1, r1, r1, lsl #16     @ r1 = value | (value << 16)

    @ Copy to multiple registers for STM
    mov     r3, r1
    mov     r4, r1
    mov     r5, r1
    mov     r6, r1
    mov     r7, r1
    mov     r8, r1
    mov     r9, r1

    @ Process 16 halfwords (32 bytes) at a time
.Lmemset_loop16:
    cmp     r2, #16
    blt     .Lmemset_loop4
    stmia   r0!, {r1, r3, r4, r5, r6, r7, r8, r9}
    sub     r2, r2, #16
    b       .Lmemset_loop16

.Lmemset_loop4:
    cmp     r2, #4
    blt     .Lmemset_loop1
    stmia   r0!, {r1, r3}
    sub     r2, r2, #4
    b       .Lmemset_loop4

.Lmemset_loop1:
    cmp     r2, #0
    ble     .Lmemset_done
    strh    r1, [r0], #2
    sub     r2, r2, #1
    b       .Lmemset_loop1

.Lmemset_done:
    pop     {r4-r9, pc}

    .end
