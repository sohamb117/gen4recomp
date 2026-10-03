#ifndef PC_DIV0_H
#define PC_DIV0_H

#include <stdint.h>

/* The eight general registers in the order x86-32's ModRM r/m field numbers
 * them, plus the program counter: eax ecx edx ebx esp ebp esi edi. */
struct pc_x86_regs {
    uint32_t r[8];
    uint32_t eip;
};

/* Looks at the instruction at g->eip. If it is a div or idiv whose divisor is
 * zero, rewrites the registers to the answer the DS runtime gives, steps eip
 * past it and returns 1. Returns 0 for anything else, including an idiv
 * overflow, which the caller should treat as fatal exactly as before. */
int pc_div0_fixup(struct pc_x86_regs *g);

/* How many divisions this has answered, for a test to assert on. */
unsigned long pc_div0_count(void);

/* The vector suite: divides by zero on purpose and checks each answer against
 * the ROM's runtime. Returns 1 if every case held. */
int pc_div0_selftest(void);

#endif /* PC_DIV0_H */
