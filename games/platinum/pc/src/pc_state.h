/*
 * A digest of the guest's whole state.
 *
 * Determinism is a claim about two runs, so no amount of reading the port
 * settles it; it needs two runs and something to compare. The frame dumps
 * compare a *consequence* of determinism: a divergence inside guest memory is
 * invisible to them until it reaches a pixel, and it may never reach one. This
 * is the thing itself: one 64-bit digest per mapped region plus a combined one,
 * taken at a defined point and printed in a form a test can diff.
 *
 * It is deliberately not a debugger. Reading a named object out of guest memory
 * wants the symbol table and is its own task; this answers the narrower
 * question "did the port end up in the same state as last time", which is what
 * a differential runner will later ask of two programs instead of two runs.
 */

#ifndef POKEPLATINUM_PC_STATE_H
#define POKEPLATINUM_PC_STATE_H

#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The hash itself, over any buffer. Guest-only is a policy of the digest
 * functions below rather than of the arithmetic: pc/src/pc_video.c fingerprints
 * host-side framebuffers for the frame manifest with this same function, so
 * that a digest in a manifest and a digest from --state-digest mean the same
 * thing. One implementation, one meaning.
 */
#define PC_STATE_FNV64_OFFSET 0xcbf29ce484222325ULL
uint64_t pc_state_fnv1a(uint64_t seed, const void *p, uint32_t n);

/* Digest one span of guest memory. Reads nothing outside it. Words
 * marked by pc_state_mark_host_word() are skipped; they hold a host
 * pointer, which is a fact about the link layout, not about the game. */
uint64_t pc_state_digest_span(uint32_t base, uint32_t size);

/*
 * Record that the guest word at `addr` holds a host pointer. The digest
 * then skips exactly that word. `addr` is rounded down to 4 bytes;
 * addresses outside guest space (the host image at 0x10000000 and
 * above) are ignored. Marking the same word twice is a no-op.
 *
 * This is how host pointers are kept out of the digest: by ADDRESS,
 * at the write, not by folding every word whose VALUE looks like a
 * pointer. Folding by value was measured (76% of changing words sit
 * at or above 0x10000000 because graphics and compressed data have
 * a high top nibble) and rejected.
 */
void pc_state_mark_host_word(uint32_t addr);

/* Mark the guest word that *is* `field`, i.e. &some_struct->ptr. */
void pc_state_mark_host_field(const void *field);

/* After a constructor has filled `obj`, mark every aligned word in
 * [obj, obj+n) whose current value is a host address. Used at the
 * sites that install a handful of callbacks at once, so the patch
 * does not have to name each field. */
void pc_state_scan_mark_host_words(const void *obj, uint32_t n);

/* How many guest words are currently marked. For the dump and for
 * the completeness proof (perturb the link; every word that moves
 * must already be in this set). */
uint32_t pc_state_host_word_count(void);

/* Digest every region armrec_mem_init() mapped, in table order, and return the
 * combined value. Zero if guest memory is not mapped. */
uint64_t pc_state_digest(void);

/*
 * Write the per-region digests and the combined one to `out`, one region per
 * line, prefixed so a test can find them in a boot's ordinary chatter:
 *
 *   pc-state <label> main RAM 02000000 00400000 3a1f...
 *   pc-state <label> TOTAL 8 regions c40e...
 */
void pc_state_report(FILE *out, const char *label);

/*
 * Report at the end of the run, however it ends: normal return, exit(), or a
 * fatal signal. A crashed run's digest is exactly as comparable as a returned
 * one's, and opting in is what keeps this from displacing the port's own fault
 * handler, which prints the faulting address and a backtrace.
 */
void pc_state_report_at_exit(FILE *out);

/*
 * The general form, because there can only be one set of signal handlers and
 * more than one thing will eventually want to report at the end, each of
 * these re-installs SIG_DFL and re-raises, so a second independently installed
 * handler would never run and the failure would be a silently missing report.
 *
 * `label` is "exit" for a normal ending and "sigsegv"/"sigabrt"/... for a fatal
 * one, so a report can say which it was. Callbacks run in registration order,
 * once, whichever ending arrives first.
 */
typedef void (*pc_state_ending_fn)(FILE *out, const char *label);
void pc_state_at_ending(pc_state_ending_fn fn, FILE *out);

#ifdef __cplusplus
}
#endif

#endif /* POKEPLATINUM_PC_STATE_H */
