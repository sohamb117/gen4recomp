/*
 * Optional output low-pass ("Music filter" in Options), Gen1Recomp's
 * OFF / 1X / 2X / 3X: 1 to 3 cascaded one-pole low-passes at 6 kHz on the
 * core's stereo output. It takes the edge off the SPU's raw PSG and
 * square-ish voices the way a DS speaker or cheap headphones did; each step
 * steepens the roll-off (-6, -12, -18 dB per octave) without moving the
 * corner, so voices keep their pitch and level below it.
 *
 * The core mixes music and effects into one stream, so the filter applies
 * to both. SDL-free and unit-tested.
 */
#ifndef NP_LOWPASS_H
#define NP_LOWPASS_H

#include <stddef.h>
#include <stdint.h>

#define NP_LOWPASS_MAX_STAGES 3
#define NP_LOWPASS_CUTOFF_HZ 6000.0f

typedef struct np_lowpass {
    int stages;   /* 0 = off */
    float alpha;  /* per-sample coefficient for the rate */
    uint32_t rate;
    float y[NP_LOWPASS_MAX_STAGES][2]; /* stage outputs per channel */
} np_lowpass;

/* Sets the strength (0..3) and the sample rate; resets the state when either
 * changes. */
void np_lowpass_config(np_lowpass *f, int stages, uint32_t rate);
/* Filters `frames` interleaved stereo frames in place. */
void np_lowpass_run(np_lowpass *f, int16_t *samples, size_t frames);

#endif
