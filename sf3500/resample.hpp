/* Sample-rate conversion for streamed music: a windowed-sinc interpolator.
 *
 * The mixer used to convert a track's rate (44100 Hz) to the output's (48000 Hz) by
 * drawing a straight line between the two nearest samples. That is the cheapest
 * way, but it lets false overtones into the treble (strings and brass come out
 * metallic). Here every output sample is the sum of the 32 nearest input samples,
 * weighted by a low-pass (sinc) curve that is faded out at its ends with a
 * Blackman-Harris window, with the curve looked up in a table of fine sub-sample
 * positions. */
#ifndef WC_RESAMPLE_H
#define WC_RESAMPLE_H

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace wcresample {

constexpr int PHASES = 1024;       /* positions between two input samples */
constexpr int BASE_HALF_TAPS = 16; /* input samples used each side of the output position */

struct Kernel {
  double ratio = 0.0; /* input rate / output rate the table was built for */
  int half = BASE_HALF_TAPS;
  std::vector<float> table; /* PHASES x (2 * half) */
};

inline double blackman_harris(double x) /* x in -1..1 */
{
  if (x <= -1.0 || x >= 1.0) return 0.0;
  const double a0 = 0.35875, a1 = 0.48829, a2 = 0.14128, a3 = 0.01168;
  const double t = M_PI * (x + 1.0); /* 0..2pi */
  return a0 - a1 * std::cos(t) + a2 * std::cos(2.0 * t) - a3 * std::cos(3.0 * t);
}

inline void build(Kernel &k, double ratio)
{
  k.ratio = ratio;
  /* when going down in rate the curve has to be wider, to cut at the output's
   * Nyquist rather than the input's */
  const double scale = ratio > 1.0 ? ratio : 1.0;
  k.half = static_cast<int>(std::ceil(BASE_HALF_TAPS * scale));
  const double fc = 0.94 / scale; /* cut-off, as a fraction of the input Nyquist */
  const int taps = 2 * k.half;
  k.table.assign(static_cast<size_t>(PHASES) * taps, 0.0f);

  for (int p = 0; p < PHASES; p++) {
    const double t = static_cast<double>(p) / PHASES;
    double sum = 0.0;
    double *tmp = new double[taps];
    for (int j = 0; j < taps; j++) {
      const double d = (j - (k.half - 1)) - t; /* distance from the output position */
      double s;
      if (std::fabs(d) < 1e-9) {
        s = fc;
      } else {
        s = std::sin(M_PI * fc * d) / (M_PI * d);
      }
      tmp[j] = s * blackman_harris(d / k.half);
      sum += tmp[j];
    }
    /* every position adds up to exactly 1, so a steady level stays steady */
    for (int j = 0; j < taps; j++) {
      k.table[static_cast<size_t>(p) * taps + j] = static_cast<float>(tmp[j] / sum);
    }
    delete[] tmp;
  }
}

/* One output frame at input position pos (in input samples, counted from the start
 * of buf). buf holds `count` frames of `channels` (1 or 2) 16-bit samples; samples
 * outside it count as silence. */
inline void frame(const Kernel &k, const int16_t *buf, int channels, int count, double pos, float *outL, float *outR)
{
  int i0 = static_cast<int>(pos);
  int phase = static_cast<int>((pos - i0) * PHASES + 0.5);
  if (phase >= PHASES) {
    phase -= PHASES;
    i0++;
  }

  const int taps = 2 * k.half;
  const float *w = &k.table[static_cast<size_t>(phase) * taps];
  const int first = i0 - (k.half - 1);
  float l = 0.0f, r = 0.0f;

  if (first >= 0 && first + taps <= count) {
    if (channels == 1) {
      for (int j = 0; j < taps; j++) l += w[j] * buf[first + j];
      r = l;
    } else {
      const int16_t *s = buf + static_cast<size_t>(first) * 2;
      for (int j = 0; j < taps; j++) {
        l += w[j] * s[j * 2];
        r += w[j] * s[j * 2 + 1];
      }
    }
  } else {
    for (int j = 0; j < taps; j++) {
      const int idx = first + j;
      if (idx < 0 || idx >= count) continue;
      if (channels == 1) {
        l += w[j] * buf[idx];
      } else {
        l += w[j] * buf[idx * 2];
        r += w[j] * buf[idx * 2 + 1];
      }
    }
    if (channels == 1) r = l;
  }

  *outL = l;
  *outR = r;
}

} // namespace wcresample

#endif
