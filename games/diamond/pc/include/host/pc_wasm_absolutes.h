/*
 * games/platinum/pc/wasm/include/nitro/fs/overlay.h includes this for
 * Platinum's overlay ordinals (generated there from Platinum's main.lsf, so
 * FS_OVERLAY_ID(name) can be a constant). The host layer names no overlay by
 * FS_OVERLAY_ID, so the Diamond/Pearl host compile, which searches that
 * shadow directory for pc_wasm.h, defines none: a host file that starts
 * using FS_OVERLAY_ID fails to compile here instead of getting Platinum's
 * numbers.
 */
#ifndef PC_DP_HOST_WASM_ABSOLUTES_H
#define PC_DP_HOST_WASM_ABSOLUTES_H
#endif
