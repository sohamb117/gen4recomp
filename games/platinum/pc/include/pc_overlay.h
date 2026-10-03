/*
 * What the overlay-statics reset has done so far.
 *
 * pc_fs_overlay.c traces itself under PC_TRACE_OVERLAY, which is enough on a
 * host with an environment. A console has none, so the numbers are kept and
 * this hands them to whatever writes that host's report. Read-only.
 */

#ifndef POKEPLATINUM_PC_OVERLAY_H
#define POKEPLATINUM_PC_OVERLAY_H

#ifdef __cplusplus
extern "C" {
#endif

/*
 * `slots` is how many writable statics were resolved, or -1 if nothing has
 * loaded an overlay yet. The rest count overlay loads that found slots, the
 * ones that restored a snapshot rather than taking the first, the bytes
 * copied back, and how many of those bytes had actually changed; which is
 * the number that says the reset was doing work rather than running.
 */
void pc_ov_totals(int *slots, unsigned *reloads, unsigned *restores,
                  unsigned *bytes, unsigned *dirty);

#ifdef __cplusplus
}
#endif

#endif /* POKEPLATINUM_PC_OVERLAY_H */
