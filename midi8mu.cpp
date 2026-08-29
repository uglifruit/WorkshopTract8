// TRACT8 - Music Thing 8mu dispatch. See midi8mu.h for the mapping table.

#include "midi8mu.h"
#include "shared.h"

namespace tract8 {

// The accelerometer gestures arrive as separate controllers per direction,
// so each axis is the difference of a pair. Held here so a message on one
// direction does not wipe the other.
static int32_t s_vol_up = 0;
static int32_t s_vol_down = 0;
static int32_t s_round_left = 0;
static int32_t s_round_right = 0;

// Toggle edge state for the freeze button.
static bool s_freeze_held = false;

// A 7-bit CC scaled to Q15. 127 gives 32512, which is 0.07 dB shy of full
// scale - close enough to read as fully open, and it avoids a branch to
// special-case the top value.
static inline int32_t CcToQ15(uint8_t v) { return (int32_t)v << 8; }

// Volume from the net front/back tilt.
//
// Rests at UNITY, not at half. A controller lying flat must be at full
// volume, because that is where it spends most of its life and nobody
// wants to hold a tilt just to be heard. Tilting back ducks all the way to
// silence; tilting forward does nothing extra, unity being already the
// ceiling. Splitting the range around a centre was tried and wasted half
// of it: a full back tilt reached only half volume, so the card could not
// be faded out at all.
static void UpdateVolume() {
  int32_t vol = 32767 - s_vol_down + s_vol_up;
  if (vol < 0) vol = 0;
  if (vol > 32767) vol = 32767;
  g_state.volume = vol;
  g_state.volume_from_midi = 1;
}

// Rounding, the vowel cube third axis, from the net left/right tilt.
// Rests spread (0) with the controller level, so the flat position is the
// ordinary unrounded vowel and rounding is something you reach for.
static void UpdateRound() {
  int32_t r = s_round_right - s_round_left;
  if (r < 0) r = 0;
  if (r > 32767) r = 32767;
  g_state.round = r;
  g_state.round_from_midi = 1;
}

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
      // Linear, unlike nothing else here: breath is a balance, so the
      // middle of the fader should be half buzz and half noise.
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

    case kCcInverted:
      // Upside down is a hard mute. The 8mu sends this as a level rather
      // than a switch, so treat the top half of its range as inverted.
      g_state.muted = (v >= 64) ? 1 : 0;
      return;

    case kCcVolUp:
      s_vol_up = CcToQ15(v);
      UpdateVolume();
      return;

    case kCcVolDown:
      s_vol_down = CcToQ15(v);
      UpdateVolume();
      return;

    case kCcRoundLeft:
      if (g_state.freeze) return;
      s_round_left = CcToQ15(v);
      UpdateRound();
      return;

    case kCcRoundRight:
      if (g_state.freeze) return;
      s_round_right = CcToQ15(v);
      UpdateRound();
      return;

    default:
      // Faders 2, 6, 7, the remaining gestures, everything else: dropped.
      return;
  }
}

static void HandleNoteOn(uint8_t note, uint8_t vel) {
  // A note-on with velocity 0 is the running-status note-off.
  if (vel == 0) {
    if (note == kNoteVoiced) {
      g_state.gate_voiced = 0;
      g_state.breath_button_a = 0;
    } else if (note == kNoteNoise) {
      g_state.gate_noise = 0;
    } else if (note == kNoteFreeze) {
      s_freeze_held = false;
      g_state.breath_button_d = 0;
    }
    return;
  }

  switch (note) {
    case kNoteVoiced:
      g_state.gate_voiced = 1;
      // Adds breath while held. The button already gates the buzz and its
      // held state was otherwise unused, so this is free expression.
      g_state.breath_button_a = 1;
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
      g_state.breath_button_d = 1;
      break;
    default:
      break;
  }
}

static void HandleNoteOff(uint8_t note) {
  if (note == kNoteVoiced) {
    g_state.gate_voiced = 0;
    g_state.breath_button_a = 0;
  } else if (note == kNoteNoise) {
    g_state.gate_noise = 0;
  } else if (note == kNoteFreeze) {
    s_freeze_held = false;
    g_state.breath_button_d = 0;
  }
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
