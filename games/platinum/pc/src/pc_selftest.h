/*
 * The port's in-binary vector suites. See pc_selftest.c for why they share one
 * input, and why the SPU and graphics known-answer tests are not among them.
 */
#ifndef POKEPLATINUM_PC_SELFTEST_H
#define POKEPLATINUM_PC_SELFTEST_H

#include <stdio.h>

/* Runs every suite, printing one line each plus a summary. Returns 1 if all
 * passed. The caller is expected to exit on the result: a run that has just
 * proved its own crypto has nothing further to say about the game. */
int pc_selftest_run(FILE *out);

#endif /* POKEPLATINUM_PC_SELFTEST_H */
