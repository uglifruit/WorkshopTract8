// TRACT8 - Music Thing 8mu dispatch.
//
// Decodes the 8mu factory MIDI mapping. Nothing needs configuring in the
// 8mu web editor: plug it into the front USB-C jack and play.
//
// The factory mapping, confirmed against musicthing.co.uk/8mu.html:
//
//   CC 34-41   faders 1-8, left to right with the cable on the right
//   CC 42-49   accelerometer gestures:
//                42 lift front   43 lift back
//                44 lift left    45 lift right
//                46 rotate CW    47 rotate CCW
//                48 not inverted 49 inverted
//   Notes      the four buttons, C2/C3/C4/C5 (36/48/60/72) out of the box
//
// Mapped here as:
//
//   CC 34  fader 1   OPENNESS   vowel square, close <-> open
//   CC 35  fader 2   FRONT      vowel square, back  <-> front
//   CC 36  fader 3   BREATH     buzz <-> noise
//   CC 37  fader 4   PITCH      F0, 50..500 Hz
//   CC 38  fader 5   BRIGHT     spectral tilt
//   CC 39-41         unassigned
//   CC 42/43         VOLUME     tilt front/back
//   CC 44-49         unassigned
//   Note 36 (C2)     gate voiced buzz
//   Note 48 (C3)     gate unvoiced noise
//   Note 60 (C4)     plosive burst
//   Note 72 (C5)     toggle formant freeze
//
// THREE FADERS ARE DELIBERATELY UNUSED. An earlier version drove one
// filter band from each of seven faders and it was unplayable: a vowel is
// a position of the mouth, not eight independent numbers, and shaping one
// meant operating all seven at once. Two faders on the two real vowel axes
// reach the entire space. Leaving 6-8 free is the point, not an omission.
//
// Everything else - other CCs, other notes, pitch bend, sysex, program
// change, clock - is silently dropped. Channel-agnostic.

#ifndef TRACT8_MIDI8MU_H_
#define TRACT8_MIDI8MU_H_

#include <stdint.h>

namespace tract8 {

// Faders, in 8mu order.
static constexpr uint8_t kCcOpenness = 34;   // fader 1
static constexpr uint8_t kCcFront = 35;      // fader 2
static constexpr uint8_t kCcBreath = 36;     // fader 3
static constexpr uint8_t kCcPitch = 37;      // fader 4
static constexpr uint8_t kCcBright = 38;     // fader 5

// Accelerometer front/back - volume.
static constexpr uint8_t kCcVolUp = 42;
static constexpr uint8_t kCcVolDown = 43;

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
