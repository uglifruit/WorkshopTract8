// TRACT8 - Music Thing 8mu dispatch. See midi8mu.h for the mapping table.

#include "midi8mu.h"
#include "shared.h"

namespace tract8 {

// The two accelerometer gestures arrive as separate controllers, so the
// net tilt is their difference. Held here so a message on one axis does
// not wipe the other.
static int32_t s_vol_up = 0;
static int32_t s_vol_down = 0;

// Toggle edge state for the freeze button.
static bool s_freeze_held = false;

// A 7-bit CC scaled to Q15. 127 gives 32512, which is 0.07 dB shy of full
// scale - close enough to read as fully open, and it avoids a branch to
// special-case the top value.
static inline int32_t CcToQ15(uint8_t v) { return (int32_t)v << 8; }

static void HandleCc(uint8_t cc, uint8_t v) {
  switch (cc) {
    case kCcOpenness:
      // Frozen formants ignore the vowel controls; that is what freeze is.
      if (!g_state.freeze) {
        g_state.openness = CcToQ15(v);
        g_state.openness_from_midi = 1;
      }
      return;

    case kCcFront:
      if (!g_state.freeze) {
        g_state.front = CcToQ15(v);
        g_state.front_from_midi = 1;
      }
      return;

    case kCcBreath:
      // Linear, unlike the vowel axes: breath is a balance, so the middle
      // of the fader should be half buzz and half noise.
      g_state.breath = CcToQ15(v);
      g_state.breath_from_midi = 1;
      return;

    case kCcPitch:
      g_state.pitch = CcToQ15(v);
      g_state.pitch_from_midi = 1;
      return;

    case kCcBright:
      g_state.bright = CcToQ15(v);
      g_state.bright_from_midi = 1;
      return;

    case kCcVolUp:
      s_vol_up = CcToQ15(v);
      break;

    case kCcVolDown:
      s_vol_down = CcToQ15(v);
      break;

    default:
      // Faders 6-8, the other six gestures, everything else: dropped.
      return;
  }

  // Volume from the net front/back tilt.
  //
  // Rests at UNITY, not at half. A controller lying flat must be at full
  // volume, because that is where it spends most of its life and nobody
  // wants to hold a tilt just to be heard. Tilting back ducks all the way
  // to silence; tilting forward does nothing extra, since unity is
  // already the ceiling.
  //
  // Splitting the range around a centre was tried and wasted half of it:
  // a full back tilt only reached half volume, so the card could not be
  // faded out, and the forward half had nowhere to go above unity.
  int32_t vol = 32767 - s_vol_down + s_vol_up;
  if (vol < 0) vol = 0;
  if (vol > 32767) vol = 32767;
  g_state.volume = vol;
  g_state.volume_from_midi = 1;
}

static void HandleNoteOn(uint8_t note, uint8_t vel) {
  // A note-on with velocity 0 is the running-status note-off.
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
      // Counted, not flagged - see shared.h.
      g_state.plosive_count++;
      break;
    case kNoteFreeze:
      // Toggle on the rising edge only, so holding does not chatter it.
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
