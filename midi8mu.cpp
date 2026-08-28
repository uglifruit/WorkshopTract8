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
    // Faders are the performance, and their CURVE is what makes them feel
    // like one.
    //
    // A linear v<<8 was tried first and the faders barely changed the
    // sound. The reason is in the numbers: a real vowel has about 27 dB
    // between its loudest and quietest band, but a linear fader at any
    // ordinary hand position gives every band roughly the same value - a
    // flat spectrum, which reads as a filter sweep rather than a voice.
    // The bottom half of the travel was doing almost nothing.
    //
    // Squaring gives about 42 dB across the throw, which is the same order
    // as the vowel table's own contrast: the bottom of a fader genuinely
    // shuts its band, and formants appear where the hand puts them.
    // Cubing was also tried at ~63 dB and is too much - the fader feels
    // dead until the last third.
    //
    // Ignored while frozen: that is what freeze means.
    if (!g_state.freeze) {
      const int32_t sq = ((int32_t)v * (int32_t)v * 32767) / (127 * 127);
      g_state.band_gain[cc - kCcFaderFirst] = sq;
      g_state.faders_touched = 1;
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
