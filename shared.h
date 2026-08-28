// TRACT8 - cross-core state.
//
// Read/written by:
//   Core 0 - VoderCard::ProcessSample() at 48 kHz, in interrupt context
//   Core 1 - the TinyUSB host task pump, whenever the 8mu sends something
//
// Every field is a single byte or a naturally-aligned 32-bit word, so reads
// and writes are atomic on the Cortex-M0+ and no mutex is needed. `volatile`
// on the global instance forces a re-read on every access so a change made on
// one core becomes visible on the other.
//
// Direction of writes:
//   band_gain, pitch_bend, gate_voiced, gate_noise, freeze
//                     - written by Core 1 (MIDI) and Core 0 (panel), last
//                       writer wins. See the note on that in main.cpp.
//   plosive_count     - incremented by Core 1 only, read by Core 0
//   midi_connected    - written by Core 1 only, read by Core 0 for the LED

#ifndef TRACT8_SHARED_H_
#define TRACT8_SHARED_H_

#include <stdint.h>

struct VoderState {
  // --- the five 8mu faders that matter -------------------------------
  //
  // Fader 1  OPENNESS   vowel square, close <-> open   (F1)
  // Fader 2  FRONT      vowel square, back  <-> front  (F2)
  // Fader 3  BREATH     buzz <-> noise
  // Fader 4  PITCH      F0, 50..500 Hz
  // Fader 5  BRIGHT     spectral tilt
  //
  // Faders 6-8 are unassigned. That is deliberate: this card was
  // unplayable when every fader did something, because a vowel is not
  // eight independent numbers to a player. Five controls that each mean
  // something beat eight that need to be operated as a chord.
  int32_t openness;      // Q15
  int32_t front;         // Q15
  int32_t breath;        // Q15, 0 = all buzz, 32767 = all noise
  int32_t pitch;         // Q15, mapped to F0 in main.cpp
  int32_t bright;        // Q15, 16384 = flat

  // One flag per control, set the first time the 8mu sends it. Until
  // then the panel keeps that control. A controller sitting plugged in
  // but untouched must never seize a knob the player has a hand on.
  uint8_t openness_from_midi;
  uint8_t front_from_midi;
  uint8_t breath_from_midi;
  uint8_t pitch_from_midi;
  uint8_t bright_from_midi;

  // --- accelerometer --------------------------------------------------
  // Front/back tilt is VOLUME. It was pitch until playing showed that
  // pitch wants to be set and left, while volume wants a gesture - the
  // whole point of holding the controller is to be able to swell and
  // duck a phrase without letting go of anything.
  int32_t volume;        // Q15, 32767 = unity
  uint8_t volume_from_midi;

  // --- band gains handed to the filter bank ---------------------------
  // Computed in main.cpp from the vowel square. Nothing else writes it;
  // it lives here so the LEDs can display it.
  int32_t band_gain[8];

  // --- buttons ---------------------------------------------------------
  uint8_t gate_voiced;
  uint8_t gate_noise;
  uint8_t freeze;
  uint8_t midi_connected;

  // Counted, not flagged - a flag set on Core 1 can be missed entirely or
  // fired twice depending on how the ISR interleaves with it.
  uint32_t plosive_count;
};

extern volatile VoderState g_state;

#endif  // TRACT8_SHARED_H_
