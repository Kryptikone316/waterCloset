#include "hwaudio.hpp"
#include "tfenv.h"

#include <stdio.h>
#include <string.h>
#include <dlfcn.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>
#include <sched.h>

static void  *g_handle = NULL;
static int    g_active = 0;

typedef int (*fn_init_t)(void *device_name, int sample_rate, int channels);
typedef int (*fn_play_t)(const void *buffer, int frame_count);
typedef int (*fn_deinit_t)(void);

static fn_init_t   p_init   = NULL;
static fn_play_t   p_play   = NULL;
static fn_deinit_t p_deinit = NULL;

#define RING_MASK (HWAUDIO_RING_FRAMES - 1)
static short ring[HWAUDIO_RING_FRAMES * 2];
static unsigned ring_w = 0, ring_r = 0;
static pthread_mutex_t ring_mtx = PTHREAD_MUTEX_INITIALIZER;

static pthread_t audio_thread;
static volatile int thread_running = 0;

static volatile unsigned g_underrun_count = 0;
static volatile unsigned g_dropped_frames = 0;

static short g_last_l = 0, g_last_r = 0;
static int   g_was_underrun = 0;

static void *audio_thread_fn(void *unused) {
    struct timespec next_wake;
    long period_ns;
    short chunk[HWAUDIO_CHUNK * 2];
    (void)unused;
    period_ns = (long)((long long)HWAUDIO_CHUNK * 1000000000LL / HWAUDIO_RATE); /* 10ms */

    clock_gettime(CLOCK_MONOTONIC, &next_wake);

    while (thread_running) {
        next_wake.tv_nsec += period_ns;
        if (next_wake.tv_nsec >= 1000000000L) {
            next_wake.tv_nsec -= 1000000000L;
            next_wake.tv_sec += 1;
        }
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next_wake, NULL);

        pthread_mutex_lock(&ring_mtx);
        if (ring_w - ring_r >= (unsigned)HWAUDIO_CHUNK) {
            int i;
            for (i = 0; i < HWAUDIO_CHUNK; i++) {
                unsigned idx = (ring_r + i) & RING_MASK;
                chunk[i * 2]     = ring[idx * 2];
                chunk[i * 2 + 1] = ring[idx * 2 + 1];
            }
            ring_r += HWAUDIO_CHUNK;

            if (g_was_underrun) {
                for (i = 0; i < HWAUDIO_CHUNK; i++) {
                    int scale = (i * 256) / HWAUDIO_CHUNK;
                    chunk[i * 2]     = (short)((chunk[i * 2]     * scale) >> 8);
                    chunk[i * 2 + 1] = (short)((chunk[i * 2 + 1] * scale) >> 8);
                }
                g_was_underrun = 0;
            }
            g_last_l = chunk[(HWAUDIO_CHUNK - 1) * 2];
            g_last_r = chunk[(HWAUDIO_CHUNK - 1) * 2 + 1];
        } else {
            int i;
            for (i = 0; i < HWAUDIO_CHUNK; i++) {
                int scale = 256 - (i * 256) / HWAUDIO_CHUNK;
                chunk[i * 2]     = (short)((g_last_l * scale) >> 8);
                chunk[i * 2 + 1] = (short)((g_last_r * scale) >> 8);
            }
            g_was_underrun = 1;
            g_last_l = 0;
            g_last_r = 0;
            g_underrun_count++;
        }
        pthread_mutex_unlock(&ring_mtx);

        p_play(chunk, HWAUDIO_CHUNK);
    }
    return NULL;
}

static int try_init(void) {
    int rc = -1;
    int attempt;
    for (attempt = 0; attempt < 20; attempt++) {
        rc = p_init(NULL, HWAUDIO_RATE, 2);
        if (rc >= 0) break;
        usleep(100 * 1000);
    }
    return rc;
}

int hwaudio_init(void) {
    if (g_active) return 0;

    g_handle = dlopen(tfdriver_path(), RTLD_NOW | RTLD_GLOBAL);
    if (!g_handle) {
        fprintf(stderr, "hwaudio: dlopen %s failed: %s\n", tfdriver_path(), dlerror());
        return -1;
    }

    p_init   = (fn_init_t)  dlsym(g_handle, "sound_driver_init");
    p_play   = (fn_play_t)  dlsym(g_handle, "sound_driver_playframe");
    p_deinit = (fn_deinit_t)dlsym(g_handle, "sound_driver_deinit");

    if (!p_init || !p_play) {
        fprintf(stderr, "hwaudio: dlsym failed (init=%p play=%p)\n", (void*)p_init, (void*)p_play);
        dlclose(g_handle); g_handle = NULL;
        return -1;
    }

    if (try_init() < 0) {
        fprintf(stderr, "hwaudio: initial sound_driver_init failed\n");
        dlclose(g_handle); g_handle = NULL;
        return -1;
    }
    fprintf(stderr, "hwaudio: clean AUDDEC/I2SO startup cycle\n");
    if (p_deinit) p_deinit();
    usleep(100 * 1000);

    if (try_init() < 0) {
        fprintf(stderr, "hwaudio: second sound_driver_init failed\n");
        dlclose(g_handle); g_handle = NULL;
        return -1;
    }

    ring_w = ring_r = 0;
    thread_running = 1;
    {
        pthread_attr_t attr;
        struct sched_param sp;
        int created;

        pthread_attr_init(&attr);
        pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
        pthread_attr_setschedpolicy(&attr, SCHED_FIFO);
        sp.sched_priority = sched_get_priority_max(SCHED_FIFO) - 1;
        pthread_attr_setschedparam(&attr, &sp);

        created = pthread_create(&audio_thread, &attr, audio_thread_fn, NULL);
        if (created != 0) {
            fprintf(stderr, "hwaudio: SCHED_FIFO thread create failed (%d), falling back to default scheduling\n", created);
            created = pthread_create(&audio_thread, NULL, audio_thread_fn, NULL);
        }
        pthread_attr_destroy(&attr);

        if (created != 0) {
            fprintf(stderr, "hwaudio: failed to start audio thread\n");
            thread_running = 0;
            if (p_deinit) p_deinit();
            dlclose(g_handle); g_handle = NULL;
            return -1;
        }
    }

    g_active = 1;
    fprintf(stderr, "hwaudio: active (rate=%d chunk=%d, dedicated thread)\n", HWAUDIO_RATE, HWAUDIO_CHUNK);
    return 0;
}

int hwaudio_active(void) { return g_active; }

int hwaudio_write(const short *buf, int frames) {
    int i;
    if (!g_active) return 0;

    pthread_mutex_lock(&ring_mtx);
    for (i = 0; i < frames; i++) {
        if (ring_w - ring_r >= HWAUDIO_RING_SOFT_CAP) {
            break;
        }
        unsigned idx = ring_w & RING_MASK;
        ring[idx * 2]     = buf[i * 2];
        ring[idx * 2 + 1] = buf[i * 2 + 1];
        ring_w++;
    }
    if (i < frames) g_dropped_frames += (frames - i);
    pthread_mutex_unlock(&ring_mtx);
    return i;
}

void hwaudio_get_stats(unsigned *underruns, unsigned *dropped, unsigned *fill) {
    pthread_mutex_lock(&ring_mtx);
    if (underruns) *underruns = g_underrun_count;
    if (dropped)   *dropped   = g_dropped_frames;
    if (fill)       *fill      = ring_w - ring_r;
    pthread_mutex_unlock(&ring_mtx);
}

void hwaudio_deinit(void) {
    if (!g_active) return;
    thread_running = 0;
    pthread_join(audio_thread, NULL);
    if (p_deinit) p_deinit();
    if (g_handle) { dlclose(g_handle); g_handle = NULL; }
    p_init = NULL; p_play = NULL; p_deinit = NULL;
    g_active = 0;
}
