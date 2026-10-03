#!/usr/bin/env python3
"""pc/wasm/check_module.py: is the linked module what the runtime expects?

    check_module.py MODULE.wasm ABI_HEADER STUBS_LIST [--map MAP] [--quiet]

Reads the module itself (not the build's intentions) and fails on:

  * imports: anything but module "np_host" (and then only a name the ABI
    header declares with NP_IMPORT) or "wasi_snapshot_preview1";
  * exports: _start, memory, np_fiber_entry missing;
  * layout: __stack_pointer's initial value, or (with --map) __heap_base or
    the start of data, below NP_GUEST_C_BASE (the DS map lives below it);
  * signature mismatches: wasm-ld turns a call whose type disagrees with the
    callee's definition into a trapping thunk named "signature_mismatch:<sym>"
    (C89 implicit declarations are the usual source; i386 cdecl tolerates
    them, wasm does not). A thunk for a name in pc/stubs.list is expected,
    those stubs trap by design and take no arguments; any other is a call the
    ELF build makes and this module would trap on, and fails the check.

Prints the import list either way.
"""

import re
import sys

WASI = "wasi_snapshot_preview1"


def leb(b, i):
    r = s = 0
    while True:
        x = b[i]
        i += 1
        r |= (x & 0x7F) << s
        s += 7
        if not x & 0x80:
            return r, i


def sleb(b, i):
    r = s = 0
    while True:
        x = b[i]
        i += 1
        r |= (x & 0x7F) << s
        s += 7
        if not x & 0x80:
            if x & 0x40:
                r -= 1 << s
            return r, i


def name(b, i):
    n, i = leb(b, i)
    return b[i:i + n].decode(), i + n


def sections(b):
    if b[:4] != b"\0asm":
        sys.exit("check_module: not a wasm module")
    i = 8
    while i < len(b):
        sid = b[i]
        size, j = leb(b, i + 1)
        yield sid, j, j + size
        i = j + size


def parse(path):
    b = open(path, "rb").read()
    imports, exports, globals_init, fnames, gnames = [], [], [], {}, {}
    nfunc_imports = 0
    for sid, s, e in sections(b):
        if sid == 2:
            n, i = leb(b, s)
            for _ in range(n):
                mod, i = name(b, i)
                fld, i = name(b, i)
                kind = b[i]
                i += 1
                if kind == 0:
                    _, i = leb(b, i)
                    nfunc_imports += 1
                elif kind == 1:
                    i += 1
                    fl, i = leb(b, i)
                    _, i = leb(b, i)
                    if fl & 1:
                        _, i = leb(b, i)
                elif kind == 2:
                    fl, i = leb(b, i)
                    _, i = leb(b, i)
                    if fl & 1:
                        _, i = leb(b, i)
                elif kind == 3:
                    i += 2
                else:
                    sys.exit(f"check_module: import kind {kind} not understood")
                imports.append((mod, fld, kind))
        elif sid == 6:
            n, i = leb(b, s)
            for _ in range(n):
                i += 2                      # valtype, mutability
                op = b[i]
                i += 1
                if op == 0x41:              # i32.const
                    v, i = sleb(b, i)
                    globals_init.append(v & 0xFFFFFFFF)
                else:
                    globals_init.append(None)
                    while b[i] != 0x0B:     # skip to end
                        i += 1
                i += 1                      # end
        elif sid == 7:
            n, i = leb(b, s)
            for _ in range(n):
                fld, i = name(b, i)
                kind = b[i]
                idx, i = leb(b, i + 1)
                exports.append((fld, kind, idx))
        elif sid == 0:
            cname, i = name(b, s)
            if cname != "name":
                continue
            while i < e:
                sub = b[i]
                size, j = leb(b, i + 1)
                if sub in (1, 7):
                    n, k = leb(b, j)
                    table = fnames if sub == 1 else gnames
                    for _ in range(n):
                        idx, k = leb(b, k)
                        nm, k = name(b, k)
                        table[idx] = nm
                i = j + size
    return imports, exports, globals_init, fnames, gnames


def main(argv):
    args = [a for a in argv[1:] if not a.startswith("--")]
    mapfile = None
    if "--map" in argv:
        mapfile = argv[argv.index("--map") + 1]
        args.remove(mapfile)
    if len(args) != 3:
        sys.exit(__doc__)
    module, header, stubs = args
    hdr = open(header).read()
    contract = set(re.findall(r"NP_IMPORT\((\w+)\)", hdr))
    cbase = int(re.search(r"#define NP_GUEST_C_BASE (0x[0-9A-Fa-f]+)u", hdr).group(1), 16)
    stub_names = set()
    for line in open(stubs):
        line = line.split("#", 1)[0].strip()
        if line:
            stub_names.add(line.split()[0])

    imports, exports, ginit, fnames, gnames = parse(module)
    bad = []

    print("imports:")
    for mod, fld, kind in imports:
        k = "func" if kind == 0 else ("table", "memory", "global", "tag")[kind - 1]
        print(f"  {mod}.{fld} ({k})")
        if kind != 0:
            bad.append(f"non-function import {mod}.{fld}")
        elif mod == "np_host":
            if fld not in contract:
                bad.append(f"np_host.{fld} is not in {header}")
        elif mod != WASI:
            bad.append(f"import from module '{mod}' ({mod}.{fld})")

    names = {e[0] for e in exports}
    for want in ("_start", "memory", "np_fiber_entry"):
        if want not in names:
            bad.append(f"export '{want}' missing")
    print("exports: " + ", ".join(sorted(names)))

    if mapfile:
        # wasm-ld -Map: output data segments are the top-level rows
        # "Addr Off Size .name" (no input-file column). __heap_base is not
        # listed; with --no-stack-first it is the stack's top, i.e.
        # __stack_pointer's initial value checked above.
        segs = re.findall(r"^\s+([0-9a-f]+)\s+[0-9a-f]+\s+([0-9a-f]+) (\.\w+)\s*$",
                          open(mapfile).read(), re.M)
        if not segs:
            bad.append(f"no output data segments found in {mapfile}")
        for addr, size, seg in segs:
            a, n = int(addr, 16), int(size, 16)
            print(f"segment {seg}: {a:#010x}..{a + n:#010x}")
            if a < cbase:
                bad.append(f"{seg} at {a:#x} below NP_GUEST_C_BASE {cbase:#x}")
    sp = [i for i, n in gnames.items() if n == "__stack_pointer"]
    if not sp:
        bad.append("no global named __stack_pointer (name section stripped?)")
    else:
        v = ginit[sp[0]]
        print(f"__stack_pointer initial: {v:#010x}")
        if v is None or v < cbase:
            bad.append(f"__stack_pointer starts below NP_GUEST_C_BASE {cbase:#x}")

    thunks = sorted(n.split(":", 1)[1] for n in fnames.values()
                    if n.startswith("signature_mismatch:"))
    unexpected = [t for t in thunks if t not in stub_names]
    print(f"signature-mismatch thunks: {len(thunks)} "
          f"({len(thunks) - len(unexpected)} generated trap stubs)")
    for t in unexpected:
        bad.append(f"call to {t} has the wrong signature and would trap "
                   "(an implicit declaration?)")

    if bad:
        for m in bad:
            print("check_module: " + m, file=sys.stderr)
        return 1
    print("check_module: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
