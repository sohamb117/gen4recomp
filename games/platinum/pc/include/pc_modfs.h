#ifndef PC_MODFS_H
#define PC_MODFS_H

/*
 * Runtime content packages. A directory with mod.toml is a package the
 * running port can load; FS and NARC overlays look up host files through
 * the two host_* functions. Empty boot (nothing enabled) is a no-op.
 *
 * host_file / host_member return a host path the overlay claims, or
 * NULL (use the cartridge). member_stat / member_read are the narc.c
 * hook: 1 = claimed (dest / size filled), 0 = cartridge. bind_narc
 * records the Nitro path on a NARC* so the object methods can look
 * the member up without growing the NARC struct.
 */
void pc_modfs_boot(void);

/* Host path for a Nitro file the overlay claims, or NULL. */
const char *pc_modfs_host_file(const char *nitro_path);

/* Host path for a NARC member the overlay claims, or NULL. */
const char *pc_modfs_host_member(const char *nitro_path, unsigned index);

/* 1 if claimed (writes size). 0 if the cartridge wins. */
int pc_modfs_member_stat(const char *nitro_path, unsigned index, unsigned *out_size);

/* 1 if claimed and dest filled. bytesToRead 0 means the whole member. */
int pc_modfs_member_read(const char *nitro_path, unsigned index,
                         void *dest, unsigned offset, unsigned bytesToRead);

void pc_modfs_bind_narc(const void *narc, const char *nitro_path);
void pc_modfs_unbind_narc(const void *narc);
const char *pc_modfs_narc_path(const void *narc);

/*
 * Land-data (and anything else that streams a member) uses
 * NARC_ReadFromMember for the header, then NARC_ReadFile / NARC_Seek
 * for the rest. After a hooked member read, seq_note records the
 * cursor; seq_read / seq_seek serve the rest of that overlayed
 * member. A ROM member read clears the cursor.
 */
void pc_modfs_narc_seq_note(const void *narc, unsigned member,
                            unsigned offset, unsigned n);
void pc_modfs_narc_seq_clear(const void *narc);
int pc_modfs_narc_seq_read(const void *narc, void *dest, unsigned n);
int pc_modfs_narc_seq_seek(const void *narc, unsigned delta);

/*
 * ROM FAT count plus the highest planted append. A planted index
 * >= rom_count is an append; every index from rom_count through that
 * highest must exist (a hole is a boot error that names the missing
 * member). NARC.numFiles is u16, so an append at 65535 or past it
 * dies. Returns rom_count when nothing was appended.
 */
unsigned pc_modfs_narc_file_count(const char *nitro_path, unsigned rom_count);

/*
 * Cooked billboard person: gfx id -> mmodel.narc NSBTX member, or -1.
 * overlay005's four tables are sentinel-scanned and GF_ASSERT on a miss;
 * the port hook clones the youngster row and uses this member for the
 * texture. Written by cook as .cooked/generated/billboard_gfx.txt.
 */
int pc_modfs_billboard_nsbtx(int gfx_id);

/*
 * Cooked map prop: Nth extra build_model id the area loader should
 * pull in on top of area_build, or -1. Written by cook as
 * .cooked/generated/extra_props.txt. The file is loaded only;
 * matshp locators stop at the vanilla ceiling, so a cooked id
 * draws through Easy3D_DrawRenderObj (count 0).
 */
int pc_modfs_extra_prop(int index);

/*
 * Cooked map header. 1 if `id` is a cooked header and *out is
 * filled; 0 if the compiled table should answer. Written by cook
 * as .cooked/generated/cooked_maps.txt. map_header.c copies these
 * fields onto a MapHeader so a content package can be addressed
 * without rebuilding generated/map_headers.h.
 */
struct pc_modfs_map_header {
    unsigned area;
    unsigned preloaded;
    unsigned matrix;
    unsigned scripts;
    unsigned init_scripts;
    unsigned msg;
    unsigned day;
    unsigned night;
    unsigned wild;
    unsigned events;
    unsigned label;
    unsigned window;
    unsigned weather;
    unsigned camera;
    unsigned map_type;
    unsigned battle_bg;
    unsigned bike;
    unsigned run;
    unsigned escape;
    unsigned fly;
};

int pc_modfs_map_header(int id, struct pc_modfs_map_header *out);

#endif /* PC_MODFS_H */
