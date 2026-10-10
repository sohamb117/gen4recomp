/*
 * The Download Play client of the Poké Transfer child's station: the part of
 * a DS's firmware that receives a child program from a parent over the air
 * and boots it (docs/BW_PLAN.md "Poké Transfer").
 *
 * pc_main.c calls pc_mb_child_download() once every subsystem is up and
 * before NitroMain, the firmware's place. The protocol is NitroSDK 4.2's
 * own multiboot child (libraries/mb: MB_Init, MB_StartChild, the beacon
 * game-info list, MB_CommDownloadRequest, MB_CommStartDownload) over its
 * ARM9 WM library (libraries/wm), both compiled into this image
 * (pc/Makefile.wasm VER=poketransfer), talking to pc_wm.c as any WM library
 * does. They scan for a multiboot parent, take the first game whose beacon
 * information validates, connect, request the file and receive every block.
 * The library writes each block where the parent's download information says
 * (the ROM header at the NITRO HW_ROM_HEADER_BUF, pc_pt_fw_mb.h, the ARM9
 * static at its load address, the ARM7 static into the receive buffer above
 * the load area). The header is then copied to this map's HW_ROM_HEADER_BUF,
 * the same byte on a DS. When the
 * parent sends the boot request and the library has disconnected, it leaves
 * the download information and the parent's BSS description at
 * MB_DOWNLOAD_FILEINFO_ADDRESS / MB_BSSDESC_ADDRESS, as the firmware's does.
 *
 * Nothing boots that was not received. The three segments are checked
 * against the image this build recompiled (SHA-1s from pc_pt_image.c,
 * ndsrec.py mbimage-c), and a mismatch stops the run. Then crt0's steps up
 * to its jump to main (the host runs no crt0): the BLZ-compressed ARM9
 * static is decompressed in place and its module parameters'
 * compressed-end word cleared, and the result must hash to the decompressed
 * image the recompiled code and its data came from. Each autoload block is
 * copied from the end of the static to its place and its bss cleared, then
 * the static's bss, which begins on the autoload bytes. The ARM7 static is
 * checked but not run; the host models the ARM7.
 *
 * The boot state a Download Play child reads, left as the firmware leaves
 * it:
 *   - MBParam at HW_WM_BOOT_BUF: MB_TYPE_MULTIBOOT and the parent's BSS
 *     description, the leading MBParentBssDesc part of the received
 *     WMBssDesc. The child reconnects to its parent with it.
 *   - the parent's 32-byte user parameter (the download information's
 *     reserved words) at HW_DOWNLOAD_PARAMETER.
 *
 * Until a parent is found the station waits, as a DS in Download Play does.
 * Frames pass through OS_Halt with a VBlank handler of the firmware's own,
 * which is removed before the child starts.
 */
#include <nitro.h>
#include <nitro/mb.h>
#include <string.h>
#include "mb_private.h"
#include <nitro/dgt/dgt.h>
#include <stdio.h>

extern const unsigned char pc_pt_child_header_sha1[20];
extern const unsigned int pc_pt_child_arm9_ram, pc_pt_child_arm9_size;
extern const unsigned char pc_pt_child_arm9_sha1[20];
extern const unsigned int pc_pt_child_arm7_size;
extern const unsigned char pc_pt_child_arm7_sha1[20];
extern const unsigned int pc_pt_child_module_params, pc_pt_child_image_size;
extern const unsigned char pc_pt_child_image_sha1[20];

extern void pc_trap_unreached(const char *sym, const char *why) __attribute__((noreturn));

/* The child's OS_IRQTable (pc_pt_child.c), where OS_Halt finds the VBlank
 * handler, and its WM DMA channel. */
#define PT_OS_IRQ_TABLE ((OSIrqFunction *)0x02FE0020u)
#define PT_FW_DMA 2

static u8 sWork[MB_CHILD_SYSTEM_BUF_SIZE] ATTRIBUTE_ALIGN(32);
static int sPending = -1; /* a validated game to request, by list index */
static int sRequested;
static int sBootReady;

static void fw_vblank(void)
{
}

static void fw_frame(void)
{
    OS_Halt();
}

static void fw_state(u32 status, void *arg)
{
    static u32 last = ~0u;

    if (status != last && status < MB_COMM_CSTATE_GAMEINFO_VALIDATED) {
        extern u32 pc_os_vblank_count;
        fprintf(stderr, "pc_pt_dlplay: frame %u: child state %u\n", (unsigned)pc_os_vblank_count, (unsigned)status);
        last = status;
    }
    switch (status) {
    case MB_COMM_CSTATE_GAMEINFO_VALIDATED:
        if (!sRequested && sPending < 0) {
            const MbBeaconRecvStatus *rs = MB_GetBeaconRecvStatus();
            int i;

            for (i = 0; i < MB_GAME_INFO_RECV_LIST_NUM; i++) {
                if (arg == &rs->list[i].gameInfo) {
                    sPending = i;
                    break;
                }
            }
        }
        break;
    case MB_COMM_CSTATE_CONNECT:
        fprintf(stderr, "pc_pt_dlplay: connected to the parent\n");
        break;
    case MB_COMM_CSTATE_DLINFO_ACCEPTED:
        fprintf(stderr, "pc_pt_dlplay: download information received, starting the download\n");
        (void)MB_CommStartDownload();
        break;
    case MB_COMM_CSTATE_RECV_COMPLETE:
        fprintf(stderr, "pc_pt_dlplay: every block received\n");
        break;
    case MB_COMM_CSTATE_BOOT_READY:
        sBootReady = 1;
        break;
    case MB_COMM_CSTATE_CONNECT_FAILED:
    case MB_COMM_CSTATE_DISCONNECTED_BY_PARENT:
    case MB_COMM_CSTATE_REQ_REFUSED:
    case MB_COMM_CSTATE_MEMBER_FULL:
    case MB_COMM_CSTATE_CANCELLED:
        fprintf(stderr, "pc_pt_dlplay: download ended early (state %u); scanning again\n", (unsigned)status);
        sRequested = 0;
        break;
    case MB_COMM_CSTATE_ERROR: {
        static u16 last_err = 0xffff;
        u16 err = arg ? ((MBErrorStatus *)arg)->errcode : 0;

        if (err != last_err) {
            fprintf(stderr, "pc_pt_dlplay: multiboot error %u\n", (unsigned)err);
            if (err == MB_ERRCODE_INVALID_DLFILEINFO) {
                const MbSegmentInfo *seg = (const MbSegmentInfo *)(&pCwork->dl_fileinfo.header + 1);
                int i;

                for (i = 0; i < MB_DL_SEGMENT_NUM; i++)
                    fprintf(stderr, "pc_pt_dlplay:   segment %d: recv %08x load %08x size %08x\n", i,
                            (unsigned)seg[i].recv_addr, (unsigned)seg[i].load_addr, (unsigned)seg[i].size);
            }
            last_err = err;
        }
        break;
    }
    default:
        break;
    }
}

static void check_sha1(const char *what, const void *p, u32 len, const unsigned char want[20])
{
    DGTHash2Context ctx;
    unsigned char got[20];

    DGT_Hash2Reset(&ctx);
    DGT_Hash2SetSource(&ctx, (const unsigned char *)p, len);
    DGT_Hash2GetDigest(&ctx, got);
    if (memcmp(got, want, sizeof got) != 0) {
        fprintf(stderr, "pc_pt_dlplay: the received %s (%u bytes) is not the image this build recompiled\n", what,
                (unsigned)len);
        pc_trap_unreached("pc_mb_child_download", "received image does not match");
    }
}

static void verify_and_place(void)
{
    const MBDownloadFileInfo *fi = (const MBDownloadFileInfo *)MB_DOWNLOAD_FILEINFO_ADDRESS;
    const MbSegmentInfo *seg = (const MbSegmentInfo *)(&fi->header + 1);
    const MbSegmentInfo *hdr = &seg[0], *arm9 = &seg[1], *arm7 = &seg[2];
    const WMBssDesc *bss = (const WMBssDesc *)MB_BSSDESC_ADDRESS;
    MBParam *param = (MBParam *)HW_WM_BOOT_BUF;

    fprintf(stderr,
            "pc_pt_dlplay: received header %u bytes at %08x, ARM9 %u at %08x (load %08x), ARM7 %u at %08x (load %08x)\n",
            (unsigned)hdr->size, (unsigned)hdr->recv_addr, (unsigned)arm9->size, (unsigned)arm9->recv_addr,
            (unsigned)arm9->load_addr, (unsigned)arm7->size, (unsigned)arm7->recv_addr, (unsigned)arm7->load_addr);
    if (hdr->size != ROM_HEADER_SIZE_FULL || arm9->load_addr != pc_pt_child_arm9_ram
        || arm9->size != pc_pt_child_arm9_size || arm7->size != pc_pt_child_arm7_size)
        pc_trap_unreached("pc_mb_child_download", "the received segments are not the image this build recompiled");
    check_sha1("ROM header", (const void *)hdr->recv_addr, hdr->size, pc_pt_child_header_sha1);
    check_sha1("ARM9 static", (const void *)arm9->recv_addr, arm9->size, pc_pt_child_arm9_sha1);
    check_sha1("ARM7 static", (const void *)arm7->recv_addr, arm7->size, pc_pt_child_arm7_sha1);
    if (hdr->recv_addr != HW_ROM_HEADER_BUF)
        memcpy((void *)HW_ROM_HEADER_BUF, (const void *)hdr->recv_addr, ROM_HEADER_SIZE_FULL);

    /* crt0: decompress the ARM9 static in place, then clear the word that
     * says it is compressed (_start_ModuleParams + 0x14). */
    {
        u32 *mp = (u32 *)pc_pt_child_module_params;
        u32 list, src, ent;

        if (arm9->recv_addr != arm9->load_addr)
            memmove((void *)arm9->load_addr, (const void *)arm9->recv_addr, arm9->size);
        if (mp[5] != 0) {
            MIi_UncompressBackward((void *)mp[5]);
            mp[5] = 0;
        }
        check_sha1("ARM9 static, decompressed", (const void *)pc_pt_child_arm9_ram, pc_pt_child_image_size,
                   pc_pt_child_image_sha1);

        /* crt0 do_autoload: _start_ModuleParams' list (+0x00 .. +0x04) of
         * {address, size, bss size}, with a static-initializer word before
         * the bss size from TWL-SDK 5 on (SDK version word, +0x18), whose
         * bytes follow one another from +0x08; then the static's bss
         * (+0x0C .. +0x10). */
        ent = (mp[6] >> 24) >= 5 ? 16 : 12;
        src = mp[2];
        for (list = mp[0]; list + ent <= mp[1]; list += ent) {
            const u32 *e = (const u32 *)list;
            u32 size = e[1], bss_size = e[ent == 16 ? 3 : 2];

            memmove((void *)e[0], (const void *)src, size);
            memset((void *)(e[0] + size), 0, bss_size);
            src += size;
        }
        memset((void *)mp[3], 0, mp[4] - mp[3]);
    }

    memset(param, 0, sizeof *param);
    param->boot_type = MB_TYPE_MULTIBOOT;
    memcpy(&param->parent_bss_desc, bss, sizeof param->parent_bss_desc);
    memcpy((void *)HW_DOWNLOAD_PARAMETER, fi->reserved, HW_DOWNLOAD_PARAMETER_SIZE);
    fprintf(stderr, "pc_pt_dlplay: image verified; booting the child (parent %02x:%02x:%02x:%02x:%02x:%02x, channel %u)\n",
            bss->bssid[0], bss->bssid[1], bss->bssid[2], bss->bssid[3], bss->bssid[4], bss->bssid[5],
            (unsigned)bss->channel);
}

void pc_mb_child_download(void)
{
    OSOwnerInfo owner;
    MBUserInfo user;
    int i;

    /* The firmware's own VBlank, so OS_Halt passes frames. */
    PT_OS_IRQ_TABLE[0] = fw_vblank;
    reg_OS_IE = OS_IE_V_BLANK;
    reg_OS_IME = 1;

    OS_GetOwnerInfo(&owner);
    memset(&user, 0, sizeof user);
    user.favoriteColor = (u8)(owner.favoriteColor & 0xF);
    user.nameLength = (u8)MATH_MIN(owner.nickNameLength, MB_USER_NAME_LENGTH);
    for (i = 0; i < user.nameLength; i++)
        user.name[i] = owner.nickName[i];

    if (MB_Init(sWork, &user, 0, 0, PT_FW_DMA) != MB_ERRCODE_SUCCESS)
        pc_trap_unreached("pc_mb_child_download", "MB_Init failed");
    MB_CommSetChildStateCallback(fw_state);
    if (MB_StartChild() != MB_ERRCODE_SUCCESS)
        pc_trap_unreached("pc_mb_child_download", "MB_StartChild failed");
    fprintf(stderr, "pc_pt_dlplay: Download Play, waiting for a parent\n");

    while (!sBootReady) {
        static u16 tenth;
        u16 pct;

        if (sPending >= 0 && !sRequested) {
            fprintf(stderr, "pc_pt_dlplay: requesting the game at list entry %d\n", sPending);
            sRequested = MB_CommDownloadRequest(sPending) == MB_ERRCODE_SUCCESS;
            sPending = -1;
        }
        if (MB_CommGetChildState() == MB_COMM_CSTATE_RECV_PROCEED && (pct = MB_GetChildProgressPercentage()) / 10 > tenth) {
            extern u32 pc_os_vblank_count;
            tenth = pct / 10;
            fprintf(stderr, "pc_pt_dlplay: frame %u: %u%% received\n", (unsigned)pc_os_vblank_count, (unsigned)pct);
        }
        fw_frame();
    }

    verify_and_place();

    /* The child starts as a booted program does, before any VBlank of its
     * own: line 0, no VBlank flag, nothing pending (pc_os_lite.c OS_Halt).
     * Its OS_Init spins until VCOUNT reads 0, and the firmware's frames left
     * the line at 192. */
    PT_OS_IRQ_TABLE[0] = NULL;
    reg_OS_IME = 0;
    reg_OS_IE = 0;
    reg_OS_IF = 0;
    reg_GX_VCOUNT = 0;
    reg_GX_DISPSTAT &= (u16)~1;
}
