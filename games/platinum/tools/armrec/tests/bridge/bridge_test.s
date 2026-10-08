; The bridge test's assembly: recompiled by armrec.py --wasm, called from and
; calling into the C in bridge_main.c / bridge_c2.c. Every function is at a
; made-up main-RAM address; only consistency matters.

	.text

; r0 + r1<<4 + r2<<8 + r3<<12 + [sp]<<16 + [sp+4]<<20: six words, two on
; the emulated stack, each in its own nibble so a misplaced word shows.
	arm_func_start asm_mix6
asm_mix6: ; 0x02000000
	add r0, r0, r1, lsl #0x4
	add r0, r0, r2, lsl #0x8
	add r0, r0, r3, lsl #0xc
	ldr r1, [sp, #0x0]
	add r0, r0, r1, lsl #0x10
	ldr r1, [sp, #0x4]
	add r0, r0, r1, lsl #0x14
	bx lr
	arm_func_end asm_mix6

	thumb_func_start asm_thumb_mul
asm_thumb_mul: ; 0x02000020
	mul r0, r1
	bx lr
	thumb_func_end asm_thumb_mul

; uint64_t asm_ret64(u32 a, u32 b, u32 c, u64 d) = d + a: d is r3 (low) and
; [sp] (high), no even-register alignment.
	arm_func_start asm_ret64
asm_ret64: ; 0x02000024
	ldr r1, [sp, #0x0]
	adds r0, r3, r0
	adc r1, r1, #0x0
	bx lr
	arm_func_end asm_ret64

; int asm_callback(int (*fn)(int, int, int, int, int), int x)
;   = fn(x, x+1, x+2, x+3, x+4) + 1
	arm_func_start asm_callback
asm_callback: ; 0x02000034
	stmdb sp!, {r4, lr}
	sub sp, sp, #0x8
	mov r12, r0
	add r0, r1, #0x4
	str r0, [sp, #0x0]
	mov r0, r1
	add r1, r0, #0x1
	add r2, r0, #0x2
	add r3, r0, #0x3
	blx r12
	add r0, r0, #0x1
	add sp, sp, #0x8
	ldmia sp!, {r4, pc}
	arm_func_end asm_callback

; int asm_call_c(int x) = c_helper6(x, (short)-2, 0x0000000700000003, 5, 6)
	arm_func_start asm_call_c
asm_call_c: ; 0x02000068
	stmdb sp!, {lr}
	sub sp, sp, #0xc
	mov r1, #0x5
	str r1, [sp, #0x0]
	mov r1, #0x6
	str r1, [sp, #0x4]
	mvn r1, #0x1
	mov r2, #0x3
	mov r3, #0x7
	bl c_helper6
	add sp, sp, #0xc
	ldmia sp!, {pc}
	arm_func_end asm_call_c

; int asm_sum_var(int n, ...): the sum of the n words after n
	arm_func_start asm_sum_var
asm_sum_var: ; 0x02000098
	stmdb sp!, {r1, r2, r3}
	mov r12, sp
	mov r1, #0x0
_020000A4:
	cmp r0, #0x0
	beq _020000BC
	ldr r2, [r12], #0x4
	add r1, r1, r2
	sub r0, r0, #0x1
	b _020000A4
_020000BC:
	mov r0, r1
	add sp, sp, #0xc
	bx lr
	arm_func_end asm_sum_var

; struct Big { int a, b, c; } asm_make_big(int a, int b): hidden result
; pointer in r0
	arm_func_start asm_make_big
asm_make_big: ; 0x020000C8
	str r1, [r0, #0x0]
	str r2, [r0, #0x4]
	add r3, r1, r2
	str r3, [r0, #0x8]
	bx lr
	arm_func_end asm_make_big

; struct Small { u16 x, y; } asm_make_small(int x, int y): returned in r0
	arm_func_start asm_make_small
asm_make_small: ; 0x020000DC
	orr r0, r0, r1, lsl #0x10
	bx lr
	arm_func_end asm_make_small

; int asm_struct_stack(int a, int b, struct S12 s): s in r2, r3, [sp]
	arm_func_start asm_struct_stack
asm_struct_stack: ; 0x020000E4
	add r0, r0, r1, lsl #0x4
	add r0, r0, r2, lsl #0x8
	add r0, r0, r3, lsl #0xc
	ldr r1, [sp, #0x0]
	add r0, r0, r1, lsl #0x10
	bx lr
	arm_func_end asm_struct_stack

; Identities, declared by C with different prototypes: what comes back is
; exactly r0 (and r1).
	arm_func_start asm_id_a
asm_id_a: ; 0x020000FC
	bx lr
	arm_func_end asm_id_a

	arm_func_start asm_id_b
asm_id_b: ; 0x02000100
	bx lr
	arm_func_end asm_id_b

	arm_func_start asm_id_c
asm_id_c: ; 0x02000104
	bx lr
	arm_func_end asm_id_c

; int asm_call_word(int x) = c_target(x), through a literal-pool .word
	arm_func_start asm_call_word
asm_call_word: ; 0x02000108
	stmdb sp!, {lr}
	sub sp, sp, #0x4
	ldr r1, _02000124 ; =c_target
	blx r1
	add sp, sp, #0x4
	ldmia sp!, {pc}
_02000120: ; 0x02000120
	.word 0
_02000124: .word c_target
	arm_func_end asm_call_word

; int asm_read_cdata(void) = c_data[1], through a literal-pool .word
	arm_func_start asm_read_cdata
asm_read_cdata: ; 0x02000128
	ldr r0, _02000134 ; =c_data
	ldr r0, [r0, #0x4]
	bx lr
_02000134: .word c_data
	arm_func_end asm_read_cdata

; int asm_call_var(void) = c_varsum(6, 10, 20, 30, 40, 50, 60)
	arm_func_start asm_call_var
asm_call_var: ; 0x02000138
	stmdb sp!, {lr}
	sub sp, sp, #0xc
	mov r0, #0x28
	str r0, [sp, #0x0]
	mov r0, #0x32
	str r0, [sp, #0x4]
	mov r0, #0x3c
	str r0, [sp, #0x8]
	mov r0, #0x6
	mov r1, #0xa
	mov r2, #0x14
	mov r3, #0x1e
	bl c_varsum
	add sp, sp, #0xc
	ldmia sp!, {pc}
	arm_func_end asm_call_var

; int asm_call_structs(int *out3) : c_make_big(out3, 7, 9) (hidden pointer in
; r0), then returns c_make_small(3, 4) (a 4-byte struct back in r0)
	arm_func_start asm_call_structs
asm_call_structs: ; 0x02000174
	stmdb sp!, {lr}
	sub sp, sp, #0x4
	mov r1, #0x7
	mov r2, #0x9
	bl c_make_big
	mov r0, #0x3
	mov r1, #0x4
	bl c_make_small
	add sp, sp, #0x4
	ldmia sp!, {pc}
	arm_func_end asm_call_structs

; int asm_call_u8(void) = c_ret_s8() + 1000 (a signed char comes back
; sign-extended in r0)
	arm_func_start asm_call_u8
asm_call_u8: ; 0x0200019C
	stmdb sp!, {lr}
	sub sp, sp, #0x4
	bl c_ret_s8
	add r0, r0, #0x3e8
	add sp, sp, #0x4
	ldmia sp!, {pc}
	arm_func_end asm_call_u8

; int asm_ldm_self(const int *p) = p[0] + p[1], loaded through r0 into r0 and
; r1: a Thumb LDMIA whose base is in its list keeps the loaded value (no
; writeback), which mwcc relies on to pass a two-word struct on.
	thumb_func_start asm_ldm_self
asm_ldm_self: ; 0x020001B4
	ldmia r0!, {r0, r1}
	add r0, r0, r1
	bx lr
	thumb_func_end asm_ldm_self

; u32 asm_thumb_lit(void) = the literal-pool word naming asm_thumb_mul: its
; Thumb address, bit 0 set, the same value as `.word asm_thumb_mul` in data
; and as C's &asm_thumb_mul (code compares the two: ov18_02249684).
	arm_func_start asm_thumb_lit
asm_thumb_lit: ; 0x020001BC
	ldr r0, _020001C4 ; =asm_thumb_mul
	bx lr
_020001C4: .word asm_thumb_mul
	arm_func_end asm_thumb_lit

	.data

	.global asm_table
asm_table: ; 0x02001000
	.word 0x11111111, 0x22222222, 0x33333333

	.global asm_cptrs
asm_cptrs: ; 0x0200100C
	.word c_target
	.word c_data
	.word asm_thumb_mul
