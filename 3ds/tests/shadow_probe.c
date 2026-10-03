/*
 * 3ds/tests/shadow_probe.c: one game translation unit, compiled by the real
 * build with the real GAME_CFLAGS, whose entire purpose is to include SDK
 * names and state what the shadow chain has to deliver.
 *
 * Why a dummy tu and not a comment. The game link compiles a thousand game files.
 * If the include order, the ABI flags or a shadow is wrong, that is a thousand
 * confusing errors, or worse, no errors and a silently different struct
 * layout. This file is the same compile in miniature, and it is a prerequisite
 * of `all`, so it fails on the run that broke it rather than in the game link.
 *
 * It is compiled and never linked. There is no game to link it to yet, and it
 * defines nothing that would be worth linking.
 *
 * It must not include <3ds.h>. A TU seeing both the DS SDK
 * and libctru is the thing this port must not have, the two disagree about
 * enum width. $(CTRULIB)/include is off the GAME chain for that reason and
 * this file must not smuggle it back in.
 *
 * Later shadows are added here: the ioreg shadows, the
 * arena and HW_* constants, the VRAM window bases. Each one arrives with
 * the assertion that says what the shadow is for.
 *
 * What this file cannot check. Whether a name resolved to the shadow or to
 * the SDK's own copy is not visible from inside the preprocessor: both files
 * use the guard NITRO_TYPES_H_ and both give u32 a 32-bit type. That question
 * is answered by measurement instead, 3ds/tests/shadow_chain.sh reads the
 * compiler's own -H include trace. Here we assert the properties; there we
 * assert which file supplied them.
 */

#include <nitro/types.h>
#include <nitro/hw/ARM9/ioreg.h>
#include <nitro/hw/common/mmap_shared.h>
#include <nitro/hw/ARM9/mmap_vram.h>

/*
 * The register base moved to the slab, so the guest address the DS
 * answers at needs a name of its own. Every hook that classifies by guest
 * address recovers it as `addr - HW_REG_BASE + HW_REG_BASE_GUEST`, and these
 * two say the offsets that arithmetic rests on are still what the SDK wrote.
 */
_Static_assert(HW_REG_BASE_GUEST == 0x04000000u, "the DS answers I/O at 0x04000000");
_Static_assert(REG_KEYINPUT_OFFSET == 0x130, "the keypad is 0x130 into the I/O page");
_Static_assert(REG_DB_DISPCNT_OFFSET == 0x1000, "engine B is 0x1000 into the I/O page");

/*
 * The memory map moved onto the slab, and the captured DS values are
 * what every hook that still speaks guest will use. These say the capture
 * happened: an unshadowed build would leave HW_ROM_HEADER_BUF a constant and
 * HWi_G_ROM_HEADER_BUF undefined, and this file would not compile.
 */
_Static_assert(HWi_G_MAIN_MEM == 0x02000000, "main RAM is at 0x02000000 on a DS");
_Static_assert(HWi_G_ROM_HEADER_BUF == 0x027FFE00, "the ROM header buffer is at 0x027FFE00");
_Static_assert(HWi_G_BUTTON_XY_BUF == 0x027FFFA8, "the X/Y word is at 0x027FFFA8");

/*
 * And the ones that did not move. A VRAM window base cannot be a host
 * pointer, nine banks in five windows, wherever VRAMCNT put them this frame,
 * so these stay the addresses the DS gives them and are translated per
 * access. Asserted here as well as on the console because it is the kind of
 * thing a later task would "finish" by accident.
 */
_Static_assert(HW_BG_VRAM == 0x06000000, "the main BG window is still a DS address");
_Static_assert(HW_LCDC_VRAM == 0x06800000, "the LCDC window is still a DS address");
_Static_assert(HW_LCDC_VRAM_I == 0x068A0000, "bank I's LCDC address is still a DS address");

/*
 * The widths the ROM assumes. On the DS these come from `unsigned long` being
 * 32 bits; the shadow pins them to stdint instead so the answer does not
 * depend on the host ABI being remembered. Both are 32-bit on ARM, so this is
 * not the interesting assertion; it is the one that fails loudly if someone
 * puts a 64-bit host on the game chain.
 */
_Static_assert(sizeof(u8) == 1, "u8 is one byte");
_Static_assert(sizeof(u16) == 2, "u16 is two bytes");
_Static_assert(sizeof(u32) == 4, "u32 is four bytes");
_Static_assert(sizeof(u64) == 8, "u64 is eight bytes");
_Static_assert(sizeof(s32) == 4, "s32 is four bytes");
_Static_assert(sizeof(f32) == 4, "f32 is four bytes");

/*
 * BOOL is `int`, and the SDK returns it from functions whose callers compare
 * against TRUE. Four bytes on both compilers; asserted because a two-byte BOOL
 * would be an ABI break that produces no diagnostic at all.
 */
_Static_assert(sizeof(BOOL) == 4, "BOOL is int");

/*
 * -fno-short-enums, which is half of the measured ABI answer. ARM GCC
 * defaults to shortening an enum to the smallest type that holds it; mwcc for
 * the DS does not (nitro/types.h says `#pragma enumsalwaysint on` for its own
 * compiler). Every struct in the game with an enum field depends on this, and
 * without the flag the sizes disagree with no warning anywhere.
 */
enum shadow_probe_small_enum { SHADOW_PROBE_ZERO = 0, SHADOW_PROBE_ONE = 1 };
_Static_assert(sizeof(enum shadow_probe_small_enum) == 4, "enums are int-sized");

/*
 * And the other half: mwcc for the DS aligns an 8-byte type to 4, ARM
 * AAPCS aligns it to 8, and a struct with a u64 in it lays out differently on
 * the two. 93 of the 251 measured disagreements were this alone. The
 * aligned(4) is on the typedef in 3ds/include/nitro/types.h; asserted here
 * because GCC's own documentation says the attribute can only *increase*
 * alignment, and this is the measurement that says it does not.
 * __alignof__ rather than _Alignof because the game chain is gnu99.
 */
_Static_assert(__alignof__(u64) == 4, "u64 aligns to 4, the way the DS laid it out");
_Static_assert(__alignof__(s64) == 4, "s64 aligns to 4");
_Static_assert(sizeof(u64) == 8, "and it is still eight bytes");

/*
 * What that is for, in the shape it actually matters: a struct whose second
 * member is 64-bit. The DS puts it at offset 4 and stock AAPCS at 8, and
 * nothing warns about the difference.
 */
struct shadow_probe_u64_layout { u32 a; u64 b; };
_Static_assert(sizeof(struct shadow_probe_u64_layout) == 12, "u32 then u64 is 12 bytes");

/*
 * Something with external linkage, so the object is not empty and `nm` on it
 * means something. Its type is the point: a struct that a later task can grow
 * to hold whatever the shadows are supposed to produce.
 */
const u32 shadow_probe_ok = 1;
