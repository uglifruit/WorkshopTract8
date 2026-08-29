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
//   CC 42            VOLUME     tilt front/back, centre detent
//   CC 44            ROUND      tilt left/right, centre detent
//
// THE ACCELEROMETER CCs ARE GESTURE MAGNITUDES, NOT BIPOLAR AXES. Each
// direction is its own controller reporting how much of that gesture is
// happening, and a controller lying flat does not necessarily send 0 on
// any of them. Nothing here may assume a resting value: the volume and
// round axes calibrate themselves from the first message they see. Two
// hardware rounds were lost to assuming otherwise - see midi8mu.cpp.
//   CC 49            MUTE       LOW value = turned over = muted
//   CC 46, 47        unassigned
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
//
// BOTH TILT AXES ARE BIPOLAR WITH A CENTRE DETENT. The 8mu reports each
// axis as a pair of complementary controllers that add to 127, so a level
// device sits at 64 on each, and 64 is therefore the neutral position:
//
//   VOLUME  (front/back, CC 42)  64 = full volume, either extreme = silent
//   ROUND   (left/right, CC 44)  64 = unrounded,   either extreme = OO
//
// Only one controller of each pair is read. Its partner adds to 127 and so
// carries no information the first does not already have; reading both
// would mean two writers racing for one value with the last message
// winning.
static constexpr uint8_t kCcVolume = 42;      // tilt front/back
static constexpr uint8_t kCcVolumePartner = 43;
static constexpr uint8_t kCcRound = 44;       // tilt left/right
static constexpr uint8_t kCcRoundPartner = 45;

// CC 49 reads HIGH while the device is the RIGHT WAY UP and falls when it
// is turned over, despite being labelled "inverted" in the 8mu docs. The
// mute therefore triggers on a LOW value. CC 48 is its complementary
// partner and is not read. See midi8mu.cpp for the failed attempts that
// established this.
static constexpr uint8_t kCcNotInverted = 48;  // partner, unread
static constexpr uint8_t kCcInverted = 49;     // low = turned over = mute

// Centre of a bipolar tilt axis, in raw CC units. A level 8mu sits here
// because the two halves of each pair add to 127.
static constexpr int32_t kTiltCentre = 64;

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
