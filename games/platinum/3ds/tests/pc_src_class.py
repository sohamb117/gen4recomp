#!/usr/bin/env python3
"""
3ds/tests/pc_src_class.py: every file in pc/src, classified, and checked.

The PC port's host layer is 45 C files and 8 headers, and this console takes
most of them unchanged. Which ones it does not take was, until now, whatever
the build happened to do: three files compiled *as failures* and left a
.skipped marker beside a compiler error, two more compiled to an object with
no symbols in it, and the rest came in wholesale. None of that stated an
intention, so none of it could be wrong.

The table below states it. Every file in pc/src is exactly one of:

    port     this port's own code, compiled and linked here as it is on PC.
    rewrite  a 3ds/src file supplies the same symbols; the pc/src file is
             not compiled here at all.
    drop     never linked here. `where` says when it left, or which task
             takes it out, a file still in the link today is still in the
             link until that task runs.

What the checks are for:

  - a new pc/src file arrives unclassified. The PC port adds host files
    regularly and the 3DS build globs the directory, so an unclassified file
    is linked into this console by default. That is the wrong default: it is
    how a POSIX dependency arrives without anybody deciding to take it.
  - a `port` file stops compiling. It shows up here as a skipped object
    rather than as a smaller link.
  - a `rewrite` file loses its 3ds/src answer, or the Makefile and this
    table disagree about which files those are.
  - the archive's table goes stale against this one.

    3ds/tests/pc_src_class.py          verify, print the summary
    3ds/tests/pc_src_class.py --list   one line per file
    3ds/tests/pc_src_class.py --md     the table, as the archive holds it
"""

import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SRC = os.path.join(ROOT, "pc", "src")
OBJ = os.path.join(ROOT, "build", "3ds", "obj", "pc")
MAKEFILE = os.path.join(ROOT, "3ds", "Makefile")

# file, class, where, why.
#
# `where` is the 3ds/src file that answers a rewrite (or the task that will),
# and for a drop it is "now" if the Makefile already excludes it, otherwise
# the task that takes it out.
TABLE = [
    ("pc_args.c", "port", "", "the port's whole configuration surface. 3dslink and hbmenu both pass argv, and KEY=VALUE is already how the Windows build is driven"),
    ("pc_arm7snd.c", "port", "", "the ARM7 sound driver's host. Its six unresolved arm7_SND_* names are 8.4's, not this file's"),
    ("pc_audio.c", "port", "", "the sample sink 8.4 points at NDSP"),
    ("pc_audio_lab.c", "drop", "now", "a lab: sweeps every sequence on a build machine and writes WAVs to a directory. PC_LAB_AUDIO cannot be set on a console, and 3ds/src/3ds_hooks.c answers its frame hook"),
    ("pc_bench.c", "port", "", "per-subsystem frame time. Inert without --bench and clock_gettime is in newlib"),
    ("pc_bgm_mute.c", "port", "", "PC_MUTE_BGM, 164 bytes of guest-side sound calls"),
    ("pc_boot_glue.c", "port", "", "one empty crt hook the SDK's autoload branches through"),
    ("pc_card_rom.c", "port", "", "6.2 finished it: the seek+read pair works on newlib, and RomFS is behind the same open()"),
    ("pc_crypto_rc4.c", "port", "", "RC4, replacing hand-written ARM assembly the tree does not build"),
    ("pc_dgt.c", "port", "", "the digest library, which ships prebuilt and has no C to compile"),
    ("pc_diff.c", "drop", "now", "the port's half of the melonDS differential runner. It needs an oracle trace and a build machine to compare against; OS_Halt's call lands in 3ds_hooks.c"),
    ("pc_diff.h", "drop", "now", "its interface, with it"),
    ("pc_div0.c", "rewrite", "3ds/src/3ds_div0.c", "the PC fix-up reads an x86 fault frame and steps over a `div`. ARM has neither: 5.6 answers it three instructions earlier, in the linker's --wrap"),
    ("pc_div0.h", "rewrite", "3ds/src/3ds_div0.h", "same"),
    ("pc_dma.c", "port", "", "DMA as a memory operation, over the IO window"),
    ("pc_dwc_auth.c", "port", "", "the wifi-account surface boot touches, as an unconfigured console"),
    ("pc_fs_overlay.c", "port", "", "7.8's file. Overlays never leave memory on either host"),
    ("pc_guest_window.c", "rewrite", "3ds/src/3ds_window.c", "the PC file casts a guest address to a host pointer, which is true only where the two are the same number"),
    ("pc_gx.c", "port", "", "the GX FIFO writers that ship as mwcc assembly"),
    ("pc_gx_g3_util.c", "port", "", "the five SDK helpers that cache the geometry command port"),
    ("pc_input.c", "rewrite", "3ds/src/3ds_input.c", "same cast, and 7.5 is where the console's own buttons reach the two keypad words"),
    ("pc_lab.c", "drop", "now", "the save lab: mints a save by driving the game on a build machine. 9.2 copies a PC-made save instead. It was the only caller of pc_input_hold_idle anywhere, so dropping it took a wall with it"),
    ("pc_main.c", "rewrite", "3ds/src/3ds_main.c", "mmap, sigaction, /proc. The 3DS entry point is a different file, not a #ifdef in this one"),
    ("pc_mi.c", "port", "", "MI memory, the SDK's asm-in-C surface"),
    ("pc_mtx.c", "port", "", "the fx matrix routines, same reason"),
    ("pc_modfs.c", "port", "", "the runtime content-package overlay. It compiles here as it stands, opendir/readdir are in newlib and a console with no mods directory simply finds none, and its own header says the 3DS builds it"),
    ("pc_os_context.c", "rewrite", "7.10", "getcontext/makecontext on POSIX, fibers on Windows, and devkitARM has neither. Excluded now so the missing backend is a named task rather than a compiler error"),
    ("pc_os_lite.c", "port", "", "the plan expected a rewrite for OS_Halt's host half; there is nothing to rewrite. Everything OS_Halt does is guest work, and the one host step is pc_video_frame_end's weak pc_view_publish hook, which 3ds/src overrides"),
    ("pc_pm.c", "port", "", "the power-management IC as a register file (7.7)"),
    ("pc_png.h", "port", "", "the PNG encoder pc_video.c dumps through"),
    ("pc_polydump.c", "port", "", "the polygon list as text. Inert without --dump-polys"),
    ("pc_probe2d.c", "port", "", "the 2D state a frame was drawn from. Inert without PC_PROBE_2D"),
    ("pc_pxi.c", "port", "", "the ARM9<->ARM7 FIFO, modeled without an ARM7 (7.7)"),
    ("pc_rtc.c", "port", "", "the real-time clock behind PXI tag 5 (7.7)"),
    ("pc_scrcov.c", "port", "", "field-script coverage. Inert without PC_SCRIPT_COV"),
    ("pc_selftest.c", "port", "", "the in-binary vector suites. Nothing reaches it here, pc_main.c was its only caller, and wiring it into 3ds_main.c is worth more than dropping it"),
    ("pc_selftest.h", "port", "", "its interface"),
    ("pc_snd.c", "port", "", "the sound-system surface that answers before an SPU exists (7.7)"),
    ("pc_snd_bios.c", "port", "", "the ARM7 BIOS sound tables, which are data"),
    ("pc_sprite_lab.c", "drop", "now", "a lab: every species through pokemon_sprite.c, digested to a build directory"),
    ("pc_state.c", "rewrite", "3ds/src/3ds_state.c", "the digest walks guest memory by cast on PC and through the translator here"),
    ("pc_state.h", "port", "", "the interface both digests answer"),
    ("pc_sym.c", "rewrite", "3ds/src/3ds_sym.c", "it reads the running binary's own .symtab out of /proc/self/exe, and a 3DSX has neither. The rewrite answers the one caller that needs symbols here from a table written into the link"),
    ("pc_sym.h", "port", "", "its interface"),
    ("pc_text_lab.c", "drop", "now", "a lab: every message bank through the printer, to a build directory"),
    ("pc_tp.c", "port", "", "the touch panel with no pen on it. 7.6 gives it the console's"),
    ("pc_trap.c", "port", "", "17 lines: print and abort"),
    ("pc_video.c", "port", "", "the two surfaces and the dumps. 8.2 is where 3ds_view consumes them"),
    ("pc_view.c", "rewrite", "3ds/src/3ds_view.c", "/dev/shm, a POSIX viewer and its protocol. The console has two LCDs instead"),
    ("pc_win_fiber.c", "drop", "now", "another operating system's file: one #if defined(_WIN32) with an empty tail, so it compiled here to an object with no symbols"),
    ("pc_win_ipc.c", "drop", "now", "the same"),
    ("pc_wvr.c", "port", "", "the WVR radio behind PXI tag 15 (7.7)"),
    ("pc_args.h", "port", "", "with pc_args.c"),
    ("pc_bench.h", "port", "", "with pc_bench.c"),
]


def makefile_lists():
    """PC_REPLACED and PC_DROPPED as the build reads them."""
    text = open(MAKEFILE, errors="replace").read()
    out = {}
    for name in ("PC_REPLACED", "PC_DROPPED"):
        m = re.search(r"^%s\s*:?=\s*((?:[^\n\\]*\\\n)*[^\n]*)" % name, text, re.M)
        out[name] = set(m.group(1).replace("\\\n", " ").split()) if m else set()
    return out


def check():
    errs = []
    rows = {f: (k, w, why) for f, k, w, why in TABLE}
    if len(rows) != len(TABLE):
        errs.append("a file has two rows")

    on_disk = set(f for f in os.listdir(SRC) if f.endswith((".c", ".h")))
    for f in sorted(on_disk - set(rows)):
        errs.append("%s is in pc/src and has no row: classify it before the "
                    "build links it by default" % f)
    for f in sorted(set(rows) - on_disk):
        errs.append("%s has a row and is not in pc/src any more" % f)

    mk = makefile_lists()
    want_replaced = set(f for f, (k, w, _) in rows.items()
                        if k == "rewrite" and f.endswith(".c"))
    want_dropped = set(f for f, (k, w, _) in rows.items()
                       if k == "drop" and w == "now" and f.endswith(".c"))
    if mk["PC_REPLACED"] != want_replaced:
        errs.append("3ds/Makefile PC_REPLACED %s, table says %s"
                    % (sorted(mk["PC_REPLACED"]), sorted(want_replaced)))
    if mk["PC_DROPPED"] != want_dropped:
        errs.append("3ds/Makefile PC_DROPPED %s, table says %s"
                    % (sorted(mk["PC_DROPPED"]), sorted(want_dropped)))

    for f, (k, w, _) in sorted(rows.items()):
        if k == "rewrite" and w.startswith("3ds/") \
           and not os.path.exists(os.path.join(ROOT, w)):
            errs.append("%s is rewritten in %s, which does not exist" % (f, w))

    # The object tree, when there is one: a `port` file that stopped compiling
    # is a smaller link and no error anywhere else.
    built = os.path.isdir(OBJ)
    for f, (k, w, _) in sorted(rows.items()):
        if not built or not f.endswith(".c"):
            continue
        obj = os.path.join(OBJ, f[:-2] + ".o")
        linked = k == "port" or (k == "drop" and w != "now")
        if linked and not os.path.exists(obj):
            errs.append("%s is linked here and has no object%s"
                        % (f, " (it skipped)" if os.path.exists(obj + ".skipped") else ""))
        if not linked and os.path.exists(obj):
            errs.append("%s is not linked here and has an object" % f)

    return errs, rows


def main():
    errs, rows = check()
    if "--md" in sys.argv:
        print("| file | class | where | why |")
        print("|---|---|---|---|")
        for f, k, w, why in sorted(TABLE):
            print("| `%s` | %s | %s | %s |" % (f, k, w or "--", why))
    elif "--list" in sys.argv:
        for f, k, w, why in sorted(TABLE):
            print("%-20s %-8s %-24s %s" % (f, k, w or "--", why))

    if errs:
        for e in errs:
            print("pc_src_class: " + e, file=sys.stderr)
        return 1

    n = lambda k: sum(1 for f, (kk, w, _) in rows.items() if kk == k)
    later = sorted(set(w for f, (k, w, _) in rows.items()
                       if k == "drop" and w != "now"))
    print("pc_src_class: %d file(s) in pc/src, %d port, %d rewritten in "
          "3ds/src, %d dropped (%d still linked, %s)"
          % (len(rows), n("port"), n("rewrite"), n("drop"),
             sum(1 for f, (k, w, _) in rows.items() if k == "drop" and w != "now"),
             " ".join(later) or "none pending"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
