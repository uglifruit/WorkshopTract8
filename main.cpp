// TRACT8 - a Voder for the Music Thing Modular Workshop System Computer.
//
// An eight-band reimplementation of the 1939 Bell Labs Voder, played from a
// Music Thing 8mu over USB MIDI Host or from the card's own panel.
//
// Structure:
//   voder.cpp     excitation and the 8-band filter bank
//                                       (verified: tools/filter_check.py,
//                                                  tools/excite_check.py)
//   vowels.h      vowel gain vectors    (verified: tools/vowel_check.py)
//   midi8mu.cpp   8mu CC/note dispatch  (verified: tools/midi_check.py)
//   usb_core1.cpp Core 1 USB host pump
//   main.cpp      panel, CV, LEDs, core scheduling
//
// The Voder's controls map onto this card as:
//   wrist bar (buzz/hiss)  -> notes C2/C3, Knob 3, Pulse In 2
//   foot pedal (pitch)     -> accelerometer CC 42/43, CV In 1, Knob 1 page 2
//   ten filter keys        -> 8mu faders CC 34-41, Knob 1/2 on the panel
//   three stop keys        -> note C4, Pulse In 1, Switch down

#include "ComputerCard.h"

#include "pico/multicore.h"
#include "hardware/clocks.h"
#include "hardware/timer.h"

#include "shared.h"
#include "voder.h"
#include "vowels.h"

using namespace tract8;

extern "C" void core1_entry(void);

// Boot mute, samples. 0.5 s of silence while the DAC settles and Core 1
// brings the USB controller up. Without it the card clicks on power-up.
static constexpr int32_t kBootMute = 24000;

// Boot splash, samples. Runs past the mute so there is visible feedback
// that the card is alive before it makes any sound.
static constexpr int32_t kBootSplash = 36000;

// Panel read rate divider. Reading one knob per sample round-robin costs a
// third of an ADC read per sample instead of three, and knobs do not move
// at 48 kHz.
static constexpr int32_t kPanelDiv = 3;

// External-input detection. A patched signal must exceed this ADC level
// (about 1.5% of full scale, well above the converter's noise floor) to
// take over from the internal excitation, and the takeover is then held for
// kExtHoldSamples so a waveform's zero crossings do not chatter it on and
// off. 4800 samples is 100 ms - longer than the gap between zero crossings
// of anything above 10 Hz.
static constexpr int32_t kExtGateLevel = 30;
static constexpr int32_t kExtHoldSamples = 4800;

// Vowel morph recompute interval, samples. 384 samples is 125 Hz - far
// faster than a hand can turn a knob, and it takes the morph from roughly
// a quarter of the ISR budget down to under one percent. See ReadPanel().
static constexpr int32_t kMorphDiv = 384;

// DSP load is averaged over this many samples before being written to
// CV Out 2. 256 samples is 5.3 ms - fast enough to see a transient, slow
// enough that the reading is stable to look at.
static constexpr int32_t kLoadWindow = 256;

// Per-sample budget at 48 kHz, microseconds, times 256 to keep the load
// arithmetic in integers. 1e6/48000 = 20.833 us.
static constexpr uint32_t kBudgetUs256 = 5333;  // 20.833 * 256

static inline int32_t Clamp15(int32_t x) {
  if (x > 32767) return 32767;
  if (x < 0) return 0;
  return x;
}

class VoderCard : public ComputerCard {
 public:
  VoderCard() {
    voder_.Init(time_us_32() | 1u);

    boot_mute_ = kBootMute;
    boot_splash_ = kBootSplash;
    panel_phase_ = 0;
    morph_phase_ = 0;
    ext_hold_ = 0;
    gate_seen_ = false;
    diag_use_ext_ = false;
    diag_gated_ = false;
    knob_main_ = knob_x_ = knob_y_ = 0;
    last_plosive_ = 0;
    last_pulse1_ = false;
    load_acc_ = 0;
    load_count_ = 0;
    load_out_ = 0;
    led_phase_ = 0;
    energy_smooth_ = 0;
    f0_base_milli_ = kF0DefaultMilliHz;

    // Start with a neutral vowel so the card makes a recognisable sound the
    // moment it boots, rather than silence until a fader is touched.
    for (int i = 0; i < kBands; i++) {
      g_state.band_gain[i] = kVowelTable[0][i];
    }
    g_state.pitch_bend = 0;
    g_state.gate_voiced = 0;
    g_state.gate_noise = 0;
    g_state.freeze = 0;
    g_state.midi_connected = 0;
    g_state.plosive_count = 0;
    g_state.vowel_pos = 0;
    g_state.vowel_from_midi = 0;
    g_state.breath = 0;
    g_state.breath_from_midi = 0;
    g_state.faders_touched = 0;
  }

  virtual void __not_in_flash_func(ProcessSample)() override {
    const uint32_t t_start = time_us_32();

    // --- boot mute --------------------------------------------------------
    if (boot_mute_ > 0) {
      boot_mute_--;
      AudioOut1(0);
      AudioOut2(0);
      CVOut1(0);
      CVOut2(0);
      PulseOut1(false);
      PulseOut2(false);
      UpdateLeds();
      if (boot_splash_ > 0) boot_splash_--;
      return;
    }
    if (boot_splash_ > 0) boot_splash_--;

    // --- panel, at a reduced rate ----------------------------------------
    ReadPanel();

    // --- assemble the parameter set --------------------------------------
    Params p;

    // Band gains come from g_state, which both the 8mu (Core 1) and the
    // panel (below, in ReadPanel) write to. Last writer wins, deliberately:
    // there is no mode switch, so a fader move and a knob turn are equally
    // valid ways to move a formant and the player can use either at any
    // moment. The 8mu only transmits on change, so a parked fader never
    // fights a moving knob.
    for (int i = 0; i < kBands; i++) {
      p.band_gain[i] = g_state.band_gain[i];
    }

    // Pitch: base from page 2, plus 1V/oct from CV In 1, plus the
    // accelerometer. CVIn is +/-2048 for +/-6V or so; a semitone is about
    // 1/12 V. Rather than an exp() per sample, F0 is scaled by a shift-and-
    // add approximation of 2^(v/12) accurate to a few cents over +/-2 oct.
    int32_t f0 = f0_base_milli_;

    if (Connected(Input::CV1)) {
      f0 = ApplyOctaves(f0, (int32_t)CVIn1());
    }

    // Accelerometer bend, +/-32767 Q15 spanning +/-1 octave.
    const int32_t bend = g_state.pitch_bend;
    if (bend != 0) {
      // 32767 Q15 == one octave == doubling. Scale into the same units the
      // CV path uses (2048 per octave) and reuse the approximation.
      f0 = ApplyOctaves(f0, (bend * 2048) >> 15);
    }
    p.f0_milli_hz = f0;

    // Source mix: fader 8 if the 8mu has sent it, otherwise Knob 3 (Y).
    //
    // Takeover is one-way and latched on first use, not on the 8mu merely
    // being plugged in. A controller sitting connected but untouched must
    // not seize a knob the player already has a hand on.
    p.source_mix = g_state.breath_from_midi ? g_state.breath : knob_y_;

    // Gates. The 8mu's buttons and a patched gate are OR'd - either opens a
    // source. With no controller and nothing patched, both sources are open
    // so the card drones, which is what you want when exploring the vowels.
    //
    // The gate is taken as OPEN unless Pulse In 2 has actually been seen
    // going high at some point. Testing Connected(Pulse2) alone was the
    // first hardware run's silence bug, or half of it: if the
    // normalisation probe reports Pulse2 connected when nothing is
    // patched, PulseIn2() reads low forever, both excitation levels sit at
    // zero and the card is mute. Plosives still sounded because they are
    // summed after the filter bank - which is the symptom that found this.
    //
    // Latching on "have we ever seen this jack go high" means a
    // misdetected jack leaves the card droning (recoverable, obvious)
    // rather than silent (looks broken). A real gate patched in takes
    // control the first time it rises.
    if (PulseIn2()) gate_seen_ = true;
    const bool ext_gate = gate_seen_ ? PulseIn2() : true;
    const bool any_midi_gate = g_state.gate_voiced || g_state.gate_noise;

    if (any_midi_gate) {
      p.voiced_level = g_state.gate_voiced ? 32767 : 0;
      p.noise_level = g_state.gate_noise ? 32767 : 0;
    } else {
      p.voiced_level = ext_gate ? 32767 : 0;
      p.noise_level = ext_gate ? 32767 : 0;
    }

    // Audio In 1 replaces the internal excitation when patched.
    //
    // Gated on the signal actually being there, not on the jack alone -
    // the other half of the silence bug. ComputerCard forces a
    // disconnected input to zero, so a jack misdetected as connected fed
    // the filter bank a constant zero and muted the card.
    //
    // Requiring real signal makes the failure safe in the right
    // direction: a false "connected" now falls back to the internal
    // sources and the card still speaks. A genuinely patched signal
    // crosses the threshold within a few samples.
    const int32_t ain = (int32_t)AudioIn1();
    if (ain > kExtGateLevel || ain < -kExtGateLevel) {
      ext_hold_ = kExtHoldSamples;
    } else if (ext_hold_ > 0) {
      ext_hold_--;
    }
    p.use_ext = ext_hold_ > 0;
    p.ext_input = p.use_ext ? ain : 0;

    // --- plosive triggers -------------------------------------------------
    // Counted, not flagged: fire once per increment Core 1 has made since
    // we last looked, so a burst that lands between two samples is never
    // lost and never doubled.
    const uint32_t pc = g_state.plosive_count;
    if (pc != last_plosive_) {
      last_plosive_ = pc;
      voder_.TriggerPlosive();
    }

    // Pulse In 1, rising edge.
    const bool pulse1 = PulseIn1();
    if (pulse1 && !last_pulse1_) voder_.TriggerPlosive();
    last_pulse1_ = pulse1;

    // Switch down is a momentary plosive key - the Voder's stop keys were
    // played with the left hand, and this is the nearest thing the panel
    // has to one.
    if (SwitchVal() == Switch::Down && SwitchChanged()) {
      voder_.TriggerPlosive();
    }

    // --- audio ------------------------------------------------------------
    int32_t out = voder_.Process(p);

    // Diagnostic state for the LED display. Kept because the first
    // hardware run was silent and there was no way to see WHY from the
    // panel - every candidate cause looked identical from outside.
    diag_use_ext_ = p.use_ext;
    diag_gated_ = (p.voiced_level == 0 && p.noise_level == 0);

    if (out > 2047) out = 2047;
    if (out < -2048) out = -2048;

    AudioOut1((int16_t)out);
    AudioOut2((int16_t)out);

    // --- CV / pulse out ---------------------------------------------------

    // CV Out 1: formant energy, smoothed. One-pole with a ~5 ms time
    // constant so it reads as an envelope rather than as audio.
    const int32_t e = voder_.Energy();
    energy_smooth_ += (e - energy_smooth_) >> 6;
    int32_t cv1 = energy_smooth_ >> 3;
    if (cv1 > 2047) cv1 = 2047;
    CVOut1((int16_t)cv1);

    // CV Out 2: measured DSP load. This is the authority on cost - not any
    // figure derived on paper. Everything in tools/budget_check.py is a
    // prediction until this pin is read on real hardware.
    CVOut2((int16_t)load_out_);

    PulseOut1(voder_.Voiced());
    PulseOut2(g_state.freeze != 0);

    UpdateLeds();

    // --- load measurement -------------------------------------------------
    // Taken last so it covers essentially the whole ISR body. The timer
    // read itself costs a few cycles and is inside the measurement, which
    // is the conservative direction to err in.
    load_acc_ += (time_us_32() - t_start);
    if (++load_count_ >= kLoadWindow) {
      // mean_us * 256 / budget_us_256 * 2047, rearranged to stay integral
      // and avoid overflow: load_acc_ is at most 256 * ~21 = 5376.
      int32_t load = (int32_t)((load_acc_ * 2047u) / kBudgetUs256);
      if (load > 2047) load = 2047;
      load_out_ = load;
      load_acc_ = 0;
      load_count_ = 0;
    }
  }

 private:
  // Scale a frequency by a signed number of octaves given in CV units,
  // where 2048 units is one octave (the Computer's roughly 1 V/oct scaling
  // on a +/-6 V input).
  //
  // A true 2^x needs exp(); this uses the standard piecewise-linear-in-the
  // -mantissa trick: integer octaves are a shift, and the fractional part
  // is approximated by a straight line between 1.0 and 2.0. That is within
  // about 6% at the worst point (a semitone and a half), which is audible
  // as pitch but perfectly usable for a speech formant source where F0 is
  // an expressive control rather than a tuned note. If this card ever
  // needs to track a keyboard properly, replace this with CVOutMIDINote's
  // calibrated path.
  static int32_t __not_in_flash_func(ApplyOctaves)(int32_t f, int32_t cv) {
    const int32_t oct = cv >> 11;          // whole octaves, floor
    const int32_t frac = cv - (oct << 11);  // 0..2047 within the octave

    int32_t r = f;
    if (oct > 0) {
      r <<= (oct > 4 ? 4 : oct);
    } else if (oct < 0) {
      r >>= (-oct > 4 ? 4 : -oct);
    }
    // r * (1 + frac/2048)
    r += (int32_t)(((int64_t)r * frac) >> 11);
    return r;
  }

  void __not_in_flash_func(ReadPanel)() {
    // One knob per sample, round-robin.
    switch (panel_phase_) {
      case 0: knob_main_ = KnobVal(Knob::Main) << 3; break;
      case 1: knob_x_ = KnobVal(Knob::X) << 3; break;
      default: knob_y_ = KnobVal(Knob::Y) << 3; break;
    }
    if (++panel_phase_ >= kPanelDiv) panel_phase_ = 0;

    const Switch sw = SwitchVal();

    if (sw == Switch::Up) {
      // Page 2: Knob 1 is F0, Knob 2 is unused for now, Knob 3 still mixes.
      // 50..500 Hz over the knob's travel, in milli-Hz.
      f0_base_milli_ = kF0MinMilliHz +
                       (int32_t)(((int64_t)knob_main_ *
                                  (kF0MaxMilliHz - kF0MinMilliHz)) >> 15);
      return;
    }

    // Page 1: the vowel morph. Frozen formants ignore the panel, same as
    // they ignore the faders.
    if (g_state.freeze) return;

    // Once the faders are in play they own bands 1-7 and the morph must
    // not fight them. It still drives band 8, which no fader claims.
    const bool faders = FadersInUse();

    // The morph is the second most expensive thing in the ISR after the
    // filter bank - eight bands, each a lerp and a scale, both 64-bit.
    // Running it every sample costs about 28% of the per-sample budget to
    // recompute a value that only changes when a hand moves a knob.
    //
    // Once every kMorphDiv samples is 125 Hz, far above the rate any knob
    // can actually be turned, and it drops the morph's share to under 1%.
    // The band gains are held in g_state between updates, so the filter
    // bank still sees a value every sample.
    if (++morph_phase_ < kMorphDiv) return;
    morph_phase_ = 0;
    ext_hold_ = 0;
    gate_seen_ = false;
    diag_use_ext_ = false;
    diag_gated_ = false;

    // Vowel position: the 8mu's left/right tilt if it has ever been sent,
    // otherwise Knob 1. Same one-way latched takeover as breath.
    //
    // Putting the vowel on the accelerometer came out of playing the card:
    // the morph carries more of the character than any single band gain
    // does, and having to let go of the faders to reach a knob broke the
    // performance. Tilting the controller leaves both hands where they are.
    const int32_t vowel_src =
        g_state.vowel_from_midi ? g_state.vowel_pos : knob_main_;

    // Walk the vowel table, crossfading between adjacent entries.
    // kNumVowels-1 segments across the Q15 travel.
    const int32_t scaled = vowel_src * (kNumVowels - 1);  // 0 .. 5*32767
    int32_t idx = scaled >> 15;
    if (idx >= kNumVowels - 1) idx = kNumVowels - 2;
    const int32_t f = scaled - (idx << 15);  // 0..32767 within the segment

    // Knob 2 tilts the result toward the high bands - "mouth openness".
    // At centre it is flat; fully clockwise the top bands are boosted and
    // the bottom cut, which brightens the vowel without changing which
    // vowel it is.
    const int32_t tilt = knob_x_ - 16384;  // -16384 .. +16383

    for (int i = 0; i < kBands; i++) {
      const int32_t a = kVowelTable[idx][i];
      const int32_t b = kVowelTable[idx + 1][i];
      int32_t g = a + (int32_t)((((int64_t)(b - a)) * f) >> 15);

      // Tilt: scale each band by a factor that rises across the spectrum
      // when the knob is clockwise and falls when it is anticlockwise.
      //
      // This is MULTIPLICATIVE, not additive, and that is the whole point.
      // An additive tilt was tried first and cannot be made to work: the
      // offset needed to brighten a loud band audibly is larger than a
      // quiet band's entire value, so one end of the spectrum always
      // clamps. With AH, whose 250 Hz gain is only 1364, an additive tilt
      // strong enough to hear drove band 0 negative while band 2 - already
      // at full scale - was clipped at the other end of the knob. Bands
      // pinned at 0 are holes no fader can reopen, and they read as a
      // broken filter rather than as a tone control.
      //
      // Scaling sidesteps both: a band at zero stays at zero, a loud band
      // is cut proportionally, and nothing can go negative. The factor is
      // Q15, so 32768 is unity.
      //
      // The >>4 gives a +/-22% swing at the outermost bands - about 1.2 dB
      // of spectral change, audible as brightness without displacing the
      // vowel. tools/vowel_check.py check 3 verifies no band pins at
      // either extreme of the knob for ANY vowel in the table; at >>2 two
      // bands pin, at >>3 one does.
      const int32_t pos = (i * 2) - 7;  // -7 .. +7, odd steps
      int32_t factor = 32768 + (int32_t)(((int64_t)tilt * pos) >> 4);
      if (factor < 0) factor = 0;
      g = (int32_t)(((int64_t)g * factor) >> 15);

      // Bands the faders own are left alone once the faders are in use.
      if (faders && i < 7) continue;
      g_state.band_gain[i] = Clamp15(g);
    }
  }

  // True once the 8mu has sent any band fader, after which the panel morph
  // stops writing the seven bands the faders own. Without this the morph
  // would overwrite every fader move 125 times a second and the faders
  // would appear dead.
  bool FadersInUse() const { return g_state.faders_touched != 0; }

  void __not_in_flash_func(UpdateLeds)() {
    if (boot_splash_ > 0) {
      // Chase across all six while the card wakes up.
      const int32_t step = (kBootSplash - boot_splash_) >> 12;
      for (int i = 0; i < 6; i++) LedOn(i, (step % 6) == i);
      return;
    }

    // Rate-limit: the LED matrix does not need refreshing at 48 kHz, and
    // doing so costs more than the display is worth.
    if (++led_phase_ < 240) return;
    led_phase_ = 0;

    // LEDs 0-3: band energy in four pairs, low to high.
    for (int i = 0; i < 4; i++) {
      const int32_t g = (g_state.band_gain[i * 2] +
                         g_state.band_gain[i * 2 + 1]) >> 1;
      LedBrightness(i, (uint16_t)(g >> 3));
    }

    // Switch Down turns LEDs 0-3 into a diagnostic display instead of the
    // band meters. Left in deliberately: when this card is silent, every
    // possible cause looks the same from the front panel, and that cost a
    // whole hardware round trip once already.
    //
    //   LED 0  excitation is gated shut (no buzz, no noise)
    //   LED 1  external input has taken over from the internal sources
    //   LED 2  formant freeze is latched
    //   LED 3  all eight band gains are near zero
    if (SwitchVal() == Switch::Down) {
      int32_t gsum = 0;
      for (int i = 0; i < kBands; i++) gsum += g_state.band_gain[i];
      LedOn(0, diag_gated_);
      LedOn(1, diag_use_ext_);
      LedOn(2, g_state.freeze != 0);
      LedOn(3, gsum < 4096);
      LedOn(4, g_state.midi_connected != 0);
      LedBrightness(5, 4095);
      return;
    }

    // LED 4: 8mu connected.
    LedOn(4, g_state.midi_connected != 0);

    // LED 5: voiced/unvoiced, or full brightness while frozen.
    if (g_state.freeze) {
      LedBrightness(5, 4095);
    } else {
      LedBrightness(5, voder_.Voiced() ? 3000 : 400);
    }
  }

  Voder voder_;

  int32_t boot_mute_, boot_splash_;
  int32_t panel_phase_, morph_phase_;
  int32_t ext_hold_;
  bool gate_seen_;
  bool diag_use_ext_, diag_gated_;
  int32_t knob_main_, knob_x_, knob_y_;
  int32_t f0_base_milli_;
  uint32_t last_plosive_;
  bool last_pulse1_;
  uint32_t load_acc_;
  int32_t load_count_, load_out_;
  int32_t led_phase_;
  int32_t energy_smooth_;
};

// Global, not on the stack: the card holds the filter bank and its state,
// and the stack is only 4 KB.
//
// Being a global means this constructor runs BEFORE main(). That is what
// made v1.0.1 silent: the constructor called Voder::Init(), which copied
// coefficients that VoderInit() had not computed yet. Voder::Init() now
// builds them itself, so the ordering cannot bite again - but do not add
// anything here that depends on work done in main().
VoderCard card;

int main() {
  // 192 MHz. A proven clock on this hardware - grains/51, glitter/53 and
  // WorkshopSpectral all run here. Eight biquads at 48 kHz is not cheap on
  // a core with no FPU and no 64-bit multiply, and the headroom matters.
  set_sys_clock_khz(192000, true);

  // Recompute the coefficients at the real system clock. Voder::Init()
  // already built them from the card's constructor (see the note there),
  // and VoderInit() is idempotent - this is not what makes the card work,
  // it just keeps main() honest about what has been initialised.
  VoderInit(0);

  // Core 1 owns the USB host stack and nothing else. See usb_core1.cpp for
  // why the audio ISR stays on Core 0.
  multicore_launch_core1(core1_entry);

  card.EnableNormalisationProbe();
  card.Run();
}
