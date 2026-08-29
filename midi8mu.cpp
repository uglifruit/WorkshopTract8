// TRACT8 - Music Thing 8mu dispatch. See midi8mu.h for the mapping table.

#include "midi8mu.h"
#include "shared.h"

namespace tract8 {


// Toggle edge state for the freeze button.
static bool s_freeze_held = false;

// A 7-bit CC scaled to Q15. 127 gives 32512, which is 0.07 dB shy of full
// scale - close enough to read as fully open, and it avoids a branch to
// special-case the top value.
static inline int32_t CcToQ15(uint8_t v) { return (int32_t)v << 8; }

// Volume and rounding from the accelerometer.
//
// THE LESSON THAT COST TWO HARDWARE ROUNDS: the 8mu's accelerometer CCs
// are GESTURE MAGNITUDES, not a bipolar axis with a known resting value.
// "Lift front" and "lift back" are two independent controllers that each
// report how much of that gesture is happening, and a controller lying
// flat does not necessarily send 0 on either of them.
//
// The first version computed
//
//     volume = 32767 - lift_back + lift_front
//
// which assumed lift_back rests at 0. If it rests anywhere above zero the
// card is quiet or silent from the moment it boots, and - this is the part
// that made it look broken rather than wrong - the volume is already
// clamped at the bottom, so moving the controller appears to do nothing at
// all. Reported from hardware as "tilt isn't doing volume now".
//
// The fix is to assume nothing about resting values and CALIBRATE. The
// first message on each axis establishes that axis's rest point; from then
// on only the DEVIATION from rest is used. That works whatever the 8mu
// happens to send when level, and it needs no configuration.
static int32_t s_vol_rest = -1;      // -1 = not yet calibrated
static int32_t s_round_rest = -1;

// Deviation from an axis's resting value, Q15.
//
// Rest is tracked as the RUNNING MINIMUM rather than the first value seen.
// A gesture magnitude is non-negative and falls back toward its floor when
// the controller is level, so the minimum converges on the true resting
// value within a second of ordinary handling.
//
// Taking the first value instead has a nasty failure mode: if the 8mu
// happens to be tilted when the first message arrives - or if it only
// transmits once movement starts, so the first message is already
// mid-gesture - then "tilted" becomes the zero point, level becomes a
// duck, and the only cure is a power cycle. The running minimum recovers
// from that on its own the first time the controller is put down.
static int32_t Deviation(int32_t* rest, int32_t value) {
  if (*rest < 0 || value < *rest) *rest = value;
  return value - *rest;
}

// Volume: rests at UNITY and ducks as the controller is tilted either way
// from where it was first seen. Full volume is the resting state, because
// that is where the controller spends most of its life and nobody wants to
// hold a pose just to be heard.
static void UpdateVolume(int32_t deviation) {
  const int32_t mag = deviation < 0 ? -deviation : deviation;
  int32_t vol = 32767 - (mag << 1);   // full duck within half a tilt range
  if (vol < 0) vol = 0;
  if (vol > 32767) vol = 32767;
  g_state.volume = vol;
  g_state.volume_from_midi = 1;
}

// Rounding, the vowel cube third axis. Rests spread and rounds as the
// controller is tilted, in either direction - which of left or right
// rounds is not worth caring about, since the player will simply learn
// whichever way their unit moves.
static void UpdateRound(int32_t deviation) {
  int32_t r = deviation < 0 ? -deviation : deviation;
  r <<= 1;
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
      // Upside down is a hard mute, and it LATCHES ON while the gesture
      // is being reported.
      //
      // This is the other half of the same lesson. The 8mu sends
      // "inverted" (CC 49) and "not inverted" (CC 48) as separate
      // gestures; CC 49 does not fall back to zero when the controller is
      // turned right side up, it simply stops being sent while CC 48 is
      // sent instead. Treating CC 49 as a level meant the mute engaged and
      // then never released - reported from hardware as the mute not
      // working at all.
      //
      // So: CC 49 mutes, CC 48 unmutes, and neither is a level.
      g_state.muted = 1;
      return;

    case kCcNotInverted:
      g_state.muted = 0;
      return;

    case kCcVolUp:
      UpdateVolume(Deviation(&s_vol_rest, CcToQ15(v)));
      return;

    case kCcVolDown:
      // Front and back report the same physical axis from opposite ends,
      // and only one of them is non-zero at a time. Both feed the same
      // rest tracker and the same magnitude, so whichever the unit
      // actually sends is the one that takes effect - the card does not
      // need to know which way up the gesture is defined.
      UpdateVolume(Deviation(&s_vol_rest, CcToQ15(v)));
      return;

    case kCcRoundLeft:
      if (g_state.freeze) return;
      UpdateRound(Deviation(&s_round_rest, CcToQ15(v)));
      return;

    case kCcRoundRight:
      if (g_state.freeze) return;
      UpdateRound(Deviation(&s_round_rest, CcToQ15(v)));
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
