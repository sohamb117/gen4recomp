/* pc_trap_unreached: the one runtime for every link stub.
 *
 * Deliberately tiny and dependency-free: fprintf to stderr, flush, abort.
 * abort() rather than exit() so a debugger or core dump lands exactly on
 * the offending call, with the stub's caller still on the stack.
 */
#include <stdio.h>
#include <stdlib.h>

#include "pc_trap.h"

#if defined(__wasm__)
/* abort() in wasm is an anonymous `unreachable`; np_host_trap carries the
 * message to the native runtime, which is where the report has to land. */
#include <pc_wasm.h>
#endif

void pc_trap_unreached(const char *sym, const char *why)
{
#if defined(__wasm__)
    pc_wasm_fatalf("\n*** PC TRAP: %s was called ***\n    %s", sym, why);
#else
    fprintf(stderr, "\n*** PC TRAP: %s was called ***\n    %s\n", sym, why);
    fflush(stderr);
    abort();
#endif
}
