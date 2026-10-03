#ifndef POKEDIAMOND_ARM7_MMAP_H
#define POKEDIAMOND_ARM7_MMAP_H

#include "nitro/mmap_shared.h"

#define HW_WRAM_END                 0x03800000
#define HW_PRV_WRAM                 0x03800000
#define HW_PRV_WRAM_END             0x03810000

#define HW_PRV_WRAM_SIZE            (HW_PRV_WRAM_END-HW_PRV_WRAM)

#define HW_PRV_WRAM_SYSRV_SIZE      0x40

#define HW_MAIN_MEM_SUB_SIZE        (HW_MAIN_MEM_SIZE - HW_MAIN_MEM_MAIN_SIZE - HW_MAIN_MEM_SHARED_SIZE)

#define HW_MAIN_MEM_SUB             (HW_MAIN_MEM_MAIN_END + 0x400000)
#define HW_MAIN_MEM_SUB_END         (HW_MAIN_MEM_SUB + HW_MAIN_MEM_SUB_SIZE)

#define HW_PRV_WRAM_IRQ_STACK_END   (HW_PRV_WRAM_SVC_STACK)
#define HW_PRV_WRAM_SVC_STACK       (HW_PRV_WRAM_SVC_STACK_END - HW_SVC_STACK_SIZE)
#define HW_PRV_WRAM_SVC_STACK_END   (HW_PRV_WRAM_SYSRV)

#define HW_PRV_WRAM_SYSRV           (HW_PRV_WRAM + HW_PRV_WRAM_SIZE - HW_PRV_WRAM_SYSRV_SIZE)

#define HW_INTR_CHECK_BUF           (HW_PRV_WRAM_SYSRV + 0x38)

#define HW_VBLANK_COUNT_BUF         (HW_MAIN_MEM + 0x007ffc3c)
#define HW_LOCK_ID_FLAG_SUB         (HW_MAIN_MEM + 0x007fffb8)

/*
 * The floor below which a word the ARM9 sends over the sound FIFO is not a
 * command list. On hardware the list is in main RAM and the only other value
 * ever sent is zero, the "process what is queued" poke, so main RAM's base is
 * the test SND_command.c makes.
 *
 * This port hands the driver a HOST pointer instead, and where the host puts
 * the ARM9's objects is not the driver's business, so the floor is a
 * property of the port and each one states its own. It is only ever compared
 * against the FIFO word; the wave-offset test in SND_bank.c keeps main RAM's
 * base, because there the number below the floor is a real file offset and
 * the margin is worth having.
 */
#ifndef SND_CMD_ADDR_FLOOR
#define SND_CMD_ADDR_FLOOR          HW_MAIN_MEM
#endif

#endif //POKEDIAMOND_ARM7_MMAP_H
