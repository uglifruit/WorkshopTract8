// TRACT8 - the Voder engine. See voder.h for the overview.
//
// Verified by: tools/filter_check.py, tools/excite_check.py

#include "voder.h"

#include <math.h>

#ifdef TRACT8_HOST_BUILD
  // The host test harness compiles this file straight onto a PC, where the
  // Pico attribute does not exist. Same shim as WorkshopSpectral's fft.h.
  #define __not_in_flash_func(f) f
#else
  #include "pico.h"
#endif

namespace tract8 {

// Coefficient template for the eight bands, filled by VoderInit(). Plain
// globals in the .cpp, not header statics: these are WRITTEN at init, and a
// header static would land in flash where writes are silently discarded.
// That failure mode is invisible until the filters produce nothing.
int32_t band_b0[kBands];
int32_t band_a1[kBands];
int32_t band_a2[kBands];

// Phase increment per milli-Hz, Q32. The sawtooth phase is a uint32_t that
// wraps naturally at 2^32, so the increment for frequency f is
//
//   inc = f * 2^32 / fs
//
// Precomputed here for f in milli-Hz to keep a divide off the hot path:
//
//   inc = f_mhz * 2^32 / (fs * 1000)
//
// 2^32 / 48000000 is 89.478, too small to hold usefully as an integer, so
// the multiply is done at runtime in 64 bits against a Q16 constant.
// 2^32 * 65536 / 48000000 = 5864062.5, rounded.
static constexpr uint64_t kIncPerMilliHz_Q16 = 5864063;

void VoderInit(uint32_t seed) {
  (void)seed;

  // Standard RBJ audio-EQ-cookbook constant-skirt bandpass, evaluated in
  // double and stored as Q15. This is the one place floats are allowed: it
  // runs once at boot, before Run() starts the audio interrupt, so the
  // ~360 ns per soft-float operation costs nothing that matters.
  //
  //   w0    = 2*pi*f0/fs
  //   alpha = sin(w0) / (2*Q)
  //   b0 =  alpha,  b1 = 0,  b2 = -alpha
  //   a0 =  1 + alpha,  a1 = -2*cos(w0),  a2 = 1 - alpha
  //
  // Everything is divided through by a0 and the signs of a1/a2 are flipped
  // so the difference equation is a straight sum of products:
  //
  //   y = b0*(x - x2) - a1*y1 - a2*y2
  //
  const double q = 4.0;  // kFilterQ_Q15 / 32768.0, spelled out for clarity

  for (int i = 0; i < kBands; i++) {
    const double w0 = 2.0 * M_PI * (double)kBandHz[i] / (double)kSampleRate;
    const double alpha = sin(w0) / (2.0 * q);
    const double a0 = 1.0 + alpha;

    const double b0 = alpha / a0;
    const double a1 = (-2.0 * cos(w0)) / a0;
    const double a2 = (1.0 - alpha) / a0;

    // Q15, i.e. a scale of 32768. The largest coefficient across the bank
    // is a1 for the 250 Hz band at -1.99079, so the stored values reach
    // +/-65234 - well inside int32, and the int64 accumulator in Process()
    // absorbs the products.
    //
    // Q13 was tried first and is NOT enough: tools/filter_check.py measured
    // the 250 Hz band landing at 239 Hz, a 74-cent error, with 0.6 dB of
    // level lost. The cause is that a1 approaches -2.0 as the centre
    // frequency falls, so the coefficient needs resolution exactly where
    // Q13 has least to give. Q15 brings that error to 1.4 cents. Do not
    // reduce this shift without re-running the test.
    band_b0[i] = (int32_t)lrint(b0 * 32768.0);
    band_a1[i] = (int32_t)lrint(a1 * 32768.0);
    band_a2[i] = (int32_t)lrint(a2 * 32768.0);
  }
}

void Voder::Init(uint32_t seed) {
  for (int i = 0; i < kBands; i++) {
    bank_[i].b0 = band_b0[i];
    bank_[i].a1 = band_a1[i];
    bank_[i].a2 = band_a2[i];
    bank_[i].x1 = bank_[i].x2 = 0;
    bank_[i].y1 = bank_[i].y2 = 0;
  }
  phase_ = 0;
  phase_inc_ = 0;
  rng_ = seed ? seed : 0xDEADBEEFu;  // xorshift must never be zero
  plosive_ = 0;
  gate_env_ = 0;
  noise_env_ = 0;
  energy_ = 0;
  voiced_ = false;
}

uint32_t __not_in_flash_func(Voder::Random)() {
  rng_ ^= rng_ << 13;
  rng_ ^= rng_ >> 17;
  rng_ ^= rng_ << 5;
  return rng_;
}

void __not_in_flash_func(Voder::TriggerPlosive)() {
  plosive_ = kPlosiveSamples;
}

// polyBLEP correction around a sawtooth discontinuity.
//
// A naive sawtooth at F0 = 110 Hz has harmonics every 110 Hz all the way to
// Nyquist and beyond; everything above 24 kHz folds back as inharmonic
// rubbish, and the 2600 and 3800 Hz bands sit right where it lands. The
// audible result is a metallic edge that no amount of filtering removes,
// because the aliases are already inside the passband.
//
// polyBLEP fixes it by replacing the instantaneous jump with a two-sample
// polynomial approximation to a bandlimited step. It costs two multiplies
// only on the samples adjacent to the wrap, and needs no table.
//
//   t  = position within one sample of the discontinuity, Q15
//   returns the correction to ADD to the naive value, Q15
static inline int32_t __not_in_flash_func(PolyBlep)(int32_t t_q15) {
  // t in [0,1): approaching the discontinuity   -> t*t - 2t + 1 ... etc.
  // Standard form:  t<dt:  t = t/dt; return t+t - t*t - 1
  //                 t>1-dt: t = (t-1)/dt; return t*t + t+t + 1
  const int32_t t2 = (int32_t)(((int64_t)t_q15 * t_q15) >> 15);
  return (t_q15 << 1) - t2 - 32768;
}

int32_t __not_in_flash_func(Voder::Process)(const Params& p) {
  // --- excitation -------------------------------------------------------

  // Buzz: bandlimited sawtooth. The phase accumulator is a plain uint32_t
  // wrapping at 2^32, so the raw sawtooth is just the top bits of it.
  int32_t f0 = p.f0_milli_hz;
  if (f0 < kF0MinMilliHz) f0 = kF0MinMilliHz;
  if (f0 > kF0MaxMilliHz) f0 = kF0MaxMilliHz;

  phase_inc_ = (uint32_t)(((uint64_t)f0 * kIncPerMilliHz_Q16) >> 16);
  const uint32_t prev_phase = phase_;
  phase_ += phase_inc_;

  // Naive saw in Q15, -32768..32767.
  int32_t buzz = (int32_t)(phase_ >> 17) - 16384;
  buzz <<= 1;

  // Apply the polyBLEP correction if we just wrapped, or are about to. dt
  // is the phase increment expressed as a fraction of a full cycle.
  if (phase_ < prev_phase) {
    // Wrapped this sample. Distance past the discontinuity, as a fraction
    // of one sample step.
    const uint32_t dt = phase_inc_ ? phase_inc_ : 1;
    const int32_t t = (int32_t)(((uint64_t)phase_ << 15) / dt);
    if (t < 32768) buzz -= PolyBlep(t);
  }

  // Hiss: xorshift32, taken from the top 16 bits because the low bits of a
  // shift-register PRNG are the least well distributed.
  const int32_t noise = ((int32_t)(Random() >> 16)) - 32768;

  // Gate envelopes. A one-pole toward the target would take ~10 ms to
  // settle, which slurs consonant attacks; a linear ramp over kGateRamp
  // samples is both faster and click-free.
  const int32_t v_target = p.voiced_level;
  const int32_t n_target = p.noise_level;
  const int32_t step = 32768 / kGateRamp;
  if (gate_env_ < v_target) {
    gate_env_ += step; if (gate_env_ > v_target) gate_env_ = v_target;
  } else if (gate_env_ > v_target) {
    gate_env_ -= step; if (gate_env_ < v_target) gate_env_ = v_target;
  }
  if (noise_env_ < n_target) {
    noise_env_ += step; if (noise_env_ > n_target) noise_env_ = n_target;
  } else if (noise_env_ > n_target) {
    noise_env_ -= step; if (noise_env_ < n_target) noise_env_ = n_target;
  }

  // Source mix. The Voder's wrist bar was a hard either/or; a continuous
  // crossfade is more useful and reduces to the original at the extremes.
  const int32_t buzz_amt = (int32_t)(((int64_t)buzz * gate_env_) >> 15);
  const int32_t hiss_amt = (int32_t)(((int64_t)noise * noise_env_) >> 15);

  const int32_t mix = 32767 - p.source_mix;
  int32_t excite = (int32_t)((((int64_t)buzz_amt * mix) +
                              ((int64_t)hiss_amt * p.source_mix)) >> 15);

  // External input replaces the internal sources entirely when Audio In 1
  // is patched - the Voder as a formant filter for whatever you feed it.
  if (p.use_ext) {
    excite = p.ext_input << 3;  // 12-bit ADC to Q15-ish
  }

  // Headroom before the filter bank. Each band can contribute up to its
  // full gain, and eight bands summing at once would overflow; scaling the
  // input down by 3 bits here is cheaper than scaling eight outputs.
  excite >>= 3;

  // --- filter bank ------------------------------------------------------

  int32_t sum = 0;
  int32_t energy = 0;

  for (int i = 0; i < kBands; i++) {
    Biquad& f = bank_[i];

    // y = b0*(x - x2) - a1*y1 - a2*y2, all coefficients Q15.
    //
    // The accumulation genuinely needs 64 bits. A single product reaches
    // 65234 * ~20000 = 1.3e9, which still fits int32 - but three of them
    // sum to 4.0e9, which does not. Checked in tools/filter_check.py.
    // PICO_INT64_OPS_IN_RAM keeps the __aeabi_lmul helper this generates
    // out of flash - see CMakeLists.txt.
    const int64_t acc = (int64_t)f.b0 * (excite - f.x2)
                      - (int64_t)f.a1 * f.y1
                      - (int64_t)f.a2 * f.y2;
    const int32_t y = (int32_t)(acc >> 15);

    f.x2 = f.x1; f.x1 = excite;
    f.y2 = f.y1; f.y1 = y;

    // Scale by this band's gain and accumulate.
    sum += (int32_t)(((int64_t)y * p.band_gain[i]) >> 15);

    // Energy tracking for CV Out 1 and the LEDs. Absolute value is enough;
    // a true RMS would need a square root per band per sample.
    energy += (y >= 0) ? y : -y;
  }

  energy_ = energy >> 3;
  voiced_ = (gate_env_ > noise_env_);

  // --- plosive burst ----------------------------------------------------

  // Summed AFTER the bank, not through it. The Voder's stop keys bypassed
  // the filter chain too: a plosive release is a broadband click, and
  // running it through the formant bank turns it into a filtered thud that
  // reads as a vowel onset rather than a consonant.
  if (plosive_ > 0) {
    // Linear decay over the burst. The ear reads the envelope shape of a
    // stop release as its identity, and linear is close enough at 8 ms.
    const int32_t env = (plosive_ << 15) / kPlosiveSamples;
    const int32_t burst = ((int32_t)(Random() >> 16)) - 32768;
    sum += (int32_t)(((int64_t)burst * env) >> 17);
    plosive_--;
  }

  // --- output -----------------------------------------------------------

  // Back down to the 12-bit DAC range. The >>3 headroom taken before the
  // bank plus this shift is the overall gain staging; tools/filter_check.py
  // checks a full-scale input with all bands open does not clip.
  return sum >> 4;
}

}  // namespace tract8
