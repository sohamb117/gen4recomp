/*
 * Player-facing options from the nativeplat runtime (core/include/
 * np_guest_abi.h, enum np_opt / enum np_status), as the port sees them.
 *
 * The wasm frame publisher (pc/src/pc_view.c) copies the descriptor's opt[]
 * into pc_np_opt after every np_host_vblank and the status below into the
 * descriptor before the next one; every other host leaves both at their
 * defaults. Each option's default is the cartridge's own behaviour, and
 * every reader treats the default as "do exactly what the game does", so a
 * run that never changes an option is the run it always was.
 *
 * Plain C with no game types, so the game's own sources (through
 * pc/patches) and the ARM7 sound driver's glue can include it.
 */
#ifndef PC_NP_OPTIONS_H
#define PC_NP_OPTIONS_H

typedef struct pc_np_options {
    unsigned bgm_volume;   /* 0..256: sequence players 1 FIELD, 2 ME, 7 BGM */
    unsigned se_volume;    /* 0..256: players 0 PV (cries) and 3..6 SE */
    unsigned render_scale; /* 1..4: 3D internal resolution */
    unsigned widescreen;   /* 0 or 1: PC_VIEW_WIDE_MAX-wide 3D view */
    unsigned camera_zoom;  /* field camera distance, 256 = the game's */
    int camera_tilt;       /* field camera pitch, 1/16 degree, + = toward the horizon */
    unsigned quicksave_seq; /* bumped by the host: one in-game save, no UI */
    unsigned rules;        /* PC_NP_RULE_* bits */
    unsigned text_instant; /* 1: message boxes print at once */
} pc_np_options;

/* NP_RULE_FIX_BUGS: the documented cartridge bugs pc/patches fixes behind
 * this bit (docs/bugs_and_glitches.md; see pc/src/pc_np_options.c). */
#define PC_NP_RULE_FIX_BUGS 1u

extern pc_np_options pc_np_opt;

/* Guest -> host, published with every frame (enum np_status). */
typedef struct pc_np_status {
    unsigned link_active;      /* a wireless session is running */
    unsigned field_ready;      /* the player is free in the field */
    unsigned quicksave_seq;    /* last quicksave_seq handled */
    unsigned quicksave_result; /* PC_NP_QS_* for it */
    unsigned map_id;           /* current field map header id */
} pc_np_status;

enum { PC_NP_QS_NONE = 0, PC_NP_QS_SAVED = 1, PC_NP_QS_REFUSED = 2, PC_NP_QS_FAILED = 3 };

extern pc_np_status pc_np_stat;

/* Applies a fresh option set (the publisher, after vblank): range checks,
 * the sound driver's per-player attenuation. */
void pc_np_options_set(const pc_np_options *o);

/* Game-side work at the frame boundary, before the frame is published:
 * field readiness, the map id, a pending quick save. Platinum's lives in
 * pc/src/pc_np_field.c; weak, so a game without one links. */
void pc_np_frame(void) __attribute__((weak));

/* The field camera's per-frame override, called from the field renderer
 * (pc/patches/src/overlay005/fieldmap.c.patch) around its view matrix. A
 * no-op at zoom 256 and tilt 0. The argument is the game's Camera. */
struct Camera;
void pc_np_camera_begin(struct Camera *camera);
void pc_np_camera_end(struct Camera *camera);

#endif /* PC_NP_OPTIONS_H */
