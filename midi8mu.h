// TRACT8 - Music Thing 8mu dispatch.
//
// Decodes the 8mu's FACTORY MIDI mapping into g_state. Nothing needs
// configuring in the 8mu web editor: plug it into the front USB-C jack and
// the faders move formants.
//
// The factory mapping, confirmed against musicthing.co.uk/8mu.html:
//
//   CC 34-41   faders 1-8, left to right with the cable on the right
//   CC 42-49   accelerometer gestures:
//                42 lift front   43 lift back
//                44 lift left    45 lift right
//                46 rotate CW    47 rotate CCW
//                48 not inverted 49 inverted
//   Notes      the four buttons along the top edge, configurable, sending
//              C2/C3/C4/C5 (36/48/60/72) out of the box
//
// Mapped here as:
//
//   CC 34-40       band OFFSETS 1-7, centre 64    the Voder's ten keys
//   CC 41          BREATH, buzz <-> noise         the Voder's wrist bar
//   CC 42/43       F0 up / down                   the Voder's foot pedal
//   CC 44/45       VOWEL, left / right tilt       (no 1939 equivalent)
//   Note 36 (C2)   gate voiced buzz               the wrist bar
//   Note 48 (C3)   gate unvoiced noise            the wrist bar
//   Note 60 (C4)   plosive burst                  the stop keys
//   Note 72 (C5)   toggle formant freeze          (no 1939 equivalent)
//
// Everything else - other CCs, other notes, pitch bend, sysex, program
// change, clock - is silently dropped. Channel-agnostic: the 8mu can be set
// to any channel and this still works.

#ifndef TRACT8_MIDI8MU_H_
#define TRACT8_MIDI8MU_H_

#include <stdint.h>

namespace tract8 {

// Fader CCs. Faders 1-7 are band gains; fader 8 is breath.
//
// Giving up a band to breath is a deliberate trade. Eight faders on eight
// bands was tidy, but in playing it turned out that the band gains alone
// are not what makes this sound like a voice - the buzz/noise balance is,
// and reaching for a panel knob to change it breaks the performance. Band 8
// is 3800 Hz, the least missed of the eight: it carries sibilance rather
// than vowel identity, and the noise source feeds it plenty. It is still
// fully reachable from the vowel morph and from Knob 2's tilt.
// How far one fader can bend its band, Q15. 20000 is about +/-2x in
// linear terms - enough to bring a quiet formant forward or push a loud
// one back convincingly, without letting a single fader obliterate the
// vowel the knob is making.
static constexpr int32_t kFaderOffsetMax = 20000;

// How far down a fader can scale its band, Q15. 3900/32768 is -18 dB:
// clearly audible shading, but the band is still there. A fader that shut
// its band completely punched a hole in the spectrum that read as a broken
// filter rather than as a performance, and wasted the bottom quarter of
// its travel on the difference between silent and silent.
static constexpr int32_t kFaderCutFloor = 3900;

static constexpr uint8_t kCcFaderFirst = 34;
static constexpr uint8_t kCcFaderLast = 40;   // 7 bands
static constexpr uint8_t kCcBreath = 41;      // fader 8

// Accelerometer gesture CCs. Front/back lifts F0 (the Voder's foot pedal);
// left/right sweeps the vowel morph, which is the control that turned out
// to carry the most character and wants to be playable without letting go
// of the faders.
static constexpr uint8_t kCcTiltUp = 42;
static constexpr uint8_t kCcTiltDown = 43;
static constexpr uint8_t kCcVowelLeft = 44;
static constexpr uint8_t kCcVowelRight = 45;

// Button notes.
static constexpr uint8_t kNoteVoiced = 36;   // C2
static constexpr uint8_t kNoteNoise = 48;    // C3
static constexpr uint8_t kNotePlosive = 60;  // C4
static constexpr uint8_t kNoteFreeze = 72;   // C5

// Feed one decoded MIDI channel-voice message in. Status is the full byte
// including channel; the channel is ignored.
void Midi8muMessage(uint8_t status, uint8_t d1, uint8_t d2);

}  // namespace tract8

#endif  // TRACT8_MIDI8MU_H_
