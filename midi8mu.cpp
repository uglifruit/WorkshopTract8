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

// Toggle edge state for the freeze button. Note-on fires the toggle;
// note-off is ignored, so freeze latches on press rather than being held.
static bool s_freeze_held = false;

static void HandleCc(uint8_t cc, uint8_t v) {
  if (cc >= kCcFaderFirst && cc <= kCcFaderLast) {
    // Faders are the performance. A 7-bit CC scaled to Q15 by <<8 gives
    // 0..32512, which is 0.2 dB shy of full scale - close enough that the
    // top of the fader reads as fully open, and it avoids a conditional to
    // special-case 127.
    //
    // Ignored while frozen: that is what freeze means.
    if (!g_state.freeze) {
      g_state.band_gain[cc - kCcFaderFirst] = (int32_t)v << 8;
    }
    return;
  }

  if (cc == kCcTiltUp) {
    s_tilt_up = (int32_t)v << 8;
  } else if (cc == kCcTiltDown) {
    s_tilt_down = (int32_t)v << 8;
  } else {
    return;  // every other CC, including the six unused gestures
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
