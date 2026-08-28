#!/usr/bin/env python3
"""Can the card actually be played? Regression test for the control model.

TWO ROUNDS OF HARDWARE FEEDBACK SHAPED THIS FILE, and both said the same
thing in different words.

v1.1.0: "with faders moving, the main knob doesn't do anything." The faders
wrote band gains directly and latched the panel out on first touch.

v1.2.0: "I would prefer the vowel sounds just on three sliders. It's too
hard to try to manipulate the partials." That is the deeper of the two.
Per-band control is the WRONG ABSTRACTION for playing, however well behaved
it is made: a vowel is a position of the mouth, not eight independent
numbers, and asking a player to operate seven faders as a chord is asking
them to solve the Voder's original problem - the one its operators trained
for months to overcome.

So the partials left the interface entirely. Two controls place a point in
the vowel square (openness = F1, front = F2) and the eight band gains are a
bilinear blend of four corner vowels. Everything a mouth can do is reachable
with two fingers.

What this file pins down is that the space is genuinely playable: both axes
do something wherever you are in it, the corners are exact, no position
produces silence or a hole, and the vowels that are not corners are still
reachable inside it.

Checks:
  1. The four corners reproduce their vowels exactly.
  2. Both axes move the spectrum audibly, everywhere in the square.
  3. UH is reachable inside the square; OH's known miss has not grown.
  4. No position in the square is silent or holed.
  5. Brightness tilts without pinning any band.

Run: python tools/playable_check.py
"""

import sys
import math

# Transcribed from vowels.h.
NUM_BANDS = 8
CORNERS = {
    'OO': [16000, 8369, 6043, 6689, 3106, 2657, 2533, 1271],     # close back
    'AH': [666, 4003, 16000, 15423, 7647, 4226, 3923, 1675],     # open  back
    'EE': [16000, 4167, 864, 929, 2824, 8556, 12114, 5952],      # close front
    'EH': [1518, 16000, 12137, 3653, 7991, 14057, 10935, 3136],  # open  front
}
CLOSE_BACK, OPEN_BACK = CORNERS['OO'], CORNERS['AH']
CLOSE_FRONT, OPEN_FRONT = CORNERS['EE'], CORNERS['EH']

# Vowels that are NOT corners, to check the square contains them.
#
# UH is genuinely inside the square and lands within 2 dB. OH does NOT,
# and that is a real and accepted limitation rather than a bug: OH is
# rounder than anything on the OO-AH edge, its F2 (840 Hz) sitting BELOW
# both back corners, so no bilinear blend of these four reaches it. The
# alternative was to make OH a corner instead of AH, which trades a 6.0 dB
# miss on OH for a 5.1 dB miss on AH - a bad deal, because AH is the more
# useful vowel and the one a player reaches for first.
#
# The tolerance below records that: UH must land, OH is allowed its miss,
# and if anyone retunes the corners this test says which trade they made.
NON_CORNER = {
    'UH': ([801, 7697, 16000, 11856, 8440, 5035, 3707, 1537], 3.0),
    'OH': ([881, 10568, 16000, 8051, 2410, 1883, 2144, 1100], 6.5),
}


def clamp15(x):
    return max(0, min(32767, x))


def blend(openness, front):
    """Transcription of BlendVowel() in vowels.h."""
    out = []
    for i in range(NUM_BANDS):
        back = CLOSE_BACK[i] + (((OPEN_BACK[i] - CLOSE_BACK[i]) * openness) >> 15)
        fr = CLOSE_FRONT[i] + (((OPEN_FRONT[i] - CLOSE_FRONT[i]) * openness) >> 15)
        out.append(back + (((fr - back) * front) >> 15))
    return out


def with_bright(gains, bright):
    """Transcription of the brightness tilt in ReadPanel()."""
    tilt = bright - 16384
    out = []
    for i, g in enumerate(gains):
        pos = (i * 2) - 7
        factor = max(0, 32768 + ((tilt * pos) >> 4))
        out.append(clamp15((g * factor) >> 15))
    return out


def spectral_distance(v1, v2):
    d = 0.0
    for a, b in zip(v1, v2):
        da = 20 * math.log10(max(a, 1) / 32767.0)
        db = 20 * math.log10(max(b, 1) / 32767.0)
        d += (da - db) ** 2
    return math.sqrt(d / len(v1))


def check_corners():
    print("\n1. The four corners reproduce their vowels exactly")
    ok = True
    for name, (o, f) in (('OO', (0, 0)), ('AH', (32767, 0)),
                         ('EE', (0, 32767)), ('EH', (32767, 32767))):
        d = spectral_distance(blend(o, f), CORNERS[name])
        good = d < 0.5
        if not good:
            ok = False
        print(f"   {name}  openness={o:5d} front={f:5d}   error {d:5.2f} dB   "
              f"{'ok' if good else '<-- FAIL'}")
    return ok


def check_axes_live():
    """Both axes must do something WHEREVER you are in the square."""
    print("\n2. Both axes move the spectrum, everywhere in the square")
    ok = True
    print("   sweeping OPENNESS at each front position:")
    for f in (0, 16384, 32767):
        d = spectral_distance(blend(0, f), blend(32767, f))
        good = d > 5.0
        if not good:
            ok = False
        print(f"     front={f:5d}   close->open moves {d:5.1f} dB   "
              f"{'ok' if good else '<-- DEAD AXIS'}")

    print("   sweeping FRONT at each openness position:")
    for o in (0, 16384, 32767):
        d = spectral_distance(blend(o, 0), blend(o, 32767))
        good = d > 5.0
        if not good:
            ok = False
        print(f"     openness={o:5d}   back->front moves {d:5.1f} dB   "
              f"{'ok' if good else '<-- DEAD AXIS'}")
    print("   (the v1.1.0 regression generalised: no control may stop")
    print("    working because of where another one happens to be set)")
    return ok


def check_non_corner_vowels():
    print("\n3. Non-corner vowels are reachable inside the square")
    ok = True
    for name, (target, tol) in NON_CORNER.items():
        best = (999.0, 0, 0)
        for o in range(0, 32768, 512):
            for f in range(0, 32768, 512):
                d = spectral_distance(blend(o, f), target)
                if d < best[0]:
                    best = (d, o, f)
        # Vowels are 6-17 dB apart, so landing well inside that means the
        # square genuinely contains the vowel. Per-vowel tolerance above.
        good = best[0] < tol
        if not good:
            ok = False
        print(f"   {name}  closest {best[0]:4.1f} dB at "
              f"openness={best[1]*100//32767:3d}% front={best[2]*100//32767:3d}%   "
              f"{'ok' if good else '<-- NOT REACHABLE'}"
              f"{'  (accepted miss, see the note above)' if name == 'OH' else ''}")
    return ok


def check_no_dead_spots():
    print("\n4. No position in the square is silent or holed")
    worst_sum = (1 << 30, 0, 0)
    worst_min = (1 << 30, 0, 0)
    for o in range(0, 32768, 2048):
        for f in range(0, 32768, 2048):
            g = blend(o, f)
            total = sum(g)
            lowest = min(g)
            if total < worst_sum[0]:
                worst_sum = (total, o, f)
            if lowest < worst_min[0]:
                worst_min = (lowest, o, f)
    # A vowel needs real energy somewhere, and no band should sit at zero:
    # a band at zero is a hole no other control can reopen.
    good_sum = worst_sum[0] > 20000
    good_min = worst_min[0] > 200
    print(f"   quietest position: total {worst_sum[0]:6d} at "
          f"({worst_sum[1]},{worst_sum[2]})   "
          f"{'ok' if good_sum else '<-- TOO QUIET'}")
    print(f"   lowest single band: {worst_min[0]:6d} at "
          f"({worst_min[1]},{worst_min[2]})   "
          f"{'ok' if good_min else '<-- HOLE'}")
    return good_sum and good_min


def check_brightness():
    print("\n5. Brightness tilts without pinning a band")
    ok = True
    for name, (o, f) in (('OO', (0, 0)), ('AH', (32767, 0)),
                         ('EE', (0, 32767)), ('EH', (32767, 32767))):
        base = blend(o, f)
        worst_pin = 0
        span = 0.0
        for b in (0, 8192, 16384, 24576, 32767):
            t = with_bright(base, b)
            pinned = sum(1 for i in range(NUM_BANDS)
                         if t[i] in (0, 32767) and base[i] not in (0, 32767))
            worst_pin = max(worst_pin, pinned)
            span = max(span, spectral_distance(base, t))
        good = worst_pin == 0 and span > 0.5
        if not good:
            ok = False
        print(f"   {name}  {worst_pin}/8 pinned, span {span:4.1f} dB   "
              f"{'ok' if good else '<-- FAIL'}")
    return ok


def main():
    print("TRACT8 playability check - the 2-D vowel square")
    print("  Two controls, four corner vowels, bilinear between them.")
    ok = check_corners()
    ok &= check_axes_live()
    ok &= check_non_corner_vowels()
    ok &= check_no_dead_spots()
    ok &= check_brightness()
    print("\nPASS" if ok else "\nFAIL")
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
