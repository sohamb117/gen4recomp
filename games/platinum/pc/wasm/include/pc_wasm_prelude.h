/* Force-included ahead of every wasm translation unit (pc/Makefile.wasm's
 * EXTRA_BASEFLAGS), before pc/include/pc_prelude.h.
 *
 * The linker-script values the SDK's C reads (SDK_MAIN_ARENA_LO and the
 * rest) are absolute symbols on every other host. A wasm object cannot
 * carry one, so each name becomes a macro over a variable holding the xMAP
 * value; see pc/wasm/gen_lcf_vars.py for why that shape parses at every
 * declaration and use the SDK has. */
#ifndef PC_WASM_PRELUDE_H
#define PC_WASM_PRELUDE_H

#include <pc_wasm_lcf.h>

#endif /* PC_WASM_PRELUDE_H */
