# armrec

A static recompiler: it reads the repository's own `.s` files and emits C that
reproduces the instruction semantics.

This tree is fully decompiled, so nothing here runs during a normal build. It
is kept because the runtime is not optional: `armrec_rt.c` and `armrec_rt.h`
are the port's guest memory, its function table and its hardware hooks, and
every host build links them. The rest is the recompiler and the generators that
feed it, which the sibling ports still use.

| File | What it does |
| --- | --- |
| `armrec.py` | the recompiler |
| `armrec_rt.c` `armrec_rt.h` | the runtime: guest memory, dispatch, hooks |
| `icall_thunk.py` | routes indirect calls in compiled guest C through the table |
| `gen_stkargs.py` | trampolines for calls with more than four argument words |
| `gen_decomp_syms.py` | registers decompiled functions at their guest addresses |
| `gen_decomp_thumb.py` | says whether a named symbol is ARM, Thumb, or data |
| `strip_asm.py` `unstatic.py` | make SDK sources that mwcc accepted compile with gcc |
| `recover_asm.py` | brings back assembly a decompilation deleted, out of git |
| `pe_weak_promote.py` | the weak-symbol fix-up the PE object format needs |
| `irbridge.py` `gen_bridge.py` | wasm32 Diamond/Pearl: the typed bridge between C prototypes and recompiled code (per-TU IR rewrite, then generated wrappers, `c2u$` adapters, extern bindings) |
| `armrec_bridge.h` `armrec_bridge_wasm.c` | the bridge's runtime: C function pointer (wasm table index) to adapter; `tests/bridge/` runs it end to end |

Each file's own header carries the reasoning. `armrec_rt.h` is the one to read
first: it is where the memory model, the calling convention and the four places
guest memory is not memory are written down.

Diamond/Pearl's wasm32 build (`games/diamond/pc/mk/armrec.mk`) runs `armrec.py`
with `--wasm` (calls into C go to the bridge's `c2u$NAME` adapters; the bridge
writes the externs), `--xmap` (the ROM link map places every function, data
label and section, which Pearl needs because the `; 0x...` comments are
Diamond's), `--classes` (the F/D/B/X file the bridge rewrites C against),
`--host-override`, `--guest-libc` and `--undef DIAMOND --define PEARL`.
