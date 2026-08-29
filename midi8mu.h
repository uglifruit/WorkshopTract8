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
//   CC 34  fader 1   OPENNESS   vowel cube, close <-> open
//   CC 41  fader 8   FRONT      vowel cube, back  <-> front
//   CC 36  fader 3   BREATH     buzz <-> noise
//   CC 37  fader 4   PITCH      F0, 50..500 Hz
//   CC 38  fader 5   BRIGHT     spectral tilt
//   CC 35, 39, 40    unassigned
//   CC 42/43         VOLUME     tilt front/back
//   CC 44/45         ROUND      tilt left/right - the vowel cube third axis
//   CC 49            MUTE       held while the 8mu is upside down
//   CC 46-48         unassigned
//   Note 36 (C2)     voiced buzz gate, and adds breath
//   Note 48 (C3)     unvoiced noise gate
//   Note 60 (C4)     plosive burst
//   Note 72 (C5)     formant freeze, and adds breath
//
// FADERS 1 AND 8 CARRY THE VOWEL, not 1 and 2. They are the outermost
// faders, so the two axes that get played constantly sit under a thumb and
// a little finger with the whole hand spanning the device. Adjacent faders
// need the same finger twice or a wrist move. This was reported from
// playing, and it is the kind of thing only playing tells you.
//
// Faders 2, 6 and 7 are deliberately unassigned. Five controls that each
// mean something beat eight that must be operated as a chord - the version
// that put one filter band on each fader was unplayable.
//
// Everything else - other CCs, other notes, pitch bend, sysex, program
// change, clock - is silently dropped. Channel-agnostic.

#ifndef TRACT8_MIDI8MU_H_
#define TRACT8_MIDI8MU_H_

#include <stdint.h>

namespace tract8 {

// Faders. 1 and 8 are the vowel axes - the outermost pair, for the reason
// in the header comment above.
static constexpr uint8_t kCcOpenness = 34;   // fader 1
static constexpr uint8_t kCcFront = 41;      // fader 8
static constexpr uint8_t kCcBreath = 36;     // fader 3
static constexpr uint8_t kCcPitch = 37;      // fader 4
static constexpr uint8_t kCcBright = 38;     // fader 5

// Accelerometer.
static constexpr uint8_t kCcVolUp = 42;      // tilt front
static constexpr uint8_t kCcVolDown = 43;    // tilt back
static constexpr uint8_t kCcRoundLeft = 44;  // tilt left
static constexpr uint8_t kCcRoundRight = 45; // tilt right
static constexpr uint8_t kCcInverted = 49;   // upside down - mute

// Button notes.
static constexpr uint8_t kNoteVoiced = 36;   // C2
static constexpr uint8_t kNoteNoise = 48;    // C3
static constexpr uint8_t kNotePlosive = 60;  // C4
static constexpr uint8_t kNoteFreeze = 72;   // C5

// How much breath buttons A and D add while held, Q15. A whisper of noise
// under a voiced sound is what makes it breathy rather than buzzy; enough
// to hear, not enough to swamp the buzz.
static constexpr int32_t kButtonBreath = 9000;

// Feed one decoded MIDI channel-voice message in. Status is the full byte
// including channel; the channel is ignored.
void Midi8muMessage(uint8_t status, uint8_t d1, uint8_t d2);

}  // namespace tract8

#endif  // TRACT8_MIDI8MU_H_
