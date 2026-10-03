/*
 * The port's argument layer. See pc_args.c for why a flag is translated into
 * its environment variable rather than carried as state: one input, two
 * spellings, and nothing downstream can tell which was used.
 */
#ifndef PC_ARGS_H
#define PC_ARGS_H

#include <stdio.h>

struct pc_opt {
    const char *flag;   /* "--frames" */
    const char *var;    /* "PC_FRAMES": what the flag becomes */
    const char *arg;    /* metavar, or NULL for a presence-only input */
    const char *help;   /* one line, printed by --help */
    /*
     * 1 = repeating the flag APPENDS, comma-separated, instead of replacing.
     * --watch needs it: several watches are one list in one variable rather
     * than PC_WATCH_1..N, so the input surface stays "every input is a
     * variable" and a recorded command line still round-trips. No spec
     * grammar in the port uses a comma, which is what makes the separator
     * safe.
     */
    int append;
};

/* The table itself, for anything that needs to enumerate the contract. */
const struct pc_opt *pc_args_table(int *count);

void pc_args_help(FILE *out);

/*
 * Translate argv into the environment. 0 = continue booting, 1 = exit 0
 * (--help was asked for), -1 = bad argument, nothing applied.
 */
int pc_args_apply(int argc, char **argv);

#endif /* PC_ARGS_H */
