/*
 * 3ds/src/3ds_sym.c: pc_sym.h, answered from the link instead of a file.
 *
 * pc_sym.c reads the running binary's own .symtab out of /proc/self/exe. This
 * console has neither: a 3DSX is code, data and a relocation table, and there
 * is no /proc to find it through even if it had one. So pc_sym.c is replaced
 * here (PC_REPLACED in 3ds/Makefile) and the table it would have read is
 * carried in the image instead.
 *
 * What this answers, and what it does not. Exactly one caller needs symbols on
 * this console: pc_fs_overlay.c, which resets an overlay's writable statics on
 * every load after the first because on hardware the ROM copy is that reset.
 * Its name list is generated (pc/gen_overlay_statics.py) and linked in; what
 * the generator cannot supply is addresses, because they are link-time values
 * and a table holding them would move the very things it named. So the table
 * is emitted at its final size filled with a sentinel and 3ds/gen_ov_addrs.py
 * writes the values into the linked ELF afterwards, no second link, and the
 * numbers cannot go stale against the binary, because they are in it.
 *
 * Every other name is unknown here, and pc_sym_error() says so rather than
 * letting --watch quietly find nothing. That is the whole cost of the
 * replacement: this console has no state inspector by name. It never had one.
 *
 * Why the entries are differences. The loader places a 3DSX wherever it likes
 * and relocates it, so an absolute address baked in at link time would be
 * wrong by the slide. Each entry is signed and relative to the table itself,
 * which is one more object in the same image; the difference is the same after
 * any relocation, and the table's own address is something this file can take
 * directly.
 */

/* By path, and not by -I. pc_sym.h is one of eight headers in pc/src, which
 * is on neither compile chain, pc/src files find their siblings because a
 * quoted include starts in the including file's own directory. Naming this one
 * reaches the contract this file has to satisfy without putting the other
 * seven in front of every port translation unit. */
#include "../../pc/src/pc_sym.h"

#include "armrec_rt.h"

#include "3ds_sym.h"

#include <string.h>

/* Generated, and patched after the link. Both arrays are the same length and
 * the same order; pc_ov_static_desc[i] is named by pc_ov_static_addr[i]. */
struct pc_ov_static_desc {
    unsigned overlay;
    const char *name;
    unsigned size;
};

struct pc_ov_static_addr {
    int off;
    unsigned size;
};

extern const struct pc_ov_static_desc pc_ov_static_desc[];
extern const int pc_ov_static_desc_n;
extern struct pc_ov_static_addr pc_ov_static_addr[];

/*
 * An entry whose offset is not an offset says which of three things happened,
 * in the size field. 3ds/gen_ov_addrs.py writes them; this reads them back.
 *
 *   0    nobody patched the binary. Every entry looks like this in a build
 *        whose post-link step did not run, and answering from one would
 *        restore an overlay's statics out of whatever lies at the table's
 *        own address, so it is reported as no table at all.
 *   1    the link kept no copy. --gc-sections arrives through 3dsx.specs and
 *        discards a tentative definition nothing reads; there is nothing in
 *        the image to reset, which is PC_SYM_DROPPED and not "unknown".
 *   >=2  that many addresses, so it cannot be assigned without a file of
 *        origin. The caller skips those, here and on the desktop.
 */
#define SYM_NOT_AN_OFFSET (-1)
#define SYM_UNPATCHED 0u
#define SYM_DROPPED 1u

static const char *const kNoTable =
    "this console carries no symbol table: the link was not given the "
    "overlay statics' addresses";
static const char *const kOnlyOverlays =
    "this console knows the overlay statics and nothing else; there is no "
    ".symtab in a 3DSX";

static int sChecked;
static int sAnswered;

/* Is the table there at all? A build whose post-link step did not run has
 * every entry at the sentinel, and answering from it would restore an
 * overlay's statics out of whatever lies at the table's own address. */
static int table_ready(void)
{
    int i;

    if (sChecked) {
        return sAnswered > 0;
    }
    sChecked = 1;
    for (i = 0; i < pc_ov_static_desc_n; i++) {
        /* Anything but the untouched sentinel means the patcher ran. An
         * entry it marked dropped or ambiguous is an answer too. */
        if (pc_ov_static_addr[i].off != SYM_NOT_AN_OFFSET
                || pc_ov_static_addr[i].size != SYM_UNPATCHED) {
            sAnswered++;
        }
    }
    return sAnswered > 0;
}

static int index_of(const char *name)
{
    int i;

    if (name == NULL) {
        return -1;
    }
    for (i = 0; i < pc_ov_static_desc_n; i++) {
        if (strcmp(pc_ov_static_desc[i].name, name) == 0) {
            return i;
        }
    }
    return -1;
}

static uint32_t addr_of(int i)
{
    return (uint32_t)(uintptr_t)((char *)pc_ov_static_addr
                                 + pc_ov_static_addr[i].off);
}

void pc_sym_set_exe(const char *hint)
{
    /* There is no file to find. */
    (void)hint;
}

int pc_sym_count(void)
{
    if (!table_ready()) {
        return -1;
    }
    return sAnswered;
}

const char *pc_sym_error(void)
{
    return table_ready() ? kOnlyOverlays : kNoTable;
}

int pc_sym_addresses(const char *name, uint32_t *out, int max)
{
    int i;

    if (!table_ready()) {
        return 0;
    }
    i = index_of(name);
    if (i < 0) {
        return 0;
    }
    if (pc_ov_static_addr[i].off == SYM_NOT_AN_OFFSET) {
        if (pc_ov_static_addr[i].size == SYM_DROPPED) {
            return PC_SYM_DROPPED;
        }
        /* Ambiguous: the count, so the caller skips rather than picking one.
         * Picking one is the defect this whole path exists to avoid. */
        return (int)pc_ov_static_addr[i].size;
    }
    if (out != NULL && max > 0) {
        out[0] = addr_of(i);
    }
    return 1;
}

int pc_sym_lookup(const char *name, uint32_t *addr, uint32_t *size, int *ndup)
{
    int i;

    if (ndup != NULL) {
        *ndup = 0;
    }
    if (!table_ready()) {
        return 0;
    }
    i = index_of(name);
    if (i < 0 || pc_ov_static_addr[i].off == SYM_NOT_AN_OFFSET) {
        return 0;
    }
    if (addr != NULL) {
        *addr = addr_of(i);
    }
    /* The LINKER's size, which the patcher read out of the link, not the
     * one beside it in pc_ov_static_desc, which came from the object. The
     * caller compares the two and a match has to mean something. */
    if (size != NULL) {
        *size = pc_ov_static_addr[i].size;
    }
    if (ndup != NULL) {
        *ndup = 1;
    }
    return 1;
}

int pc_sym_space(uint32_t addr, uint32_t len)
{
    return armrec_guest_span_ok(addr, len) ? PC_SYM_SPACE_GUEST
                                           : PC_SYM_SPACE_NONE;
}

int pc_sym_add_watch(const char *spec, FILE *err)
{
    if (err != NULL) {
        fprintf(err, "pc-sym: --watch %s: %s.\n", spec ? spec : "",
                pc_sym_error());
    }
    return 0;
}

int pc_sym_watch_count(void) { return 0; }
void pc_sym_clear_watches(void) {}
void pc_sym_report(FILE *out, const char *label) { (void)out; (void)label; }
void pc_sym_report_start(FILE *out) { (void)out; }
void pc_sym_frame(uint64_t frame) { (void)frame; }

/*
 * The self-test, and it exists for one claim a build machine cannot make.
 *
 * 3ds/tests/ov_addrs.py proves the numbers are right against the link: it can
 * read the ELF and check every entry. What it cannot check is that they are
 * still right once the loader has moved the image, and that is the whole
 * reason the entries are differences rather than addresses. So this runs on
 * the console, after relocation, and asks whether the addresses the table
 * resolves to are inside the image the loader actually placed, 3dsx.ld's
 * __start__ and __end__, the same pair 3ds_state.c uses to decide whether a
 * pointer is a host image address.
 *
 * Every placed entry is checked and not a sample. A slide applied to one and
 * not another is not a failure anybody would invent, but it costs 181 compares
 * once at startup to rule it out.
 */
extern char __start__[];
extern char __end__[];

int ov_addr_selftest(int *ran)
{
    uintptr_t lo = (uintptr_t)__start__;
    uintptr_t hi = (uintptr_t)__end__;
    int checks = 0;
    int bad = 0;
    int placed = 0;
    int i;

    if (!table_ready()) {
        /* Nothing patched the binary. One check, and it fails, because the
         * alternative is a silent run whose overlays never reset. */
        *ran = 1;
        return 1;
    }

    for (i = 0; i < pc_ov_static_desc_n; i++) {
        uintptr_t a;

        if (pc_ov_static_addr[i].off == SYM_NOT_AN_OFFSET) {
            continue;
        }
        placed++;
        a = (uintptr_t)addr_of(i);
        checks++;
        if (a < lo || a + pc_ov_static_addr[i].size > hi) {
            bad++;
            continue;
        }
        /* Statics are aligned; an entry that is not is an offset written
         * against the wrong base. */
        checks++;
        if ((a & 3u) != 0 && pc_ov_static_addr[i].size >= 4) {
            bad++;
        }
    }

    /* And that there is a table at all, rather than a handful of survivors. */
    checks++;
    if (placed < pc_ov_static_desc_n / 2) {
        bad++;
    }

    *ran = checks;
    return bad;
}
