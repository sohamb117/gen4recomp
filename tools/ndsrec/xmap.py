"""
The mwldarm link map (`arm9.elf.xMAP`) as ground truth for ndsrec.

Only the measurement and the signature learner read this; the front end
itself never does. Each section listing line is

    ADDRESS SIZE SECTION NAME (OBJECT)

and the listing carries the ELF mapping symbols `$a` (ARM code), `$t`
(Thumb code), `$d` (data) and `$b` (a Thumb BL), so every symbol can be told
apart as code or data and its instruction set read off without the sources.
"""
import re

LINE = re.compile(r"^  ([0-9A-F]{8}) ([0-9A-F]{8}) (\S+)\s+(\S+)\s+\((.*)\)\s*$")
MAPPING = ("$a", "$t", "$d", "$b")
SECTION_NAMES = (".text", ".rodata", ".data", ".bss", ".sinit", ".itcm",
                 ".dtcm", ".version", ".exception", ".init", ".ctor")


def module_of(section):
    """The ndsrec module a link-map output section belongs to."""
    s = section.split(".")[1] if section.startswith(".") else section
    if s in ("arm9", "ITCM", "DTCM"):
        return "arm9"
    m = re.match(r"^OVERLAY_(\d+)$", s)
    if m:
        return "ov%03d" % int(m.group(1))
    return None


class XMap(object):
    def __init__(self, path):
        self.rows = []          # (module, section, addr, size, kind, name, obj)
        self.lcf = {}
        sec = None
        for line in open(path, errors="replace"):
            line = line.rstrip("\n")
            if line.startswith("# ."):
                sec = line[2:].strip()
                continue
            if line.startswith("#>"):
                p = line[2:].split()
                if len(p) >= 2:
                    try:
                        self.lcf[p[1]] = int(p[0], 16)
                    except ValueError:
                        pass
                continue
            m = LINE.match(line)
            if m and sec and not sec.endswith(".bss"):
                mod = module_of(sec)
                if mod is None:
                    continue
                self.rows.append((mod, sec, int(m.group(1), 16), int(m.group(2), 16),
                                  m.group(3), m.group(4), m.group(5).strip()))
            elif m and sec and sec.endswith(".bss"):
                mod = module_of(sec[:-4])
                self.rows.append((mod, sec, int(m.group(1), 16), int(m.group(2), 16),
                                  m.group(3), m.group(4), m.group(5).strip()))

    def name_addrs(self):
        out = {}
        for mod, sec, addr, size, kind, name, obj in self.rows:
            if name in MAPPING or name in SECTION_NAMES:
                continue
            out.setdefault(name, addr)
        return out

    def symbols(self):
        """(module, addr, size, kind, name, obj, state) for every named
        symbol; state is 'arm', 'thumb' or 'data' from the mapping symbols."""
        by_obj = {}
        for r in self.rows:
            by_obj.setdefault((r[0], r[6]), []).append(r)
        out = []
        for (mod, obj), rs in by_obj.items():
            maps = sorted((r[2], r[5]) for r in rs if r[5] in ("$a", "$t", "$d"))
            at = {}
            for a, n in maps:
                at.setdefault(a, set()).add(n)
            starts = [a for a, _ in maps]
            import bisect
            for r in rs:
                if r[5] in MAPPING or r[5] in SECTION_NAMES:
                    continue
                if r[4] not in (".text", ".itcm"):
                    out.append((mod, r[2], r[3], r[4], r[5], obj, "data"))
                    continue
                here = at.get(r[2], set())
                if "$a" in here:
                    st = "arm"
                elif "$t" in here:
                    st = "thumb"
                else:
                    i = bisect.bisect_right(starts, r[2]) - 1
                    st = "data"
                    while i >= 0:
                        n = maps[i][1]
                        st = {"$a": "arm", "$t": "thumb", "$d": "data"}[n]
                        break
                out.append((mod, r[2], r[3], r[4], r[5], obj, st))
        return out

    def functions(self):
        """{module: {addr: (name, thumb, size)}} for every code symbol."""
        out = {}
        for mod, addr, size, kind, name, obj, st in self.symbols():
            if st == "data":
                continue
            out.setdefault(mod, {})
            if addr not in out[mod]:
                out[mod][addr] = (name, st == "thumb", size)
        return out
