/*
 * Forced into the firmware's Download Play client (pc/Makefile.wasm
 * PTFW_CFLAGS, before each source's own includes): the address where the
 * client receives the child's ROM header.
 *
 * The firmware is NITRO code, and NitroSDK 4.2's MB_ROM_HEADER_ADDRESS is
 * its HW_ROM_HEADER_BUF, 0x027FFE00, the address a parent puts in the header
 * segment of its download information (B/W's parent does). This build's
 * memory map is the TWL-SDK's, where HW_ROM_HEADER_BUF is 0x02FFFE00. On the
 * DS both are the same byte of the 4 MB main RAM, which repeats up to
 * 0x03000000. The host has no such repetition, so the client keeps the NITRO
 * address and pc_pt_dlplay.c copies the received header to the child's
 * HW_ROM_HEADER_BUF before boot. Every other address in the client's
 * protocol is below 4 MB and the same in both maps.
 */
#ifndef PC_PT_FW_MB_H
#define PC_PT_FW_MB_H

#include "mb_child.h"

#undef MB_ROM_HEADER_ADDRESS
#define MB_ROM_HEADER_ADDRESS (HW_MAIN_MEM + 0x007ffe00) /* NITRO HW_ROM_HEADER_BUF */

#endif
