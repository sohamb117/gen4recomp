; Overlay 2 of the bridge test's overlay-dispatch case: the same address as
; overlay 1's ov01_02100000 (bridge_ovl1.s).

	.text

; r0 + 200
	thumb_func_start ov02_02100000
ov02_02100000: ; 0x02100000
	add r0, #0xc8
	bx lr
	thumb_func_end ov02_02100000
