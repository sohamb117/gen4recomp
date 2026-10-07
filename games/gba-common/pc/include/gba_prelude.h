/*
 * Force-included ahead of every decomp TU of a GBA guest (gbabuild.py).
 * wasi-libc's <limits.h> defines PAGE_SIZE, which pokeemerald's pokedex.c
 * uses as an enum constant; agbcc's libc headers have no such macro.
 */
#ifndef GBA_PRELUDE_H
#define GBA_PRELUDE_H
#include <limits.h>
#undef PAGE_SIZE
#endif
