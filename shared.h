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
  // Filter band gains, Q15 (0..32767). One per band, driven by 8mu faders
  // CC 34-41 or by the panel vowel morph.
  int32_t band_gain[8];

  // F0 offset from the 8mu accelerometer, Q15 signed. +/-32767 spans one
  // octave either way. The Voder's foot pedal.
  int32_t pitch_bend;

  // Vowel position from the 8mu's left/right tilt, Q15 (0..32767), and a
  // flag saying whether it has ever been sent. Until the controller is
  // actually tilted sideways the panel's Knob 1 keeps the vowel, so
  // plugging an 8mu in does not silently seize a control the player is
  // already using.
  int32_t vowel_pos;
  uint8_t vowel_from_midi;

  // Breath: how much of the excitation is noise rather than buzz, Q15.
  // Mirrors Knob 3, driven by fader 8 when the 8mu is present.
  int32_t breath;
  uint8_t breath_from_midi;

  // Set the first time a band fader moves. After that the panel's vowel
  // morph stops writing bands 1-7, because the faders own them - otherwise
  // the morph would overwrite every fader move 125 times a second and the
  // faders would appear dead.
  uint8_t faders_touched;

  // Excitation gates. The Voder's wrist bar chose one *or* the other; here
  // they are independent so both can sound at once (useful for voiced
  // fricatives like "z", which the original could not do).
  uint8_t gate_voiced;
  uint8_t gate_noise;

  // Formant freeze latch. When set, band_gain stops tracking its sources.
  uint8_t freeze;

  // 1 while a USB MIDI device is mounted. Drives LED 4.
  uint8_t midi_connected;

  // Plosive triggers are COUNTED, not flagged. A flag set on Core 1 can be
  // missed entirely if the ISR happens to read either side of it, or fired
  // twice if the ISR reads before Core 1 clears it. The ISR keeps its own
  // copy of this counter and fires once per increment it observes, which is
  // robust to any interleaving.
  uint32_t plosive_count;
};

extern volatile VoderState g_state;

#endif  // TRACT8_SHARED_H_
