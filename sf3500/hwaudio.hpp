/* Real audio via driver_sf3500.so's sound_driver_* functions - see hwaudio.cpp
 * for the calling-convention and clean-cycle fixes (confirmed from
 * picoarch's own production source).
 *
 * Playback runs on a DEDICATED background thread, mirroring picoarch's own
 * architecture (sf3000_audio_thread_fn in plat_sdl.c): a ring buffer the
 * game loop writes into non-blocking, drained in steady 480-frame/10ms
 * chunks by a thread that wakes on its own schedule. */
#ifndef SF3000_HWAUDIO_H
#define SF3000_HWAUDIO_H

#define HWAUDIO_RATE  48000
#define HWAUDIO_CHUNK 480   /* frames (10ms @ 48000Hz), fixed by the driver */

/* Ring sizing - the single shared source of truth for both hwaudio.cpp's own
 * buffer and any caller's backlog-cap math. Must stay a power of two -
 * hwaudio.cpp masks the ring index with (HWAUDIO_RING_FRAMES - 1). */
#define HWAUDIO_RING_FRAMES    65536
#define HWAUDIO_RING_SOFT_CAP  (HWAUDIO_RING_FRAMES * 3 / 4)

#ifdef __cplusplus
extern "C" {
#endif

int  hwaudio_init(void);
int  hwaudio_active(void);

/* Non-blocking: copies into the ring buffer and returns immediately. May
 * accept fewer than `frames` if the ring already has enough cushion ahead of
 * the consumer - returns the number ACTUALLY accepted. Callers that track
 * their own cumulative output total MUST advance by the return value, not
 * the requested count. */
int hwaudio_write(const short *buf, int frames);

void hwaudio_deinit(void);

void hwaudio_get_stats(unsigned *underruns, unsigned *dropped, unsigned *fill);

#ifdef __cplusplus
}
#endif

#endif
