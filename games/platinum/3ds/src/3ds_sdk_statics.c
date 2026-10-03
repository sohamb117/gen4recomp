/*
 * 3ds/src/3ds_sdk_statics.c: the two SDK statics the memory shadow un-made.
 *
 * A game translation unit, like 3ds_ioreg.c and 3ds_hwmem.c: it sees the DS
 * SDK and never libctru.
 *
 * On a DS, and on the PC port which identity-maps guest memory,
 * `HW_ROM_HEADER_BUF` and `&reg_G2_BG0CNT` are addresses the preprocessor
 * knows, so a file-scope initialiser can use them. Here they are
 * `armrec_shared_base + n` and `armrec_io_base + n`, where the bases are
 * pointers into a slab malloc'd at start-up. That is not a constant
 * expression, and two SDK files stop compiling because of it:
 *
 *   card_rom.c            u32 cardi_rom_header_addr = HW_ROM_HEADER_BUF;
 *   g2di_BGManipulator.c  REGType16v * const NNSiG2dBGCNTTable[] = { &reg... };
 *
 * 3ds/patches drops the two initialisers; this file supplies the values at run
 * time. Both are read-mostly tables set once and never changed, so a fill
 * straight after armrec_mem_init() is what the linker did on hardware, one
 * moment later.
 *
 * These are the only two in the whole tree, which is a claim the build makes:
 * any third would fail to compile with "initializer element is not constant"
 * and be counted as a skip.
 *
 * The table is written from here and not from its own file. NNSiG2dBGCNTTable
 * keeps the `const` its header declares, so filling it is a write through a
 * cast. Doing that in the defining file would let the compiler see a const
 * object written and read in one unit. The patch also moves the object into an
 * explicit .data section, because an uninitialised const array lands in
 * .rodata and the fill would fault on hardware.
 */

#include <nitro.h>
#include <nnsys.h>

#include "3ds_sdk_statics.h"

/*
 * Both are declared WEAK, and that is what lets one file serve two binaries.
 * `make -f 3ds/Makefile` links the port's own objects and no game code at all,
 * so neither symbol exists there; a strong reference would refuse to link and
 * this file would have to be kept out of that build, which is how a file stops
 * being exercised. Weak, the port binary sees them at address zero, publish
 * does nothing and the check reports nothing run, while the game link gets
 * the real objects and the real nine checks.
 */

/* card_rom.c's, with its initialiser removed by 3ds/patches. */
extern u32 cardi_rom_header_addr __attribute__((weak));

/* g2di_BGManipulator.c's, declared here without the const the header gives it
 * so the fill needs no cast at the point of assignment. Same object. */
extern REGType16v *NNSiG2dBGCNTTable[] __attribute__((weak));

int sdk_statics_present(void)
{
    return &cardi_rom_header_addr != NULL && NNSiG2dBGCNTTable != NULL;
}

void sdk_statics_publish(void)
{
    if (!sdk_statics_present()) {
        return;
    }

    cardi_rom_header_addr = HW_ROM_HEADER_BUF;

    NNSiG2dBGCNTTable[0] = &reg_G2_BG0CNT;
    NNSiG2dBGCNTTable[1] = &reg_G2_BG1CNT;
    NNSiG2dBGCNTTable[2] = &reg_G2_BG2CNT;
    NNSiG2dBGCNTTable[3] = &reg_G2_BG3CNT;
    NNSiG2dBGCNTTable[4] = &reg_G2S_DB_BG0CNT;
    NNSiG2dBGCNTTable[5] = &reg_G2S_DB_BG1CNT;
    NNSiG2dBGCNTTable[6] = &reg_G2S_DB_BG2CNT;
    NNSiG2dBGCNTTable[7] = &reg_G2S_DB_BG3CNT;
}

int sdk_statics_check(void)
{
    int bad = 0;
    int i;

    if (!sdk_statics_present()) {
        return 0;
    }
    if (cardi_rom_header_addr != (u32)HW_ROM_HEADER_BUF) {
        bad++;
    }
    for (i = 0; i < 8; i++) {
        if (NNSiG2dBGCNTTable[i] == NULL) {
            bad++;
        }
    }
    return bad;
}
