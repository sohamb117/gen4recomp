/*
 * Reading the port's state by name.
 *
 * --state-digest answers "did this run end up where the last one did" and
 * nothing finer. When the answer is no, the next question is what moved. The
 * decomp supplies real names for the things worth asking about, such as the
 * LCRNG seed or a keypad word, and this turns a name into bytes.
 *
 * Which address space a name lives in matters, because getting it backwards
 * would make the instrument refuse every name it exists to print. A global that
 * has been decompiled is a host object: the compiler placed it where the host
 * linker wanted it. Guest memory holds only what runs there, dynamic
 * allocations out of the arenas and hardware-shaped state, and none of that has
 * a name in the symbol table.
 *
 * This tree is fully decompiled C, so that split is total: every name resolves
 * to a host address. The reader still reads both spaces and every line says
 * which one it read, because an instrument that quietly conflates two address
 * spaces is worse than none.
 *
 * The names come from the linked binary's own .symtab, read at runtime out of
 * /proc/self/exe. That is post-link truth and there is exactly one copy of it.
 * Generating a table with `nm` at build time cannot work here, because the
 * addresses are host addresses and a table big enough to hold 31,000 of them
 * moves every one of them by existing.
 *
 * The cost is a dependency on the binary being findable and unstripped, and
 * both fail loudly: a stripped binary says "no .symtab" rather than "symbol not
 * found".
 *
 * Read-only, and an observer: --watch reads memory and writes lines to stderr.
 * It must not change what the port computes, and test_sym checks that by
 * requiring an identical --state-digest with and without it.
 */

#ifndef POKEPLATINUM_PC_SYM_H
#define POKEPLATINUM_PC_SYM_H

#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Which address space an answer came out of. Guest memory is what
 * armrec_mem_init() maps; host memory is this executable's own image. */
enum {
    PC_SYM_SPACE_NONE = 0,
    PC_SYM_SPACE_GUEST,
    PC_SYM_SPACE_HOST
};

/*
 * A span longer than this is refused rather than truncated. A watch is printed
 * once per frame, so an unbounded one would be an unbounded log; and a
 * truncated line that looked complete is the kind of quiet wrongness this file
 * exists to avoid.
 */
#define PC_SYM_MAX_LEN 4096

/* How many watches may be given. Nothing needs more, and a bound means the
 * table is static and the parse cannot fail for want of memory. */
#define PC_SYM_MAX_WATCH 64

/*
 * Where the symbol table is read from. /proc/self/exe is tried first and is
 * exact; `hint` (argv[0]) is the fallback for a host without /proc. Optional,
 * calling it is what makes the fallback available, not what enables lookup.
 */
void pc_sym_set_exe(const char *hint);

/*
 * Resolve a name to the address and size the linker gave it.
 *
 * Returns 1 on success. Returns 0 if the name is unknown, and 0 *with
 * `ndup` > 1* if the name is defined at more than one distinct address, 1,971
 * object names in this binary are, almost all armrec's per-file `blobN` pools,
 * but eight are real (what.1 is three different statics). Answering with the
 * first would be the same defect that hid armrec's colliding file-local
 * functions for the life of the project, so the caller is expected to report
 * the ambiguity rather than pick.
 *
 * `size` is the linker's st_size, which is 0 for the guest absolutes armrec
 * emits with `.set`; there is no size in a `.weak`/`.set` pair to carry.
 */
int pc_sym_lookup(const char *name, uint32_t *addr, uint32_t *size, int *ndup);

/*
 * Fill `out` with up to `max` addresses a name is defined at; returns how many
 * distinct ones there are, which may exceed `max`. For the ambiguity message.
 *
 * NEGATIVE means something a count cannot say: the name is one this host's
 * table knows about, and the LINK kept no copy of it. That is not the same as
 * unknown, and a caller working from a generated name list has to tell them
 * apart, unknown means the list and the binary have gone out of step, which
 * is a build error, while dropped means there is nothing in the image to act
 * on. Only a host whose linker discards unreferenced definitions can answer
 * this way; the ELF reader below never does, so on this host the value is
 * always >= 0.
 */
#define PC_SYM_DROPPED (-1)
int pc_sym_addresses(const char *name, uint32_t *out, int max);

/* Symbols in the table, or -1 if it could not be read; pc_sym_error() says
 * why. Reading is lazy: the table is only loaded when something asks. */
int pc_sym_count(void);
const char *pc_sym_error(void);

/* Which space `addr`..`addr+len` lies in, or PC_SYM_SPACE_NONE if the span is
 * not wholly inside one mapped guest region or one allocated section of this
 * executable. Reading anything else would fault, so it is refused. */
int pc_sym_space(uint32_t addr, uint32_t len);

/*
 * Add a watch. SPEC is one of
 *
 *     NAME              NAME+OFF          NAME:LEN         NAME+OFF:LEN
 *     0xADDR            0xADDR:LEN
 *
 * with OFF and LEN decimal or 0x-hex. LEN defaults to the symbol's own size,
 * or 4 when it has none. Resolution happens here, once, so a bad spec is a
 * startup error rather than a surprise at frame 300.
 *
 * Returns 1 if the watch was added; on 0 it has already printed what was wrong
 * with it to `err`.
 */
int pc_sym_add_watch(const char *spec, FILE *err);
int pc_sym_watch_count(void);

/* Drop every watch. For tests, which add a great many; nothing in the port
 * calls it, the way nothing calls pc_irq_reset(). */
void pc_sym_clear_watches(void);

/*
 * Print every watch, one line each:
 *
 *   pc-sym <label> <spec> <space> <addr> <len> <digest> <hex>
 *
 * The digest is pc_state_fnv1a over exactly the bytes printed, so a long watch
 * still shows movement at a glance and two runs can be diffed without reading
 * the hex. To stderr, for the reason pc-state goes there: stdout belongs to
 * the guest.
 */
void pc_sym_report(FILE *out, const char *label);

/* The two hooks. `start` is once, after every static initialiser has run and
 * before any guest code; `frame` is at each VBlank, beside the input step and
 * the frame capture. Both no-ops when no watch was given. */
void pc_sym_report_start(FILE *out);
void pc_sym_frame(uint64_t frame);

#ifdef __cplusplus
}
#endif

#endif /* POKEPLATINUM_PC_SYM_H */
