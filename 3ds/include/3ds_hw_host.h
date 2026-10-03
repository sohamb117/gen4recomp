/*
 * 3ds/include/3ds_hw_host.h: the DS memory map, moved onto the slab.
 *
 * What forced the shape of this file. The SDK writes the shared work area as
 * an offset from the start of an 8 MB main memory: HW_ROM_HEADER_BUF is
 * `HW_MAIN_MEM + 0x007ffe00`, and every one of the sixty-one constants around
 * it is written the same way. On hardware that is one region and the
 * arithmetic is honest. In this port's guest map it is two: main RAM is 4 MB
 * at 0x02000000 and the shared work area is 128 KB at 0x027E0000, with 4 MB of
 * nothing between them that a DS faults on. The slab does not carry that hole,
 * because 4 MB is 6% of this console's whole budget for address space nothing
 * may touch, so no single value of HW_MAIN_MEM can make both HW_MAIN_MEM and
 * HW_MAIN_MEM + 0x7ffe00 land in the right place.
 *
 * So the constants are moved per row: HW_MAIN_MEM becomes the host pointer to
 * the main-RAM row, and the sixty-one shared-work constants are redefined
 * against the host pointer to the shared row.
 *
 * Host, not guest, and here is why. The alternative was to leave these as
 * guest addresses and translate at each dereference, which does not survive
 * contact with the arena: the arena bounds become OS_AllocFromMainArenaLo,
 * which the game calls to get pointers it then dereferences directly, a
 * thousand times, in code that must not change.
 *
 * What that costs, stated plainly. An arena allocation now has a host address,
 * so anything that must survive a 26- or 27-bit hardware field cannot come
 * from an arena. That is what the port window exists for, and why the sound
 * path allocates from it. Guest values are still available: every constant
 * this file moves keeps its DS address as `HWi_G_<name>`.
 *
 * The list is derived, not typed. Sixty-one names maintained by hand is a list
 * that will one day be missing one, silently, because a constant left with the
 * SDK's value still compiles and still points somewhere.
 * 3ds/tests/mmap_pin.py derives the set from the SDK headers, emits the block
 * below, and fails the gate if the two disagree.
 *
 * It includes all three maps because the capture has to happen when every
 * constant still says what a DS says, and the three headers are mutually
 * recursive through macros. Each of the three shadows includes this file after
 * its own #include_next, so whichever the translation unit reaches first the
 * capture still runs exactly once.
 *
 * Left alone, deliberately:
 *
 *   HW_MAIN_MEM_DEBUGGER is HW_MAIN_MEM + 0x700000, 7 MB into a 4 MB row. It
 *     is the MAINEX arena's high bound and that arena is disabled. On PC it is
 *     equally unmapped, so the two ports fail the same way if it is enabled.
 *   ITCM, WRAM and the AGB slot have rows in the map but no constant here:
 *     their bases are link questions and the cartridge probe.
 *   The VRAM windows, and this one is not a deferral. 16 MB of addresses over
 *     0xA4000 of memory, placed by nine registers, is not something a base plus
 *     an offset can express: two 16 KB blocks a page apart in a window can be
 *     in different banks, far apart in the store, or in no bank at all. So
 *     every VRAM constant keeps the address the DS gives it and is translated
 *     per access.
 *
 *     The LCDC window is the tempting exception and it is refused. It happens
 *     to be a flat contiguous view of the whole store, so a single host base
 *     would work for its arithmetic. It would also make a write to a bank that
 *     is not in LCDC mode land in that bank's storage, which hardware drops
 *     and the PC port faults on. One model for all five windows is worth more
 *     than one saved lookup.
 *   The ARM7 copies of these headers are not shadowed: the sound tree compiles
 *     with its own include chain and its own register header.
 */

#ifndef POKEPLATINUM_3DS_HW_HOST_H
#define POKEPLATINUM_3DS_HW_HOST_H

#include <nitro/hw/ARM9/mmap_global.h>
#include <nitro/hw/ARM9/mmap_main.h>
#include <nitro/hw/common/mmap_shared.h>

#ifndef SDK_ASM

/*
 * Host pointers to the rows this file resolves through. 3ds/src/3ds_guest.c
 * sets them in guest_bind() and clears them on unbind, the same way it sets
 * armrec_io_base, so a constant used before the slab exists is a null
 * dereference with an obvious cause rather than a write into whatever is at
 * 0x027FFE00.
 */
extern unsigned char *armrec_main_base;
extern unsigned char *armrec_shared_base;
extern unsigned char *armrec_palette_base;
extern unsigned char *armrec_oam_base;

/*
 * The rows' guest bases, written here as literals and pinned by measurement
 * rather than by reading: 3ds/src/3ds_hwmem.c checks every constant below
 * against armrec_host_ptr() of its captured guest value, on the console, so a
 * wrong base fails on the first frame.
 *
 * The palette and OAM are flat 4 KB rows and both engines live in one of each:
 * The sub engine's half is 0x400 and 0x200 in. They are the easy case, and
 * the reason they are worth moving at all is that decompiled C fills them
 * through MI_CpuFill16 with no accessor to put a translation behind.
 */
#define HWi_SHARED_HOST(g)  ((u32)armrec_shared_base  + ((u32)(g) - 0x027E0000u))
#define HWi_PALETTE_HOST(g) ((u32)armrec_palette_base + ((u32)(g) - 0x05000000u))
#define HWi_OAM_HOST(g)     ((u32)armrec_oam_base     + ((u32)(g) - 0x07000000u))

/* --- generated by 3ds/tests/mmap_pin.py --emit; do not edit by hand --- */

/* Guest values, captured while the SDK's constants still say what a DS
 * says. This has to happen before anything is redefined: the shared
 * work area is written as an offset from HW_MAIN_MEM, which moves. */
enum { HWi_G_RED_RESERVED = (int)(HW_RED_RESERVED) };
enum { HWi_G_RED_RESERVED_END = (int)(HW_RED_RESERVED_END) };
enum { HWi_G_CARD_ROM_HEADER = (int)(HW_CARD_ROM_HEADER) };
enum { HWi_G_DOWNLOAD_PARAMETER = (int)(HW_DOWNLOAD_PARAMETER) };
enum { HWi_G_MAIN_MEM_SYSTEM = (int)(HW_MAIN_MEM_SYSTEM) };
enum { HWi_G_BOOT_CHECK_INFO_BUF = (int)(HW_BOOT_CHECK_INFO_BUF) };
enum { HWi_G_BOOT_CHECK_INFO_BUF_END = (int)(HW_BOOT_CHECK_INFO_BUF_END) };
enum { HWi_G_RESET_PARAMETER_BUF = (int)(HW_RESET_PARAMETER_BUF) };
enum { HWi_G_ROM_BASE_OFFSET_BUF = (int)(HW_ROM_BASE_OFFSET_BUF) };
enum { HWi_G_ROM_BASE_OFFSET_BUF_END = (int)(HW_ROM_BASE_OFFSET_BUF_END) };
enum { HWi_G_CTRDG_MODULE_INFO_BUF = (int)(HW_CTRDG_MODULE_INFO_BUF) };
enum { HWi_G_CTRDG_MODULE_INFO_BUF_END = (int)(HW_CTRDG_MODULE_INFO_BUF_END) };
enum { HWi_G_VBLANK_COUNT_BUF = (int)(HW_VBLANK_COUNT_BUF) };
enum { HWi_G_WM_BOOT_BUF = (int)(HW_WM_BOOT_BUF) };
enum { HWi_G_WM_BOOT_BUF_END = (int)(HW_WM_BOOT_BUF_END) };
enum { HWi_G_NVRAM_USER_INFO = (int)(HW_NVRAM_USER_INFO) };
enum { HWi_G_NVRAM_USER_INFO_END = (int)(HW_NVRAM_USER_INFO_END) };
enum { HWi_G_BIOS_EXCP_STACK_MAIN = (int)(HW_BIOS_EXCP_STACK_MAIN) };
enum { HWi_G_BIOS_EXCP_STACK_MAIN_END = (int)(HW_BIOS_EXCP_STACK_MAIN_END) };
enum { HWi_G_EXCP_VECTOR_MAIN = (int)(HW_EXCP_VECTOR_MAIN) };
enum { HWi_G_ARENA_INFO_BUF = (int)(HW_ARENA_INFO_BUF) };
enum { HWi_G_REAL_TIME_CLOCK_BUF = (int)(HW_REAL_TIME_CLOCK_BUF) };
enum { HWi_G_DMA_CLEAR_DATA_BUF = (int)(HW_DMA_CLEAR_DATA_BUF) };
enum { HWi_G_DMA_CLEAR_DATA_BUF_END = (int)(HW_DMA_CLEAR_DATA_BUF_END) };
enum { HWi_G_ROM_HEADER_BUF = (int)(HW_ROM_HEADER_BUF) };
enum { HWi_G_ROM_HEADER_BUF_END = (int)(HW_ROM_HEADER_BUF_END) };
enum { HWi_G_ISD_RESERVED = (int)(HW_ISD_RESERVED) };
enum { HWi_G_ISD_RESERVED_END = (int)(HW_ISD_RESERVED_END) };
enum { HWi_G_PXI_SIGNAL_PARAM_ARM9 = (int)(HW_PXI_SIGNAL_PARAM_ARM9) };
enum { HWi_G_PXI_SIGNAL_PARAM_ARM7 = (int)(HW_PXI_SIGNAL_PARAM_ARM7) };
enum { HWi_G_PXI_HANDLE_CHECKER_ARM9 = (int)(HW_PXI_HANDLE_CHECKER_ARM9) };
enum { HWi_G_PXI_HANDLE_CHECKER_ARM7 = (int)(HW_PXI_HANDLE_CHECKER_ARM7) };
enum { HWi_G_MIC_LAST_ADDRESS = (int)(HW_MIC_LAST_ADDRESS) };
enum { HWi_G_MIC_SAMPLING_DATA = (int)(HW_MIC_SAMPLING_DATA) };
enum { HWi_G_WM_CALLBACK_CONTROL = (int)(HW_WM_CALLBACK_CONTROL) };
enum { HWi_G_WM_RSSI_POOL = (int)(HW_WM_RSSI_POOL) };
enum { HWi_G_SET_CTRDG_MODULE_INFO_ONCE = (int)(HW_SET_CTRDG_MODULE_INFO_ONCE) };
enum { HWi_G_IS_CTRDG_EXIST = (int)(HW_IS_CTRDG_EXIST) };
enum { HWi_G_COMPONENT_PARAM = (int)(HW_COMPONENT_PARAM) };
enum { HWi_G_THREADINFO_MAIN = (int)(HW_THREADINFO_MAIN) };
enum { HWi_G_THREADINFO_SUB = (int)(HW_THREADINFO_SUB) };
enum { HWi_G_BUTTON_XY_BUF = (int)(HW_BUTTON_XY_BUF) };
enum { HWi_G_TOUCHPANEL_BUF = (int)(HW_TOUCHPANEL_BUF) };
enum { HWi_G_AUTOLOAD_SYNC_BUF = (int)(HW_AUTOLOAD_SYNC_BUF) };
enum { HWi_G_LOCK_ID_FLAG_MAIN = (int)(HW_LOCK_ID_FLAG_MAIN) };
enum { HWi_G_LOCK_ID_FLAG_SUB = (int)(HW_LOCK_ID_FLAG_SUB) };
enum { HWi_G_VRAM_C_LOCK_BUF = (int)(HW_VRAM_C_LOCK_BUF) };
enum { HWi_G_VRAM_D_LOCK_BUF = (int)(HW_VRAM_D_LOCK_BUF) };
enum { HWi_G_WRAM_BLOCK0_LOCK_BUF = (int)(HW_WRAM_BLOCK0_LOCK_BUF) };
enum { HWi_G_WRAM_BLOCK1_LOCK_BUF = (int)(HW_WRAM_BLOCK1_LOCK_BUF) };
enum { HWi_G_CARD_LOCK_BUF = (int)(HW_CARD_LOCK_BUF) };
enum { HWi_G_CTRDG_LOCK_BUF = (int)(HW_CTRDG_LOCK_BUF) };
enum { HWi_G_INIT_LOCK_BUF = (int)(HW_INIT_LOCK_BUF) };
enum { HWi_G_MMEMCHECKER_MAIN = (int)(HW_MMEMCHECKER_MAIN) };
enum { HWi_G_MMEMCHECKER_SUB = (int)(HW_MMEMCHECKER_SUB) };
enum { HWi_G_CMD_AREA = (int)(HW_CMD_AREA) };
enum { HWi_G_CHECK_DEBUGGER_SW = (int)(HW_CHECK_DEBUGGER_SW) };
enum { HWi_G_CHECK_DEBUGGER_BUF1 = (int)(HW_CHECK_DEBUGGER_BUF1) };
enum { HWi_G_CHECK_DEBUGGER_BUF2 = (int)(HW_CHECK_DEBUGGER_BUF2) };
enum { HWi_G_BG_PLTT = (int)(HW_BG_PLTT) };
enum { HWi_G_BG_PLTT_END = (int)(HW_BG_PLTT_END) };
enum { HWi_G_OBJ_PLTT = (int)(HW_OBJ_PLTT) };
enum { HWi_G_OBJ_PLTT_END = (int)(HW_OBJ_PLTT_END) };
enum { HWi_G_DB_BG_PLTT = (int)(HW_DB_BG_PLTT) };
enum { HWi_G_DB_BG_PLTT_END = (int)(HW_DB_BG_PLTT_END) };
enum { HWi_G_DB_OBJ_PLTT = (int)(HW_DB_OBJ_PLTT) };
enum { HWi_G_DB_OBJ_PLTT_END = (int)(HW_DB_OBJ_PLTT_END) };
enum { HWi_G_OAM = (int)(HW_OAM) };
enum { HWi_G_OAM_END = (int)(HW_OAM_END) };
enum { HWi_G_DB_OAM = (int)(HW_DB_OAM) };
enum { HWi_G_DB_OAM_END = (int)(HW_DB_OAM_END) };
enum { HWi_G_MAIN_MEM_EX_END = (int)(HW_MAIN_MEM_EX_END) };
enum { HWi_G_MAIN_MEM_SUB = (int)(HW_MAIN_MEM_SUB) };

#undef HW_RED_RESERVED
#undef HW_RED_RESERVED_END
#undef HW_CARD_ROM_HEADER
#undef HW_DOWNLOAD_PARAMETER
#undef HW_MAIN_MEM_SYSTEM
#undef HW_BOOT_CHECK_INFO_BUF
#undef HW_BOOT_CHECK_INFO_BUF_END
#undef HW_RESET_PARAMETER_BUF
#undef HW_ROM_BASE_OFFSET_BUF
#undef HW_ROM_BASE_OFFSET_BUF_END
#undef HW_CTRDG_MODULE_INFO_BUF
#undef HW_CTRDG_MODULE_INFO_BUF_END
#undef HW_VBLANK_COUNT_BUF
#undef HW_WM_BOOT_BUF
#undef HW_WM_BOOT_BUF_END
#undef HW_NVRAM_USER_INFO
#undef HW_NVRAM_USER_INFO_END
#undef HW_BIOS_EXCP_STACK_MAIN
#undef HW_BIOS_EXCP_STACK_MAIN_END
#undef HW_EXCP_VECTOR_MAIN
#undef HW_ARENA_INFO_BUF
#undef HW_REAL_TIME_CLOCK_BUF
#undef HW_DMA_CLEAR_DATA_BUF
#undef HW_DMA_CLEAR_DATA_BUF_END
#undef HW_ROM_HEADER_BUF
#undef HW_ROM_HEADER_BUF_END
#undef HW_ISD_RESERVED
#undef HW_ISD_RESERVED_END
#undef HW_PXI_SIGNAL_PARAM_ARM9
#undef HW_PXI_SIGNAL_PARAM_ARM7
#undef HW_PXI_HANDLE_CHECKER_ARM9
#undef HW_PXI_HANDLE_CHECKER_ARM7
#undef HW_MIC_LAST_ADDRESS
#undef HW_MIC_SAMPLING_DATA
#undef HW_WM_CALLBACK_CONTROL
#undef HW_WM_RSSI_POOL
#undef HW_SET_CTRDG_MODULE_INFO_ONCE
#undef HW_IS_CTRDG_EXIST
#undef HW_COMPONENT_PARAM
#undef HW_THREADINFO_MAIN
#undef HW_THREADINFO_SUB
#undef HW_BUTTON_XY_BUF
#undef HW_TOUCHPANEL_BUF
#undef HW_AUTOLOAD_SYNC_BUF
#undef HW_LOCK_ID_FLAG_MAIN
#undef HW_LOCK_ID_FLAG_SUB
#undef HW_VRAM_C_LOCK_BUF
#undef HW_VRAM_D_LOCK_BUF
#undef HW_WRAM_BLOCK0_LOCK_BUF
#undef HW_WRAM_BLOCK1_LOCK_BUF
#undef HW_CARD_LOCK_BUF
#undef HW_CTRDG_LOCK_BUF
#undef HW_INIT_LOCK_BUF
#undef HW_MMEMCHECKER_MAIN
#undef HW_MMEMCHECKER_SUB
#undef HW_CMD_AREA
#undef HW_CHECK_DEBUGGER_SW
#undef HW_CHECK_DEBUGGER_BUF1
#undef HW_CHECK_DEBUGGER_BUF2
#undef HW_BG_PLTT
#undef HW_BG_PLTT_END
#undef HW_OBJ_PLTT
#undef HW_OBJ_PLTT_END
#undef HW_DB_BG_PLTT
#undef HW_DB_BG_PLTT_END
#undef HW_DB_OBJ_PLTT
#undef HW_DB_OBJ_PLTT_END
#undef HW_OAM
#undef HW_OAM_END
#undef HW_DB_OAM
#undef HW_DB_OAM_END
#undef HW_MAIN_MEM_EX_END
#undef HW_MAIN_MEM_SUB

#define HW_RED_RESERVED HWi_SHARED_HOST(HWi_G_RED_RESERVED)
#define HW_RED_RESERVED_END HWi_SHARED_HOST(HWi_G_RED_RESERVED_END)
#define HW_CARD_ROM_HEADER HWi_SHARED_HOST(HWi_G_CARD_ROM_HEADER)
#define HW_DOWNLOAD_PARAMETER HWi_SHARED_HOST(HWi_G_DOWNLOAD_PARAMETER)
#define HW_MAIN_MEM_SYSTEM HWi_SHARED_HOST(HWi_G_MAIN_MEM_SYSTEM)
#define HW_BOOT_CHECK_INFO_BUF HWi_SHARED_HOST(HWi_G_BOOT_CHECK_INFO_BUF)
#define HW_BOOT_CHECK_INFO_BUF_END HWi_SHARED_HOST(HWi_G_BOOT_CHECK_INFO_BUF_END)
#define HW_RESET_PARAMETER_BUF HWi_SHARED_HOST(HWi_G_RESET_PARAMETER_BUF)
#define HW_ROM_BASE_OFFSET_BUF HWi_SHARED_HOST(HWi_G_ROM_BASE_OFFSET_BUF)
#define HW_ROM_BASE_OFFSET_BUF_END HWi_SHARED_HOST(HWi_G_ROM_BASE_OFFSET_BUF_END)
#define HW_CTRDG_MODULE_INFO_BUF HWi_SHARED_HOST(HWi_G_CTRDG_MODULE_INFO_BUF)
#define HW_CTRDG_MODULE_INFO_BUF_END HWi_SHARED_HOST(HWi_G_CTRDG_MODULE_INFO_BUF_END)
#define HW_VBLANK_COUNT_BUF HWi_SHARED_HOST(HWi_G_VBLANK_COUNT_BUF)
#define HW_WM_BOOT_BUF HWi_SHARED_HOST(HWi_G_WM_BOOT_BUF)
#define HW_WM_BOOT_BUF_END HWi_SHARED_HOST(HWi_G_WM_BOOT_BUF_END)
#define HW_NVRAM_USER_INFO HWi_SHARED_HOST(HWi_G_NVRAM_USER_INFO)
#define HW_NVRAM_USER_INFO_END HWi_SHARED_HOST(HWi_G_NVRAM_USER_INFO_END)
#define HW_BIOS_EXCP_STACK_MAIN HWi_SHARED_HOST(HWi_G_BIOS_EXCP_STACK_MAIN)
#define HW_BIOS_EXCP_STACK_MAIN_END HWi_SHARED_HOST(HWi_G_BIOS_EXCP_STACK_MAIN_END)
#define HW_EXCP_VECTOR_MAIN HWi_SHARED_HOST(HWi_G_EXCP_VECTOR_MAIN)
#define HW_ARENA_INFO_BUF HWi_SHARED_HOST(HWi_G_ARENA_INFO_BUF)
#define HW_REAL_TIME_CLOCK_BUF HWi_SHARED_HOST(HWi_G_REAL_TIME_CLOCK_BUF)
#define HW_DMA_CLEAR_DATA_BUF HWi_SHARED_HOST(HWi_G_DMA_CLEAR_DATA_BUF)
#define HW_DMA_CLEAR_DATA_BUF_END HWi_SHARED_HOST(HWi_G_DMA_CLEAR_DATA_BUF_END)
#define HW_ROM_HEADER_BUF HWi_SHARED_HOST(HWi_G_ROM_HEADER_BUF)
#define HW_ROM_HEADER_BUF_END HWi_SHARED_HOST(HWi_G_ROM_HEADER_BUF_END)
#define HW_ISD_RESERVED HWi_SHARED_HOST(HWi_G_ISD_RESERVED)
#define HW_ISD_RESERVED_END HWi_SHARED_HOST(HWi_G_ISD_RESERVED_END)
#define HW_PXI_SIGNAL_PARAM_ARM9 HWi_SHARED_HOST(HWi_G_PXI_SIGNAL_PARAM_ARM9)
#define HW_PXI_SIGNAL_PARAM_ARM7 HWi_SHARED_HOST(HWi_G_PXI_SIGNAL_PARAM_ARM7)
#define HW_PXI_HANDLE_CHECKER_ARM9 HWi_SHARED_HOST(HWi_G_PXI_HANDLE_CHECKER_ARM9)
#define HW_PXI_HANDLE_CHECKER_ARM7 HWi_SHARED_HOST(HWi_G_PXI_HANDLE_CHECKER_ARM7)
#define HW_MIC_LAST_ADDRESS HWi_SHARED_HOST(HWi_G_MIC_LAST_ADDRESS)
#define HW_MIC_SAMPLING_DATA HWi_SHARED_HOST(HWi_G_MIC_SAMPLING_DATA)
#define HW_WM_CALLBACK_CONTROL HWi_SHARED_HOST(HWi_G_WM_CALLBACK_CONTROL)
#define HW_WM_RSSI_POOL HWi_SHARED_HOST(HWi_G_WM_RSSI_POOL)
#define HW_SET_CTRDG_MODULE_INFO_ONCE HWi_SHARED_HOST(HWi_G_SET_CTRDG_MODULE_INFO_ONCE)
#define HW_IS_CTRDG_EXIST HWi_SHARED_HOST(HWi_G_IS_CTRDG_EXIST)
#define HW_COMPONENT_PARAM HWi_SHARED_HOST(HWi_G_COMPONENT_PARAM)
#define HW_THREADINFO_MAIN HWi_SHARED_HOST(HWi_G_THREADINFO_MAIN)
#define HW_THREADINFO_SUB HWi_SHARED_HOST(HWi_G_THREADINFO_SUB)
#define HW_BUTTON_XY_BUF HWi_SHARED_HOST(HWi_G_BUTTON_XY_BUF)
#define HW_TOUCHPANEL_BUF HWi_SHARED_HOST(HWi_G_TOUCHPANEL_BUF)
#define HW_AUTOLOAD_SYNC_BUF HWi_SHARED_HOST(HWi_G_AUTOLOAD_SYNC_BUF)
#define HW_LOCK_ID_FLAG_MAIN HWi_SHARED_HOST(HWi_G_LOCK_ID_FLAG_MAIN)
#define HW_LOCK_ID_FLAG_SUB HWi_SHARED_HOST(HWi_G_LOCK_ID_FLAG_SUB)
#define HW_VRAM_C_LOCK_BUF HWi_SHARED_HOST(HWi_G_VRAM_C_LOCK_BUF)
#define HW_VRAM_D_LOCK_BUF HWi_SHARED_HOST(HWi_G_VRAM_D_LOCK_BUF)
#define HW_WRAM_BLOCK0_LOCK_BUF HWi_SHARED_HOST(HWi_G_WRAM_BLOCK0_LOCK_BUF)
#define HW_WRAM_BLOCK1_LOCK_BUF HWi_SHARED_HOST(HWi_G_WRAM_BLOCK1_LOCK_BUF)
#define HW_CARD_LOCK_BUF HWi_SHARED_HOST(HWi_G_CARD_LOCK_BUF)
#define HW_CTRDG_LOCK_BUF HWi_SHARED_HOST(HWi_G_CTRDG_LOCK_BUF)
#define HW_INIT_LOCK_BUF HWi_SHARED_HOST(HWi_G_INIT_LOCK_BUF)
#define HW_MMEMCHECKER_MAIN HWi_SHARED_HOST(HWi_G_MMEMCHECKER_MAIN)
#define HW_MMEMCHECKER_SUB HWi_SHARED_HOST(HWi_G_MMEMCHECKER_SUB)
#define HW_CMD_AREA HWi_SHARED_HOST(HWi_G_CMD_AREA)
#define HW_CHECK_DEBUGGER_SW HWi_SHARED_HOST(HWi_G_CHECK_DEBUGGER_SW)
#define HW_CHECK_DEBUGGER_BUF1 HWi_SHARED_HOST(HWi_G_CHECK_DEBUGGER_BUF1)
#define HW_CHECK_DEBUGGER_BUF2 HWi_SHARED_HOST(HWi_G_CHECK_DEBUGGER_BUF2)
#define HW_BG_PLTT HWi_PALETTE_HOST(HWi_G_BG_PLTT)
#define HW_BG_PLTT_END HWi_PALETTE_HOST(HWi_G_BG_PLTT_END)
#define HW_OBJ_PLTT HWi_PALETTE_HOST(HWi_G_OBJ_PLTT)
#define HW_OBJ_PLTT_END HWi_PALETTE_HOST(HWi_G_OBJ_PLTT_END)
#define HW_DB_BG_PLTT HWi_PALETTE_HOST(HWi_G_DB_BG_PLTT)
#define HW_DB_BG_PLTT_END HWi_PALETTE_HOST(HWi_G_DB_BG_PLTT_END)
#define HW_DB_OBJ_PLTT HWi_PALETTE_HOST(HWi_G_DB_OBJ_PLTT)
#define HW_DB_OBJ_PLTT_END HWi_PALETTE_HOST(HWi_G_DB_OBJ_PLTT_END)
#define HW_OAM HWi_OAM_HOST(HWi_G_OAM)
#define HW_OAM_END HWi_OAM_HOST(HWi_G_OAM_END)
#define HW_DB_OAM HWi_OAM_HOST(HWi_G_DB_OAM)
#define HW_DB_OAM_END HWi_OAM_HOST(HWi_G_DB_OAM_END)
#define HW_MAIN_MEM_EX_END HWi_SHARED_HOST(HWi_G_MAIN_MEM_EX_END)
#define HW_MAIN_MEM_SUB HWi_SHARED_HOST(HWi_G_MAIN_MEM_SUB)

/* Every one of them, for whatever wants to walk the set; which is
 * 3ds/src/3ds_hwmem.c, checking each against the translator. The end
 * of a row is passed so an exclusive bound can be checked at its last
 * byte: no translator can answer for a pointer one past a region. */
#define HWi_MOVED_EACH(X) \
    X(HW_RED_RESERVED, RED_RESERVED, 0x02800000u) \
    X(HW_RED_RESERVED_END, RED_RESERVED_END, 0x02800000u) \
    X(HW_CARD_ROM_HEADER, CARD_ROM_HEADER, 0x02800000u) \
    X(HW_DOWNLOAD_PARAMETER, DOWNLOAD_PARAMETER, 0x02800000u) \
    X(HW_MAIN_MEM_SYSTEM, MAIN_MEM_SYSTEM, 0x02800000u) \
    X(HW_BOOT_CHECK_INFO_BUF, BOOT_CHECK_INFO_BUF, 0x02800000u) \
    X(HW_BOOT_CHECK_INFO_BUF_END, BOOT_CHECK_INFO_BUF_END, 0x02800000u) \
    X(HW_RESET_PARAMETER_BUF, RESET_PARAMETER_BUF, 0x02800000u) \
    X(HW_ROM_BASE_OFFSET_BUF, ROM_BASE_OFFSET_BUF, 0x02800000u) \
    X(HW_ROM_BASE_OFFSET_BUF_END, ROM_BASE_OFFSET_BUF_END, 0x02800000u) \
    X(HW_CTRDG_MODULE_INFO_BUF, CTRDG_MODULE_INFO_BUF, 0x02800000u) \
    X(HW_CTRDG_MODULE_INFO_BUF_END, CTRDG_MODULE_INFO_BUF_END, 0x02800000u) \
    X(HW_VBLANK_COUNT_BUF, VBLANK_COUNT_BUF, 0x02800000u) \
    X(HW_WM_BOOT_BUF, WM_BOOT_BUF, 0x02800000u) \
    X(HW_WM_BOOT_BUF_END, WM_BOOT_BUF_END, 0x02800000u) \
    X(HW_NVRAM_USER_INFO, NVRAM_USER_INFO, 0x02800000u) \
    X(HW_NVRAM_USER_INFO_END, NVRAM_USER_INFO_END, 0x02800000u) \
    X(HW_BIOS_EXCP_STACK_MAIN, BIOS_EXCP_STACK_MAIN, 0x02800000u) \
    X(HW_BIOS_EXCP_STACK_MAIN_END, BIOS_EXCP_STACK_MAIN_END, 0x02800000u) \
    X(HW_EXCP_VECTOR_MAIN, EXCP_VECTOR_MAIN, 0x02800000u) \
    X(HW_ARENA_INFO_BUF, ARENA_INFO_BUF, 0x02800000u) \
    X(HW_REAL_TIME_CLOCK_BUF, REAL_TIME_CLOCK_BUF, 0x02800000u) \
    X(HW_DMA_CLEAR_DATA_BUF, DMA_CLEAR_DATA_BUF, 0x02800000u) \
    X(HW_DMA_CLEAR_DATA_BUF_END, DMA_CLEAR_DATA_BUF_END, 0x02800000u) \
    X(HW_ROM_HEADER_BUF, ROM_HEADER_BUF, 0x02800000u) \
    X(HW_ROM_HEADER_BUF_END, ROM_HEADER_BUF_END, 0x02800000u) \
    X(HW_ISD_RESERVED, ISD_RESERVED, 0x02800000u) \
    X(HW_ISD_RESERVED_END, ISD_RESERVED_END, 0x02800000u) \
    X(HW_PXI_SIGNAL_PARAM_ARM9, PXI_SIGNAL_PARAM_ARM9, 0x02800000u) \
    X(HW_PXI_SIGNAL_PARAM_ARM7, PXI_SIGNAL_PARAM_ARM7, 0x02800000u) \
    X(HW_PXI_HANDLE_CHECKER_ARM9, PXI_HANDLE_CHECKER_ARM9, 0x02800000u) \
    X(HW_PXI_HANDLE_CHECKER_ARM7, PXI_HANDLE_CHECKER_ARM7, 0x02800000u) \
    X(HW_MIC_LAST_ADDRESS, MIC_LAST_ADDRESS, 0x02800000u) \
    X(HW_MIC_SAMPLING_DATA, MIC_SAMPLING_DATA, 0x02800000u) \
    X(HW_WM_CALLBACK_CONTROL, WM_CALLBACK_CONTROL, 0x02800000u) \
    X(HW_WM_RSSI_POOL, WM_RSSI_POOL, 0x02800000u) \
    X(HW_SET_CTRDG_MODULE_INFO_ONCE, SET_CTRDG_MODULE_INFO_ONCE, 0x02800000u) \
    X(HW_IS_CTRDG_EXIST, IS_CTRDG_EXIST, 0x02800000u) \
    X(HW_COMPONENT_PARAM, COMPONENT_PARAM, 0x02800000u) \
    X(HW_THREADINFO_MAIN, THREADINFO_MAIN, 0x02800000u) \
    X(HW_THREADINFO_SUB, THREADINFO_SUB, 0x02800000u) \
    X(HW_BUTTON_XY_BUF, BUTTON_XY_BUF, 0x02800000u) \
    X(HW_TOUCHPANEL_BUF, TOUCHPANEL_BUF, 0x02800000u) \
    X(HW_AUTOLOAD_SYNC_BUF, AUTOLOAD_SYNC_BUF, 0x02800000u) \
    X(HW_LOCK_ID_FLAG_MAIN, LOCK_ID_FLAG_MAIN, 0x02800000u) \
    X(HW_LOCK_ID_FLAG_SUB, LOCK_ID_FLAG_SUB, 0x02800000u) \
    X(HW_VRAM_C_LOCK_BUF, VRAM_C_LOCK_BUF, 0x02800000u) \
    X(HW_VRAM_D_LOCK_BUF, VRAM_D_LOCK_BUF, 0x02800000u) \
    X(HW_WRAM_BLOCK0_LOCK_BUF, WRAM_BLOCK0_LOCK_BUF, 0x02800000u) \
    X(HW_WRAM_BLOCK1_LOCK_BUF, WRAM_BLOCK1_LOCK_BUF, 0x02800000u) \
    X(HW_CARD_LOCK_BUF, CARD_LOCK_BUF, 0x02800000u) \
    X(HW_CTRDG_LOCK_BUF, CTRDG_LOCK_BUF, 0x02800000u) \
    X(HW_INIT_LOCK_BUF, INIT_LOCK_BUF, 0x02800000u) \
    X(HW_MMEMCHECKER_MAIN, MMEMCHECKER_MAIN, 0x02800000u) \
    X(HW_MMEMCHECKER_SUB, MMEMCHECKER_SUB, 0x02800000u) \
    X(HW_CMD_AREA, CMD_AREA, 0x02800000u) \
    X(HW_CHECK_DEBUGGER_SW, CHECK_DEBUGGER_SW, 0x02800000u) \
    X(HW_CHECK_DEBUGGER_BUF1, CHECK_DEBUGGER_BUF1, 0x02800000u) \
    X(HW_CHECK_DEBUGGER_BUF2, CHECK_DEBUGGER_BUF2, 0x02800000u) \
    X(HW_BG_PLTT, BG_PLTT, 0x05001000u) \
    X(HW_BG_PLTT_END, BG_PLTT_END, 0x05001000u) \
    X(HW_OBJ_PLTT, OBJ_PLTT, 0x05001000u) \
    X(HW_OBJ_PLTT_END, OBJ_PLTT_END, 0x05001000u) \
    X(HW_DB_BG_PLTT, DB_BG_PLTT, 0x05001000u) \
    X(HW_DB_BG_PLTT_END, DB_BG_PLTT_END, 0x05001000u) \
    X(HW_DB_OBJ_PLTT, DB_OBJ_PLTT, 0x05001000u) \
    X(HW_DB_OBJ_PLTT_END, DB_OBJ_PLTT_END, 0x05001000u) \
    X(HW_OAM, OAM, 0x07001000u) \
    X(HW_OAM_END, OAM_END, 0x07001000u) \
    X(HW_DB_OAM, DB_OAM, 0x07001000u) \
    X(HW_DB_OAM_END, DB_OAM_END, 0x07001000u) \
    X(HW_MAIN_MEM_EX_END, MAIN_MEM_EX_END, 0x02800000u) \
    X(HW_MAIN_MEM_SUB, MAIN_MEM_SUB, 0x02800000u)

/* --- end generated --- */

/*
 * And main RAM itself, last, because everything above had to be captured
 * while this still meant 0x02000000. HW_MAIN_MEM_END, HW_MAIN_MEM_MAIN and
 * HW_MAIN_MEM_MAIN_END follow from it and need nothing: a macro expands at its
 * use site, and all three stay inside the 4 MB row.
 */
#undef HW_MAIN_MEM
#define HW_MAIN_MEM ((u32)armrec_main_base)

/* The DS address of main RAM, for anything a 26- or 27-bit field will see. */
#define HWi_G_MAIN_MEM 0x02000000

#endif /* SDK_ASM */

#endif /* POKEPLATINUM_3DS_HW_HOST_H */
