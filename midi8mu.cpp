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
// The accelerometer axes are CONTINUOUS LEVELS, 0-127, reported as
// complementary pairs that add to 127. A level device therefore sits at 64
// on each axis, and 64 is the neutral point of a bipolar control.
//
// Both axes are BIPOLAR WITH A CENTRE DETENT: the effect is the DEVIATION
// from centre, in either direction, not the raw level. Tilting one way or
// the other does the same thing, which is what makes them playable - you
// do not have to remember which way is which, only how far.
//
// The response is SQUARED, which biases the control toward its neutral
// state. That is deliberate for volume in particular: the card should stay
// audible through most of the travel and only fall away near the ends, so
// that an imperfectly level controller does not quietly rob level. At a
// quarter tilt a squared curve is 0.6 dB down where a linear one is 2.5 dB
// down; at the extreme both reach silence. A cubed curve was tried and
// clings to full volume too long to read as a fade at all.

// Deviation from the centre detent, squared, Q15. Returns 0 at centre and
// 32767 at either extreme of the axis.
static int32_t TiltDeviation(uint8_t v) {
  int32_t d = (int32_t)v - kTiltCentre;
  if (d < 0) d = -d;
  if (d > kTiltCentre) d = kTiltCentre;      // v = 0 gives exactly 64
  // (d/64)^2 in Q15, computed as (d*d << 15) / (64*64) without overflow:
  // d*d is at most 4096, so the shift is safe in int32.
  return (d * d * 32767) / (kTiltCentre * kTiltCentre);
}

// Volume: full at the centre detent, falling to silence at either extreme.
static void UpdateVolume(uint8_t v) {
  g_state.volume = 32767 - TiltDeviation(v);
  g_state.volume_from_midi = 1;
}

// Rounding, the vowel cube third axis: unrounded at the centre detent,
// moving toward the OO end of the cube at either extreme.
static void UpdateRound(uint8_t v) {
  g_state.round = TiltDeviation(v);
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
      // CC 49 is HIGH while the device is the RIGHT WAY UP, and falls when
      // it is turned over. The name is misleading - despite being listed
      // as "inverted" it reads as an upright indicator on the hardware -
      // so the sense here is deliberately the opposite of what the label
      // suggests. Do not "fix" this to match the documentation.
      //
      // Fourth attempt at this control, and the previous three are worth
      // recording because each failed differently:
      //
      //   1. Treated as a one-shot event: "if CC 49 arrives, mute". These
      //      are continuous levels, so it fired constantly.
      //   2. Treated as a level meaning inverted: "v >= 64 mutes". Since
      //      the level is high when UPRIGHT, this muted the card during
      //      normal use and unmuted it only when turned over - exactly
      //      backwards, which is what was reported.
      //   3. Paired with CC 48 as mutual exclusives, which cannot help
      //      when the polarity of the reading itself is wrong.
      //
      // So: LOW means turned over, which mutes. The threshold sits at 64
      // with the hysteresis the device already applies.
      g_state.muted = (v < 64) ? 1 : 0;
      return;

    case kCcNotInverted:
      // The complementary partner. CC 48 and CC 49 add to 127 like the
      // other accelerometer pairs, so this carries no information CC 49
      // does not already have, and reading both would mean two writers
      // racing for one value. Left unread on purpose.
      return;

    case kCcVolume:
      // Front/back tilt is VOLUME. Level is full, either extreme silent.
      UpdateVolume(v);
      return;

    case kCcRound:
      // Left/right tilt is ROUNDING - the vowel cube third axis. Level is
      // unrounded, either extreme moves toward OO.
      if (g_state.freeze) return;
      UpdateRound(v);
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
