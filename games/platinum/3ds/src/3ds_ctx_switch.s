@ 3ds/src/3ds_ctx_switch.s, the context switch itself, on ARM11.
@
@ 7.10. Three entry points, and the reason all three are assembly is one
@ measured fact about where a resumed context lands.
@
@ The obvious implementation is wrong, and it was written and run first.
@ newlib's ARM setjmp saves r4-r11, sp, lr and d8-d15, the whole callee-saved
@ state of this ABI, so `OS_SaveContext(c) { if (setjmp(buf)) return TRUE;
@ return FALSE; }` looks like a complete switch. It is not, and the console
@ froze on the first switch back. setjmp records lr and sp INSIDE
@ OS_SaveContext, so a later longjmp resumes inside OS_SaveContext's frame --
@ and by then that frame is dead and its memory has been reused, because the
@ caller's shape is
@
@     if (OS_SaveContext(&cur->context)) return;   /* returns, frame popped */
@     OS_LoadContext(&next->context);              /* SAME stack, reused */
@
@ so OS_SaveContext's epilogue pops a return address that OS_LoadContext's
@ locals overwrote, and branches into nothing. It is not a race and not a
@ timing bug: it happens on the first resumption, every time.
@
@ What makes it correct is saving lr and sp as they are AT ENTRY to
@ OS_SaveContext, the CALLER's return address and the CALLER's stack pointer.
@ Resuming then lands in the caller, on the caller's own frame, which is still
@ live because the caller is the scheduler that is waiting for this thread to
@ be picked again. That cannot be expressed in C: a C function that wants to
@ return two different values has a frame, and its frame is the thing that must
@ not matter. It is also why the DS's own version of these was assembly.
@
@ The bookkeeping still belongs in C, so OS_SaveContext calls ctx_begin_save()
@ for the slot lookup and the stack-guard check, and only then, after
@ restoring sp and lr to their entry values, performs the save.
@
@ The register list and the d8-d15 half are newlib's, deliberately: they are
@ this ABI's callee-saved set, and `-mfloat-abi=hard` makes the VFP half part
@ of the contract rather than an optimisation.

	.syntax unified
	.arm
	.fpu vfpv2
	.text

@ ctx_start(void *sp_top, unsigned long context, void (*trampoline)(unsigned long))
@
@ Begin executing on a stack that is not this one. A context that has never run
@ has nothing to restore, so something has to put sp on its fresh stack and
@ call its entry. Never returns: the trampoline runs the thread body and then
@ the lr the scheduler wrote into the OSContext, which reaches OS_LoadContext.
@
@ fp and lr are zeroed rather than left holding the caller's. Both the fault
@ report and a debugger walk back through fp, and a frame pointer into the
@ other stack would make this stack's backtrace read as though it were still
@ inside the scheduler. Zero ends the walk, which is the truth.
	.global	ctx_start
	.type	ctx_start, %function
ctx_start:
	mov	sp, r0
	mov	r0, r1
	mov	fp, #0
	mov	lr, #0
	bx	r2
	.size	ctx_start, . - ctx_start

@ BOOL OS_SaveContext(OSContext *context)
@
@ FALSE on the way past; TRUE when a later ctx_resume picks this context up.
@ The push/pop around the C call is what lets the save record the ENTRY sp and
@ lr rather than this function's own.
	.global	OS_SaveContext
	.type	OS_SaveContext, %function
OS_SaveContext:
	push	{r4, lr}
	bl	ctx_begin_save		@ (OSContext *) -> u32 *buf
	mov	r1, r0
	pop	{r4, lr}		@ sp and lr are the caller's again
	stmia	r1!, {r4, r5, r6, r7, r8, r9, sl, fp, sp, lr}
	vstmia	r1, {d8-d15}
	mov	r0, #0
	bx	lr
	.size	OS_SaveContext, . - OS_SaveContext

@ void ctx_resume(u32 *buf)
@
@ Restore a saved context and return TRUE from the OS_SaveContext that saved
@ it. Never returns to its own caller, sp belongs to the other context now,
@ which is exactly what OS_LoadContext's contract says.
	.global	ctx_resume
	.type	ctx_resume, %function
ctx_resume:
	ldmia	r0!, {r4, r5, r6, r7, r8, r9, sl, fp, sp, lr}
	vldmia	r0, {d8-d15}
	mov	r0, #1
	bx	lr
	.size	ctx_resume, . - ctx_resume
