#include "sf3500_mixer.hpp"
#include "hwaudio.hpp"

#include <pthread.h>
#include <time.h>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {

constexpr int MAX_VOICES = 32;
/* Arbitrary stereo-image width in "world units" (SuperTux position space is
 * pixels); old AL code used reference_distance=128 in the same space as a
 * rough scale cue, reused here as the pan falloff range. */
constexpr float PAN_RANGE = 400.0f;
constexpr long PUMP_PERIOD_NS = 20000000L; /* wake every 20ms */

enum class VoiceState { Free, Stopped, Playing, Paused };
enum class VoiceKind { Static, Stream };

struct Voice
{
  VoiceState state = VoiceState::Free;
  VoiceKind kind = VoiceKind::Static;

  float gain = 1.0f;
  bool relative = true;
  float x = 0.0f;
  bool looping = false;

  // VoiceKind::Static
  const int16_t *s_pcm = nullptr;
  int s_frames = 0;
  int s_pos = 0;

  // VoiceKind::Stream
  Sf3500StreamFillFn str_fill = nullptr;
  void *str_userdata = nullptr;
  int str_native_rate = 48000;
  int str_native_channels = 2;
  std::vector<int16_t> str_fifo;
  int str_fifo_count = 0;
  double str_fifo_pos = 0.0;
  bool str_eof = false;
};

Voice g_voices[MAX_VOICES];
pthread_mutex_t g_mtx = PTHREAD_MUTEX_INITIALIZER;

float g_listener_x = 0.0f;

pthread_t g_thread;
volatile int g_running = 0;
bool g_init_ok = false;

int16_t *g_mixbuf = nullptr;

inline Sf3500Voice *to_handle(Voice *v) { return reinterpret_cast<Sf3500Voice *>(v); }
inline Voice *from_handle(Sf3500Voice *h) { return reinterpret_cast<Voice *>(h); }

void compute_gains(const Voice &v, float *out_l, float *out_r)
{
  if (v.relative) {
    *out_l = v.gain;
    *out_r = v.gain;
    return;
  }
  float pan = (v.x - g_listener_x) / PAN_RANGE;
  if (pan < -1.0f) pan = -1.0f;
  if (pan > 1.0f) pan = 1.0f;
  *out_l = v.gain * (1.0f - (pan > 0.0f ? pan : 0.0f));
  *out_r = v.gain * (1.0f - (pan < 0.0f ? -pan : 0.0f));
}

inline void mix_add(int16_t *out, int i, float l, float r, float gl, float gr)
{
  int ol = out[i * 2]     + static_cast<int>(l * gl);
  int orr = out[i * 2 + 1] + static_cast<int>(r * gr);
  if (ol > 32767) ol = 32767; else if (ol < -32768) ol = -32768;
  if (orr > 32767) orr = 32767; else if (orr < -32768) orr = -32768;
  out[i * 2]     = static_cast<int16_t>(ol);
  out[i * 2 + 1] = static_cast<int16_t>(orr);
}

void mix_static_voice(Voice &v, int16_t *out, int frames)
{
  float gl, gr;
  compute_gains(v, &gl, &gr);

  int out_i = 0;
  int n = frames;
  while (n > 0) {
    int avail = v.s_frames - v.s_pos;
    if (avail <= 0) {
      if (v.looping && v.s_frames > 0) {
        v.s_pos = 0;
        avail = v.s_frames;
      } else {
        v.state = VoiceState::Stopped;
        break;
      }
    }
    int take = n < avail ? n : avail;
    for (int i = 0; i < take; i++) {
      int16_t l = v.s_pcm[(v.s_pos + i) * 2];
      int16_t r = v.s_pcm[(v.s_pos + i) * 2 + 1];
      mix_add(out, out_i + i, l, r, gl, gr);
    }
    v.s_pos += take;
    out_i += take;
    n -= take;
  }
}

/* Tops up v's fifo with freshly decoded native-rate frames up to
 * `want_frames`. */
void stream_fill(Voice &v, int want_frames)
{
  int need_cap = (want_frames + 64) * v.str_native_channels;
  if (need_cap > static_cast<int>(v.str_fifo.size())) {
    v.str_fifo.resize(need_cap);
  }

  while (v.str_fifo_count < want_frames && !v.str_eof) {
    int16_t *dst = v.str_fifo.data() + static_cast<size_t>(v.str_fifo_count) * v.str_native_channels;
    int got = v.str_fill(v.str_userdata, dst, want_frames - v.str_fifo_count);
    if (got <= 0) {
      v.str_eof = true;
      break;
    }
    v.str_fifo_count += got;
  }
}

/* Drops whole frames already consumed (fifo_pos past 1.0) from the front of
 * the fifo so it doesn't grow without bound. */
void stream_drain_consumed(Voice &v)
{
  int consumed = static_cast<int>(v.str_fifo_pos);
  if (consumed <= 0) return;
  if (consumed > v.str_fifo_count) consumed = v.str_fifo_count;
  int remaining = v.str_fifo_count - consumed;
  if (remaining > 0) {
    std::memmove(v.str_fifo.data(),
                 v.str_fifo.data() + static_cast<size_t>(consumed) * v.str_native_channels,
                 static_cast<size_t>(remaining) * v.str_native_channels * sizeof(int16_t));
  }
  v.str_fifo_count = remaining;
  v.str_fifo_pos -= consumed;
}

void mix_stream_voice(Voice &v, int16_t *out, int frames)
{
  float gl, gr;
  compute_gains(v, &gl, &gr);

  double ratio = static_cast<double>(v.str_native_rate) / HWAUDIO_RATE;
  int need = static_cast<int>(v.str_fifo_pos + frames * ratio) + 2;
  stream_fill(v, need);

  for (int i = 0; i < frames; i++) {
    double pos = v.str_fifo_pos + i * ratio;
    int i0 = static_cast<int>(pos);
    double frac = pos - i0;
    int i1 = i0 + 1;

    int16_t l0, r0, l1, r1;
    if (v.str_fifo_count == 0) {
      l0 = r0 = l1 = r1 = 0;
    } else {
      if (i0 >= v.str_fifo_count) i0 = v.str_fifo_count - 1;
      if (i1 >= v.str_fifo_count) i1 = i0;
      if (v.str_native_channels == 1) {
        l0 = r0 = v.str_fifo[i0];
        l1 = r1 = v.str_fifo[i1];
      } else {
        l0 = v.str_fifo[i0 * 2];     r0 = v.str_fifo[i0 * 2 + 1];
        l1 = v.str_fifo[i1 * 2];     r1 = v.str_fifo[i1 * 2 + 1];
      }
    }

    int16_t l = static_cast<int16_t>(l0 + (l1 - l0) * frac);
    int16_t r = static_cast<int16_t>(r0 + (r1 - r0) * frac);
    mix_add(out, i, l, r, gl, gr);
  }

  v.str_fifo_pos += frames * ratio;
  stream_drain_consumed(v);

  if (v.str_eof && v.str_fifo_count == 0) {
    v.state = VoiceState::Stopped;
  }
}

void *pump_thread_fn(void *)
{
  struct timespec start, now;
  long long out_frames_produced = 0;

  clock_gettime(CLOCK_MONOTONIC, &start);

  const int max_batch = HWAUDIO_RING_SOFT_CAP;
  g_mixbuf = static_cast<int16_t *>(std::malloc(static_cast<size_t>(max_batch) * 2 * sizeof(int16_t)));

  struct timespec next_wake = start;
  while (g_running) {
    next_wake.tv_nsec += PUMP_PERIOD_NS;
    if (next_wake.tv_nsec >= 1000000000L) {
      next_wake.tv_nsec -= 1000000000L;
      next_wake.tv_sec += 1;
    }
    clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next_wake, nullptr);

    clock_gettime(CLOCK_MONOTONIC, &now);
    long long elapsed_ns = static_cast<long long>(now.tv_sec - start.tv_sec) * 1000000000LL
                         + (now.tv_nsec - start.tv_nsec);
    long long out_target = elapsed_ns * HWAUDIO_RATE / 1000000000LL;

    long long want_ll = out_target - out_frames_produced;
    if (want_ll > max_batch) {
      out_frames_produced = out_target - max_batch;
      want_ll = max_batch;
    }
    if (want_ll <= 0) continue;
    int want = static_cast<int>(want_ll);

    std::memset(g_mixbuf, 0, static_cast<size_t>(want) * 2 * sizeof(int16_t));

    pthread_mutex_lock(&g_mtx);
    for (auto &v : g_voices) {
      if (v.state != VoiceState::Playing) continue;
      if (v.kind == VoiceKind::Static) {
        mix_static_voice(v, g_mixbuf, want);
      } else {
        mix_stream_voice(v, g_mixbuf, want);
      }
    }
    pthread_mutex_unlock(&g_mtx);

    int accepted = hwaudio_write(g_mixbuf, want);
    out_frames_produced += accepted;
  }

  std::free(g_mixbuf);
  g_mixbuf = nullptr;
  return nullptr;
}

Voice *alloc_voice()
{
  for (auto &v : g_voices) {
    if (v.state == VoiceState::Free) {
      v = Voice();
      v.state = VoiceState::Stopped;
      return &v;
    }
  }
  return nullptr;
}

} // namespace

bool sf3500_mixer_init(void)
{
  if (g_init_ok) return true;
  if (hwaudio_init() < 0) return false;

  g_running = 1;
  if (pthread_create(&g_thread, nullptr, pump_thread_fn, nullptr) != 0) {
    g_running = 0;
    hwaudio_deinit();
    return false;
  }
  g_init_ok = true;
  return true;
}

void sf3500_mixer_deinit(void)
{
  if (!g_init_ok) return;
  g_running = 0;
  pthread_join(g_thread, nullptr);
  hwaudio_deinit();
  g_init_ok = false;
}

bool sf3500_mixer_active(void)
{
  return g_init_ok;
}

void sf3500_mixer_set_listener_x(float x)
{
  g_listener_x = x;
}

Sf3500Voice *sf3500_voice_create_static(const int16_t *pcm, int frames)
{
  pthread_mutex_lock(&g_mtx);
  Voice *v = alloc_voice();
  if (v) {
    v->kind = VoiceKind::Static;
    v->s_pcm = pcm;
    v->s_frames = frames;
    v->s_pos = 0;
  }
  pthread_mutex_unlock(&g_mtx);
  return to_handle(v);
}

Sf3500Voice *sf3500_voice_create_stream(Sf3500StreamFillFn fill, void *userdata,
                                         int native_rate, int native_channels)
{
  pthread_mutex_lock(&g_mtx);
  Voice *v = alloc_voice();
  if (v) {
    v->kind = VoiceKind::Stream;
    v->str_fill = fill;
    v->str_userdata = userdata;
    v->str_native_rate = native_rate;
    v->str_native_channels = native_channels;
    v->str_fifo_count = 0;
    v->str_fifo_pos = 0.0;
    v->str_eof = false;
  }
  pthread_mutex_unlock(&g_mtx);
  return to_handle(v);
}

void sf3500_voice_destroy(Sf3500Voice *h)
{
  if (!h) return;
  Voice *v = from_handle(h);
  pthread_mutex_lock(&g_mtx);
  *v = Voice();
  pthread_mutex_unlock(&g_mtx);
}

void sf3500_voice_play(Sf3500Voice *h)
{
  if (!h) return;
  Voice *v = from_handle(h);
  pthread_mutex_lock(&g_mtx);
  if (v->kind == VoiceKind::Static) v->s_pos = 0;
  v->state = VoiceState::Playing;
  pthread_mutex_unlock(&g_mtx);
}

void sf3500_voice_stop(Sf3500Voice *h)
{
  if (!h) return;
  Voice *v = from_handle(h);
  pthread_mutex_lock(&g_mtx);
  v->state = VoiceState::Stopped;
  pthread_mutex_unlock(&g_mtx);
}

void sf3500_voice_pause(Sf3500Voice *h)
{
  if (!h) return;
  Voice *v = from_handle(h);
  pthread_mutex_lock(&g_mtx);
  if (v->state == VoiceState::Playing) v->state = VoiceState::Paused;
  pthread_mutex_unlock(&g_mtx);
}

void sf3500_voice_resume(Sf3500Voice *h)
{
  if (!h) return;
  Voice *v = from_handle(h);
  pthread_mutex_lock(&g_mtx);
  if (v->state == VoiceState::Paused) v->state = VoiceState::Playing;
  pthread_mutex_unlock(&g_mtx);
}

bool sf3500_voice_playing(Sf3500Voice *h)
{
  if (!h) return false;
  Voice *v = from_handle(h);
  return v->state == VoiceState::Playing;
}

bool sf3500_voice_paused(Sf3500Voice *h)
{
  if (!h) return false;
  Voice *v = from_handle(h);
  return v->state == VoiceState::Paused;
}

void sf3500_voice_set_looping(Sf3500Voice *h, bool looping)
{
  if (!h) return;
  from_handle(h)->looping = looping;
}

void sf3500_voice_set_gain(Sf3500Voice *h, float gain)
{
  if (!h) return;
  from_handle(h)->gain = gain;
}

void sf3500_voice_set_relative(Sf3500Voice *h, bool relative)
{
  if (!h) return;
  from_handle(h)->relative = relative;
}

void sf3500_voice_set_x(Sf3500Voice *h, float x)
{
  if (!h) return;
  from_handle(h)->x = x;
}

void sf3500_voice_reset_stream(Sf3500Voice *h)
{
  if (!h) return;
  Voice *v = from_handle(h);
  pthread_mutex_lock(&g_mtx);
  if (v->kind == VoiceKind::Stream) {
    v->str_fifo_count = 0;
    v->str_fifo_pos = 0.0;
    v->str_eof = false;
  }
  pthread_mutex_unlock(&g_mtx);
}
