#!/usr/bin/env python3
"""
gbabuild: build a GBA decomp (pokeemerald, pokeruby) as a nativeplat wasm32
guest.

    gbabuild.py emerald|ruby|sapphire [-j N] [--only TU...]

Inputs: the decomp tree with its matching ROM build (.elf beside it; the
graphics and sound it built are what INCBIN sizes come from), the pinned
wasi-sdk, and games/gba-common/pc (the GBA machine) plus games/<game>/pc.
Output: games/emerald/build/pc-wasm/pokeemerald.wasm, games/ruby/build/pc-wasm/
poke{ruby,sapphire}.wasm (intermediates in a directory of the module's name
beside it; gitignored like the DS games' builds).

Per decomp TU: clang -E, the decomp's own preproc (charmap strings, INCBIN),
srcfix.py (agbcc struct layout, asm), clang -S -emit-llvm without
optimisation passes, gbabridge.py (cartridge addresses, dispatch, volatile
I/O), then clang -O2 -c. The port's TUs take the same path with no FILE
group. gen_dispatch.py then builds the address table, and wasm-ld links the
module with the layout core/include/np_guest_abi.h fixes.

The game's data is never compiled into the module: every global with an
ELF address is dropped by the bridge and read from the player's ROM at run
time. check_module.py-style scans are left to `--check-rom ROM`, which looks
for long runs of ROM bytes in the module's data.
"""
import argparse
import concurrent.futures as cf
import glob
import json
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
COMMON = os.path.dirname(HERE)
NPROOT = os.path.abspath(os.path.join(COMMON, "..", ".."))
WASI = os.path.join(NPROOT, ".cache", "toolchains", "wasi-sdk")
CLANG = os.path.join(WASI, "bin", "clang")
TARGET = "--target=wasm32-wasip1"
ABI_H = os.path.join(NPROOT, "core", "include", "np_guest_abi.h")

# pokeruby's Makefile: US English, revision 0, no debug menu
RS_DEFS = ["-DREVISION=0", "-DENGLISH", "-DDEBUG=0", "-DDEBUG_FIX=0"]

GAMES = {
    "emerald": dict(decomp=".cache/gba/pokeemerald", elf="pokeemerald.elf", defs=[],
                    port="games/emerald/pc", wasm="pokeemerald"),
    "ruby": dict(decomp=".cache/gba/pokeruby", elf="pokeruby.elf", defs=["-DRUBY"] + RS_DEFS,
                 port="games/ruby/pc", wasm="pokeruby"),
    "sapphire": dict(decomp=".cache/gba/pokeruby", elf="pokesapphire.elf", defs=["-DSAPPHIRE"] + RS_DEFS,
                     port="games/ruby/pc", wasm="pokesapphire"),
}

# The decomp TUs the port replaces (pc/src has their API) or cannot run.
EXCLUDE_COMMON = {
    "agb_flash.c", "agb_flash_1m.c", "agb_flash_le.c", "agb_flash_mx.c",  # gba_flash.c
    "siirtc.c",                                                           # gba_rtc.c
    "libc.c",                                                             # wasi-libc
    "libisagbprn.c",                                                      # debug print (SWI 0xFF)
    "librfu_intr.c",                                                      # naked asm, wireless adapter
    "multiboot.c",                                                        # naked asm, link cable boot
}

CFLAGS_GAME = ["-O2", "-funsigned-char", "-fno-short-enums", "-fwrapv", "-fno-strict-aliasing",
               "-fno-builtin-memcpy", "-w", "-ffunction-sections", "-fdata-sections"]
CPPFLAGS_GAME = ["-DMODERN=0", "-DUBFIX", "-DNONMATCHING", "-DNP_GBA_PORT=1", "-Wno-trigraphs"]


def run(cmd, **kw):
    r = subprocess.run(cmd, capture_output=True, text=True, **kw)
    if r.returncode != 0:
        raise RuntimeError(" ".join(cmd[:3]) + " ...\n" + r.stderr[-4000:])
    return r


class Build:
    def __init__(self, game, jobs):
        self.game = game
        self.cfg = GAMES[game]
        self.decomp = os.path.join(NPROOT, self.cfg["decomp"])
        # games/<game>/build/pc-wasm/<module>/, beside the DS games' modules
        self.out = os.path.join(NPROOT, os.path.dirname(self.cfg["port"]), "build", "pc-wasm", self.cfg["wasm"])
        self.tmp = os.path.join(self.out, "tmp")
        self.obj = os.path.join(self.out, "obj")
        os.makedirs(self.tmp, exist_ok=True)
        os.makedirs(self.obj, exist_ok=True)
        self.jobs = jobs
        self.syms = os.path.join(self.out, "syms.json")
        self.preproc = os.path.join(NPROOT, "build", "rse", "tools", "preproc-" + os.path.basename(self.decomp))
        self.inc = ["-iquote", os.path.join(self.decomp, "include"), "-iquote", self.decomp]
        self.port_inc = ["-I", os.path.join(COMMON, "pc", "include"), "-I", os.path.join(NPROOT, "core", "include")]

    def tools(self):
        if not os.path.exists(self.preproc):
            os.makedirs(os.path.dirname(self.preproc), exist_ok=True)
            run(["c++", "-std=c++17", "-O2", "-w"] + glob.glob(os.path.join(self.decomp, "tools", "preproc", "*.cpp"))
                + ["-o", self.preproc])
        elf = os.path.join(self.decomp, self.cfg["elf"])
        if not os.path.exists(self.syms) or os.path.getmtime(self.syms) < max(
                os.path.getmtime(elf), os.path.getmtime(os.path.join(HERE, "elfsyms.py"))):
            run([sys.executable, os.path.join(HERE, "elfsyms.py"), elf, self.syms])

    def game_tus(self):
        srcs = []
        for pat in ("src/*.c", "src/*/*.c", "src/*/*/*.c"):
            srcs += glob.glob(os.path.join(self.decomp, pat))
        srcs = [s for s in sorted(srcs) if ".inc.c" not in s and os.path.basename(s) not in EXCLUDE_COMMON]
        return srcs

    def port_tus(self):
        srcs = sorted(glob.glob(os.path.join(COMMON, "pc", "src", "*.c")))
        srcs += sorted(glob.glob(os.path.join(NPROOT, self.cfg["port"], "src", "*.c")))
        return srcs

    def compile_tu(self, src, kind):
        base = os.path.splitext(os.path.relpath(src, NPROOT if kind == "port" else self.decomp))[0].replace("/", "_")
        o = os.path.join(self.obj, base + ".o")
        sigs = os.path.join(self.obj, base + ".sigs")
        deps = [src, self.syms, os.path.join(HERE, "gbabridge.py"), os.path.join(HERE, "srcfix.py")]
        # <port>/patches/<file>.patch: a fix to a decomp TU the port cannot
        # build as is (pokeruby's asm-only functions), applied to a copy
        patch = os.path.join(NPROOT, self.cfg["port"], "patches", os.path.basename(src) + ".patch")
        if kind != "game" or not os.path.exists(patch):
            patch = None
        else:
            deps.append(patch)
        if os.path.exists(o) and os.path.exists(sigs) and all(os.path.getmtime(o) >= os.path.getmtime(d) for d in deps) \
                and not self.dirty_headers(o, kind):
            return o, sigs, None
        i1 = os.path.join(self.tmp, base + ".i")
        i2 = os.path.join(self.tmp, base + ".pp.i")
        i3 = os.path.join(self.tmp, base + ".fix.i")
        ll = os.path.join(self.tmp, base + ".ll")
        bl = os.path.join(self.tmp, base + ".b.ll")
        pc = os.path.join(self.tmp, base + ".patched.c")
        try:
            flags = [TARGET] + CPPFLAGS_GAME + self.cfg["defs"] + self.inc + \
                ["-include", os.path.join(COMMON, "pc", "include", "gba_prelude.h")]
            if kind == "port":
                flags = [TARGET] + CPPFLAGS_GAME + self.cfg["defs"] + self.port_inc + self.inc + \
                        ["-I", os.path.join(NPROOT, self.cfg["port"], "include")]
            cpp_src = src
            if patch:
                run(["patch", "-s", "-o", pc, src, patch])
                cpp_src = pc
                # a patch may read host options (np_guest_abi.h, gba_option)
                flags = flags + ["-iquote", os.path.dirname(src), "-idirafter", os.path.join(NPROOT, "core", "include")]
            std = ["-std=gnu89"] if kind == "game" else ["-std=gnu11"]
            run([CLANG, "-E", "-funsigned-char"] + std + [ "-MD", "-MF", o + ".d"] + flags + [cpp_src, "-o", i1], cwd=self.decomp)
            if kind == "game":
                with open(i2, "w") as f:
                    assets = ["-g", "build/assets"] if os.path.isdir(os.path.join(self.decomp, "build", "assets")) else []
                    r = subprocess.run([self.preproc] + assets + [i1, "charmap.txt"], stdout=f, stderr=subprocess.PIPE,
                                       text=True, cwd=self.decomp)
                if r.returncode:
                    raise RuntimeError("preproc " + src + "\n" + r.stderr[-2000:])
            else:
                os.replace(i1, i2)
            run([sys.executable, os.path.join(HERE, "srcfix.py"), i2, i3])
            std = ["-std=gnu89"] if kind == "game" else ["-std=gnu11"]
            run([CLANG, TARGET, "-x", "cpp-output", "-S", "-emit-llvm", "-Xclang", "-disable-llvm-passes"]
                + std + CFLAGS_GAME + [i3, "-o", ll])
            objname = os.path.basename(base) + ".o" if kind == "game" else ""
            if kind == "game":
                objname = os.path.splitext(os.path.basename(src))[0] + ".o"
            run([sys.executable, os.path.join(HERE, "gbabridge.py"), self.syms, objname, ll, bl, sigs])
            run([CLANG, TARGET, "-c", "-O2", "-w", bl, "-o", o])
            return o, sigs, None
        except RuntimeError as e:
            return None, None, str(e)
        finally:
            for t in (i1, i2, i3, ll, bl, pc):
                if os.path.exists(t) and not os.environ.get("GBA_KEEP"):
                    os.remove(t)

    def dirty_headers(self, o, kind):
        d = o + ".d"
        if not os.path.exists(d):
            return True
        t = os.path.getmtime(o)
        txt = open(d).read().replace("\\\n", " ")
        for h in txt.split(":", 1)[1].split():
            if os.path.exists(h) and os.path.getmtime(h) > t:
                return True
        return False

    def build(self, only=None):
        self.tools()
        tus = [(s, "game") for s in self.game_tus()] + [(s, "port") for s in self.port_tus()]
        if only:
            tus = [t for t in tus if any(os.path.basename(t[0]).startswith(x) for x in only)]
        objs, sigs, errs = [], [], []
        with cf.ThreadPoolExecutor(self.jobs) as ex:
            futs = {ex.submit(self.compile_tu, s, k): s for s, k in tus}
            for f in cf.as_completed(futs):
                o, sg, err = f.result()
                if err:
                    errs.append((futs[f], err))
                    print("FAIL", os.path.relpath(futs[f], NPROOT), flush=True)
                else:
                    objs.append(o)
                    sigs.append(sg)
        errfile = os.path.join(self.out, "errors.txt")
        if os.path.exists(errfile):
            os.remove(errfile)
        if errs:
            with open(errfile, "w") as f:
                for s, e in errs:
                    f.write(f"==== {s}\n{e}\n")
            print(f"{len(errs)} TU(s) failed; see {self.out}/errors.txt")
            if only is None:
                return 1
        if only:
            return 0
        return self.link(sorted(objs), sorted(sigs))

    def link(self, objs, sigs):
        disp_c = os.path.join(self.out, "gba_dispatch_table.c")
        run([sys.executable, os.path.join(HERE, "gen_dispatch.py"), disp_c, os.path.join(self.out, "bridge_report.txt")] + sigs)
        disp_o = os.path.join(self.obj, "gba_dispatch_table.o")
        run([CLANG, TARGET, "-O2", "-c", disp_c, "-o", disp_o])

        def abi(name):
            m = re.search(r"#define %s (0x[0-9A-Fa-f]+)u" % name, open(ABI_H).read())
            return int(m.group(1), 16)
        wasm = os.path.join(os.path.dirname(self.out), self.cfg["wasm"] + ".wasm")
        rsp = os.path.join(self.out, "link.rsp")
        with open(rsp, "w") as f:
            f.write("\n".join(objs + [disp_o]))
        run([CLANG, TARGET, "@" + rsp, "-o", wasm, "-lm",
             f"-Wl,--global-base={abi('NP_GUEST_C_BASE')}", "-Wl,--no-stack-first",
             f"-Wl,--initial-memory={abi('NP_GUEST_MEMORY_BYTES')}",
             f"-Wl,--max-memory={abi('NP_GUEST_MEMORY_BYTES')}",
             "-Wl,-z,stack-size=1048576", "-Wl,--gc-sections",
             "-Wl,--export=np_fiber_entry", "-Wl,-Map=" + os.path.join(self.out, self.cfg["wasm"] + ".map")])
        print("built", os.path.relpath(wasm, NPROOT), os.path.getsize(wasm))
        return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("game", choices=sorted(GAMES))
    ap.add_argument("-j", type=int, default=4)
    ap.add_argument("--only", nargs="*")
    a = ap.parse_args()
    sys.exit(Build(a.game, a.j).build(a.only))


if __name__ == "__main__":
    main()
