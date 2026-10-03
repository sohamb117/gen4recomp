#ifndef POKEPLATINUM_PC_PROF_H
#define POKEPLATINUM_PC_PROF_H

/* PC_PROF=path[:hz], start the SIGPROF EIP sampler if asked for.
 * See pc_prof.c; pc/prof_fold.py folds the samples into cost centres. */
void pc_prof_init(void);

#include <stdint.h>

/* A frame boundary in the sample stream: {0, work_us} appended as one
 * write (EIP 0 is never a sample; nothing executes at the null page).
 * Called by the pacer's trace with each period's work time, so the fold
 * can keep only the samples of frames past a threshold (--spike-us): a
 * spike on one frame in twenty is diluted 25:1 in a whole-run profile
 * and unmissable in its own. A no-op unless PC_PROF is armed. */
void pc_prof_frame(uint32_t work_us);

#endif /* POKEPLATINUM_PC_PROF_H */
