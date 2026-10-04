/*
 * Windows x64 fiber switch for np_fiber_win64.c (GNU assembler: mingw,
 * zig cc, clang).
 *
 * Callee-saved state per the Microsoft x64 ABI: rbx, rbp, rdi, rsi,
 * r12-r15, xmm6-xmm15, and the control bits of MXCSR and the x87 control
 * word. The TEB's stack bounds (NT_TIB StackBase gs:0x08, StackLimit
 * gs:0x10) and DeallocationStack (gs:0x1478), which stack probes, guard-page
 * handling and RtlUnwindEx (msvcrt longjmp) consult, are switched with the
 * stack. Everything goes on the suspended fiber's own stack, so a fiber's
 * whole context is its stack bytes plus the saved stack pointer, which is
 * what lets snapshots capture and restore it.
 *
 * Frame, from the saved stack pointer up (264 bytes, mirrored by
 * initial_frame() in np_fiber_win64.c):
 *     0  mxcsr (4), x87 control word (2), pad (2)
 *     8  xmm6 .. xmm15 (16 each)
 *   168  DeallocationStack, StackLimit, StackBase
 *   192  r15, r14, r13, r12, rsi, rdi, rbx, rbp
 *   256  return address
 */
#if defined(_WIN32) && defined(__x86_64__)

    .text

/* void np_fiber_asm_switch(void **save_sp (rcx), void *new_sp (rdx)) */
    .globl np_fiber_asm_switch
    .def np_fiber_asm_switch; .scl 2; .type 32; .endef
    .p2align 4
np_fiber_asm_switch:
    pushq %rbp
    pushq %rbx
    pushq %rdi
    pushq %rsi
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    pushq %gs:0x08
    pushq %gs:0x10
    pushq %gs:0x1478
    subq $168, %rsp
    stmxcsr (%rsp)
    fnstcw 4(%rsp)
    movups %xmm6, 8(%rsp)
    movups %xmm7, 24(%rsp)
    movups %xmm8, 40(%rsp)
    movups %xmm9, 56(%rsp)
    movups %xmm10, 72(%rsp)
    movups %xmm11, 88(%rsp)
    movups %xmm12, 104(%rsp)
    movups %xmm13, 120(%rsp)
    movups %xmm14, 136(%rsp)
    movups %xmm15, 152(%rsp)
    movq %rsp, (%rcx)

    movq %rdx, %rsp
    ldmxcsr (%rsp)
    fldcw 4(%rsp)
    movups 8(%rsp), %xmm6
    movups 24(%rsp), %xmm7
    movups 40(%rsp), %xmm8
    movups 56(%rsp), %xmm9
    movups 72(%rsp), %xmm10
    movups 88(%rsp), %xmm11
    movups 104(%rsp), %xmm12
    movups 120(%rsp), %xmm13
    movups 136(%rsp), %xmm14
    movups 152(%rsp), %xmm15
    addq $168, %rsp
    popq %gs:0x1478
    popq %gs:0x10
    popq %gs:0x08
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %rsi
    popq %rdi
    popq %rbx
    popq %rbp
    ret

/* First activation of a fresh fiber: r12 = fn, r13 = arg. Aligns the stack,
 * reserves the 32-byte home area the ABI gives every callee, and calls
 * fn(arg). fn must not return; if it does, trap. */
    .globl np_fiber_asm_start
    .def np_fiber_asm_start; .scl 2; .type 32; .endef
    .p2align 4
np_fiber_asm_start:
    andq $-16, %rsp
    subq $32, %rsp
    movq %r13, %rcx
    xorl %ebp, %ebp
    callq *%r12
    ud2

#endif /* _WIN32 && __x86_64__ */
