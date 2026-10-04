/* Cascaded one-pole low-pass (see lowpass.h). */
#include "lowpass.h"

#include <math.h>
#include <string.h>

void np_lowpass_config(np_lowpass *f, int stages, uint32_t rate)
{
    if (stages < 0)
        stages = 0;
    if (stages > NP_LOWPASS_MAX_STAGES)
        stages = NP_LOWPASS_MAX_STAGES;
    if (f->stages == stages && f->rate == rate)
        return;
    f->stages = stages;
    f->rate = rate;
    /* y += a (x - y) with a = 1 - e^(-2 pi fc / fs): the exact one-pole
     * response at fc. */
    f->alpha = rate ? 1.0f - expf(-2.0f * 3.14159265f * NP_LOWPASS_CUTOFF_HZ / (float)rate) : 1.0f;
    memset(f->y, 0, sizeof f->y);
}

void np_lowpass_run(np_lowpass *f, int16_t *samples, size_t frames)
{
    if (!f->stages)
        return;
    const float a = f->alpha;
    for (size_t i = 0; i < frames; i++)
        for (int c = 0; c < 2; c++) {
            float x = samples[2 * i + c];
            for (int s = 0; s < f->stages; s++) {
                f->y[s][c] += a * (x - f->y[s][c]);
                x = f->y[s][c];
            }
            /* A one-pole low-pass never overshoots its input range. */
            samples[2 * i + c] = (int16_t)lrintf(x);
        }
}
