/*
 * System V x86_64 fiber switch for np_fiber_posix.c (Apple and ELF).
 *
 * Callee-saved state per the SysV ABI: rbx, rbp, r12-r15, plus the control
 * bits of MXCSR and the x87 control word, which the ABI also requires a
 * callee to preserve. No xmm register is callee-saved on SysV. The 64-byte
 * frame layout (control words, r15, r14, r13, r12, rbx, rbp, return address)
 * is mirrored by initial_frame() in np_fiber_posix.c.
 *
 * Built on every non-Windows target; it assembles to nothing unless the
 * target is x86_64, so one source list serves universal builds.
 */
#if defined(__x86_64__)

#if defined(__APPLE__)
#define NP_SYM(name) _##name
#else
#define NP_SYM(name) name
#endif

    .text

/* void np_fiber_asm_switch(void **save_sp (rdi), void *new_sp (rsi)) */
    .globl NP_SYM(np_fiber_asm_switch)
#if defined(__ELF__)
    .type np_fiber_asm_switch, @function
#endif
    .p2align 4
NP_SYM(np_fiber_asm_switch):
    pushq %rbp
    pushq %rbx
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    subq $8, %rsp
    stmxcsr (%rsp)
    fnstcw 4(%rsp)
    movq %rsp, (%rdi)
    movq %rsi, %rsp
    ldmxcsr (%rsp)
    fldcw 4(%rsp)
    addq $8, %rsp
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %rbx
    popq %rbp
    ret
#if defined(__ELF__)
    .size np_fiber_asm_switch, . - np_fiber_asm_switch
#endif

/* First activation of a fresh fiber: r12 = fn, r13 = arg, rsp 16-aligned.
 * fn must not return; if it does, trap. */
    .globl NP_SYM(np_fiber_asm_start)
#if defined(__ELF__)
    .type np_fiber_asm_start, @function
#endif
    .p2align 4
NP_SYM(np_fiber_asm_start):
    movq %r13, %rdi
    xorl %ebp, %ebp
    callq *%r12
    ud2
#if defined(__ELF__)
    .size np_fiber_asm_start, . - np_fiber_asm_start
#endif

#endif /* __x86_64__ */

#if defined(__ELF__)
    .section .note.GNU-stack, "", %progbits
#endif
