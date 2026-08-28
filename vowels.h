// TRACT8 - vowel gain vectors.
//
// Eight band gains per vowel, Q15 (0..32767), for the fixed band centres
//
//   250  450  700  1000  1400  1900  2600  3800 Hz
//
// These are derived, not invented. For each vowel the first three formants
// are taken from the standard adult-male measurements (Peterson & Barney
// 1952, the table every speech textbook reprints), each formant is given a
// relative amplitude that falls with formant number as it does in real
// speech, and each band's gain is the energy those formants deposit in it
// under a log-frequency gaussian 0.42 octaves wide. A -26 dB floor is added
// before normalising, because real vowels have no silent regions and bands
// pinned to true zero sound synthetic.
//
//   Vowel   F1    F2    F3     as in
//   AH      730   1090  2440   "father"
//   OH      570   840   2410   "bought"
//   OO      300   870   2240   "boot"
//   UH      640   1190  2390   "but"
//   EH      530   1840  2480   "bet"
//   EE      270   2290  3010   "beet"
//
// THE ROW ORDER IS LOAD-BEARING. Knob 1 crossfades between ADJACENT rows,
// so two similar vowels sitting next to each other produce a stretch of
// knob travel where nothing audible happens. AH and UH are the closest
// pair in the set - only 2.3 dB apart, because acoustically they really
// are neighbours - and an earlier version of this table had them adjacent.
// tools/vowel_check.py caught it: the morph had a dead zone.
//
// The order below was chosen by searching all 720 permutations for the one
// with the shortest articulatory path in the F1/F2 plane among those whose
// worst adjacent step still exceeds 6 dB. It comes out as
//
//   AH -> OH -> OO -> UH -> EH -> EE
//
// which walks open-back, close-back, central, front, close-front: a lap of
// the vowel quadrilateral, and a path a real mouth could make. Sweeping
// Knob 1 slowly is the closest this card gets to a diphthong.
//
// Worst adjacent step is 6.1 dB and the mean is 9.5 dB. If you reorder
// these rows or retune the formants, re-run tools/vowel_check.py.
//
// THE PEAK IS 16000, NOT 32767, AND THAT IS DELIBERATE - twice over.
//
// First, Knob 2's tilt multiplies each band by up to 1.219, so a table
// peaking at full scale would clip its loudest band the moment the knob
// moved clockwise, and the vowels whose peak sits at band 0 (OO and EE)
// would lose exactly the formant that identifies them.
//
// Second, and this is what set the current figure: the 8mu's faders ADD a
// signed offset of up to +/-20000 to bend the vowel. At the previous peak
// of 26800 there was only 1.7 dB of room above the loudest band, so
// pushing a prominent formant up did almost nothing - the fader hit the
// ceiling immediately and felt dead exactly where a player would reach for
// it. At 16000 there is about 6 dB of boost on even the loudest band.
//
// The scaling is uniform, so it costs nothing in vowel distinctness (a
// constant factor cancels in the dB comparison) and the level is made up
// in the output gain staging - see the output shift in voder.cpp, which
// tools/chain_check.py measures.
//
// Read-only, so flash is the right home for this table - it is never
// written, and the hot path only ever indexes it.

#ifndef TRACT8_VOWELS_H_
#define TRACT8_VOWELS_H_

#include <stdint.h>

namespace tract8 {

static constexpr int kNumVowels = 6;
static constexpr int kNumBands = 8;

// Q15 band gains. Rows are vowels in MORPH order (see above); columns are
// bands 1-8.
static const int16_t kVowelTable[kNumVowels][kNumBands] = {
  //  250    450    700   1000   1400   1900   2600   3800
  {   666,  4002, 16000, 15423,  7647,  4226,  3922,  1675 },  // AH  F1 730 F2 1090
  {   881, 10568, 16000,  8051,  2410,  1883,  2144,  1100 },  // OH  F1 570 F2 840
  { 16000,  8368,  6043,  6689,  3106,  2657,  2533,  1270 },  // OO  F1 300 F2 870
  {   801,  7697, 16000, 11856,  8440,  5035,  3707,  1537 },  // UH  F1 640 F2 1190
  {  1518, 16000, 12137,  3653,  7990, 14057, 10936,  3136 },  // EH  F1 530 F2 1840
  { 16000,  4167,   864,   929,  2824,  8556, 12114,  5952 },  // EE  F1 270 F2 2290
};
}  // namespace tract8

#endif  // TRACT8_VOWELS_H_
