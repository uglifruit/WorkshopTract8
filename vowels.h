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
// THE PEAK IS 26800, NOT 32767, AND THAT IS DELIBERATE. Knob 2's tilt
// multiplies each band by up to 1.219, so a table that peaked at full
// scale would clip its loudest band the moment the knob moved clockwise -
// and the vowels whose peak sits at band 0 (OO and EE) would lose exactly
// the formant that identifies them. 26800 * 1.219 = 32669, just inside
// Q15. Raising these numbers to "use the full range" reintroduces the bug;
// tools/vowel_check.py check 3 fails if you do.
//
// The scaling is uniform, so it costs nothing in distinctness - a constant
// factor cancels in the dB comparison - and the lost 1.8 dB of level is
// made up in the output gain staging.
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
  {  1116,  6704, 26800, 25834, 12808,  7078,  6570,  2806 },  // AH  F1 730 F2 1090
  {  1475, 17701, 26800, 13485,  4036,  3154,  3591,  1843 },  // OH  F1 570 F2 840
  { 26800, 14017, 10122, 11204,  5202,  4450,  4242,  2128 },  // OO  F1 300 F2 870
  {  1341, 12892, 26800, 19858, 14137,  8433,  6209,  2574 },  // UH  F1 640 F2 1190
  {  2543, 26800, 20330,  6119, 13384, 23545, 18317,  5253 },  // EH  F1 530 F2 1840
  { 26800,  6979,  1448,  1556,  4731, 14331, 20291,  9970 },  // EE  F1 270 F2 2290
};
}  // namespace tract8

#endif  // TRACT8_VOWELS_H_
