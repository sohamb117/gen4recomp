/*
 * --watch: read the port's state by name.
 *
 * The argument for all of this is in pc/include/pc_sym.h. What is here is the
 * ELF reader, the spec parser and the two report hooks.
 *
 * Why an ELF reader and not a generated table. Tools/armrec/gen_decomp_syms.py
 * is this project's established pattern for "ask the link what it produced",
 * and it does not work for this: what a state inspector must name is at *host*
 * addresses, and a generated table large enough to hold 31,000 of them moves
 * every one of them by existing. That is a two-pass link and two copies of the
 * truth. The binary's own .symtab is neither; it is written by
 * the linker after every address is final, it costs nothing at build time, and
 * it is read only when a watch asks for it.
 *
 * The cost is a dependency on the binary being findable and unstripped. Both
 * fail loudly: /proc/self/exe is exact on this host, argv[0] is the fallback,
 * and a stripped binary produces "no .symtab" rather than "symbol not found",
 * which is the difference between a diagnosable message and a misleading one.
 */

#include "pc_sym.h"

#include "armrec_rt.h"
#include "pc_state.h"

#if defined(_WIN32)
/*
 * The table this file reads is the port's own ELF symtab, and a Windows
 * build is PE, a different container wanting a different parser (or
 * dbghelp), which is its own task. Until then --watch refuses by name and
 * everything else answers "no symbols", which the callers already handle:
 * A headless Windows run loses the symbol vocabulary, not the run.
 */
#include <string.h>

void pc_sym_set_exe(const char *hint) { (void)hint; }
int pc_sym_lookup(const char *name, uint32_t *addr, uint32_t *size,
                  int *ndup) {
    (void)name; (void)addr; (void)size; (void)ndup;
    return 0;
}
int pc_sym_addresses(const char *name, uint32_t *out, int max) {
    (void)name; (void)out; (void)max;
    return 0;
}
int pc_sym_count(void) { return 0; }
const char *pc_sym_error(void) {
    return "the symbol table is ELF-only; the Windows build has no reader "
           "for its own PE yet";
}
int pc_sym_space(uint32_t addr, uint32_t len) {
    return armrec_guest_span_ok(addr, len) ? PC_SYM_SPACE_GUEST
                                           : PC_SYM_SPACE_NONE;
}
int pc_sym_add_watch(const char *spec, FILE *err) {
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

#else /* the real thing, over the ELF symtab */

#include <elf.h>
#include <stdlib.h>
#include <string.h>

/*
 * <stdlib.h> and <string.h> here are arm9/lib/MSL_C/include/'s, the *guest's*
 * C library, which the include order puts ahead of the host's. MSL's stdlib.h
 * declares abs, srand and rand and nothing else, and its string.h has no
 * strchr. The symbols are still the host's at link time (that is what
 * test_libc_split guarantees), so only the declarations are missing, and
 * pc_video.c already documents the same problem for strerror and exit.
 *
 * This is not cosmetic here. An implicitly declared function returns int, and
 * that is *silently right* for every one of these on i386 and wrong the moment
 * one of them returns 64 bits, writing this file cost an hour to a test that
 * called strtoull and got its low half.
 */
extern void *malloc(size_t n);
extern void free(void *p);
extern void qsort(void *base, size_t n, size_t width,
                  int (*cmp)(const void *, const void *));
extern unsigned long strtoul(const char *s, char **end, int base);
extern char *strchr(const char *s, int c);

/* ------------------------------------------------------------------ */
/* The table                                                           */
/* ------------------------------------------------------------------ */

struct sym {
    uint32_t value;
    uint32_t size;
    const char *name;
};

struct span {
    uint32_t addr;
    uint32_t size;
};

static struct sym *syms;
static int nsyms;
static char *strtab;
static struct span *sections; /* SHF_ALLOC only: what is safe to read */
static int nsections;

static int loaded; /* 0 not tried, 1 ok, -1 failed */
static char load_err[256];
static const char *exe_hint;

void pc_sym_set_exe(const char *hint) { exe_hint = hint; }

static void fail(const char *path, const char *why) {
    snprintf(load_err, sizeof load_err, "%s: %s", path, why);
}

/* Read `n` bytes at `off`, or NULL. Sections are read individually rather than
 * the file being slurped: build/pc/pokeplatinum is 51 MB and the two sections
 * this needs are under two of them. */
static void *read_at(FILE *f, long off, size_t n) {
    void *p;
    if (n == 0 || fseek(f, off, SEEK_SET) != 0) return NULL;
    p = malloc(n);
    if (p == NULL) return NULL;
    if (fread(p, 1, n, f) != n) {
        free(p);
        return NULL;
    }
    return p;
}

static int sym_cmp(const void *a, const void *b) {
    const struct sym *x = a, *y = b;
    int c = strcmp(x->name, y->name);
    if (c) return c;
    return x->value < y->value ? -1 : x->value > y->value;
}

static int try_path(const char *path) {
    FILE *f = fopen(path, "rb");
    Elf32_Ehdr eh;
    Elf32_Shdr *sh = NULL;
    Elf32_Sym *raw = NULL;
    char *str = NULL;
    int i, symi = -1, n = 0;

    if (f == NULL) {
        fail(path, "cannot open");
        return 0;
    }
    if (fread(&eh, 1, sizeof eh, f) != sizeof eh ||
        memcmp(eh.e_ident, ELFMAG, SELFMAG) != 0) {
        fail(path, "not an ELF file");
        goto out;
    }
    /*
     * ELFCLASS32 is not a portability check, it is showing
     * up here: the port is -m32 because guest struct offsets are 32-bit, and
     * a 64-bit symbol table would mean this binary was built some other way.
     */
    if (eh.e_ident[EI_CLASS] != ELFCLASS32 ||
        eh.e_ident[EI_DATA] != ELFDATA2LSB) {
        fail(path, "not a little-endian 32-bit ELF; the port is built -m32");
        goto out;
    }
    if (eh.e_shoff == 0 || eh.e_shnum == 0 ||
        eh.e_shentsize != sizeof(Elf32_Shdr)) {
        fail(path, "no section headers");
        goto out;
    }
    sh = read_at(f, (long)eh.e_shoff, (size_t)eh.e_shnum * sizeof *sh);
    if (sh == NULL) {
        fail(path, "could not read the section headers");
        goto out;
    }
    for (i = 0; i < eh.e_shnum; i++) {
        if (sh[i].sh_type == SHT_SYMTAB) symi = i;
    }
    if (symi < 0 || sh[symi].sh_entsize != sizeof(Elf32_Sym) ||
        sh[symi].sh_link >= eh.e_shnum) {
        fail(path, "no .symtab, is the binary stripped?");
        goto out;
    }
    raw = read_at(f, (long)sh[symi].sh_offset, sh[symi].sh_size);
    str = read_at(f, (long)sh[sh[symi].sh_link].sh_offset,
                  sh[sh[symi].sh_link].sh_size);
    if (raw == NULL || str == NULL) {
        fail(path, "could not read .symtab/.strtab");
        goto out;
    }
    /* A name index past the end of .strtab would run off the buffer; a
     * terminator at the end makes every in-range index a valid C string. */
    str[sh[sh[symi].sh_link].sh_size - 1] = '\0';

    n = (int)(sh[symi].sh_size / sizeof(Elf32_Sym));
    syms = malloc((size_t)n * sizeof *syms);
    if (syms == NULL) {
        fail(path, "out of memory");
        goto out;
    }
    nsyms = 0;
    for (i = 0; i < n; i++) {
        unsigned type = ELF32_ST_TYPE(raw[i].st_info);
        /*
         * SHN_UNDEF is a reference rather than a definition and has no
         * address; SECTION and FILE entries carry names that are not object
         * names (".text", "pc_sym.c") and would shadow real ones. SHN_ABS is
         * kept deliberately; that is what armrec's `.weak X / .set X, 0x…`
         * guest data labels are, and they are half the point of this file.
         */
        if (raw[i].st_shndx == SHN_UNDEF) continue;
        if (type == STT_SECTION || type == STT_FILE) continue;
        if (raw[i].st_name == 0 ||
            raw[i].st_name >= sh[sh[symi].sh_link].sh_size)
            continue;
        syms[nsyms].value = raw[i].st_value;
        syms[nsyms].size = raw[i].st_size;
        syms[nsyms].name = str + raw[i].st_name;
        if (syms[nsyms].name[0] == '\0') continue;
        nsyms++;
    }
    qsort(syms, (size_t)nsyms, sizeof *syms, sym_cmp);

    sections = malloc((size_t)eh.e_shnum * sizeof *sections);
    nsections = 0;
    if (sections != NULL) {
        for (i = 0; i < eh.e_shnum; i++) {
            if (!(sh[i].sh_flags & SHF_ALLOC) || sh[i].sh_addr == 0) continue;
            sections[nsections].addr = sh[i].sh_addr;
            sections[nsections].size = sh[i].sh_size;
            nsections++;
        }
    }
    strtab = str;
    str = NULL;
    free(sh);
    free(raw);
    fclose(f);
    return 1;

out:
    free(sh);
    free(raw);
    free(str);
    free(syms);
    syms = NULL;
    nsyms = 0;
    fclose(f);
    return 0;
}

static void load(void) {
    if (loaded) return;
    if (try_path("/proc/self/exe")) {
        loaded = 1;
        return;
    }
    /* No /proc, or a host that does not have that link. argv[0] is not exact:
     * A bare name found on PATH will not open here, so it is the fallback
     * and not the first choice. */
    if (exe_hint != NULL && strchr(exe_hint, '/') != NULL &&
        try_path(exe_hint)) {
        loaded = 1;
        return;
    }
    loaded = -1;
}

int pc_sym_count(void) {
    load();
    return loaded == 1 ? nsyms : -1;
}

const char *pc_sym_error(void) {
    return load_err[0] ? load_err : "no symbol table has been read";
}

/* First entry with this name, or -1. The table is sorted by name. */
static int find_first(const char *name) {
    int lo = 0, hi = nsyms, mid, c;
    while (lo < hi) {
        mid = lo + (hi - lo) / 2;
        c = strcmp(syms[mid].name, name);
        if (c < 0) lo = mid + 1;
        else hi = mid;
    }
    return (lo < nsyms && strcmp(syms[lo].name, name) == 0) ? lo : -1;
}

int pc_sym_addresses(const char *name, uint32_t *out, int max) {
    uint32_t last = 0;
    int i, n = 0;

    load();
    if (loaded != 1) return 0;
    i = find_first(name);
    if (i < 0) return 0;
    for (; i < nsyms && strcmp(syms[i].name, name) == 0; i++) {
        /* Sorted by (name, value), so two entries at one address, an alias,
         * or the same object named in .symtab twice, are adjacent, and that
         * is one answer rather than an ambiguity. */
        if (n > 0 && syms[i].value == last) continue;
        last = syms[i].value;
        if (out != NULL && n < max) out[n] = last;
        n++;
    }
    return n;
}

int pc_sym_lookup(const char *name, uint32_t *addr, uint32_t *size, int *ndup) {
    uint32_t seen[8];
    int i, n;

    if (ndup != NULL) *ndup = 0;
    n = pc_sym_addresses(name, seen, (int)(sizeof seen / sizeof seen[0]));
    if (ndup != NULL) *ndup = n;
    if (n != 1) return 0;

    i = find_first(name);
    if (addr != NULL) *addr = syms[i].value;
    if (size != NULL) {
        /*
         * The largest size any entry for this name carries. An armrec guest
         * absolute has none (a `.set` has no size to record), and a symbol
         * that is both weak and strong at one address may record it on only
         * one of them.
         */
        uint32_t s = 0;
        for (; i < nsyms && strcmp(syms[i].name, name) == 0; i++)
            if (syms[i].size > s) s = syms[i].size;
        *size = s;
    }
    return 1;
}

int pc_sym_space(uint32_t addr, uint32_t len) {
    int i;

    if (len == 0) return PC_SYM_SPACE_NONE;
    if (armrec_guest_span_ok(addr, len)) return PC_SYM_SPACE_GUEST;
    load();
    for (i = 0; i < nsections; i++) {
        if ((uint64_t)addr >= sections[i].addr &&
            (uint64_t)addr + len <= (uint64_t)sections[i].addr + sections[i].size)
            return PC_SYM_SPACE_HOST;
    }
    return PC_SYM_SPACE_NONE;
}

static const char *space_name(int space) {
    return space == PC_SYM_SPACE_GUEST ? "guest"
         : space == PC_SYM_SPACE_HOST  ? "host"
                                       : "none";
}

/* ------------------------------------------------------------------ */
/* Watches                                                             */
/* ------------------------------------------------------------------ */

struct watch {
    char spec[64];
    uint32_t addr;
    uint32_t len;
    int space;
};

static struct watch watches[PC_SYM_MAX_WATCH];
static int nwatch;

int pc_sym_watch_count(void) { return nwatch; }

void pc_sym_clear_watches(void) { nwatch = 0; }

/*
 * A decimal or 0x-hex number, all of it. Returns 0 and leaves *out alone if
 * the whole of `s` is not one, "0x" alone and "12x" are both refusals, for
 * the reason --frames -1 was: a parser that accepts a prefix turns a typo into
 * a plausible number.
 */
static int parse_num(const char *s, uint32_t *out) {
    char *end;
    unsigned long v;
    /* Base 10 unless it says 0x, rather than strtoul's base 0: a leading zero
     * meaning octal would make --watch X:010 read eight bytes and look right. */
    int base = (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) ? 16 : 10;

    if (*s == '\0' || *s == '-' || *s == '+') return 0;
    v = strtoul(s, &end, base);
    if (*end != '\0') return 0;
    if (v > 0xFFFFFFFFUL) return 0;
    *out = (uint32_t)v;
    return 1;
}

static int looks_like_address(const char *s) {
    return (s[0] == '0' && (s[1] == 'x' || s[1] == 'X') && s[2] != '\0');
}

int pc_sym_add_watch(const char *spec, FILE *err) {
    char buf[64];
    char *colon, *plus;
    uint32_t base = 0, off = 0, len = 0, size = 0;
    int have_len = 0, ndup = 0, space;

    if (err == NULL) err = stderr;
    if (nwatch >= PC_SYM_MAX_WATCH) {
        fprintf(err, "pc-sym: more than %d --watch specs; nothing needs that many\n",
                PC_SYM_MAX_WATCH);
        return 0;
    }
    if (strlen(spec) >= sizeof buf) {
        fprintf(err, "pc-sym: --watch '%s' is longer than %d characters\n", spec,
                (int)sizeof buf - 1);
        return 0;
    }
    strcpy(buf, spec);

    colon = strchr(buf, ':');
    if (colon != NULL) {
        *colon++ = '\0';
        if (!parse_num(colon, &len)) {
            fprintf(err, "pc-sym: --watch '%s': '%s' is not a byte count\n", spec,
                    colon);
            return 0;
        }
        have_len = 1;
    }
    /* Not buf[0]: a symbol may legitimately begin with '+'? None does, but the
     * offset is only an offset when something precedes it. */
    plus = strchr(buf + (buf[0] ? 1 : 0), '+');
    if (plus != NULL) {
        *plus++ = '\0';
        if (!parse_num(plus, &off)) {
            fprintf(err, "pc-sym: --watch '%s': '%s' is not an offset\n", spec, plus);
            return 0;
        }
    }

    if (looks_like_address(buf)) {
        if (!parse_num(buf, &base)) {
            fprintf(err, "pc-sym: --watch '%s': '%s' is not an address\n", spec, buf);
            return 0;
        }
    } else if (!pc_sym_lookup(buf, &base, &size, &ndup)) {
        if (pc_sym_count() < 0) {
            fprintf(err,
                    "pc-sym: --watch '%s': no symbol table to resolve it in.\n"
                    "  %s\n"
                    "  Give an address instead (--watch 0x02000000:16), which "
                    "needs no table.\n",
                    spec, pc_sym_error());
        } else if (ndup > 1) {
            uint32_t all[8];
            int n = pc_sym_addresses(buf, all, 8), i;
            /*
             * Answering with the first is exactly the defect that hid armrec's
             * colliding file-local functions for the life of this project
             * is a documented hazard, so this refuses and says where
             * the candidates are. Eight object names in this binary collide
             * for real (what.1 is three separate statics) and 1,963 more
             * are armrec's per-file blobN pools.
             */
            fprintf(err,
                    "pc-sym: --watch '%s': %d distinct symbols are called '%s'.\n"
                    "  Naming one of them by address is the way to be "
                    "unambiguous:\n",
                    spec, ndup, buf);
            for (i = 0; i < n && i < 8; i++)
                fprintf(err, "    --watch 0x%08X%s%s\n", all[i],
                        have_len ? ":" : "", have_len ? colon : "");
            if (ndup > n) fprintf(err, "    ... and %d more\n", ndup - n);
        } else {
            fprintf(err,
                    "pc-sym: --watch '%s': no symbol called '%s' in this build.\n"
                    "  The table is the linked binary's own, %d names; a "
                    "decompiled\n"
                    "  static keeps its C spelling and an assembly label keeps "
                    "the\n"
                    "  disassembler's (sLCRNG_State, sScriptConditionTable).\n",
                    spec, buf, pc_sym_count());
        }
        return 0;
    }

    if (!have_len) {
        /* The linker's own size, minus what the offset already skipped. A
         * guest absolute has none, and neither does a raw address. */
        len = (size > off) ? size - off : 4;
    }
    if (len == 0 || len > PC_SYM_MAX_LEN) {
        fprintf(err,
                "pc-sym: --watch '%s': %u bytes is outside 1..%d.\n"
                "  A watch is printed once per frame, so an unbounded one is "
                "an unbounded log.\n",
                spec, len, PC_SYM_MAX_LEN);
        return 0;
    }
    if ((uint64_t)base + off > 0xFFFFFFFFULL) {
        fprintf(err, "pc-sym: --watch '%s': the offset runs past 32 bits\n", spec);
        return 0;
    }
    base += off;

    space = pc_sym_space(base, len);
    if (space == PC_SYM_SPACE_NONE) {
        fprintf(err,
                "pc-sym: --watch '%s': 0x%08X..0x%08X is in no mapped guest region "
                "and in no\n"
                "  section of this executable, so reading it would fault.\n",
                spec, base, (uint32_t)(base + len));
        return 0;
    }

    memcpy(watches[nwatch].spec, spec, strlen(spec) + 1);
    watches[nwatch].addr = base;
    watches[nwatch].len = len;
    watches[nwatch].space = space;
    nwatch++;
    return 1;
}

/* ------------------------------------------------------------------ */
/* Reporting                                                           */
/* ------------------------------------------------------------------ */

void pc_sym_report(FILE *out, const char *label) {
    static const char hexd[] = "0123456789ABCDEF";
    static char hex[2 * PC_SYM_MAX_LEN + 1];
    int i;

    if (nwatch == 0) return;
    if (out == NULL) out = stderr;
    if (label == NULL) label = "-";

    for (i = 0; i < nwatch; i++) {
        const struct watch *w = &watches[i];
        const unsigned char *p = (const unsigned char *)(uintptr_t)w->addr;
        uint32_t j;

        /*
         * A guest watch before armrec_mem_init() would read an unmapped page.
         * That cannot happen through pc_main.c; the hooks are all after it,
         * but pc/tests/test_sym.c reports deliberately early, and a debugging
         * instrument that segfaults is worse than one that says it has nothing.
         */
        if (w->space == PC_SYM_SPACE_GUEST && !armrec_mem_ready) {
            fprintf(out, "pc-sym %-9s %-28s %-5s %08X %8u %-16s UNMAPPED\n",
                    label, w->spec, "guest", w->addr, w->len, "-");
            continue;
        }
        for (j = 0; j < w->len; j++) {
            hex[2 * j] = hexd[p[j] >> 4];
            hex[2 * j + 1] = hexd[p[j] & 15];
        }
        hex[2 * w->len] = '\0';
        fprintf(out, "pc-sym %-9s %-28s %-5s %08X %8u %016llX %s\n", label,
                w->spec, space_name(w->space), w->addr, w->len,
                (unsigned long long)pc_state_fnv1a(PC_STATE_FNV64_OFFSET, p,
                                                   w->len),
                hex);
    }
    fflush(out);
}

void pc_sym_report_start(FILE *out) { pc_sym_report(out, "start"); }

void pc_sym_frame(uint64_t frame) {
    char label[32];

    if (nwatch == 0) return;
    snprintf(label, sizeof label, "frame:%llu", (unsigned long long)frame);
    pc_sym_report(stderr, label);
}

#endif /* !_WIN32 */
