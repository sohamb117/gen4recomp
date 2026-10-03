/* pc_trap_unreached: the one runtime for every link stub.
 *
 * Deliberately tiny and dependency-free: fprintf to stderr, flush, abort.
 * abort() rather than exit() so a debugger or core dump lands exactly on
 * the offending call, with the stub's caller still on the stack.
 */
#include <stdio.h>
#include <stdlib.h>

#include "pc_trap.h"

void pc_trap_unreached(const char *sym, const char *why)
{
    fprintf(stderr, "\n*** PC TRAP: %s was called ***\n    %s\n", sym, why);
    fflush(stderr);
    abort();
}
