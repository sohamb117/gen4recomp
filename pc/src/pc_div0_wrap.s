@ pc/src/pc_div0_wrap.s; the cartridge's answer to a division by zero, ARM.
@
@ The reasoning is pc_div0.c's, and it is the same reasoning on both hosts:
@ ARM946E-S has no divide instruction, so every `/` and `%` in the game is a
@ call into the compiler's runtime, and the DS's runtime answers a zero
@ divisor instead of faulting. Read off `_u32_div_f` and `_s32_div_f` in the
@ ROM this tree builds, both of which open with
@
@     cmp  r1, #0
@     bxeq lr
@
@ leaving r0 (the numerator) as the quotient and r1, the divisor, zero
@, as the remainder. So for signed and unsigned alike, a / 0 == a and
@ a % 0 == 0.
@
@ What was expected and is not true. Being back on ARM was supposed to make
@ pc_div0.c unnecessary, since the fault it exists to catch is an x86 one.
@ MEASURED instead, on this toolchain under qemu: a division by zero in a
@ plain armhf binary dies with SIGFPE. gcc calls __aeabi_idiv, libgcc's
@ routine saturates to +/-INT_MAX and tail-calls __aeabi_idiv0, and GLIBC'S
@ __aeabi_idiv0 RAISES. So an armhf build dies in exactly the three places
@ the x86 build died before pc_div0.c existed, the SDK's particle emitter
@ halving a one-frame lifetime, the poffin steam spawner, the contest panel
@ scroller.
@
@ Why not an __aeabi_idiv0 OVERRIDE, which would be one C function and no
@ link flags. The AEABI hands that function the value the division would
@ have returned, not the numerator: by the time it runs r0 holds +/-INT_MAX
@ and the numerator is gone. Measured, overriding it does stop the signal,
@ and answers 7/0 as INT_MAX and -7/0 as INT_MIN where the cartridge says 7
@ and -7. No implementation of it can be right.
@
@ WHY --wrap and not a replacement. __aeabi_uidiv and __udivsi3 are the same
@ symbol in libgcc, in one archive member with __aeabi_uidivmod, so defining
@ one of them means reimplementing long division rather than delegating to
@ it, and division is not cold code here. --wrap redirects the references
@ instead, leaves libgcc's own internal calls alone, and hands __real_ back
@ for the non-zero path, which stays exactly as fast as it was.
@
@ Three instructions, and the first two are the cartridge's own.
@
@ This file has a twin: 3ds/src/3ds_div0_wrap.s, which is the same answer for
@ the 3DS's ARM11 and was written first, validated on hardware, and is where
@ the fuller argument lives. Two copies of four instructions is a fork
@ waiting to happen; they should become one file when someone is next in
@ 3ds/ and can build it.
@
@ INT_MIN / -1 is not handled and needs nothing. On x86 it raises the same
@ #DE as a zero divisor and pc_div0.c deliberately lets it die, because the
@ cartridge's answer for it was never observed. On ARM there is no trap:
@ __divsi3 computes it and returns, which is what the DS did.

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

	.section .note.GNU-stack,"",%progbits
