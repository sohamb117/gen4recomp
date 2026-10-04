/*
 * The card library's private state as Diamond/Pearl's NitroSDK (3.2 era)
 * lays it out, for the shared host model of the card
 * (games/platinum/pc/src/pc_card_rom.c, which includes this instead of the
 * 4.2 SDK's libraries/card/include/card_common.h when PC_GAME_DP is set).
 *
 * pc_card_rom.c replaces CARD_Init / CARDi_Request / CARDi_SetTask and reads
 * and writes the objects D's own compiled SDK C (arm9/lib/NitroSDK/src/
 * CARD_*.c) builds: `cardi_common` and the command block it points at. The
 * host layer otherwise compiles against Platinum's 4.2 headers, and measured
 * on wasm32 (both headers compiled, u64 at mwcc's 4-byte alignment as both
 * builds' nitro/types.h shadows set it) the two agree on everything
 * pc_card_rom.c touches in CARDiCommon (sizeof 544; cmd 0, src 28, dst 32,
 * len 36, dma 40, callback 56, callback_arg 60, cur_th 260, flag 276) and
 * disagree inside the command block's chip spec, because 4.2 inserted
 * `subsect_size` after `sect_size`:
 *
 *                                    D (3.2)   P (4.2)
 *   offsetof(CARDiCommandArg, spec.page_size)       32        36
 *   offsetof(CARDiCommandArg, spec.initial_status)  72        84
 *
 * so the 4.2 header would have erased pages of the wrong size. The fields
 * below are transcribed from arm9/lib/NitroSDK/include/CARD_common.h (struct
 * CARDiCommandArg, struct CARDiCommon); the one renamed field is the command
 * block's destination, `dest` in 3.2 and `dst` in 4.2, the same word.
 *
 * The request codes (CARD_REQ_*, CARDRequest, CARDRequestMode) and
 * CARDResult/CARDBackupType come from the 4.2 public <nitro/card/common.h>
 * with the same numbers: CARD_REQ_INIT 0 .. CARD_REQ_ERASE_CHIP_BACKUP 12 in
 * both; 4.2 only appends codes (status access, subsector erase) that 3.2
 * never sends.
 */
#ifndef PC_DP_CARD_COMMON_H
#define PC_DP_CARD_COMMON_H

#include <stddef.h>

#include <nitro.h>
#include <nitro/pxi.h>

enum {
    CARD_STAT_INIT     = (1 << 0),
    CARD_STAT_INIT_CMD = (1 << 1),
    CARD_STAT_BUSY     = (1 << 2),
    CARD_STAT_TASK     = (1 << 3),
    CARD_STAT_RECV     = (1 << 4),
    CARD_STAT_REQ      = (1 << 5),
    CARD_STAT_CANCEL   = (1 << 6)
};

typedef enum {
    CARD_TARGET_NONE,
    CARD_TARGET_ROM,
    CARD_TARGET_BACKUP
} CARDTargetMode;

typedef s32 CARDiOwner;

typedef struct CARDiCommandArg {
    CARDResult result;
    CARDBackupType type;
    u32 id;
    u32 src;
    u32 dst;                    /* 3.2: dest */
    u32 len;
    struct {
        u32 total_size;
        u32 sect_size;
        u32 page_size;
        u32 addr_width;
        u32 program_page;
        u32 write_page;
        u32 write_page_total;
        u32 erase_chip;
        u32 erase_chip_total;
        u32 erase_sector;
        u32 erase_sector_total;
        u32 erase_page;
        u8 initial_status;
        u8 padding1[3];
        u32 caps;
        u8 padding2[16];
    } spec;
} CARDiCommandArg;

typedef struct CARDiCommon {
    CARDiCommandArg *cmd;
    s32 command;
    volatile CARDiOwner lock_owner;
    volatile s32 lock_ref;
    OSThreadQueue lock_queue[1];
    CARDTargetMode lock_target;
    u32 src;
    u32 dst;
    u32 len;
    u32 dma;
    CARDRequest req_type;
    s32 req_retry;
    CARDRequestMode req_mode;
    MIDmaCallback callback;
    void *callback_arg;
    void (*task_func)(struct CARDiCommon *);
    OSThread thread[1];
    OSThread *cur_th;
    u32 priority;
    OSThreadQueue busy_q[1];
    volatile u32 flag;
    u8 dummy[8];
    u8 backup_cache_page_buf[256] ATTRIBUTE_ALIGN(32);
} CARDiCommon;

/* The numbers D's own header produces in the D build (wasm32, mwcc u64
 * alignment); a drifted transcription stops the compile. */
_Static_assert(sizeof(CARDiCommandArg) == 96, "CARDiCommandArg size (3.2)");
_Static_assert(offsetof(CARDiCommandArg, dst) == 16, "CARDiCommandArg.dest");
_Static_assert(offsetof(CARDiCommandArg, spec.page_size) == 32, "spec.page_size (3.2)");
_Static_assert(offsetof(CARDiCommandArg, spec.initial_status) == 72, "spec.initial_status (3.2)");
_Static_assert(offsetof(CARDiCommandArg, spec.caps) == 76, "spec.caps (3.2)");
_Static_assert(sizeof(CARDiCommon) == 544, "CARDiCommon size (3.2)");
_Static_assert(offsetof(CARDiCommon, cur_th) == 260, "CARDiCommon.cur_th (3.2)");
_Static_assert(offsetof(CARDiCommon, flag) == 276, "CARDiCommon.flag (3.2)");

extern CARDiCommon cardi_common;

void CARDi_InitCommon(void);
void CARDi_SetTask(void (*task)(CARDiCommon *));
BOOL CARDi_Request(CARDiCommon *p, int req_type, int retry_max);

#endif /* PC_DP_CARD_COMMON_H */
