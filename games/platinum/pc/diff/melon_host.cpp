/*
 * The oracle: melonDS's core, headless, writing the same trace the port writes.
 *
 * What makes this port's differential runner different, and it is the whole
 * reason to build one. A port compared against an emulator normally compares a
 * decompilation against a *dump*, and lives with every difference that follows
 * from the two not being the same program. This tree builds its own ROM, so
 * both sides execute the same image: a divergence is the port's.
 *
 * What is comparable here is hardware state, not the game's memory, and that
 * is not a limitation of this program; it is what the port is. Every
 * decompiled global is a HOST object the host linker placed, so the game's
 * variables are not in guest memory at all and there is nothing at a guest
 * address to compare them against. What does live in guest memory is what the
 * hardware sees: VRAM, the palettes, OAM, the I/O window, and the arena
 * allocations the game makes out of main RAM. The first four are pure data
 * with no pointers in them and are the oracle's real subject; the arenas hold
 * host pointers wherever the game stored the address of a static, so they
 * diverge for a reason that is not a bug. The runner measures that rather than
 * assuming it; see the span report it prints.
 *
 * What a checkpoint is. `boot` is taken after SetupDirectBoot and before the
 * first instruction; `frame:N` after the Nth RunFrame, one-based. The port's
 * `boot` is not the same instant; it enters at the game's own entry point,
 * with the cartridge's boot already done, so the two traces are aligned by
 * the runner and the phase offset is printed rather than assumed.
 *
 * DETERMINISM. Everything that could make a run irreproducible is an explicit
 * input: the ROM, the save image, the RTC epoch and the input script. There is
 * no host clock in this program or in melon_platform.cpp.
 *
 * LICENCE. Built against melonDS, which is GPLv3-or-later, so this program is
 * too, as is the port, which already carries melonDS-derived hardware models
 * in pc/hw/. The melonDS checkout is not vendored here; pc/Makefile finds it
 * beside this repository and says so when it cannot.
 */

#include "NDS.h"
#include "GPU_Soft.h"
#include "GPU3D_Soft.h"
#include "NDSCart.h"
#include "Args.h"
#include "SPI.h"
#include "RTC.h"
#include "Platform.h"
#include "Savestate.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <string>
#include <vector>
#include <memory>
#include <algorithm>

using namespace melonDS;

namespace melonDS { namespace Platform {
extern int melon_stop_reason;
} }

static FILE* trace = nullptr;

/* ------------------------------------------------------------------ */
/* The digest                                                          */
/* ------------------------------------------------------------------ */

/*
 * FNV-1a, the same constants pc/src/pc_state.c uses. One digest in the
 * project: a number this program prints and a number --state-digest prints
 * have to mean the same thing, or the diff compares two hashes rather than
 * two memories.
 */
static const uint64_t FNV64_OFFSET = 0xcbf29ce484222325ULL;
static const uint64_t FNV64_PRIME  = 0x100000001b3ULL;

static uint64_t fnv1a(uint64_t h, const void* buf, size_t n)
{
    const unsigned char* p = (const unsigned char*)buf;
    for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= FNV64_PRIME; }
    return h;
}

/* ------------------------------------------------------------------ */
/* The span list                                                       */
/* ------------------------------------------------------------------ */

/*
 * The spans come from a file the PORT writes (`--diff-spans`), not from a list
 * here. The port's memory map is the one that has to be matched, it already
 * knows it, and a second copy in this file would be a copy that goes stale the
 * first time a region moves.
 */
struct Span { uint32_t addr, len; std::string kind; };

static std::vector<Span> spans;

static bool load_spans(const char* path)
{
    FILE* f = fopen(path, "r");
    char line[512];

    if (!f) { perror(path); return false; }
    while (fgets(line, sizeof line, f))
    {
        unsigned a, n;
        char kind[128];
        if (line[0] == '#' || line[0] == '\n') continue;
        if (sscanf(line, "%x %u %127s", &a, &n, kind) != 3) continue;
        spans.push_back(Span{ (uint32_t)a, (uint32_t)n, std::string(kind) });
    }
    fclose(f);
    return !spans.empty();
}

/*
 * Reading guest memory the way the ARM9 would.
 *
 * NDS::ARM9Read8 is the general answer and it is what a span outside main RAM
 * has to go through, because VRAM's bank mapping and the I/O window are not
 * flat arrays. Main RAM is the common case and is a flat array with a mask, so
 * it gets a fast path: a 4 MB span is four million virtual calls otherwise,
 * per checkpoint.
 */
static uint64_t digest_span(NDS& nds, uint32_t addr, uint32_t len)
{
    uint64_t h = FNV64_OFFSET;

    if (addr >= 0x02000000 && (uint64_t)addr + len <= 0x03000000)
    {
        for (uint32_t i = 0; i < len; i++)
            h = fnv1a(h, &nds.MainRAM[(addr + i) & nds.MainRAMMask], 1);
        return h;
    }
    for (uint32_t i = 0; i < len; i++)
    {
        u8 b = nds.ARM9Read8(addr + i);
        h = fnv1a(h, &b, 1);
    }
    return h;
}

/* ------------------------------------------------------------------ */
/* The input script                                                    */
/* ------------------------------------------------------------------ */

/*
 * The same file pc/src/pc_input.c reads, and deliberately parsed a second time
 * here rather than shared: the two programs describe the same *console* and
 * neither is entitled to the other's reading of a script. If the two parsers
 * disagree the trace diverges and the runner says so, which is exactly what a
 * shared parser would hide.
 *
 * The grammar is that file's: `<frame> keys A B`, `<frame> touch X Y`,
 * `<frame> release`. A `keys` line replaces the whole key state; a `touch`
 * line puts the pen down and does not touch the keys; `release` lifts it.
 * Lines are applied at or before their frame, in order.
 */
struct Event
{
    uint32_t frame;
    enum { KEYS, TOUCH, RELEASE } kind;
    uint32_t keymask;      /* active low, melonDS SetKeyMask layout */
    uint16_t x, y;
};

static std::vector<Event> events;

static int button_bit(const char* s)
{
    /* melonDS SetKeyMask: 0 A, 1 B, 2 select, 3 start, 4 right, 5 left,
     * 6 up, 7 down, 8 R, 9 L, 10 X, 11 Y, the DS KEYINPUT order with
     * EXTKEYIN's two on top, which is what NDS::SetKeyMask splits back out.
     * The names are pc_input.c's, which are the SDK's. */
    static const struct { const char* name; int bit; } tbl[] = {
        { "A", 0 }, { "B", 1 }, { "SELECT", 2 }, { "START", 3 },
        { "RIGHT", 4 }, { "LEFT", 5 }, { "UP", 6 }, { "DOWN", 7 },
        { "R", 8 }, { "L", 9 }, { "X", 10 }, { "Y", 11 },
    };
    for (const auto& e : tbl) if (!strcmp(s, e.name)) return e.bit;
    return -1;
}

static bool load_script(const char* path)
{
    FILE* f = fopen(path, "r");
    char line[512];
    int lineno = 0;

    if (!f) { perror(path); return false; }
    while (fgets(line, sizeof line, f))
    {
        char word[64];
        unsigned long long frame;
        int off = 0;
        Event ev {};

        lineno++;
        if (sscanf(line, " %63s%n", word, &off) != 1) continue;
        if (word[0] == '#') continue;
        if (sscanf(line, " %llu %63s%n", &frame, word, &off) != 2)
        {
            fprintf(stderr, "melon-host: %s:%d: not `<frame> <what> ...`\n",
                    path, lineno);
            fclose(f);
            return false;
        }
        ev.frame = (uint32_t)frame;

        if (!strcmp(word, "keys"))
        {
            const char* p = line + off;
            ev.kind = Event::KEYS;
            ev.keymask = 0xFFF;                /* nothing held */
            for (;;)
            {
                int n = 0, bit;
                if (sscanf(p, " %63s%n", word, &n) != 1) break;
                p += n;
                if (!strcmp(word, "none")) continue;
                if (!strcmp(word, "DEBUG"))
                {
                    /* PAD_BUTTON_DEBUG is an ARM7-side bit with no KEYINPUT
                     * bit; melonDS has nowhere to put it. Saying so beats
                     * dropping it, because a script that used it would then
                     * diverge with no explanation. */
                    fprintf(stderr, "melon-host: %s:%d: `DEBUG` has no emulated "
                            "equivalent; the debug button is not a KEYINPUT "
                            "bit. Drop it, or compare without it.\n",
                            path, lineno);
                    fclose(f);
                    return false;
                }
                bit = button_bit(word);
                if (bit < 0)
                {
                    fprintf(stderr, "melon-host: %s:%d: unknown button '%s'\n",
                            path, lineno, word);
                    fclose(f);
                    return false;
                }
                ev.keymask &= ~(1u << bit);
            }
        }
        else if (!strcmp(word, "touch"))
        {
            unsigned x, y;
            if (sscanf(line + off, " %u %u", &x, &y) != 2)
            {
                fprintf(stderr, "melon-host: %s:%d: touch needs x and y\n",
                        path, lineno);
                fclose(f);
                return false;
            }
            ev.kind = Event::TOUCH;
            ev.x = (uint16_t)x;
            ev.y = (uint16_t)y;
        }
        else if (!strcmp(word, "release"))
        {
            ev.kind = Event::RELEASE;
        }
        else
        {
            fprintf(stderr, "melon-host: %s:%d: unknown directive '%s'\n",
                    path, lineno, word);
            fclose(f);
            return false;
        }
        events.push_back(ev);
    }
    fclose(f);
    return true;
}

/* ------------------------------------------------------------------ */
/* The SPU                                                             */
/* ------------------------------------------------------------------ */

/*
 * Why the SPU is read out of a savestate and not out of its registers. Four of
 * the per-channel registers, SOUNDxSAD, SOUNDxTMR, SOUNDxPNT and SOUNDxLEN,
 * are WRITE-ONLY on hardware, and melonDS is faithful about it: ARM7Read32
 * answers zero for all four. Reading them back would compare four zeros
 * against four zeros and call it agreement. SPU::DoSavestate is public and
 * writes Cnt, SrcAddr, TimerReload, LoopPos and Length per channel in that
 * order, the last value written to each register under hardware's own masks,
 * which is what pc/hw/pc_spu.c stores too. SPU::Channels is private, so this is
 * the only route melonDS offers short of patching a checkout that is not ours.
 *
 * And the decode is checked rather than trusted. Byte offsets hand-derived from
 * another project's serialiser are exactly the kind of thing that goes on being
 * wrong quietly, so every field the public API *can* answer is compared against
 * it on every read, sixteen channel CNTs, SOUNDCNT, SOUNDBIAS, both capture
 * CNTs and both capture destinations. A stride one byte out cannot survive
 * sixteen 32-bit CNTs, and the payload's total length is checked too, so a
 * field added upstream stops this program by name instead of silently shifting
 * every channel.
 *
 * The length check is always live and the field check is not, which is worth
 * knowing before trusting a short run: every one of those fields is zero until
 * the game keys a channel on, so over a few frames a wrong offset reads zeros
 * and matches zeros.
 */
struct SpuState
{
    struct { u32 cnt, srcaddr; u16 timer; u32 looppos, length; } chan[16];
    struct { u8 cnt; u32 dstaddr; u16 timer; u32 length; } cap[2];
    u16 soundcnt;
    u8  mastervol;
    u16 bias;
};

/* SPUChannel::DoSavestate and SPUCaptureUnit::DoSavestate write their fields
 * back to back with no padding (Savestate::VarArray is a memcpy) so these
 * are byte counts and not sizeof()s. */
static const u32 SPU_SS_CHAN  = 105;   /* Cnt..FIFO[8] */
static const u32 SPU_SS_CAP   = 51;    /* Cnt..FIFO[4] */
static const u32 SPU_SS_HEAD  = 17;    /* Cnt, MasterVolume, Bias, last samples,
                                        * MixInterval, Mute(Bool32) */
static const u32 SPU_SS_TOTAL = SPU_SS_HEAD + 16 * SPU_SS_CHAN + 2 * SPU_SS_CAP;

static u32 ss_u32(const u8* p) { u32 v; memcpy(&v, p, 4); return v; }
static u16 ss_u16(const u8* p) { u16 v; memcpy(&v, p, 2); return v; }

static void spu_state(NDS& nds, SpuState& s)
{
    Savestate ss(64 * 1024);
    nds.SPU.DoSavestate(&ss);

    const u8* buf = (const u8*)ss.Buffer();
    u32 total = ss.Length();
    u32 magic = 0;
    bool found = false;

    if (ss.Error)
    {
        fprintf(stderr, "melon-host: SPU::DoSavestate reported an error.\n");
        exit(1);
    }
    /* Only the header and this one section are in the buffer, so the magic
     * appears once. Section() writes 4 bytes of magic, 4 of length and 8
     * reserved before the payload. */
    for (u32 i = 0; i + 16 <= total; i++)
        if (!memcmp(buf + i, "SPU.", 4)) { magic = i; found = true; break; }
    if (!found)
    {
        fprintf(stderr, "melon-host: no `SPU.` section in melonDS's own SPU "
                        "savestate (%u bytes).\n", total);
        exit(1);
    }

    const u8* p = buf + magic + 16;
    if (total - (magic + 16) != SPU_SS_TOTAL)
    {
        fprintf(stderr,
                "melon-host: melonDS's SPU savestate section is %u bytes and "
                "this program decodes %u.\n"
                "  SPUChannel::DoSavestate or SPUCaptureUnit::DoSavestate has\n"
                "  gained or lost a field upstream. Re-derive SPU_SS_CHAN,\n"
                "  SPU_SS_CAP and SPU_SS_HEAD from src/SPU.cpp rather than\n"
                "  guessing.\n",
                (unsigned)(total - (magic + 16)), (unsigned)SPU_SS_TOTAL);
        exit(1);
    }

    s.soundcnt  = ss_u16(p + 0);
    s.mastervol = p[2];
    s.bias      = ss_u16(p + 3);

    const u8* c = p + SPU_SS_HEAD;
    for (int i = 0; i < 16; i++, c += SPU_SS_CHAN)
    {
        s.chan[i].cnt     = ss_u32(c + 0);
        s.chan[i].srcaddr = ss_u32(c + 4);
        s.chan[i].timer   = ss_u16(c + 8);
        s.chan[i].looppos = ss_u32(c + 10);
        s.chan[i].length  = ss_u32(c + 14);
    }
    for (int u = 0; u < 2; u++, c += SPU_SS_CAP)
    {
        s.cap[u].cnt     = c[0];
        s.cap[u].dstaddr = ss_u32(c + 1);
        s.cap[u].timer   = ss_u16(c + 5);
        s.cap[u].length  = ss_u32(c + 7);
    }

    struct Check { const char* what; u32 got, want; };
    std::vector<Check> check;
    for (int i = 0; i < 16; i++)
        check.push_back({ "SOUNDxCNT", s.chan[i].cnt,
                          nds.SPU.Read32(0x04000400 + i * 0x10) });
    check.push_back({ "SOUNDCNT",  s.soundcnt, nds.SPU.Read16(0x04000500) });
    check.push_back({ "SOUNDBIAS", s.bias,     nds.SPU.Read16(0x04000504) });
    for (int u = 0; u < 2; u++)
        check.push_back({ "SNDCAPxCNT", s.cap[u].cnt,
                          nds.SPU.Read8(0x04000508 + u) });
    for (int u = 0; u < 2; u++)
        check.push_back({ "SNDCAPxDAD", s.cap[u].dstaddr,
                          nds.SPU.Read32(0x04000510 + u * 8) });
    for (size_t i = 0; i < check.size(); i++)
        if (check[i].got != check[i].want)
        {
            fprintf(stderr,
                    "melon-host: the SPU savestate decode disagrees with "
                    "melonDS's own register read.\n"
                    "  %s: decoded %08X, SPU::Read says %08X (check %d of %d)\n"
                    "  The field offsets in spu_state() no longer match\n"
                    "  src/SPU.cpp.\n",
                    check[i].what, check[i].got, check[i].want,
                    (int)i + 1, (int)check.size());
            exit(1);
        }
}

static void spu_report(NDS& nds)
{
    SpuState s;

    spu_state(nds, s);
    for (int i = 0; i < 16; i++)
        fprintf(trace, "spu ch %d %08X %08X %04X %08X %08X\n", i,
                s.chan[i].cnt, s.chan[i].srcaddr, s.chan[i].timer,
                s.chan[i].looppos, s.chan[i].length);
    for (int u = 0; u < 2; u++)
        fprintf(trace, "spu cap %d %02X %08X %04X %08X\n", u,
                s.cap[u].cnt, s.cap[u].dstaddr, s.cap[u].timer,
                s.cap[u].length);
    fprintf(trace, "spu glob %04X %02X %04X\n",
            s.soundcnt, s.mastervol, s.bias);
}

/* ------------------------------------------------------------------ */
/* The framebuffer                                                     */
/* ------------------------------------------------------------------ */

/*
 * A digest per screen, over the r, g and B bytes of each pixel in raster
 * order. Bytes rather than words so it says nothing about either host's byte
 * order; three rather than four so it says nothing about the fourth byte,
 * which is not a colour, melonDS keeps its own value there and the port
 * keeps zero.
 *
 * GetFramebuffers() hands back the upper and lower *LCDs* rather than engines
 * A and B: GPU_Soft.cpp's DrawScanline picks the destination by ScreenSwap, so
 * POWCNT1 bit 15 has already been applied here, exactly as pc_video.c applies
 * it on the other side.
 */
static void frame_digest(NDS& nds, uint64_t* top, uint64_t* bot)
{
    void* a = nullptr;
    void* b = nullptr;

    auto digest = [](const void* p) -> uint64_t {
        const u32* px = (const u32*)p;
        uint64_t h = FNV64_OFFSET;
        for (int i = 0; i < 256 * 192; i++)
        {
            u8 rgb[3];
            rgb[0] = (u8)(px[i] >> 16);
            rgb[1] = (u8)(px[i] >> 8);
            rgb[2] = (u8)(px[i]);
            h = fnv1a(h, rgb, 3);
        }
        return h;
    };

    *top = *bot = 0;
    if (!nds.GPU.GetFramebuffers(&a, &b)) return;
    if (a) *top = digest(a);
    if (b) *bot = digest(b);
}

/* ------------------------------------------------------------------ */
/* The trace                                                           */
/* ------------------------------------------------------------------ */

static void cp(NDS& nds, const char* label)
{
    uint64_t top, bot;

    fprintf(trace, "cp %s\n", label);
    frame_digest(nds, &top, &bot);
    fprintf(trace, "fb %016llX %016llX\n",
            (unsigned long long)top, (unsigned long long)bot);
    spu_report(nds);
    for (const Span& s : spans)
        fprintf(trace, "d %08X %u %016llX\n", s.addr, s.len,
                (unsigned long long)digest_span(nds, s.addr, s.len));
    fflush(trace);
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

static void usage(void)
{
    fprintf(stderr,
        "usage: pcdiff-melon --rom ROM --spans FILE --trace OUT [options]\n"
        "  --frames N     how many frames to run (default 60)\n"
        "  --every N      checkpoint every N frames (default 1)\n"
        "  --input FILE   the same script the port takes as PC_INPUT\n"
        "  --sav FILE     the cartridge save image to start from\n"
        "  --sav-out FILE what the cartridge save holds when the run ends\n"
        "  --wav FILE     the console's own mix, 16-bit stereo at 32728 Hz\n"
        "  --io-selftest  which of the first I/O page's halfwords are private\n"
        "                 to a processor and which are visible across, then exit\n"
        "  --rtc S        YYYY-MM-DDTHH:MM:SS (default 2000-01-01T00:00:00)\n");
}

/*
 * The console's own mix, written the way the port writes its --dump-audio:
 * 16-bit stereo little-endian at 32,728 Hz, which is what BOTH sides run at
 * (33,513,982 / 1024), so a comparison between the two files needs no
 * resampling and can be made sample for sample.
 *
 * It is drained every frame rather than at the end, and that is not a
 * convenience: SPU::ReadOutput reads out of a ring the core keeps writing
 * into, and a ring nobody drains laps and loses everything but the tail.
 */
namespace {

FILE* wav = nullptr;
u32 wav_samples = 0;

void wav_u32(FILE* f, u32 v)
{
    u8 b[4] = { (u8)v, (u8)(v >> 8), (u8)(v >> 16), (u8)(v >> 24) };
    fwrite(b, 1, 4, f);
}

void wav_u16(FILE* f, u32 v)
{
    u8 b[2] = { (u8)v, (u8)(v >> 8) };
    fwrite(b, 1, 2, f);
}

bool wav_open(const char* path)
{
    wav = fopen(path, "wb");
    if (!wav) { perror(path); return false; }
    fwrite("RIFF", 1, 4, wav);
    wav_u32(wav, 36);                 /* fixed up at close */
    fwrite("WAVEfmt ", 1, 8, wav);
    wav_u32(wav, 16);
    wav_u16(wav, 1);                  /* PCM */
    wav_u16(wav, 2);                  /* stereo */
    wav_u32(wav, 32728);
    wav_u32(wav, 32728 * 4);
    wav_u16(wav, 4);
    wav_u16(wav, 16);
    fwrite("data", 1, 4, wav);
    wav_u32(wav, 0);                  /* fixed up at close */
    fflush(wav);
    return true;
}

void wav_drain(NDS& nds)
{
    if (!wav) return;
    /* Comfortably more than one frame's worth (32728/60 is about 546), so a
     * frame that ran long does not leave samples behind in the ring. */
    static s16 buf[4096 * 2];
    int got = nds.SPU.ReadOutput(buf, 4096);
    if (got <= 0) return;
    fwrite(buf, sizeof(s16) * 2, (size_t)got, wav);
    wav_samples += (u32)got;
}

void wav_close(void)
{
    if (!wav) return;
    if (fseek(wav, 4, SEEK_SET) == 0)
    {
        wav_u32(wav, 36 + wav_samples * 4);
        if (fseek(wav, 40, SEEK_SET) == 0) wav_u32(wav, wav_samples * 4);
    }
    fclose(wav);
    wav = nullptr;
}

/*
 * Which of the first i/o page's halfwords are per-processor.
 *
 * 0x04000000-0x04000FFF is separate hardware on the two CPUs, separate IME,
 * IE, IF, four timers and four DMA channels, all at the same addresses, and
 * the port has to model that (armrec_cpu_switch saves one page and restores
 * the other). Which registers are private and which are visible across is a
 * question about the console, so the console answers it: on a freshly reset
 * machine, write a marker to every halfword from one processor's bus and read
 * it back from both.
 *
 * The rule the port's mirror list is derived from has no judgement in it:
 * A register is mirrored only when neither processor's write sticks anywhere
 * AND both read the same value. This prints the evidence for that rule rather
 * than asking anyone to take the numbers on trust; armrec_rt.h cites them.
 */
void io_selftest(NDS& nds)
{
    const u32 base = 0x04000000, count = 0x1000 / 2;
    unsigned priv9 = 0, priv7 = 0, both = 0, across = 0, dead = 0;

    printf("# addr  arm9-write: arm9 arm7   arm7-write: arm9 arm7   verdict\n");
    for (u32 i = 0; i < count; i++)
    {
        u32 a = base + i * 2;
        const u16 marker = 0xA5C3;

        nds.Reset();
        u16 before9 = nds.ARM9Read16(a), before7 = nds.ARM7Read16(a);
        nds.ARM9Write16(a, marker);
        u16 w9r9 = nds.ARM9Read16(a), w9r7 = nds.ARM7Read16(a);

        /* The baseline is re-read after the second reset rather than reused
         * from the first: Reset() does not clear every I/O register, so a
         * value the ARM9 wrote a moment ago can still be sitting there and
         * would read as the ARM7 having written it. Three registers were
         * reported "visible across" on that mistake alone. */
        nds.Reset();
        u16 base9 = nds.ARM9Read16(a), base7 = nds.ARM7Read16(a);
        nds.ARM7Write16(a, marker);
        u16 w7r9 = nds.ARM9Read16(a), w7r7 = nds.ARM7Read16(a);

        bool stuck9 = (w9r9 != before9) || (w9r7 != before7);
        bool stuck7 = (w7r9 != base9) || (w7r7 != base7);
        bool visible = (w9r7 != before7) || (w7r9 != base9);

        const char* verdict;
        if (!stuck9 && !stuck7)
        {
            verdict = (before9 == before7) ? "read-only, same on both"
                                           : "read-only, DIFFERENT per cpu";
            dead++;
        }
        else if (visible) { verdict = "VISIBLE ACROSS"; across++; }
        else if (stuck9 && stuck7) { verdict = "private to each"; both++; }
        else if (stuck9) { verdict = "arm9 only"; priv9++; }
        else { verdict = "arm7 only"; priv7++; }

        if (stuck9 || stuck7 || before9 != before7)
            printf("%08X  %04X %04X   %04X %04X   %s\n",
                   a, w9r9, w9r7, w7r9, w7r7, verdict);
    }
    printf("# %u halfword(s) private to the ARM9, %u to the ARM7, %u private "
           "to each,\n#   %u writable in all, %u visible across, %u neither\n",
           priv9, priv7, both, priv9 + priv7 + both, across, dead);
}

}  /* namespace */

int main(int argc, char** argv)
{
    const char* rom = nullptr;
    const char* spanpath = nullptr;
    const char* out = nullptr;
    const char* script = nullptr;
    const char* savpath = nullptr;
    const char* savout = nullptr;
    const char* wavpath = nullptr;
    bool ioselftest = false;
    const char* rtc = "2000-01-01T00:00:00";
    unsigned frames = 60, every = 1;
    int y = 2000, mo = 1, d = 1, h = 0, mi = 0, se = 0;

    for (int i = 1; i < argc; i++)
    {
        const char* a = argv[i];
        auto next = [&](void) -> const char* {
            if (i + 1 >= argc) { usage(); exit(2); }
            return argv[++i];
        };
        if      (!strcmp(a, "--rom"))    rom = next();
        else if (!strcmp(a, "--spans"))  spanpath = next();
        else if (!strcmp(a, "--trace"))  out = next();
        else if (!strcmp(a, "--input"))  script = next();
        else if (!strcmp(a, "--sav"))    savpath = next();
        else if (!strcmp(a, "--sav-out")) savout = next();
        else if (!strcmp(a, "--wav"))    wavpath = next();
        else if (!strcmp(a, "--io-selftest")) ioselftest = true;
        else if (!strcmp(a, "--rtc"))    rtc = next();
        else if (!strcmp(a, "--frames")) frames = (unsigned)strtoul(next(), nullptr, 10);
        else if (!strcmp(a, "--every"))  every = (unsigned)strtoul(next(), nullptr, 10);
        else if (!strcmp(a, "--help"))   { usage(); return 0; }
        else { fprintf(stderr, "melon-host: unknown option %s\n", a); usage(); return 2; }
    }
    if (!rom) { usage(); return 2; }
    if (!ioselftest && (!spanpath || !out)) { usage(); return 2; }
    if (every == 0) every = 1;
    if (sscanf(rtc, "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &se) != 6)
    {
        fprintf(stderr, "melon-host: --rtc wants YYYY-MM-DDTHH:MM:SS\n");
        return 2;
    }
    if (!ioselftest && !load_spans(spanpath))
    {
        fprintf(stderr, "melon-host: no spans in %s; the port writes this "
                        "file with --diff-spans.\n", spanpath);
        return 1;
    }
    if (script && !load_script(script)) return 1;

    std::unique_ptr<u8[]> romdata;
    u32 romlen = 0;
    {
        FILE* f = fopen(rom, "rb");
        if (!f) { perror(rom); return 1; }
        fseek(f, 0, SEEK_END);
        long len = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (len <= 0) { fprintf(stderr, "%s: empty\n", rom); fclose(f); return 1; }
        romlen = (u32)len;
        romdata = std::make_unique<u8[]>(romlen);
        if (fread(romdata.get(), 1, romlen, f) != romlen)
        {
            fprintf(stderr, "%s: short read\n", rom);
            fclose(f);
            return 1;
        }
        fclose(f);
    }

    auto cart = NDSCart::ParseROM(std::move(romdata), romlen, nullptr);
    if (!cart) { fprintf(stderr, "%s: not a DS ROM melonDS can parse\n", rom); return 1; }

    /*
     * NDSArgs' defaults are FreeBIOS and a generated firmware, so this runs
     * with nothing but the ROM this tree builds, no console files.
     */
    NDSArgs args {};

    /*
     * On the heap, and that is not style: melonDS::NDS carries the ARM9 and
     * ARM7 memory-timing tables and the GPU's nine VRAM banks as *members*,
     * about five megabytes of object. As a local it overflows an 8 MB stack in
     * main's prologue, before the first statement, a SIGSEGV with an empty
     * backtrace and no obvious cause.
     */
    std::unique_ptr<NDS> ndsp = std::make_unique<NDS>(std::move(args));
    NDS& nds = *ndsp;

    nds.SetNDSCart(std::move(cart));

    if (savpath)
    {
        FILE* sf = fopen(savpath, "rb");
        if (!sf) { perror(savpath); return 1; }
        fseek(sf, 0, SEEK_END);
        long slen = ftell(sf);
        fseek(sf, 0, SEEK_SET);
        std::vector<u8> sav((size_t)(slen > 0 ? slen : 0));
        if (slen > 0 && fread(sav.data(), 1, (size_t)slen, sf) != (size_t)slen)
        {
            fprintf(stderr, "melon-host: short read on %s\n", savpath);
            fclose(sf);
            return 1;
        }
        fclose(sf);
        nds.SetNDSSave(sav.data(), (u32)sav.size());
    }

    nds.Reset();

    /*
     * The ABI check, and it earns its place. melonDS turns its optional
     * features on with `add_definitions()` in the top-level CMakeLists,
     * GDBSTUB_ENABLED, JIT_ENABLED, OGLRENDERER_ENABLED, and add_definitions
     * is directory-scoped, so a consumer outside that CMake project does not
     * inherit them from the `core` target. GDBSTUB_ENABLED puts a
     * Gdb::GdbStub *member* inside ARM, so a library built with it and a caller
     * built without it disagree about sizeof(ARM) and therefore about the
     * offset of every NDS member after ARM9. What that looks like is a SIGSEGV
     * reading MainRAM through a pointer that is not one, with no other symptom.
     * pc/Makefile builds the core with all three off; this says so if that ever
     * stops being true. After Reset(), because Reset() is what sets MainRAMMask.
     */
    if (nds.MainRAM == nullptr || nds.MainRAMMask == 0)
    {
        fprintf(stderr,
                "melon-host: melonDS's NDS object does not look like one, "
                "MainRAM=%p mask=%08X.\n"
                "  This is what an ABI mismatch looks like: libcore.a and this\n"
                "  file must agree about GDBSTUB_ENABLED, JIT_ENABLED and\n"
                "  OGLRENDERER_ENABLED, and melonDS exports none of them on the\n"
                "  `core` target. Build the core with -DENABLE_GDBSTUB=OFF\n"
                "  -DENABLE_JIT=OFF -DENABLE_OGLRENDERER=OFF, as pc/Makefile does.\n",
                (void*)nds.MainRAM, nds.MainRAMMask);
        return 1;
    }

    /*
     * The oracle's mix is resampled to whatever OutputSampleRate says, and
     * melonDS's default is not the DS's rate, 600 frames came out as 481,404
     * stereo samples, which is 48 kHz, not the 32,728 Hz both engines were
     * assumed to share. Pinning it to the port's rate is what makes --wav
     * comparable with PC_DUMP_AUDIO sample for sample instead of after a
     * resample nobody would trust.
     */
    nds.SPU.SetOutputSampleRate(32728.0);

    if (ioselftest)
    {
        io_selftest(nds);
        return 0;
    }

    nds.RTC.SetDateTime(y, mo, d, h, mi, se);
    nds.SetupDirectBoot(std::string(rom));
    nds.Start();

    if (wavpath && !wav_open(wavpath)) return 1;

    trace = fopen(out, "w");
    if (!trace) { perror(out); return 1; }
    fprintf(trace, "pcdiff 1\n");
    fprintf(trace, "side melon\n");
    fprintf(trace, "rom %s\n", rom);
    fprintf(trace, "rtc %s\n", rtc);
    fprintf(trace, "frames %u\n", frames);
    fprintf(trace, "every %u\n", every);
    for (const Span& s : spans)
        fprintf(trace, "span %08X %u %s\n", s.addr, s.len, s.kind.c_str());

    /*
     * Before the first instruction: the cartridge's boot has finished and the
     * ARM9 has not started. This is the closest thing melonDS has to the
     * port's `boot`, which is *later*, the port enters at the game's own
     * entry point. The runner knows the two are not the same instant and
     * reports the offset; recording both is what lets it.
     */
    cp(nds, "boot");

    size_t ev = 0;
    for (unsigned f = 1; f <= frames; f++)
    {
        /*
         * Input is applied before the frame runs, because that is where the
         * port applies it, pc_input_frame() is called from OS_Halt before
         * the VBlank handler is dispatched.
         */
        while (ev < events.size() && events[ev].frame <= f)
        {
            const Event& e = events[ev++];
            switch (e.kind)
            {
            case Event::KEYS:    nds.SetKeyMask(e.keymask); break;
            case Event::TOUCH:   nds.TouchScreen(e.x, e.y); break;
            case Event::RELEASE: nds.ReleaseScreen(); break;
            }
        }

        nds.RunFrame();
        wav_drain(nds);

        if (Platform::melon_stop_reason >= 0)
        {
            fprintf(trace, "stopped %d frame %u\n",
                    Platform::melon_stop_reason, f);
            fprintf(stderr, "melon-host: the core stopped at frame %u "
                            "(reason %d); the trace ends there.\n",
                    f, Platform::melon_stop_reason);
            break;
        }
        if (f % every == 0 || f == frames)
        {
            char label[64];
            snprintf(label, sizeof label, "frame:%u", f);
            cp(nds, label);
        }
    }

    fprintf(trace, "end\n");
    fclose(trace);
    wav_close();

    /*
     * The save, ONCE, at the end, as an explicit output. Platform::WriteNDSSave
     * stays empty on purpose, a core that wrote a save file as it went would
     * make the oracle's trace depend on a previous run of itself, which is the
     * same mistake as letting it read a host clock. Asking the cart what it
     * holds when the run is over has neither problem, and it is what makes a
     * save comparison between the two engines possible at all.
     */
    if (savout)
    {
        const u8* sav = nds.GetNDSSave();
        u32 savlen = nds.GetNDSSaveLength();
        if (!sav || savlen == 0)
        {
            fprintf(stderr, "melon-host: the cart has no save memory to write\n");
            return 1;
        }
        FILE* sf = fopen(savout, "wb");
        if (!sf) { perror(savout); return 1; }
        if (fwrite(sav, 1, savlen, sf) != savlen)
        {
            fprintf(stderr, "melon-host: short write on %s\n", savout);
            fclose(sf);
            return 1;
        }
        fclose(sf);
        fprintf(stderr, "melon-host: save written to %s (%u bytes)\n",
                savout, savlen);
    }
    return 0;
}
