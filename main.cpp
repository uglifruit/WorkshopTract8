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
// Jacks:
//   Audio In 1   exciter  external audio, summed with buzz and noise
//   Audio In 2   volume   CV, adds to the fader
//   CV In 1      pitch    1V/oct
//   CV In 2      formant  bipolar, sweeps the vowel cube diagonally
//   Pulse In 1   click    trigger
//   Pulse In 2   glottal  gate
//
// All four CV inputs ADD to whatever the panel or the 8mu has set, rather
// than replacing it. That is what makes the card useful fed from random
// voltages: a slow random into CV 2 wanders the vowel around wherever the
// controls are parked, and gates into the pulses chatter it, without ever
// needing the patch to supply a sensible absolute value.
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
#include "midi8mu.h"  // for kButtonBreath

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
    knob_main_ = knob_x_ = knob_y_ = 0;

    // Panel defaults chosen so an untouched card speaks immediately:
    // a mid-open back vowel, moderate pitch, flat brightness, all buzz.
    panel_openness_ = 20000;
    panel_front_ = 0;
    panel_breath_ = 0;
    panel_pitch_ = 8000;
    panel_bright_ = 16384;

    last_plosive_ = 0;
    last_pulse1_ = false;
    load_acc_ = 0;
    load_count_ = 0;
    load_out_ = 0;
    led_phase_ = 0;
    energy_smooth_ = 0;
    formant_cv_ = 0;
    gate_seen_ = false;
    diag_use_ext_ = false;
    diag_gated_ = false;

    // Start from the panel defaults so the card makes a recognisable
    // sound the instant it boots, rather than silence until something is
    // touched.
    g_state.openness = panel_openness_;
    g_state.front = panel_front_;
    g_state.breath = panel_breath_;
    g_state.pitch = panel_pitch_;
    g_state.bright = panel_bright_;
    g_state.openness_from_midi = 0;
    g_state.front_from_midi = 0;
    g_state.breath_from_midi = 0;
    g_state.pitch_from_midi = 0;
    g_state.bright_from_midi = 0;
    g_state.click_level = 32767;
    g_state.click_decay = 32767;
    g_state.click_level_from_midi = 0;
    g_state.click_decay_from_midi = 0;
    g_state.click_gate = 0;
    g_state.round = 0;
    g_state.round_from_midi = 0;
    g_state.volume = 32767;
    g_state.volume_from_midi = 0;
    g_state.muted = 0;
    g_state.breath_button_a = 0;
    g_state.breath_button_d = 0;
    g_state.gate_voiced = 0;
    g_state.gate_noise = 0;
    g_state.freeze = 0;
    g_state.midi_connected = 0;
    g_state.plosive_count = 0;

    int32_t init_gains[kNumBands];
    BlendVowel(panel_openness_, panel_front_, 0, init_gains);
    for (int i = 0; i < kNumBands; i++) {
      g_state.band_gain[i] = Clamp15(init_gains[i]);
    }
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

    // Pitch. Fader 4 if the 8mu has sent it, otherwise Knob 1 on page 2.
    // Then 1V/oct from CV In 1 on top, so the card still tracks a keyboard
    // whichever control set the base note.
    const int32_t pitch_src =
        g_state.pitch_from_midi ? g_state.pitch : panel_pitch_;
    int32_t f0 = kF0MinMilliHz +
                 (int32_t)(((int64_t)pitch_src *
                            (kF0MaxMilliHz - kF0MinMilliHz)) >> 15);

    if (Connected(Input::CV1)) {
      f0 = ApplyOctaves(f0, (int32_t)CVIn1());
    }
    p.f0_milli_hz = f0;

    // Breath: fader 3 if the 8mu has sent it, otherwise Knob 3, plus
    // whatever buttons A and D are adding while held.
    //
    // The buttons ADD rather than set, so they are a gesture on top of
    // wherever the fader is parked - press one over a voiced sound and it
    // turns breathy without losing the pitch. Both buttons together give
    // twice as much, which is the obvious reading of holding both.
    int32_t breath = g_state.breath_from_midi ? g_state.breath : panel_breath_;

    if (g_state.breath_button_a) breath += kButtonBreath;
    if (g_state.breath_button_d) breath += kButtonBreath;
    p.source_mix = Clamp15(breath);

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

    // Click level and decay.
    //
    // BOTH DEFAULT TO FULL WITHOUT AN 8MU. The panel has no knobs free for
    // them, so with no controller attached the click must be at full level
    // and full sustain - otherwise the plosive inputs on the panel, and
    // the switch-down key, would be quiet or clipped short with no way to
    // turn them up. A card that is quieter on its own than with a
    // controller plugged in would be exactly backwards.
    p.click_level =
        g_state.click_level_from_midi ? g_state.click_level : 32767;
    p.click_decay =
        g_state.click_decay_from_midi ? g_state.click_decay : 32767;

    // Audio In 1 is an EXTERNAL EXCITER, summed with the internal buzz
    // and noise inside the engine.
    //
    // It was briefly the breath CV instead, and the two genuinely cannot
    // share a jack: a slow breath voltage means nothing to a bank of
    // bandpass filters, which have no DC gain, while an audio signal
    // summed into the excitation is not a balance control. They are
    // different functions, not two readings of one.
    //
    // The jack goes to the exciter because breath is the most reachable
    // control on the card already - fader 3, Knob 3, and buttons A and D
    // all reach it - while external audio had no route at all. Formant
    // keeps CV In 2 despite also having faders, knobs and tilt, because
    // sweeping vowels from a random voltage is the point of the card.
    // <<4 puts a full-scale input level with the internal sources, so a
    // patched signal can carry the sound rather than sitting under it.
    p.ext_input = Connected(Input::Audio1) ? ((int32_t)AudioIn1() << 4) : 0;

    // --- plosive triggers -------------------------------------------------
    // Counted, not flagged: fire once per increment Core 1 has made since
    // we last looked, so a burst that lands between two samples is never
    // lost and never doubled.
    const uint32_t pc = g_state.plosive_count;
    if (pc != last_plosive_) {
      last_plosive_ = pc;
      voder_.TriggerPlosive(p.click_decay);
    }

    // Button 1 holds the click open while held. At a long decay setting
    // that turns the burst into a sustained noise source; at a short one
    // it is still a hit, because the burst has already decayed by the
    // time the finger lifts.
    voder_.SetPlosiveSustain(g_state.click_gate != 0);

    // Pulse In 1, rising edge.
    const bool pulse1 = PulseIn1();
    if (pulse1 && !last_pulse1_) voder_.TriggerPlosive(p.click_decay);
    last_pulse1_ = pulse1;

    // Switch down is a momentary plosive key - the Voder's stop keys were
    // played with the left hand, and this is the nearest thing the panel
    // has to one.
    if (SwitchVal() == Switch::Down && SwitchChanged()) {
      voder_.TriggerPlosive(p.click_decay);
    }

    // --- audio ------------------------------------------------------------
    int32_t out = voder_.Process(p);

    // Volume, from the 8mu front/back tilt. This is the one control that
    // wants to be a gesture rather than a setting: pitch and vowel get set
    // and left, but swelling and ducking a phrase is what having the
    // controller in your hands is FOR. Unity until the accelerometer has
    // actually been used, so a card with no 8mu is never quiet.
    // Volume: the 8mu's fader and tilt, plus Audio In 2 as a CV.
    //
    // The CV is bipolar and ADDS, so it swells and ducks around whatever
    // the fader is set to - an envelope into this jack articulates a
    // phrase without needing the patch to control absolute level. With no
    // 8mu the base is full, so a negative CV ducks from full and a
    // positive one is simply already at the ceiling.
    int32_t vol = g_state.volume_from_midi ? g_state.volume : 32767;
    if (Connected(Input::Audio2)) {
      vol += (int32_t)AudioIn2() << 4;
    }
    vol = Clamp15(vol);
    if (vol != 32767) {
      out = (int32_t)(((int64_t)out * vol) >> 15);
    }

    // Upside down is a hard mute. Turning the controller over is an
    // unmistakable, deliberate gesture that nobody makes by accident,
    // which is exactly what a panic stop should be - no aim required.
    //
    // Applied after the volume scale and before the clamp, so it silences
    // the audio outputs without disturbing the CV outs: the energy and
    // DSP-load readings stay live while muted, which is what you want if
    // you are muting to look at something.
    if (g_state.muted) out = 0;

    // Diagnostic state for the LED display. Kept because the first
    // hardware run was silent and there was no way to see WHY from the
    // panel - every candidate cause looked identical from outside.
    diag_use_ext_ = (p.ext_input != 0);
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

    // The panel mirrors the 8mu, one knob per page:
    //
    //   Middle/Down   Knob1 OPENNESS   Knob2 FRONT     Knob3 BREATH
    //   Up            Knob1 PITCH      Knob2 BRIGHT    Knob3 BREATH
    //
    // Each control is taken from the 8mu instead once that fader has
    // actually been moved. The takeover is per-control and one-way, so a
    // controller sitting plugged in but untouched never seizes a knob the
    // player has a hand on, and moving one fader does not disable the
    // other knobs.
    if (sw == Switch::Up) {
      panel_pitch_ = knob_main_;
      panel_bright_ = knob_x_;
    } else {
      panel_openness_ = knob_main_;
      panel_front_ = knob_x_;
    }
    panel_breath_ = knob_y_;

    // Frozen formants ignore the vowel controls entirely.
    if (g_state.freeze) return;

    // Rebuilding the vowel is the second most expensive thing in the ISR
    // after the filter bank. Running it every sample would spend a
    // meaningful slice of the budget recomputing a value that only changes
    // when a hand moves. Once every kMorphDiv samples is 125 Hz, far above
    // the rate anything can be moved by hand; the band gains are held in
    // g_state between updates so the bank still sees a value every sample.
    if (++morph_phase_ < kMorphDiv) return;
    morph_phase_ = 0;

    int32_t openness =
        g_state.openness_from_midi ? g_state.openness : panel_openness_;
    int32_t front =
        g_state.front_from_midi ? g_state.front : panel_front_;

    // The formant CV moves openness up as front moves down. Sweeping both
    // together along one axis would mostly travel between two corners of
    // the cube; opposing them crosses the middle, which is where the
    // distinct vowels are.
    if (formant_cv_ != 0) {
      openness = Clamp15(openness + formant_cv_);
      front = Clamp15(front - formant_cv_);
    }

    // CV In 2 is a bipolar FORMANT CV. It sweeps the vowel cube along its
    // most useful diagonal - openness and front together, in opposite
    // senses - so one random voltage walks through recognisably different
    // vowels rather than along one axis of a cube. That is the input to
    // patch an S&H or a slow random into.
    //
    // Added to the existing position rather than replacing it, so the
    // faders and knobs still set where the sweep is centred.
    formant_cv_ = Connected(Input::CV2) ? ((int32_t)CVIn2() << 3) : 0;

    // Rounding: the third vowel axis, from the 8mu left/right tilt. The
    // panel has no knob free for it, so with no controller attached the
    // card stays on the spread face of the cube - which is where the
    // ordinary unrounded vowels live, so nothing is lost.
    const int32_t round_amt = g_state.round_from_midi ? g_state.round : 0;

    // Three axes, eight corner vowels, trilinear between them. vowels.h.
    int32_t gains[kNumBands];
    BlendVowel(openness, front, round_amt, gains);

    // Brightness tilts the result toward the high or low bands.
    // Multiplicative, so it cannot drive a band negative or pin one at
    // full scale - the additive version could do both, depending on the
    // vowel, and a band pinned at zero is a hole no control can reopen.
    const int32_t bright =
        g_state.bright_from_midi ? g_state.bright : panel_bright_;
    const int32_t tilt = bright - 16384;

    for (int i = 0; i < kNumBands; i++) {
      const int32_t pos = (i * 2) - 7;  // -7 .. +7
      int32_t factor = 32768 + (int32_t)(((int64_t)tilt * pos) >> 4);
      if (factor < 0) factor = 0;
      int32_t g = (int32_t)(((int64_t)gains[i] * factor) >> 15);
      g_state.band_gain[i] = Clamp15(g);
    }
  }

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
    if (g_state.muted) {
      // Dark while muted, so the panel says why it is silent. Silence with
      // no explanation is the one failure mode this card keeps finding.
      LedBrightness(5, 0);
    } else if (g_state.freeze) {
      LedBrightness(5, 4095);
    } else {
      LedBrightness(5, voder_.Voiced() ? 3000 : 400);
    }
  }

  Voder voder_;

  int32_t boot_mute_, boot_splash_;
  int32_t panel_phase_, morph_phase_;
  int32_t formant_cv_;
  bool gate_seen_;
  bool diag_use_ext_, diag_gated_;
  int32_t knob_main_, knob_x_, knob_y_;
  int32_t panel_openness_, panel_front_, panel_breath_;
  int32_t panel_pitch_, panel_bright_;
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
