/*
 * The port's argument layer, and its contract.
 *
 * Every input to this port is an environment variable, and that is the
 * determinism contract the whole test suite rests on: same inputs in,
 * byte-identical run out. Flags do not add a second way to configure the port:
 * Each one is translated into its variable here, with putenv, before a
 * single line of the port has read anything. `--frames 900` and
 * `PC_FRAMES=900` are therefore not two equivalent inputs that must be kept in
 * step; they are one input with two spellings, and the flag stops existing the
 * moment this function returns.
 *
 * That is the whole reason the translation happens at the front instead of
 * threading an options struct through the port: a struct would be a second
 * source of truth, and the two would drift the first time someone read a
 * variable directly. Nothing downstream of here can tell how it was invoked.
 *
 * KEY=VALUE arguments still work and are not deprecated. WSL interop does not
 * forward arbitrary environment into a Windows process, so
 *   pokeplatinum.exe PC_ROM=build/rom/pokeplatinum.us.nds PC_VIEW=pplat
 * is how the Windows build is driven, and pc/play-win.sh spells it that way.
 *
 * The table is the contract and `--help` prints it. There is no second list:
 * The help text, the parser and the test that checks every input is reachable
 * both ways all read PC_OPTS. Adding an input to the port means adding a row
 * here, and test_cli fails if a PC_* variable the port reads has no row;
 * which is how the five inputs that had accumulated without ever reaching the
 * documentation were found.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pc_args.h"

/*
 * arg == NULL means presence-only: the port asks `getenv(...) != NULL` and
 * ignores the value, so a bare flag sets "1". A value may still be given with
 * --flag=VALUE, because a caller pasting a recorded command line should not
 * have to know which kind it is.
 *
 * arg != NULL means the value is read, so it is required and the metavar says
 * what it is. Getting this classification wrong is not cosmetic: a valued
 * input marked presence-only becomes unreachable by flag.
 */
static const struct pc_opt PC_OPTS[] = {
    /* What the game is and where its state goes. */
    { "--rom",            "PC_ROM",            "PATH",
      "cartridge image (default: the tree's build/rom/pokeplatinum.us.nds)" },
    { "--mods",           "PC_MODS",           "LIST",
      "runtime content packages, in load order (directory names under"
      " --mods-dir). Not the compile-time MODS= plugin list", 1 },
    { "--mods-dir",       "PC_MODS_DIR",       "PATH",
      "folder of runtime packages (default pc/mods)" },
    { "--modfs",          "PC_MODFS",          "LIST",
      "extra overlay roots: absolute directories of loose fs/<nitro-path>"
      " and narc/<nitro-path>/<idx> trees, applied after every package", 1 },
    { "--modfs-probe",    "PC_MODFS_PROBE",    "PATH",
      "open this Nitro path through FS_OpenFile once the filesystem is up,"
      " print the bytes, exit" },
    { "--modfs-probe-narc", "PC_MODFS_PROBE_NARC", "PATH/IDX",
      "read this NARC member through src/narc.c once packages load,"
      " print the bytes, exit" },
    { "--modfs-probe-count", "PC_MODFS_PROBE_COUNT", "PATH/IDX",
      "once the filesystem is up, grow that NARC's file count for any"
      " planted append, read the member, print count and bytes, exit" },
    { "--modfs-probe-map", "PC_MODFS_PROBE_MAP", "ID",
      "print the cooked map header's area and matrix through the game"
      " getters, exit" },
    { "--save",           "PC_SAVE",           "PATH|none",
      "save chip (default: beside the ROM). 'none' for a run that must not persist" },
    { "--rtc",            "PC_RTC",            "\"Y-M-D H:M:S\"",
      "re-base the deterministic clock (default 2009-03-22 10:00:00, never the host's)" },
    { "--lab",            "PC_LAB",            "FILE",
      "mint a save from a recipe: boot straight into the new game, apply the"
      " recipe's map/party/bag/badges/flags through the game's own setters,"
      " write --save and exit" },
    { "--lab-at",         "PC_LAB_AT",         "N",
      "the frame the recipe is applied at (default 600)" },
    { "--sweep",          "PC_SWEEP",          "FILE",
      "one boot, many maps: each line is `mapHeaderId warpId` and the run"
      " changes map to each in turn and reports what it found" },
    { "--sweep-out",      "PC_SWEEP_OUT",      "PATH",
      "where the sweep writes its verdicts (default stdout), flushed per line"
      " so a map that takes the run down still names the ones before it" },
    { "--save-layout",    "PC_SAVE_LAYOUT",    "PATH",
      "write where each save-table page lands, as the game computes it at"
      " boot, then exit; what a save reader needs and cannot restate" },
    { "--map-scan",       "PC_LAB_MAPSCAN",    "PATH",
      "with --lab, write what is on the map the recipe loaded: the player, the"
      " object being faced, every object, and the water, waterfall, rock-climb"
      " and encounter tiles around them" },
    { "--map-scan-radius", "PC_LAB_MAPSCAN_R", "N",
      "how many tiles either side --map-scan reports (default 24)" },
    { "--lab-save-at",    "PC_LAB_SAVE_AT",    "N",
      "write the save again at frame N, once the field settles, what a"
      " station whose claim is about what the run DID has to assert on" },

    /* Driving it. */
    { "--input",          "PC_INPUT",          "FILE",
      "scripted input, frame-addressed and level-held" },
    { "--record-input",   "PC_RECORD_INPUT",   "PATH",
      "write the session's input as a replayable script" },
    { "--frames",         "PC_FRAMES",         "N",
      "end the run after N frames" },
    { "--view",           "PC_VIEW",           "NAME",
      "publish frames into /dev/shm/NAME for build/pc/pcview to present" },
    { "--console",        "PC_CONSOLE",        NULL,
      "Windows only: give this run a console to print into. The exe is a GUI"
      " program so that double-clicking it flashes no console box; when a"
      " caller has redirected the output already (every harness run does)"
      " this is a no-op" },
    { "--keep-alive",     "PC_KEEP_ALIVE",     NULL,
      "keep running when the viewer's window closes, for attaching to a long"
      " headless run and detaching again" },
    { "--pace",           "PC_PACE",           "0|1",
      "the 60 fps governor --view turns on; 0 runs as fast as it can, which"
      " breaks the sound up, the mixer produces a frame's audio per emulated"
      " frame and the ring is one second long, so anything above realtime"
      " overruns it" },
    { "--hd3d",           "PC_HD3D",           "1..4",
      "internal resolution: rasterize the 3D layer at N times the DS's pixel"
      " count on each axis (4 is 1024x768). The 2D art is the DS's own and is"
      " replicated" },
    { "--hd3d-record",    "PC_HD3D_RECORD",    NULL,
      "arm the 3D layer's high-resolution recording over a native render, so"
      " that it can be shown to move no pixel, a diagnostic, not a setting" },
    { "--gpu3d",          "PC_GPU3D",          "soft|gl",
      "which producer draws the 3D layer: soft is the rasterizer, the"
      " default and the oracle; gl is the offscreen GPU spike, refused with"
      " a printed line (and the run staying soft) when no context can be"
      " had" },
    { "--gpu3d-half",     "PC_GPU3D_HALF",     "PX",
      "the gl producer's sample point, in pixels, a property of the"
      " driver's rasterizer, swept with --gpu3d-diff per machine; the"
      " default is llvmpipe's own sweep (0.25, where the exact class"
      " lands at zero)" },
    { "--gpu3d-diff",     "PC_GPU3D_DIFF",     "DIR",
      "render every frame with the selected producer AND the software"
      " oracle, and diff the two pictures: claim-class verdicts, per-row/"
      "per-column WHERE, and worst/typical picture triples into DIR. Under"
      " soft it diffs the rasterizer against its own re-render, the null"
      " test. Costs a second render per frame" },
    { "--aspect",         "PC_ASPECT",         "auto|16:9|16:10|4:3|N",
      "widen the 3D field of view: auto follows the window, a ratio or a column"
      " count fixes it" },

    /* Recording what happened. */
    { "--dump-frames",    "PC_DUMP_FRAMES",    "DIR",
      "a PNG per frame plus a manifest, flushed per line" },
    { "--dump-from",      "PC_DUMP_FROM",      "N[:S]",
      "start dumping at frame N, every S-th frame" },
    { "--dump-digest",    "PC_DUMP_DIGEST",    NULL,
      "with --dump-frames, a digest on every manifest row and not only on"
      " the rows that got an image; what says two builds drew the same"
      " picture" },
    { "--dump-audio",     "PC_DUMP_AUDIO",     "PATH",
      "the mix as a WAV" },

    /* Instruments. Each answers one question; none is on by default. */
    { "--threads",        "PC_THREADS",        "N",
      "how many cores draw a frame: the 3D rasterizer and the 2D compositor"
      " split the screen into that many bands. Default is the machine's"
      " processor count; 1 draws on one thread. The picture is the same"
      " either way" },
    { "--bench",          "PC_BENCH",          NULL,
      "at exit, where the frame's time went: 3D rasterize, 2D compose,"
      " publish, audio mix, and everything else" },
    { "--bench-from",     "PC_BENCH_FROM",     "N",
      "with --bench, start the measurement at frame N so a boot and an"
      " intro do not average into a scene's numbers" },
    { "--prof",           "PC_PROF",           "PATH[:HZ]",
      "sample the program counter on CPU time (SIGPROF, 997 Hz unless HZ"
      " says otherwise) into PATH, one u32 per sample; pc/prof_fold.py"
      " folds them into the rasterizer's cost centres. Linux only" },
    { "--survey3d",       "PC_SURVEY3D",       NULL,
      "at exit, what the 3D frames actually used: shading modes, texture"
      " formats, translucency, fog, alpha test, edge marking, AA, and how"
      " the span interpolator ran, what prices a span specialization"
      " before it is written" },
    { "--texcache",       "PC_TEXCACHE",       "0|1",
      "decode each texture once per frame and sample the copy. Unset, it"
      " arms itself at --hd3d 3 and up, where every texel is sampled nine"
      " to sixteen times; 1 forces it on (how the byte-exactness gate runs"
      " at native), 0 keeps the per-sample decode everywhere" },
    { "--view-digest",    "PC_VIEW_DIGEST",    NULL,
      "digest the HD frame per band where it is composed and again after the"
      " locked copy into the page, and say WHERE on a mismatch, the"
      " arbitration between a compose fault, a torn publish and a viewer-side"
      " read" },
    { "--hd-serial",      "PC_HD_SERIAL",      NULL,
      "run the wide/HD compose on the main thread alone, a bisection knob"
      " for a frame that is only wrong with the pool live" },
    { "--trace-frames",   "PC_TRACE_FRAMES",   NULL,
      "heartbeat every 60 VBlanks, display/geometry probe every 300" },
    { "--ctx-trace",      "PC_CTX_TRACE",      NULL,
      "every OSContext switch as it happens: which slot, which context, the"
      " stack pointer being parked or restored, and the return address it will"
      " resume to. A switch that goes wrong leaves a program counter in the"
      " middle of nowhere and a stack that no longer says where it came from,"
      " so the report afterwards cannot name the switch that did it; this is"
      " the log that can. It is how 13.4 was found." },
    { "--trace-pace",     "PC_TRACE_PACE",     NULL,
      "the frame rate the 60 fps governor is actually keeping, every 300"
      " frames; the mixer makes one frame's audio per frame, so a run below"
      " 60 makes the sound break up and this is what says so. It also splits"
      " the frame into work, publish, slip and slack, which sum to it exactly,"
      " and prints those means over the late frames alone: a count says how"
      " often a frame missed, that split says what it missed on. With --bench"
      " as well, the worst miss names which part of the work it was" },
    { "--boot-watchdog",  "PC_BOOT_WATCHDOG",  "SECONDS",
      "after N seconds print where the run is sitting and exit, for a silent spin" },
    { "--probe-2d",       "PC_PROBE_2D",       "N[-M]",
      "end-of-frame dump: VRAMCNT, texture/palette sums, OBJ palettes, OAM" },
    { "--dump-polys",     "PC_DUMP_POLYS",     "N[-M]",
      "the published polygon list for those frames: screen-space vertices,"
      " polygon ID, alpha, depth, texture parameters" },
    { "--dump-polys-file", "PC_DUMP_POLYS_FILE", "PATH",
      "write that list to a file instead of stdout" },
    { "--probe-tex",      "PC_PROBE_TEX",      "TEX,PAL",
      "hex-dump those texture and palette bytes" },
    { "--probe-vb",       "PC_PROBE_VB",       NULL,
      "one line whenever the VBlank task manager's population changes" },
    { "--trace-lcdc",     "PC_TRACE_LCDC",     "N",
      "from frame N, one line per MI copy into the LCDC texture-load window" },
    { "--trace-script",   "PC_TRACE_SCRIPT",   "N",
      "from frame N, a native script-opcode trace" },
    { "--script-cov",     "PC_SCRIPT_COV",     "PATH",
      "append which field-script commands this run executed, and how often" },
    { "--trace-battle",   "PC_TRACE_BATTLE",   NULL,
      "one line per HP change in a battle: battler, move, delta, HP" },
    { "--trace-overlay",  "PC_TRACE_OVERLAY",  NULL,
      "one line per overlay load: id, nth load, snapshot size, dirty bytes before restore" },
    { "--trace-journal",  "PC_TRACE_JOURNAL",  NULL,
      "one line per journal event; which field move the game recorded, and where" },
    { "--trace-fish",     "PC_TRACE_FISH",     NULL,
      "one line per fishing state change, the frame the bite window opens, and how a cast ended" },
    { "--battle",         "PC_LAB_BATTLE",     "KIND ARGS",
      "start a battle once the field is up (wild/legendary/trainer/safari/first/tutorial)" },
    { "--battle-at",      "PC_LAB_BATTLE_AT",  "N",
      "the frame --battle starts on" },
    { "--battle2",        "PC_LAB_BATTLE2",    "KIND ARGS",
      "start a second battle after the first one ends and the field is back" },
    { "--battle2-at",     "PC_LAB_BATTLE2_AT", "N",
      "the frame --battle2 starts on; default is as soon as the field settles" },
    { "--battle-save",    "PC_LAB_BATTLE_SAVE", NULL,
      "write the save again when the battle ends, so its outcome is readable" },
    { "--use-item",       "PC_LAB_USE_ITEM",   "SLOT ITEM",
      "use a bag item on a party member once the field is up, then evolve what"
      " that made evolvable, a rare candy by level, a stone by item" },
    { "--use-at",         "PC_LAB_USE_AT",     "N",
      "the frame --use-item fires on" },
    { "--contest",        "PC_LAB_CONTEST",    "RANK TYPE SLOT",
      "enter one official Super Contest with a party member once the field is up" },
    { "--contest-at",     "PC_LAB_CONTEST_AT", "N",
      "the frame --contest starts on" },
    { "--underground",    "PC_LAB_UNDERGROUND", "enter|mine",
      "after the Explorer Kit arrives: skip the Roark intro, and optionally"
      " start the digging minigame" },
    { "--underground-at", "PC_LAB_UNDERGROUND_AT", "N",
      "the frame --underground fires on" },
    { "--sprite",         "PC_LAB_SPRITE",     "all|pin|IDS",
      "load species through the game's own sprite manager, digest each, exit."
      " all = 493 + forms; pin = Spinda's two PIDs plus the form species;"
      " a comma-separated list is those species only" },
    { "--sprite-at",      "PC_LAB_SPRITE_AT",  "N",
      "the frame --sprite starts on (default 2, heaps and the filesystem"
      " are up; no field is needed)" },
    { "--sprite-out",     "PC_LAB_SPRITE_OUT", "PATH",
      "write the per-sprite digest lines there as well as to stderr" },
    { "--sprite-dump",    "PC_LAB_SPRITE_DUMP", "DIR",
      "decode each loaded page to a PNG in DIR, the way to look at Spinda's spots" },
    { "--text",           "PC_LAB_TEXT",       "all|pin|IDS",
      "decode every message bank through the game's own printer, digest"
      " each, exit. all = every bank; pin = the debug-menu and sound-test"
      " banks plus the long / format-arg / trainer-name cases; a"
      " comma-separated list is those banks only" },
    { "--text-at",        "PC_LAB_TEXT_AT",    "N",
      "the frame --text starts on (default 2, fonts and the filesystem"
      " are up; no field is needed)" },
    { "--text-out",       "PC_LAB_TEXT_OUT",   "PATH",
      "write the per-bank digest lines there as well as to stderr" },
    { "--text-dump",      "PC_LAB_TEXT_DUMP",  "DIR",
      "decode each printed window to a PNG in DIR, how to look at the"
      " glyphs the printer actually wrote" },
    { "--audio",          "PC_LAB_AUDIO",      "all|pin|IDS",
      "play every sequence through the game's own player, digest the mix,"
      " exit. all = every named sseq; pin = title against silence plus one"
      " of each player; a comma-separated list is those sequence ids only" },
    { "--audio-at",       "PC_LAB_AUDIO_AT",   "N",
      "the frame --audio starts on (default 2, SoundSystem_Init has"
      " run; no field is needed)" },
    { "--audio-out",      "PC_LAB_AUDIO_OUT",  "PATH",
      "write the per-sequence digest lines there as well as to stderr" },
    { "--audio-dump",     "PC_LAB_AUDIO_DUMP", "DIR",
      "write each sequence's play window as a 32,728 Hz stereo WAV in DIR" },
    { "--trace-field",    "PC_TRACE_FIELD",    "N",
      "from frame N, one line per player move attempt: tile, direction, collision, tile behavior" },
    { "--snd-trace",      "PC_SND_TRACE",      NULL,
      "arriving sound command lists plus a register and tick heartbeat" },
    { "--spu-trace",      "PC_SPU_TRACE",      NULL,
      "SPU keyon trace, what restarts a channel and from where" },
    { "--spu-tap",        "PC_SPU_TAP",        "DIR",
      "write each SPU channel's own stream, for isolating one voice" },
    { "--mute-bgm",       "PC_MUTE_BGM",       NULL,
      "silence the music players and leave sound effects audible" },
    { "--tp-debug",       "PC_TP_DEBUG",       NULL,
      "touch-panel state, for when a pen press does not land" },
    { "--wide-debug",     "PC_WIDE_DEBUG",     NULL,
      "why the widescreen margins are black: the gate, and what the 3D engine"
      " drew out there" },
    { "--selftest",       "PC_SELFTEST",       NULL,
      "run every in-binary vector suite (RC4, MD5/SHA-1/HMAC, LZ) and exit" },
    { "--mi-selftest",    "PC_MI_SELFTEST",    NULL,
      "the LZ vectors alone, inside the boot where MI first initializes" },
    { "--state-digest",   "PC_STATE_DIGEST",   NULL,
      "hash every mapped guest region at start and at exit, signal included" },
    { "--dump-state",     "PC_DUMP_STATE",     "DIR",
      "write those regions' bytes there too, for when a digest moves and the"
      " question is what moved" },
    { "--watch",          "PC_WATCH",          "SPEC",
      "print a decomp symbol every frame: NAME, NAME:LEN, NAME+OFF:LEN, or 0xADDR:LEN"
      " (repeatable)", 1 },
    { "--diff-spans",     "PC_DIFF_SPANS",     "PATH",
      "write the guest memory map for the differential oracle, then exit" },
    { "--diff-trace",     "PC_DIFF_TRACE",     "PATH",
      "the same trace the oracle writes: picture, SPU and a digest per region" },
    { "--diff-every",     "PC_DIFF_EVERY",     "N",
      "checkpoint every N frames in that trace (default 1)" },
};

static const int PC_NOPTS = (int)(sizeof PC_OPTS / sizeof PC_OPTS[0]);

const struct pc_opt *pc_args_table(int *count)
{
    *count = PC_NOPTS;
    return PC_OPTS;
}

void pc_args_help(FILE *out)
{
    int i, w = 0, n;

    fprintf(out,
        "pokeplatinum, the decompilation, compiled and linked as a program.\n"
        "\n"
        "Usage: pokeplatinum [OPTION]... [PC_VAR=VALUE]...\n"
        "\n"
        "EVERY input to this port is on this page, and every one of them is an\n"
        "environment variable underneath: a flag is translated into its variable\n"
        "before the port reads anything, so the two spellings cannot disagree.\n"
        "Same inputs in, byte-identical run out.\n"
        "\n"
        "  pokeplatinum --view pplat &  build/pc/pcview pplat\n"
        "  pokeplatinum --input pc/replays/new-game.txt --frames 20000 --save none\n"
        "  pokeplatinum PC_ROM=game.nds PC_VIEW=pplat        # same thing, on Windows\n"
        "\n");

    for (i = 0; i < PC_NOPTS; i++) {
        n = (int)strlen(PC_OPTS[i].flag)
          + (PC_OPTS[i].arg ? (int)strlen(PC_OPTS[i].arg) + 1 : 0);
        if (n > w) w = n;
    }

    fprintf(out, "Options (the variable each one sets is named in brackets):\n");
    for (i = 0; i < PC_NOPTS; i++) {
        char spelled[64];

        if (PC_OPTS[i].arg != NULL) {
            snprintf(spelled, sizeof spelled, "%s %s",
                     PC_OPTS[i].flag, PC_OPTS[i].arg);
        } else {
            snprintf(spelled, sizeof spelled, "%s", PC_OPTS[i].flag);
        }
        fprintf(out, "  %-*s  %s\n", w + 1, spelled, PC_OPTS[i].help);
        fprintf(out, "  %-*s  [%s]\n", w + 1, "", PC_OPTS[i].var);
    }

    fprintf(out,
        "\n"
        "  --help                     this page\n"
        "\n"
        "A flag with no value is one the port only tests for presence; it sets \"1\".\n"
        "Keys are held at the level the input script sets them, so a script must\n"
        "release what it presses. pc/replays/new-game.txt is a worked example.\n");
}

/*
 * Returns 0 to continue booting, 1 to exit successfully (--help), and -1 on a
 * bad argument. Never partially applies a run: a rejected argument means
 * nothing was put into the environment, because a port that booted with half a
 * command line applied would be a port whose recorded inputs are a lie.
 */
int pc_args_apply(int argc, char **argv)
{
    int i, j, k;
    const char *prev;
    /* Two passes. The first validates and the second applies, so a typo in the
     * last argument does not leave the first three already in the environment:
     * The run either has the whole command line or has not started. */
    for (i = 1; i < argc; i++) {
        const char *a = argv[i];

        if (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0) {
            pc_args_help(stdout);
            return 1;
        }
        if (a[0] == '-' && a[1] == '-') {
            const char *eq = strchr(a, '=');
            size_t len = eq ? (size_t)(eq - a) : strlen(a);

            for (j = 0; j < PC_NOPTS; j++) {
                if (strlen(PC_OPTS[j].flag) == len
                    && strncmp(a, PC_OPTS[j].flag, len) == 0) {
                    break;
                }
            }
            if (j == PC_NOPTS) {
                fprintf(stderr, "pokeplatinum: unknown option %.*s\n"
                                "Try --help; every input is on that page.\n",
                        (int)len, a);
                return -1;
            }
            if (eq == NULL && PC_OPTS[j].arg != NULL) {
                if (i + 1 >= argc) {
                    fprintf(stderr, "pokeplatinum: %s needs a value (%s)\n",
                            PC_OPTS[j].flag, PC_OPTS[j].arg);
                    return -1;
                }
                i++;    /* consumed by the value */
            }
            continue;
        }
        if (strchr(a, '=') != NULL && a[0] != '=') {
            continue;   /* KEY=VALUE, applied below */
        }
        fprintf(stderr, "pokeplatinum: unrecognized argument %s\n"
                        "Arguments are flags (--help lists them) or KEY=VALUE.\n", a);
        return -1;
    }

    for (i = 1; i < argc; i++) {
        char *a = argv[i];

        if (a[0] == '-' && a[1] == '-') {
            char *eq = strchr(a, '=');
            size_t len = eq ? (size_t)(eq - a) : strlen(a);
            const char *value;
            char *buf;

            for (j = 0; j < PC_NOPTS; j++) {
                if (strlen(PC_OPTS[j].flag) == len
                    && strncmp(a, PC_OPTS[j].flag, len) == 0) {
                    break;
                }
            }
            if (eq != NULL) {
                value = eq + 1;
            } else if (PC_OPTS[j].arg != NULL) {
                value = argv[++i];
            } else {
                value = "1";
            }

            /* putenv keeps the storage, so this must outlive the call and
             * cannot be a local. Leaked deliberately and once per option: the
             * environment owns it for the life of the process. */
            /* An appending option joins onto whatever is already there, so
             * `--watch a --watch b` and PC_WATCH=a,b are the same input. */
            prev = PC_OPTS[j].append ? getenv(PC_OPTS[j].var) : NULL;
            k = (int)(strlen(PC_OPTS[j].var) + strlen(value) + 2
                      + (prev ? strlen(prev) + 1 : 0));
            buf = (char *)malloc((size_t)k);
            if (buf == NULL) {
                fprintf(stderr, "pokeplatinum: out of memory applying %s\n",
                        PC_OPTS[j].flag);
                return -1;
            }
            if (prev != NULL && *prev != '\0') {
                snprintf(buf, (size_t)k, "%s=%s,%s", PC_OPTS[j].var, prev, value);
            } else {
                snprintf(buf, (size_t)k, "%s=%s", PC_OPTS[j].var, value);
            }
            putenv(buf);
            continue;
        }
        putenv(a);      /* KEY=VALUE: argv storage is fine for putenv */
    }

    return 0;
}
