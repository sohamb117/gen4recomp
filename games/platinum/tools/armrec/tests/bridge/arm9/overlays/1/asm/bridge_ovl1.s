; Overlay 1 of the bridge test's overlay-dispatch case (bridge.xMAP): one
; function at 0x02100000, the address overlays 2 (bridge_ovl2.s) and 3
; (bridge_c2.c's ov03_02100000, decompiled C) also start at.

	.text

; r0 + 100
	arm_func_start ov01_02100000
ov01_02100000: ; 0x02100000
	add r0, r0, #0x64
	bx lr
	arm_func_end ov01_02100000
