; HG/SS lib/asm/msl.s's shape: one start macro (`_fadd`) that is never
; ended, then runtime routines (`_ll_udiv`, `_fmul`, ...) marked only by
; `.public` and `.type NAME, @function`. Each such routine is a function of
; its own; bridge_test.s's asm_call_typed calls one from another file.

	.text

	arm_func_start asm_rt_neg
asm_rt_neg: ; 0x02000800
	rsb r0, r0, #0x0
	bx lr
	.public asm_rt_twice
	.type asm_rt_twice, @function
asm_rt_twice: ; 0x02000808
	add r0, r0, r0
	bx lr
