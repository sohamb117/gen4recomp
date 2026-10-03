@ 3ds/src/3ds_div0_wrap.s; the cartridge's answer to a division by zero.
@
@ ARM946E-S has no divide instruction, so every `/` and `%` in the game is a
@ call into the compiler's runtime, and the DS's runtime answers a zero divisor
@ instead of faulting. Read out of the ROM this tree builds: `_u32_div_f` and
@ `_s32_div_f` take (numerator, divisor) in r0/r1, return (quotient, remainder)
@ in r0/r1, and both open with
@
@     cmp  r1, #0
@     bxeq lr
@
@ which leaves r0 (the numerator) as the quotient and r1, the divisor,
@ zero, as the remainder. `_s32_div_f` takes absolute values first and
@ branches to the same tail, which re-applies the numerator's sign to both, so
@ for signed and unsigned alike:
@
@     a / 0 == a          a % 0 == 0
@
@ ARM11 has no divide instruction either and devkitARM calls libgcc, so the
@ shape is the same and only the answer differs. MEASURED, because the task
@ said libgcc's default is not good enough until it has been: `__divsi3` on a
@ zero divisor does
@
@     cmp   r0, #0
@     mvngt r0, #0x80000000      @ INT_MAX  if the numerator was positive
@     movlt r0, #0x80000000      @ INT_MIN  if it was negative
@     b     __aeabi_idiv0        @ ... Which is `bx lr`
@
@ so the remainder agrees with the DS and the quotient does not. Nine of the
@ game's divisions by zero would return INT_MAX where the cartridge returns the
@ numerator, silently, and on the frames a player reaches: the SDK's particle
@ emitter halving a one-frame lifetime, the poffin cooking application's steam
@ spawner, and the contest panel scroller are the three the PC port found.
@
@ Why this is not an `__aeabi_idiv0` OVERRIDE. The AEABI hands that function
@ the value the division would have returned, not the numerator, by the time
@ it runs, r0 has already been overwritten with +/-INT_MAX. The numerator is
@ gone, so no implementation of it can answer `a / 0 == a`.
@
@ Why it is `--wrap` and not a replacement. `__aeabi_idiv` and `__divsi3` are
@ the same symbol in libgcc, so a strong definition of one would have to
@ reimplement long division rather than delegate, and division is not cold code
@ here. `-Wl,--wrap` sends every reference to `__wrap_`, leaves libgcc's own
@ internal calls alone, and gives `__real_` back for the non-zero path. The
@ four names below are exactly the four GCC emits for `/` and `%` on this ABI;
@ nothing else in the tree calls a division helper by another name.
@
@ INT_MIN / -1 is not handled and does not need to be. On x86 it raises the
@ same #DE as a zero divisor and pc_div0.c deliberately lets it die, because
@ the cartridge's answer for it was never observed. On ARM there is no trap at
@ all: `__divsi3` computes it and returns, which is what the DS did.

	.syntax unified
	.arm
	.text

.macro DIV0_WRAP name
	.global	__wrap_\name
	.type	__wrap_\name, %function
__wrap_\name:
	cmp	r1, #0
	bxeq	lr
	b	__real_\name
	.size	__wrap_\name, . - __wrap_\name
.endm

	DIV0_WRAP __aeabi_idiv
	DIV0_WRAP __aeabi_uidiv
	DIV0_WRAP __aeabi_idivmod
	DIV0_WRAP __aeabi_uidivmod
