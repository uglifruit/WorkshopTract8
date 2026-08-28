// TRACT8 - the vowel space.
//
// Eight band gains per corner vowel, Q15, for the fixed band centres
//
//   250  450  700  1000  1400  1900  2600  3800 Hz
//
// THIS IS A 2-D SPACE, NOT A LIST. Two controls place a point inside a
// square whose corners are four vowels, and the gains are the bilinear
// blend of those corners. That is not an arbitrary parameterisation - it
// is the vowel quadrilateral every phonetics textbook draws, and the two
// axes are the two things a mouth actually does:
//
//                     BACK  <----------> FRONT
//        CLOSE         OO                 EE
//          ^            |                  |
//          |            |   UH and OH      |
//       OPENNESS        |   live in here   |
//          |            |   as blends      |
//          v            |                  |
//         OPEN         AH                 EH
//
//   OPENNESS is F1: how far the jaw is open. 270 Hz (EE) to 730 Hz (AH).
//   FRONT    is F2: where the tongue sits.   840 Hz (OO) to 2290 Hz (EE).
//
// Formants from Peterson & Barney (1952), adult male. Each band gain is the
// energy the first three formants deposit in it under a log-frequency
// gaussian 0.42 octaves wide, with a -26 dB floor added before normalising
// because real vowels have no silent regions.
//
// WHY THIS REPLACED THE SIX-ENTRY TABLE. The old design put a 1-D morph
// through six vowels on one knob and gave seven faders one band each. In
// playing, the per-band faders turned out to be the wrong abstraction
// altogether - "it is too hard to try to manipulate the partials" - because
// a vowel is not eight independent numbers to a player, it is a position of
// the mouth. Two sliders on the two real axes reach the whole space,
// including the vowels that are not corners, and leave a hand free.
//
// The peak is 16000 rather than 32767 so Knob 2s tilt (up to 1.219x) has
// somewhere to go without clipping the loudest band.
//
// Read-only, so flash is the right home - never written, only indexed.

#ifndef TRACT8_VOWELS_H_
#define TRACT8_VOWELS_H_

#include <stdint.h>

namespace tract8 {

static constexpr int kNumBands = 8;

// Corner index. The order matters: BlendVowel() indexes it directly.
enum VowelCorner {
  kCloseBack = 0,   // OO  F1 300  F2 870
  kOpenBack = 1,    // AH  F1 730  F2 1090
  kCloseFront = 2,  // EE  F1 270  F2 2290
  kOpenFront = 3,   // EH  F1 530  F2 1840
  kNumCorners = 4
};

static const int16_t kVowelCorners[kNumCorners][kNumBands] = {
  //  250    450    700   1000   1400   1900   2600   3800
  { 16000,  8369,  6043,  6689,  3106,  2657,  2533,  1271 },  // OO close back
  {   666,  4003, 16000, 15423,  7647,  4226,  3923,  1675 },  // AH open  back
  { 16000,  4167,   864,   929,  2824,  8556, 12114,  5952 },  // EE close front
  {  1518, 16000, 12137,  3653,  7991, 14057, 10935,  3136 },  // EH open  front
};

// Place a point in the vowel square. Both arguments Q15 (0..32767):
// openness 0 = close, 32767 = open; front 0 = back, 32767 = front.
// Writes kNumBands gains into out.
//
// Bilinear, so the corners come out exact and everything between is a
// smooth interpolation. UH sits at roughly (96% open, 12% front) and OH at
// (96% open, 0% front), which is why neither is stored: the square already
// contains them.
static inline void BlendVowel(int32_t openness, int32_t front,
                              int32_t* out) {
  for (int i = 0; i < kNumBands; i++) {
    const int32_t back_edge =
        kVowelCorners[kCloseBack][i] +
        (((kVowelCorners[kOpenBack][i] - kVowelCorners[kCloseBack][i]) *
          openness) >> 15);
    const int32_t front_edge =
        kVowelCorners[kCloseFront][i] +
        (((kVowelCorners[kOpenFront][i] - kVowelCorners[kCloseFront][i]) *
          openness) >> 15);
    out[i] = back_edge + (((front_edge - back_edge) * front) >> 15);
  }
}

}  // namespace tract8

#endif  // TRACT8_VOWELS_H_
