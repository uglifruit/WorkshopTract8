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
//   CC 34-41       filter band gains 1-8          the Voder's ten keys
//   CC 42/43       F0 up / down                   the Voder's foot pedal
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

// Fader CCs, inclusive.
static constexpr uint8_t kCcFaderFirst = 34;
static constexpr uint8_t kCcFaderLast = 41;

// Accelerometer gesture CCs used for pitch. Front lifts F0, back drops it.
static constexpr uint8_t kCcTiltUp = 42;
static constexpr uint8_t kCcTiltDown = 43;

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
