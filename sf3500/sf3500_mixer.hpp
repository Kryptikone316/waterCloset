/* Direct software mixer -> hwaudio_write(), replacing OpenAL (no working
 * ALSA/OSS backend exists on this device - see supertux_handoff memory) and
 * replacing SDL_mixer's own Mix_* API (which would need a real SDL audio
 * device, which also doesn't exist here - same dead end, confirmed by a
 * prior port's own sf3500/audio.c comment on this exact device).
 *
 * Shape is the same proven pattern as every other port on this device: one
 * background thread, paced off wall-clock time (not call count), mixes a
 * fixed pool of voices into a buffer and pushes it through hwaudio_write().
 *
 * Two voice kinds:
 *  - "static": caller hands a fully-decoded, already-48kHz-stereo-S16 PCM
 *    buffer it owns for the voice's lifetime (SoundManager's own buffer
 *    cache plays this role, same as the old AL buffer-id reuse). Cheap to
 *    mix: no per-voice resampling in the hot path.
 *  - "stream": caller supplies a pull callback that decodes more native-rate
 *    PCM on demand (music, and the rare oversized one-shot). Resampled to
 *    the mix rate per-voice, with persistent FIFO/interpolation state so
 *    small irregular pulls don't reintroduce the drift/discontinuity bugs
 *    documented for every earlier stream-audio port on this device.
 *
 * Panning is a simple, deliberately non-physical left/right gain split based
 * on (voice x - listener x); there is no distance attenuation or doppler.
 * ponytail: good enough for a 2.5" handheld's stereo image, upgrade to real
 * attenuation only if it's ever audibly missed. */
#ifndef SF3500_MIXER_H
#define SF3500_MIXER_H

/* No <cstdint>/<stdint.h> here on purpose - same reason as hwdisp.hpp:
 * conflicts with this SDL1.2 build's own SDL_config_minimal.h typedef when
 * both land in one TU (e.g. this port's frontend.cpp, which needs SDL.h).
 * `short` is guaranteed 16-bit on this target, same ABI as int16_t. */

/* `bool` is a C++ keyword but needs stdbool.h in C (pre-C23) - this header
 * is included directly from plain-C ports too (e.g. watercloset's
 * input.c/init.c/sound.c), so pull it in here once rather than relying on
 * every C consumer to remember it. A no-op when already in C++. */
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct Sf3500Voice Sf3500Voice;

/* Called from the mixer's own background thread. Must write up to
 * `frames_wanted` interleaved `channels`-channel S16 frames at `native_rate`
 * (the rate/channels given at voice-creation time, fixed for the voice's
 * life) into `dst` and return the number of frames actually written.
 * Returning fewer than requested is only valid at genuine end-of-stream for
 * a non-looping voice - the mixer treats a short result as EOF. */
typedef int (*Sf3500StreamFillFn)(void *userdata, short *dst, int frames_wanted);

bool sf3500_mixer_init(void);
void sf3500_mixer_deinit(void);
bool sf3500_mixer_active(void);

/* Simple stereo-pan reference point; see panning note above. */
void sf3500_mixer_set_listener_x(float x);

/* `pcm` must stay valid for the voice's entire lifetime (caller-owned, not
 * copied) and must already be 48000Hz interleaved stereo S16. */
Sf3500Voice *sf3500_voice_create_static(const short *pcm, int frames);

/* `fill` is invoked from the mixer thread only; it owns all decode state. */
Sf3500Voice *sf3500_voice_create_stream(Sf3500StreamFillFn fill, void *userdata,
                                         int native_rate, int native_channels);

void sf3500_voice_destroy(Sf3500Voice *v);

void sf3500_voice_play(Sf3500Voice *v);
void sf3500_voice_stop(Sf3500Voice *v);
void sf3500_voice_pause(Sf3500Voice *v);
void sf3500_voice_resume(Sf3500Voice *v);
bool sf3500_voice_playing(Sf3500Voice *v);
bool sf3500_voice_paused(Sf3500Voice *v);

void sf3500_voice_set_looping(Sf3500Voice *v, bool looping);
/* 0 = silent, 1 = nominal. May be set continuously (e.g. for fades). */
void sf3500_voice_set_gain(Sf3500Voice *v, float gain);
/* true = always centered (no pan), false = panned from set_x() vs the
 * listener x. */
void sf3500_voice_set_relative(Sf3500Voice *v, bool relative);
void sf3500_voice_set_x(Sf3500Voice *v, float x);

/* Re-seeks a stream voice to its start (calls the fill callback's own loop
 * logic - this just resets the mixer-side FIFO/interpolation state so a
 * restart isn't glued to stale resample phase). Has no effect on a static
 * voice (it has no position of its own, playback is driven by play/stop). */
void sf3500_voice_reset_stream(Sf3500Voice *v);

#ifdef __cplusplus
}
#endif

#endif
