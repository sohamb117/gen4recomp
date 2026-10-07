"""
The NDS cartridge image: header, ARM9 static module, its autoload segments,
the ARM9 overlay table and overlay files, and the ARM7 binary.

Everything is read from the ROM alone. The layouts are the public ones
(GBATEK "DS Cartridge Header", "DS Cartridge NitroROM and NitroARC File
Systems") plus the NitroSDK structures crt0 itself reads at run time:

  _start_ModuleParams  nine words crt0 loads with `ldr r1, =_start_ModuleParams`:
                       autoload list, list end, autoload source start, static
                       BSS start, BSS end, compressed static end (0 when the
                       static is stored plain), SDK version, and the two
                       "nitrocode" markers 0xDEC00621 / 0x2106C0DE. The
                       markers are how the table is found without a symbol.
  autoload list        (ram_address, size, bss_size) per segment on NitroSDK
                       2-4; TWL-SDK 5 adds a fourth word, the segment's
                       static-initialiser table, so entries are 16 bytes:
                       (ram_address, size, sinit, bss_size). The segments'
                       bytes follow each other from the autoload source start.
  TWL header           unit code 2 (DSi-enhanced) or 3 (DSi-only) adds the
                       ARM9i/ARM7i modules at 0x1C0 (rom offset, -, ram, size
                       each); a DS loads neither, so NTR mode ignores them.
  overlay table entry  id, ram_address, ram_size, bss_size, sinit_init,
                       sinit_init_end, file_id, compressed size | flags << 24.
  BLZ                  the backward LZ the static and overlays are compressed
                       with (MIi_UncompressBackward): the last 8 bytes are
                       (header_len << 24 | compressed_len) and the growth.
"""

import hashlib
import struct

NITROCODE_BE = 0xDEC00621
NITROCODE_LE = 0x2106C0DE


def u32(b, o):
    return struct.unpack_from("<I", b, o)[0]


def blz_decompress(data):
    """MIi_UncompressBackward over a whole buffer; returns the plain bytes."""
    if len(data) < 8:
        return bytes(data)
    w0, growth = struct.unpack_from("<II", data, len(data) - 8)
    if w0 == 0:
        return bytes(data)
    hdr_len = w0 >> 24
    comp_len = w0 & 0xFFFFFF
    out = bytearray(data) + bytearray(growth)
    src = len(data) - hdr_len
    dst = len(out)
    stop = len(data) - comp_len
    while src > stop:
        src -= 1
        flags = out[src]
        for _ in range(8):
            if src <= stop:
                break
            if flags & 0x80:
                src -= 1
                hi = out[src]
                src -= 1
                lo = out[src]
                disp = (((hi & 0x0F) << 8) | lo) + 2
                n = (hi >> 4) + 3
                for _ in range(n):
                    dst -= 1
                    out[dst] = out[dst + disp + 1]
            else:
                src -= 1
                dst -= 1
                out[dst] = out[src]
            flags = (flags << 1) & 0xFF
    return bytes(out)


class Segment(object):
    """A run of bytes the loader puts at a fixed guest address."""

    def __init__(self, name, ram, data, bss=0):
        self.name = name
        self.ram = ram
        self.data = data
        self.bss = bss

    @property
    def end(self):
        return self.ram + len(self.data)

    def __repr__(self):
        return "Segment(%s, 0x%08X, 0x%X, bss=0x%X)" % (
            self.name, self.ram, len(self.data), self.bss)


class Overlay(object):
    def __init__(self, ovid, ram, ram_size, bss, sinit, sinit_end, file_id,
                 flags, data):
        self.id = ovid
        self.ram = ram
        self.ram_size = ram_size
        self.bss = bss
        self.sinit = sinit
        self.sinit_end = sinit_end
        self.file_id = file_id
        self.flags = flags
        self.data = data

    @property
    def end(self):
        return self.ram + len(self.data)


class Rom(object):
    def __init__(self, path):
        with open(path, "rb") as fh:
            self.raw = fh.read()
        r = self.raw
        self.path = path
        self.sha1 = hashlib.sha1(r).hexdigest()
        self.title = r[0:12].rstrip(b"\0").decode("ascii", "replace")
        self.gamecode = r[12:16].decode("ascii", "replace")
        self.unitcode = r[0x12]
        (self.arm9_off, self.arm9_entry, self.arm9_ram,
         self.arm9_size) = struct.unpack_from("<IIII", r, 0x20)
        (self.arm7_off, self.arm7_entry, self.arm7_ram,
         self.arm7_size) = struct.unpack_from("<IIII", r, 0x30)
        self.fnt_off, self.fnt_size, self.fat_off, self.fat_size = \
            struct.unpack_from("<IIII", r, 0x40)
        self.ovt9_off, self.ovt9_size, self.ovt7_off, self.ovt7_size = \
            struct.unpack_from("<IIII", r, 0x50)
        self.twl = self.unitcode in (2, 3)
        self.arm9i = self.arm7i = None
        if self.twl:
            o9, _, ram9, n9 = struct.unpack_from("<IIII", r, 0x1C0)
            o7, _, ram7, n7 = struct.unpack_from("<IIII", r, 0x1D0)
            self.arm9i = (o9, ram9, n9)
            self.arm7i = (o7, ram7, n7)
        self.arm9_autoload_cb, self.arm7_autoload_cb = \
            struct.unpack_from("<II", r, 0x70)
        self._load_arm9()
        self.arm7 = r[self.arm7_off:self.arm7_off + self.arm7_size]
        self.overlays = self._load_overlays()

    # ------------------------------------------------------------ files
    def file(self, fid):
        start, end = struct.unpack_from("<II", self.raw, self.fat_off + 8 * fid)
        return self.raw[start:end]

    # ------------------------------------------------------------ ARM9
    def _find_module_params(self, img):
        """Offset of _start_ModuleParams in the static image, by its markers."""
        pat = struct.pack("<II", NITROCODE_BE, NITROCODE_LE)
        i = img.find(pat)
        while i >= 0:
            off = i - 0x1C
            if off >= 0 and off % 4 == 0:
                return off
            i = img.find(pat, i + 1)
        raise ValueError("no _start_ModuleParams (nitrocode markers) in ARM9")

    def _load_arm9(self):
        r = self.raw
        img = bytearray(r[self.arm9_off:self.arm9_off + self.arm9_size])
        mp = self._find_module_params(img)
        self.module_params_addr = self.arm9_ram + mp
        (al_list, al_end, al_start, bss_start, bss_end, comp_end, sdk_ver,
         _be, _le) = struct.unpack_from("<9I", img, mp)
        self.sdk_version = sdk_ver
        self.compressed = comp_end != 0
        if self.compressed:
            n = comp_end - self.arm9_ram
            plain = blz_decompress(bytes(img[:n]))
            img = bytearray(plain) + img[n:]
            # crt0 zeroes the end pointer once it has decompressed, so a
            # second pass is a no-op; mirror that in the image.
            struct.pack_into("<I", img, mp + 20, 0)
        self.arm9 = bytes(img)
        self.autoload_list = al_list
        self.autoload_list_end = al_end
        self.autoload_start = al_start
        self.bss_start = bss_start
        self.bss_end = bss_end
        # The static proper ends where the autoload bytes start; what follows
        # is the autoload segments and then the list itself.
        static_len = al_start - self.arm9_ram
        self.static = Segment("arm9", self.arm9_ram, self.arm9[:static_len],
                              max(0, bss_end - (self.arm9_ram + static_len)))
        self.autoloads = []
        src = static_len
        # NitroSDK's entries are 12 bytes; TWL-SDK 5's are 16 (a sinit word
        # before the bss size). The SDK version word says which.
        twl_sdk = (self.sdk_version >> 24) >= 5
        ent = 16 if twl_sdk else 12
        n = (al_end - al_list) // ent
        for i in range(n):
            o = al_list - self.arm9_ram + i * ent
            if twl_sdk:
                ram, size, _sinit, bss = struct.unpack_from("<IIII", self.arm9, o)
            else:
                ram, size, bss = struct.unpack_from("<III", self.arm9, o)
            self.autoloads.append(Segment("autoload%d" % i, ram,
                                          self.arm9[src:src + size], bss))
            src += size

    # ------------------------------------------------------------ overlays
    def _load_overlays(self):
        ovs = []
        for i in range(self.ovt9_size // 32):
            (ovid, ram, ram_size, bss, sinit, sinit_end, fid,
             cflags) = struct.unpack_from("<8I", self.raw, self.ovt9_off + 32 * i)
            data = self.file(fid)
            flags = cflags >> 24
            if flags & 1:
                data = blz_decompress(data[:cflags & 0xFFFFFF])
            ovs.append(Overlay(ovid, ram, ram_size, bss, sinit, sinit_end,
                               fid, flags, data))
        return ovs

    # ------------------------------------------------------------ views
    def code_segments(self):
        """(module, Segment) for every ARM9 code-bearing segment."""
        out = [("arm9", self.static)]
        for s in self.autoloads:
            if s.data:
                out.append(("arm9", s))
        for ov in self.overlays:
            out.append(("ov%03d" % ov.id,
                        Segment("ov%03d" % ov.id, ov.ram, ov.data, ov.bss)))
        return out

    def overlay(self, ovid):
        for ov in self.overlays:
            if ov.id == ovid:
                return ov
        return None

    def module_segment(self, module):
        """A lookup over the segments one module occupies."""
        if module == "arm9":
            segs = [self.static] + [s for s in self.autoloads if s.data]
        elif module == "arm7":
            segs = [Segment("arm7", self.arm7_ram, self.arm7)]
        else:
            ov = self.overlay(int(module[2:]))
            segs = [Segment(module, ov.ram, ov.data, ov.bss)]
        return ModuleView(module, segs)

    def modules(self):
        return ["arm9"] + ["ov%03d" % ov.id for ov in self.overlays]

    def summary(self):
        lines = ["%s  %s  sha1 %s" % (self.title, self.gamecode, self.sha1),
                 "arm9  rom 0x%X ram 0x%08X entry 0x%08X size 0x%X%s" % (
                     self.arm9_off, self.arm9_ram, self.arm9_entry,
                     self.arm9_size, "  (BLZ)" if self.compressed else ""),
                 "      module params 0x%08X  sdk 0x%08X" % (
                     self.module_params_addr, self.sdk_version),
                 "      static 0x%08X-0x%08X  bss 0x%08X-0x%08X" % (
                     self.static.ram, self.static.end, self.bss_start,
                     self.bss_end)]
        for s in self.autoloads:
            lines.append("      %s 0x%08X size 0x%X bss 0x%X" % (
                s.name, s.ram, len(s.data), s.bss))
        lines.append("arm7  rom 0x%X ram 0x%08X entry 0x%08X size 0x%X" % (
            self.arm7_off, self.arm7_ram, self.arm7_entry, self.arm7_size))
        lines.append("overlays %d" % len(self.overlays))
        for ov in self.overlays:
            lines.append("  ov%03d ram 0x%08X size 0x%06X bss 0x%05X sinit "
                         "0x%08X-0x%08X file %d%s" % (
                             ov.id, ov.ram, len(ov.data), ov.bss, ov.sinit,
                             ov.sinit_end, ov.file_id,
                             " (BLZ)" if ov.flags & 1 else ""))
        if self.twl:
            lines.append("TWL unit %d: arm9i rom 0x%X ram 0x%08X size 0x%X; "
                         "arm7i rom 0x%X ram 0x%08X size 0x%X (not loaded "
                         "in NTR mode)" % ((self.unitcode,) + self.arm9i + self.arm7i))
        return "\n".join(lines)


class ModuleView(object):
    """The segments of one module, addressed by guest address."""

    def __init__(self, module, segs):
        self.module = module
        self.segs = segs

    def lookup(self, addr):
        """(bytes, base) of the segment holding addr, or (None, None)."""
        for s in self.segs:
            if s.ram <= addr < s.end:
                return s.data, s.ram
        return None, None

    def contains(self, addr):
        return self.lookup(addr)[0] is not None
