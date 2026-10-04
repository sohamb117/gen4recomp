/*
 * Runtime content packages on Diamond/Pearl: the half that needs D's own
 * FS layout. The packages, the load order and the claim tables are the
 * shared games/platinum/pc/src/pc_modfs.c (host layer); NARC member claims
 * are answered in src/filesystem.c (pc/patches/arm9/src/filesystem.c.patch).
 *
 * WHOLE FILES. FS_OpenFile is replaced: D's arm9/lib/NitroSDK/src/FS_file.c
 * is compiled weak (pc/mk/game.mk), so this strong definition is the one the
 * game's C and the recompiled asm (`bl FS_OpenFile`, called by name) reach.
 * A path a package claims is read whole into a buffer and opened with the
 * SDK's own FS_OpenFileDirect on a memory archive whose read callback
 * copies from that buffer: guest addresses are wasm linear addresses, so
 * the buffer's address is the file image's offset and FS_ReadFile,
 * FS_SeekFile and FS_GetLength work unchanged on D's 3.2 FSFile. The
 * archive's CLOSEFILE procedure frees the buffer, so FS_CloseFile stays the
 * SDK's. The archive is never registered by name: nothing can path into it.
 * Unclaimed paths take the SDK's own route (ConvertPathToFileID +
 * OpenFileFast).
 *
 * PROBES. PC_MODFS_PROBE=<path> and PC_MODFS_PROBE_NARC=<path>/<idx>, as on
 * Platinum: on the first FS_OpenFile (D's FS is up by then) the file, or
 * the member through both the by-id-pair readers and the NARC object
 * readers (which must agree), is printed as hex and the process exits.
 */
#include "global.h"

#include "FS_archive.h"
#include "FS_file.h"
#include "filesystem.h"
#include "pc_modfs.h"

/* src/filesystem.c, added by its pc patch. */
int NARC_FindID(const char *path);

static FSArchive sModArc;
static BOOL sModArcReady;

static FSResult ModArc_Read(FSArchive *arc, void *dst, u32 pos, u32 size) {
    (void)arc;
    __builtin_memcpy(dst, (const void *)pos, size);
    return FS_RESULT_SUCCESS;
}

static FSResult ModArc_Proc(FSFile *file, FSCommandType cmd) {
    if (cmd == FS_COMMAND_CLOSEFILE && file->prop.file.top != 0) {
        pc_modfs_file_free((void *)file->prop.file.top);
        file->prop.file.top = file->prop.file.bottom = file->prop.file.pos = 0;
    }
    return FS_RESULT_PROC_DEFAULT;
}

static void ModArc_Init(void) {
    if (sModArcReady) {
        return;
    }
    FS_InitArchive(&sModArc);
    (void)FS_LoadArchive(&sModArc, 0, 0, 0, 0, 0, ModArc_Read, NULL);
    FS_SetArchiveProc(&sModArc, ModArc_Proc, FS_ARCHIVE_PROC_CLOSEFILE);
    sModArcReady = TRUE;
}

static void ProbeFile(void) {
    const char *path = pc_modfs_probe_file();
    FSFile file;
    u32 len;
    u8 *buf;

    if (path == NULL) {
        return;
    }
    FS_InitFile(&file);
    if (!FS_OpenFile(&file, path)) {
        pc_modfs_fatal("probe: cannot open the PC_MODFS_PROBE path");
    }
    len = FS_GetLength(&file);
    buf = pc_modfs_alloc(len);
    if (len != 0 && FS_ReadFile(&file, buf, (s32)len) != (s32)len) {
        pc_modfs_fatal("probe: short read");
    }
    (void)FS_CloseFile(&file);
    pc_modfs_probe_report("probe", path, -1, buf, len);
}

static void ProbeMember(void) {
    char path[256];
    unsigned idx;
    int id;
    u32 size;
    u32 chunk;
    u8 *buf;
    u8 *obuf;
    u8 slice;
    u8 slice2;
    NARC narc;
    u32 i;

    if (!pc_modfs_probe_member(path, sizeof path, &idx)) {
        return;
    }
    id = NARC_FindID(path);
    if (id < 0) {
        pc_modfs_fatal("probe-narc: unknown narc path");
    }

    /* By-id-pair readers. */
    size = GetNarcMemberSizeByIdPair((NarcId)id, (s32)idx);
    buf = pc_modfs_alloc(size);
    ReadWholeNarcMemberByIdPair(buf, (NarcId)id, (s32)idx);

    /* The NARC object readers, on a NARC opened as NARC_New opens one
     * (NARC_New itself wants a game heap). */
    FS_InitFile(&narc.file);
    if (!FS_OpenFile(&narc.file, path)) {
        pc_modfs_fatal("probe-narc: cannot open the narc");
    }
    pc_modfs_bind_narc(&narc, path);
    narc.btaf_start = 0;
    FS_SeekFile(&narc.file, 12, FS_SEEK_SET);
    FS_ReadFile(&narc.file, &narc.btaf_start, 2);
    FS_SeekFile(&narc.file, (s32)(narc.btaf_start + 4), FS_SEEK_SET);
    FS_ReadFile(&narc.file, &chunk, 4);
    FS_ReadFile(&narc.file, &narc.num_files, 2);
    narc.num_files = (u16)pc_modfs_narc_file_count(path, narc.num_files);
    FS_SeekFile(&narc.file, (s32)(narc.btaf_start + chunk + 4), FS_SEEK_SET);
    FS_ReadFile(&narc.file, &i, 4);
    narc.gmif_start = narc.btaf_start + chunk + i;

    if (NARC_GetMemberSize(&narc, idx) != size) {
        pc_modfs_fatal("probe-narc size mismatch: pair != object");
    }
    obuf = pc_modfs_alloc(size);
    NARC_ReadWholeMember(&narc, idx, obuf);
    for (i = 0; i < size; i++) {
        if (buf[i] != obuf[i]) {
            pc_modfs_fatal("probe-narc object read != pair read");
        }
    }
    if (size != 0) {
        slice = 0;
        slice2 = 0;
        NARC_ReadFromMember(&narc, idx, 0, 1, &slice);
        ReadFromNarcMemberByIdPair(&slice2, (NarcId)id, (s32)idx, 0, 1);
        if (slice != buf[0] || slice2 != buf[0]) {
            pc_modfs_fatal("probe-narc slice mismatch");
        }
    }
    pc_modfs_unbind_narc(&narc);
    (void)FS_CloseFile(&narc.file);
    pc_modfs_probe_report("probe-narc", path, (int)idx, buf, size);
}

static void RunProbes(void) {
    static BOOL sRan;

    if (sRan) {
        return;
    }
    sRan = TRUE;
    ProbeFile();
    ProbeMember();
}

BOOL FS_OpenFile(FSFile *p_file, const char *path) {
    FSFileID file_id;
    unsigned size;
    void *buf;

    RunProbes();
    buf = pc_modfs_file_load(path, &size);
    if (buf != NULL) {
        ModArc_Init();
        return FS_OpenFileDirect(p_file, &sModArc, (u32)buf, (u32)buf + size, (u32)~0);
    }
    return FS_ConvertPathToFileID(&file_id, path) && FS_OpenFileFast(p_file, file_id);
}
