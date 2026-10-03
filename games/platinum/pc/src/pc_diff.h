/*
 * The port's half of the differential runner. See pc/src/pc_diff.c for what is
 * compared and why it is hardware state rather than the game's variables.
 */

#ifndef POKEPLATINUM_PC_DIFF_H
#define POKEPLATINUM_PC_DIFF_H

#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Write the memory map for the oracle to read the same addresses. Returns
 * non-zero on success; the caller exits, because this is a question about the
 * build and not about a run. */
int pc_diff_write_spans(const char *path);

/* Start a trace. `every` is the checkpoint interval in frames; 0 means 1. */
void pc_diff_open(const char *path, unsigned every);

/* A frame boundary. Cheap and silent when no trace is open. */
void pc_diff_frame(unsigned long long frame);

/* The ending hook, in pc_state_at_ending()'s shape: a last checkpoint and the
 * trace closed, however the run finished, a fatal signal included. */
void pc_diff_ending(FILE *out, const char *label);

void pc_diff_close(void);
int  pc_diff_active(void);

#ifdef __cplusplus
}
#endif

#endif /* POKEPLATINUM_PC_DIFF_H */
