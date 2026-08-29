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
// The accelerometer axes are CONTINUOUS LEVELS, 0-127, not gesture events
// and not magnitudes needing calibration.
//
// This took three attempts to get right, so it is worth stating plainly:
// tilting the 8mu sweeps a controller through its full 0-127 range, and
// turning it over parks the inverted controller near 127. There is no
// resting offset to learn and no gesture to detect - the value IS the
// position.
//
// The two previous versions both failed on hardware by assuming otherwise.
// The first computed 32767 - lift_back + lift_front, which silences the
// card whenever lift_back rests above zero. The second tracked a running
// minimum and used the deviation from it, which was a solution to a
// problem that does not exist, and folded the sign with abs() so half the
// travel mirrored the other half instead of continuing.
//
// Both of those are the same underlying mistake - inventing semantics
// rather than reading a level - and both produced a control that appeared
// dead rather than wrong, which is far harder to diagnose from the bench.

// Volume: the left/right tilt, straight through. Full scale is unity so a
// controller sitting level is at full volume; tilting to the other end of
// the axis fades to silence.
static void UpdateVolume(int32_t level_q15) {
  int32_t vol = level_q15;
  if (vol < 0) vol = 0;
  if (vol > 32767) vol = 32767;
  g_state.volume = vol;
  g_state.volume_from_midi = 1;
}

// Rounding: the front/back tilt, straight through. 0 is spread and full
// scale is fully rounded, which is the OO end of the vowel cube.
static void UpdateRound(int32_t level_q15) {
  int32_t r = level_q15;
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
      // Upside down mutes. The VALUE decides, not the mere arrival of the
      // message.
      //
      // Third attempt at this, so the reasoning is worth writing down.
      // Reading CC 49 as a bare event ("if it arrives, mute") did not
      // work on hardware, and the most likely reason is that the 8mu
      // streams BOTH halves of the pair continuously: CC 49 arrives and
      // mutes, CC 48 arrives and unmutes, over and over, so the mute
      // never sticks. It would also break if CC 49 carries a level that
      // sits low while the device is upright, because any CC 49 at all
      // would have latched the mute on.
      //
      // Using the value handles every one of those: a high CC 49 means
      // inverted, a low one means it is not. If the 8mu really does send
      // the pair as one-shot events, a flip still sends CC 49 at full
      // scale, so this keeps working.
      g_state.muted = (v >= 64) ? 1 : 0;
      return;

    case kCcNotInverted:
      // Explicitly right way up. Only a HIGH value clears the mute, for
      // the same reason: if this one streams continuously at a low value
      // it must not fight the CC 49 above.
      if (v >= 64) g_state.muted = 0;
      return;

    case kCcRoundFront:
      // Front/back tilt is ROUNDING - the vowel cube third axis, and the
      // OO dimension.
      //
      // ONE CONTROLLER PER AXIS. CC 43 (lift back) is deliberately not
      // read: the pair is COMPLEMENTARY, front and back adding to 127, so
      // CC 43 carries no information CC 42 does not already have.
      // Reading both would mean two writers for one value with the last
      // message winning, which is a race for no benefit.
      if (g_state.freeze) return;
      UpdateRound(CcToQ15(v));
      return;

    case kCcVolLeft:
      // Left/right tilt is VOLUME. CC 45 likewise not read: left and
      // right also add to 127, so one of them is the whole axis.
      UpdateVolume(CcToQ15(v));
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
