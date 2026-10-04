/*
 * Core audio -> SDL audio stream. The core produces 16-bit stereo at
 * np_core_audio_rate() (32728 Hz); SDL resamples to the device.
 *
 * Clock drift: the guest runs on the shell's frame clock, not the audio
 * device's, so the queue slowly grows or drains. A small feedback on the
 * stream's frequency ratio (at most +-0.5%, inaudible) holds the queue near
 * a ~60 ms target. With the 60 Hz logic clock the guest makes audio
 * 60/59.8261 faster, which the base ratio absorbs.
 *
 * Fast-forward: audio is time-stretched by dropping. Every chunk the core
 * produced is drained, but it is only queued while the device queue is
 * below the target; the rest is discarded. The player hears short
 * normal-pitch snippets instead of chipmunk audio, and latency stays bounded.
 */
#include "app.h"

#define TARGET_FRAMES 2048u
#define CHUNK_FRAMES 2048u

int np_audio_open(np_app *app)
{
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO))
        return -1;
    SDL_AudioSpec spec = {SDL_AUDIO_S16, 2, 32728};
    app->audio = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
    if (!app->audio)
        return -1;
    np_audio_update_gain(app);
    return 0;
}

void np_audio_close(np_app *app)
{
    if (app->audio)
        SDL_DestroyAudioStream(app->audio);
    app->audio = NULL;
}

void np_audio_update_gain(np_app *app)
{
    if (!app->audio)
        return;
    float gain = (float)app->opt.volume / 100.0f;
    if (app->opt.mute_unfocused && !app->focused)
        gain = 0.0f;
    SDL_SetAudioStreamGain(app->audio, gain);
}

void np_audio_set_paused(np_app *app, int paused)
{
    if (!app->audio)
        return;
    if (paused) {
        SDL_PauseAudioStreamDevice(app->audio);
        /* Stale audio would play as a burst on resume. */
        SDL_ClearAudioStream(app->audio);
    } else {
        SDL_ResumeAudioStreamDevice(app->audio);
    }
}

void np_audio_pump(np_app *app, int speed)
{
    if (!app->core)
        return;
    /* The rate is 0 until the core's first frame; follow it if it differs
     * from the stream's input format. */
    uint32_t rate = np_core_audio_rate(app->core);
    if (app->audio && rate) {
        SDL_AudioSpec spec;
        if (SDL_GetAudioStreamFormat(app->audio, &spec, NULL) && spec.freq != (int)rate) {
            spec.freq = (int)rate;
            SDL_SetAudioStreamFormat(app->audio, &spec, NULL);
        }
    }
    int16_t buf[CHUNK_FRAMES * 2];
    size_t got;
    while ((got = np_core_audio_read(app->core, buf, CHUNK_FRAMES)) > 0) {
        if (!app->audio)
            continue;
        uint32_t queued = (uint32_t)SDL_GetAudioStreamQueued(app->audio) / 4u;
        if (speed != 1 && queued >= TARGET_FRAMES)
            continue; /* fast-forward: drop */
        SDL_PutAudioStreamData(app->audio, buf, (int)(got * 4));
    }
    if (!app->audio)
        return;

    uint32_t queued = (uint32_t)SDL_GetAudioStreamQueued(app->audio) / 4u;
    if (queued > TARGET_FRAMES * 6u) {
        /* Far behind (a stall, or a burst after a hitch): resync. */
        SDL_ClearAudioStream(app->audio);
        queued = 0;
    }
    float base = app->opt.logic_clock_60 && speed == 1 ? 60.0f / 59.8261f : 1.0f;
    float err = ((float)queued - (float)TARGET_FRAMES) / (float)TARGET_FRAMES;
    if (err > 1.0f)
        err = 1.0f;
    if (err < -1.0f)
        err = -1.0f;
    SDL_SetAudioStreamFrequencyRatio(app->audio, speed == 1 ? base * (1.0f + 0.005f * err) : 1.0f);
}
