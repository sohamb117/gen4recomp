/*
 * The overlay-statics table of a ROM-only core: there are no statics.
 *
 * Diamond's host fragment generates this table from the decompiled
 * overlays' C objects (games/platinum/pc/gen_overlay_statics.py), so that
 * pc_fs_overlay.c can reset an overlay's host-side statics on reload.
 * Recompiled overlays keep their data in guest memory, where
 * armrec_load_overlay() rewrites it, so here the list is empty. pc_sym.c
 * reads a table with no entries as "no table" (a module whose post-link
 * patch never ran) and pc_fs_overlay.c then refuses to boot, so the one
 * entry is a symbol the link kept no copy of (SYM_NOT_AN_OFFSET,
 * SYM_DROPPED): counted as answered, and skipped.
 */
struct pc_ov_static_desc {
    unsigned overlay;
    const char *name;
    unsigned size;
};

const struct pc_ov_static_desc pc_ov_static_desc[] = {
    { 0u, "ndsrec_no_overlay_statics", 0u },
};
const int pc_ov_static_desc_n = 1;

struct pc_ov_static_addr {
    int off;
    unsigned size;
};

struct pc_ov_static_addr pc_ov_static_addr[1] = {
    { -1, 1u },
};
