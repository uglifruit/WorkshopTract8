// TRACT8 - Music Thing 8mu dispatch. See midi8mu.h for the mapping table.

#include "midi8mu.h"
#include "shared.h"

namespace tract8 {

// Accelerometer tilt is accumulated rather than absolute. The 8mu sends a
// gesture CC as a magnitude, and the two axes (front / back) arrive as
// separate controllers, so pitch is their difference. Held here so a CC on
// one axis does not wipe the other.
static int32_t s_tilt_up = 0;
static int32_t s_tilt_down = 0;
static int32_t s_vowel_left = 0;
static int32_t s_vowel_right = 0;

// Toggle edge state for the freeze button. Note-on fires the toggle;
// note-off is ignored, so freeze latches on press rather than being held.
static bool s_freeze_held = false;

static void HandleCc(uint8_t cc, uint8_t v) {
  if (cc >= kCcFaderFirst && cc <= kCcFaderLast) {
    // Faders BEND the vowel the morph is making; they do not replace it.
    //
    // The first version had them write the band gains outright and latch
    // the panel morph out on first touch. That made the card unplayable in
    // the most literal way: one fader move killed Knob 1 for good, and the
    // only route to a vowel was placing all eight faders by hand. Eight
    // simultaneous controls is the Voder's own problem - the one its
    // operators trained for months to solve - and there is no reason to
    // reproduce it when the morph already puts a vowel under one knob.
    //
    // So a fader is an offset around a centre detent: 64 is neutral and
    // changes nothing, below cuts that band, above boosts it. Leave seven
    // where they are and move one, and you have shaded a single formant of
    // whatever vowel the knob is currently making. Both controls stay live
    // at all times.
    //
    // The curve is squared about the centre, for the same reason the gains
    // were squared: a linear offset spends most of its travel making
    // changes too small to hear. Signed, so it works symmetrically either
    // side of the detent.
    // The two directions are deliberately NOT symmetric, because they are
    // not the same musical act.
    //
    // Boosting is ADDITIVE. A quiet band needs a real number added to it
    // to become a formant; scaling it up by a factor would leave it
    // proportionally quiet and the fader would feel dead on exactly the
    // bands a player most wants to bring forward.
    //
    // Cutting is PROPORTIONAL, floored at kFaderCutFloor. Subtracting a
    // fixed amount drove any band below that amount to exactly zero - a
    // hole in the spectrum no other control could reopen, which reads as a
    // broken filter rather than as shading. It also wasted the bottom
    // quarter of the travel on the difference between silent and silent.
    // Scaling gives every band the same -18 dB at the bottom of the fader,
    // whatever its level, and keeps the whole throw useful.
    if (!g_state.freeze) {
      const int i = cc - kCcFaderFirst;
      const int32_t centred = (int32_t)v - 64;          // -64 .. +63
      if (centred >= 0) {
        g_state.band_offset[i] =
            (centred * centred * kFaderOffsetMax) / (64 * 64);
        g_state.band_cut[i] = 32768;
      } else {
        const int32_t m = -centred;
        g_state.band_offset[i] = 0;
        g_state.band_cut[i] =
            32768 - ((m * m * (32768 - kFaderCutFloor)) / (64 * 64));
      }
    }
    return;
  }

  if (cc == kCcBreath) {
    // Fader 8: breath. Same squared curve is NOT used here - breath is a
    // balance, not a level, and wants to be linear so the midpoint of the
    // fader really is half and half.
    g_state.breath = (int32_t)v << 8;
    g_state.breath_from_midi = 1;
    return;
  }

  if (cc == kCcVowelLeft || cc == kCcVowelRight) {
    if (cc == kCcVowelLeft) {
      s_vowel_left = (int32_t)v << 8;
    } else {
      s_vowel_right = (int32_t)v << 8;
    }
    // Left and right are separate gesture CCs, so the vowel position is
    // their difference recentred into 0..32767. Tilting the 8mu left walks
    // toward AH, right toward EE.
    int32_t pos = 16384 + ((s_vowel_right - s_vowel_left) >> 1);
    if (pos < 0) pos = 0;
    if (pos > 32767) pos = 32767;
    g_state.vowel_pos = pos;
    g_state.vowel_from_midi = 1;
    return;
  }

  if (cc == kCcTiltUp) {
    s_tilt_up = (int32_t)v << 8;
  } else if (cc == kCcTiltDown) {
    s_tilt_down = (int32_t)v << 8;
  } else {
    return;  // every other CC, including the four unused gestures
  }

  // Net tilt, +/-32767 Q15, spanning one octave either way in main.cpp.
  g_state.pitch_bend = s_tilt_up - s_tilt_down;
}

static void HandleNoteOn(uint8_t note, uint8_t vel) {
  // A note-on with velocity 0 is the running-status note-off. Treat it as
  // a release, which is what every well-behaved MIDI receiver does.
  if (vel == 0) {
    if (note == kNoteVoiced) g_state.gate_voiced = 0;
    else if (note == kNoteNoise) g_state.gate_noise = 0;
    else if (note == kNoteFreeze) s_freeze_held = false;
    return;
  }

  switch (note) {
    case kNoteVoiced:
      g_state.gate_voiced = 1;
      break;
    case kNoteNoise:
      g_state.gate_noise = 1;
      break;
    case kNotePlosive:
      // Counted, not flagged - see the note in shared.h.
      g_state.plosive_count++;
      break;
    case kNoteFreeze:
      // Toggle on the rising edge only, so holding the button does not
      // chatter the latch.
      if (!s_freeze_held) {
        g_state.freeze ^= 1;
        s_freeze_held = true;
      }
      break;
    default:
      break;
  }
}

static void HandleNoteOff(uint8_t note) {
  if (note == kNoteVoiced) g_state.gate_voiced = 0;
  else if (note == kNoteNoise) g_state.gate_noise = 0;
  else if (note == kNoteFreeze) s_freeze_held = false;
}

void Midi8muMessage(uint8_t status, uint8_t d1, uint8_t d2) {
  // Mask off the channel: the 8mu can be set to any of them, and a card
  // that only listened to channel 1 would look broken to anyone who had
  // changed it.
  switch (status & 0xF0) {
    case 0xB0:
      HandleCc(d1 & 0x7F, d2 & 0x7F);
      break;
    case 0x90:
      HandleNoteOn(d1 & 0x7F, d2 & 0x7F);
      break;
    case 0x80:
      HandleNoteOff(d1 & 0x7F);
      break;
    default:
      // Pitch bend, aftertouch, program change, sysex, clock: dropped.
      break;
  }
}

}  // namespace tract8
