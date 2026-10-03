/*
 * 3ds/src/3ds_vram.h: nine banks, five windows, no mmap.
 *
 * 0x06000000 to 0x07000000 is 16 MB of addresses over 0xA4000 of memory. Nine
 * banks of 16 KB to 128 KB appear in five windows, main BG, sub BG, main OBJ,
 * sub OBJ and LCDC, wherever the nine VRAMCNT registers put them, each window
 * mirroring its contents to fill its slot.
 *
 * The PC port's answer cannot be copied. There the windows are aliasing mmaps
 * of one shared backing object, because a guest pointer is a host pointer,
 * decompiled C reaches VRAM through a plain `u16 *`, and there is no accessor
 * to put a bank lookup behind. This console cannot map anything at 0x06000000
 * at all, and a slab cannot alias one page into five windows. So the lookup
 * that PC does in the page tables happens here instead, in
 * armrec_host_ptr()'s VRAM case, and the 16 MB of address space is never
 * allocated: the backing is the nine banks and nothing else.
 *
 * What that costs is a window scan and a table lookup per access rather than a
 * page-table hit. What it does not cost is correctness: a store through the BG
 * window is visible through LCDC, because both are offsets into the same bank
 * store.
 *
 * The placement model is armrec's, copied rather than reinvented. The tables
 * were derived from `pcdiff-melon --vram-selftest`: 2,304 VRAMCNT
 * configurations read back at every 16 KB address on a console, not from a
 * document. 3ds/tests/vram_pin.py compares the copies and fails if they drift.
 *
 * No libctru here, so the host runs the whole model and its self-test.
 */

#ifndef POKEPLATINUM_3DS_VRAM_H
#define POKEPLATINUM_3DS_VRAM_H

#include <stdint.h>

/* ARM_VRAM_BANKS, ARM_VRAM_BLK and ARM_VRAM_STORE in
 * tools/armrec/armrec_rt.h; 3ds/tests/vram_banks.c static-asserts the pair. */
#define VRAM_BANKS       9
#define VRAM_BLK         0x4000u   /* 16 KB, every placement's granularity */
#define VRAM_STORE_BYTES 0xA4000u  /* the nine banks, and the whole backing */

/*
 * Point the model at the slab's VRAM row and clear the map. guest_bind() must
 * have happened; this reads the row through guest_region_base(). Safe twice.
 */
void vram_init(void);

/*
 * "VRAMCNT may have changed." Re-reads the nine registers out of the I/O row
 * and rebuilds the map if they differ from what it was built from, nine byte
 * loads and a compare when they do not. Everything that writes a VRAMCNT
 * register has to call this *after* the store; on PC that is a store hook in
 * recompiled code and -finstrument-functions on the three SDK files that write
 * them in C, and the hardware model decides how this console hears about it.
 */
void vram_touch(void);

/* Host pointer for a guest VRAM address, or NULL when no bank is mapped there,
 * which reads as zeros on hardware and is what armrec_host_ptr() answers
 * with. This is the function 3ds_guest.c reaches weakly. */
void *vram_host_ptr(uint32_t guest);

/* Which bank, if any, guest address `a` currently reads, and at what offset
 * into it. 0 when nothing is mapped there. */
int vram_lookup(uint32_t a, int *bank, uint32_t *off);

/* A bank's storage, its size, and where the LCDC window puts it. NULL / 0
 * before vram_init() or for a bank outside 0..8. */
void    *vram_bank_ptr(int bank);
uint32_t vram_bank_size(int bank);
uint32_t vram_bank_lcdc(int bank);

/* The guest address of bank `b`'s VRAMCNT register. Not nine consecutive
 * bytes: WRAMCNT sits at 0x04000247, between G and H. */
uint32_t vram_cnt_addr(int bank);

/*
 * The four roles no guest address reaches. A bank at mst 3 (and e, f, g at
 * mst 4/5) is read by the 2D or 3D engine directly and appears in none of the
 * five windows, so vram_lookup() answers 0 for every address and a renderer
 * can only ask by slot.
 *
 * NULL means "no bank is mapped there", and a caller must read that as all
 * zeros rather than as "do not draw": an unmapped read gives 0 on hardware.
 *
 * vram_extpal() returns the 8 KB extended-palette slot `slot` of one of the
 * four palette regions, slots 0-3 for the two BG regions, slot 0 only for
 * the two OBJ ones. vram_texture() returns one of the four 128 KB texture
 * slots and vram_texpal() one of the eight 16 KB texture-palette slots, of
 * which 6 and 7 have no bank that can reach them at all.
 * vram_bank_in_lcdc() answers DISPCNT's VRAM display mode, which scans a bank
 * out directly and shows black if it is not in LCDC.
 */
enum { VRAM_EXTPAL_ABG, VRAM_EXTPAL_BBG, VRAM_EXTPAL_AOBJ, VRAM_EXTPAL_BOBJ };
void *vram_extpal(int which, int slot);
void *vram_texture(int slot);
void *vram_texpal(int slot);
int   vram_bank_in_lcdc(int bank);

/*
 * The five windows, for the one caller that has to report them as regions:
 * armrec_region_at() appends them to its table the way armrec_rt.c does. The
 * size given is the *content* a window can address, 512 KB of banks in the
 * main BG window's 2 MB of addresses, so a walker hashes each bank once
 * instead of once per mirror. Returns 0 for an index out of range, and fills
 * whichever out-parameters are non-NULL.
 *
 * No name here. The reported spelling is armrec's ("VRAM main BG"), and the
 * short names in this file's placement table are compared against armrec_rt.c
 * byte for byte, so it cannot be the one that carries it.
 */
int vram_window_count(void);
int vram_window_at(int w, uint32_t *base, uint32_t *size);

/* How many times vram_touch() has rebuilt the map, and how many times two
 * banks were found in one window block or claiming one palette or texture
 * slot. The second is a fault: hardware ORs reads there and writes to both,
 * and one pointer cannot express that. The SDK makes it unreachable,
 * GX_SetBankFor* clears a bank out of its old role first, so a non-zero
 * count means the game did something this port has never seen. */
unsigned long vram_remaps(void);
unsigned long vram_overlaps(void);

/*
 * The plan's own check and the ones around it: put a bank in LCDC, write a
 * marker, move it to the main BG window and read the marker back at the new
 * address. Also the window mirrors, F and G's two-block quirk, an unmapped
 * address answering NULL, and the store still being 0xA4000. Returns failures,
 * fills `*ran`. Writes VRAMCNT and the banks; leaves both zeroed.
 */
int vram_selftest(int *ran);

#endif /* POKEPLATINUM_3DS_VRAM_H */
