/*
 * 3ds/src/3ds_cpu.h: one virtual CPU, and the trap that keeps it one.
 *
 * Every guest thread in this port runs on one host thread. See 3ds_cpu.c for
 * what that means, what it does not mean, and why the console needs a check
 * the build machine cannot run.
 */

#ifndef POKEPLATINUM_3DS_CPU_H
#define POKEPLATINUM_3DS_CPU_H

/* Record this thread as the one guest code may run on. Called from the crt,
 * before anything else that could hand work to libctru. */
void cpu_bind_main(void);

/* Whether the caller is on it. 0 before cpu_bind_main(). */
int cpu_is_main(void);

/*
 * Fault if it is not, naming what was running. `what` is a static string;
 * the report draws it and nothing frees it. Cheap enough for the frame
 * boundary: one svcGetThreadId and a compare.
 */
void cpu_assert_main(const char *what);

/* Console self-test, in the shape the others use: returns failures, sets
 * *ran. Answers 0 checks on a build machine, which has no thread ids. */
int cpu_selftest(int *ran);

#endif /* POKEPLATINUM_3DS_CPU_H */
