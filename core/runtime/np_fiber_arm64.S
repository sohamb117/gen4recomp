/*
 * AAPCS64 fiber switch for np_fiber_posix.c (Apple and ELF).
 *
 * Only the callee-saved state needs saving because np_fiber_asm_switch is an
 * ordinary call: x19-x28, the frame pointer x29, the link register x30 (our
 * return address) and the low halves d8-d15 of v8-v15. x18 is the platform
 * register (reserved on Apple) and is never touched. The 0xb0-byte frame
 * layout is mirrored by initial_frame() in np_fiber_posix.c.
 *
 * Built on every non-Windows target; it assembles to nothing unless the
 * target is arm64, so one source list serves universal builds.
 */
#if defined(__aarch64__)

#if defined(__APPLE__)
#define NP_SYM(name) _##name
#else
#define NP_SYM(name) name
#endif

    .text

/* void np_fiber_asm_switch(void **save_sp, void *new_sp) */
    .globl NP_SYM(np_fiber_asm_switch)
#if defined(__ELF__)
    .type np_fiber_asm_switch, %function
#endif
    .p2align 2
NP_SYM(np_fiber_asm_switch):
    sub sp, sp, #0xb0
    stp x19, x20, [sp, #0x00]
    stp x21, x22, [sp, #0x10]
    stp x23, x24, [sp, #0x20]
    stp x25, x26, [sp, #0x30]
    stp x27, x28, [sp, #0x40]
    stp x29, x30, [sp, #0x50]
    stp d8,  d9,  [sp, #0x60]
    stp d10, d11, [sp, #0x70]
    stp d12, d13, [sp, #0x80]
    stp d14, d15, [sp, #0x90]
    mov x9, sp
    str x9, [x0]
    mov sp, x1
    ldp x19, x20, [sp, #0x00]
    ldp x21, x22, [sp, #0x10]
    ldp x23, x24, [sp, #0x20]
    ldp x25, x26, [sp, #0x30]
    ldp x27, x28, [sp, #0x40]
    ldp x29, x30, [sp, #0x50]
    ldp d8,  d9,  [sp, #0x60]
    ldp d10, d11, [sp, #0x70]
    ldp d12, d13, [sp, #0x80]
    ldp d14, d15, [sp, #0x90]
    add sp, sp, #0xb0
    ret
#if defined(__ELF__)
    .size np_fiber_asm_switch, . - np_fiber_asm_switch
#endif

/* First activation of a fresh fiber: x19 = arg, x20 = fn. fn must not
 * return; if it does, trap rather than run off the top of the stack. */
    .globl NP_SYM(np_fiber_asm_start)
#if defined(__ELF__)
    .type np_fiber_asm_start, %function
#endif
    .p2align 2
NP_SYM(np_fiber_asm_start):
    mov x0, x19
    mov x29, #0
    mov x30, #0
    blr x20
    brk #0x1
#if defined(__ELF__)
    .size np_fiber_asm_start, . - np_fiber_asm_start
#endif

#endif /* __aarch64__ */

#if defined(__ELF__)
    .section .note.GNU-stack, "", %progbits
#endif
