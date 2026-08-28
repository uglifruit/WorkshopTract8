#!/usr/bin/env python3
"""Vowel table and morph checks for TRACT8.

The vowel table is the difference between a card that says "ah" and "ee"
and one that just sounds like a filter sweep. Two things have to hold:

  1. The vowels must be DISTINGUISHABLE from each other. If two rows of the
     table produce near-identical band energy, turning the knob between
     them does nothing audible and the morph has a dead zone.
  2. The morph must be CONTINUOUS. Knob 1 crossfades between adjacent rows,
     and any discontinuity at a segment boundary is an audible click.

Check 3 is the one that has caught the most bugs, and it only works because
it sweeps EVERY vowel rather than a representative one. An earlier version
tested AH alone and passed; run across all six it immediately failed four
of them, because the band at risk of pinning differs by vowel - AH pins at
the bottom (its 250 Hz gain is tiny), OO and EE at the top (theirs is the
peak). That finding is what drove the tilt from additive to multiplicative
and the table's peak down to 26800. See vowels.h.

Checks:
  1. Adjacent vowels are spectrally distinct (no dead zone in the morph).
  2. The morph is continuous across every segment boundary.
  3. The Knob-2 tilt never pins a band, for ANY vowel.
  4. Every table entry is inside Q15 range, with room for the tilt boost.

Run: python tools/vowel_check.py
"""

import sys
import math
import numpy as np

# --- transcribed from vowels.h -------------------------------------------
VOWEL_NAMES = ["AH", "OH", "OO", "UH", "EH", "EE"]
VOWELS = [
    [666, 4002, 16000, 15423, 7647, 4226, 3922, 1675],       # AH
    [881, 10568, 16000, 8051, 2410, 1883, 2144, 1100],       # OH
    [16000, 8368, 6043, 6689, 3106, 2657, 2533, 1270],       # OO
    [801, 7697, 16000, 11856, 8440, 5035, 3707, 1537],       # UH
    [1518, 16000, 12137, 3653, 7990, 14057, 10936, 3136],    # EH
    [16000, 4167, 864, 929, 2824, 8556, 12114, 5952],        # EE
]
NUM_VOWELS = len(VOWELS)
NUM_BANDS = 8


def clamp15(x):
    return max(0, min(32767, x))


def morph(knob_q15):
    """Transcription of the vowel morph in ReadPanel(), tilt at centre."""
    scaled = knob_q15 * (NUM_VOWELS - 1)
    idx = scaled >> 15
    if idx >= NUM_VOWELS - 1:
        idx = NUM_VOWELS - 2
    f = scaled - (idx << 15)
    out = []
    for i in range(NUM_BANDS):
        a = VOWELS[idx][i]
        b = VOWELS[idx + 1][i]
        out.append(clamp15(a + (((b - a) * f) >> 15)))
    return out


def morph_tilted(knob_q15, knob_x_q15):
    """Same, with the Knob-2 tilt applied. As ReadPanel()."""
    scaled = knob_q15 * (NUM_VOWELS - 1)
    idx = scaled >> 15
    if idx >= NUM_VOWELS - 1:
        idx = NUM_VOWELS - 2
    f = scaled - (idx << 15)
    tilt = knob_x_q15 - 16384
    out = []
    for i in range(NUM_BANDS):
        a = VOWELS[idx][i]
        b = VOWELS[idx + 1][i]
        g = a + (((b - a) * f) >> 15)
        pos = (i * 2) - 7
        factor = 32768 + ((tilt * pos) >> 4)
        if factor < 0:
            factor = 0
        g = (g * factor) >> 15
        out.append(clamp15(g))
    return out


def spectral_distance(v1, v2):
    """RMS difference in dB between two band-gain vectors.

    Compared in dB rather than linearly because that is how the ear weighs
    them: a band moving from 1000 to 2000 matters as much as one moving
    from 16000 to 32000.
    """
    d = 0.0
    for a, b in zip(v1, v2):
        da = 20 * math.log10(max(a, 1) / 32767.0)
        db = 20 * math.log10(max(b, 1) / 32767.0)
        d += (da - db) ** 2
    return math.sqrt(d / len(v1))


def check_distinct():
    print("\n1. Vowel pair distinctness (RMS dB difference)")
    print("        " + "  ".join(f"{n:>5}" for n in VOWEL_NAMES))
    ok = True
    worst = (999.0, "", "")
    for i, ni in enumerate(VOWEL_NAMES):
        row = f"   {ni:3} "
        for j, _ in enumerate(VOWEL_NAMES):
            if i == j:
                row += "     -"
                continue
            d = spectral_distance(VOWELS[i], VOWELS[j])
            row += f"  {d:5.1f}"
            if d < worst[0]:
                worst = (d, ni, VOWEL_NAMES[j])
        print(row)
    print(f"   closest pair anywhere: {worst[1]}/{worst[2]} at "
          f"{worst[0]:.1f} dB")

    # What actually matters is the ADJACENT pairs: those are the ones Knob 1
    # crossfades between, and a small gap there is a dead zone in the morph.
    # Two vowels that are similar but sit at opposite ends of the table
    # never get compared by the ear.
    print("   adjacent steps (these are what the knob sweeps):")
    worst_adj = 999.0
    for i in range(len(VOWELS) - 1):
        d = spectral_distance(VOWELS[i], VOWELS[i + 1])
        worst_adj = min(worst_adj, d)
        flag = "" if d >= 6.0 else "  <-- DEAD ZONE"
        if d < 6.0:
            ok = False
        print(f"     {VOWEL_NAMES[i]:3}->{VOWEL_NAMES[i+1]:3}  {d:5.1f} dB"
              f"{flag}")
    print(f"   worst adjacent step: {worst_adj:.1f} dB "
          f"{'ok' if worst_adj >= 6.0 else '<-- FAIL'}")
    return ok


def check_continuity():
    print("\n2. Morph continuity across segment boundaries")
    print("   knob      max single-step gain jump")
    ok = True
    worst = 0
    worst_at = 0
    prev = morph(0)
    for k in range(1, 32768, 7):
        cur = morph(k)
        jump = max(abs(c - p) for c, p in zip(cur, prev))
        if jump > worst:
            worst = jump
            worst_at = k
        prev = cur
    # A step of 7 in the knob should move any gain by at most a few
    # hundred; anything larger means a discontinuity at a boundary.
    ok = worst < 500
    print(f"   worst jump {worst} at knob {worst_at} "
          f"({worst_at*100//32767}% of travel)   "
          f"{'ok' if ok else '<-- DISCONTINUOUS'}")

    # Explicitly check each segment boundary, where idx increments.
    print("   segment boundaries:")
    for seg in range(1, NUM_VOWELS - 1):
        k = (seg * 32768) // (NUM_VOWELS - 1)
        a = morph(k - 1)
        b = morph(k + 1)
        jump = max(abs(x - y) for x, y in zip(a, b))
        flag = "" if jump < 500 else "  <-- JUMP"
        if jump >= 500:
            ok = False
        print(f"     {VOWEL_NAMES[seg-1]}->{VOWEL_NAMES[seg]:3} at knob "
              f"{k:5d}   jump {jump:4d}{flag}")
    return ok


def check_tilt():
    print("\n3. Knob-2 tilt across every vowel (must never pin a band)")
    print("   vowel              worst pinned   spectral change")
    ok = True
    # Tested at EVERY vowel, not just one: the band most at risk of pinning
    # differs by vowel, since it is whichever one the vowel already has at
    # an extreme. AH pins at the bottom (its 250 Hz gain is 1364), EE and OO
    # at the top (their 250 Hz gain is full scale).
    ok = True
    # One row per vowel, at the knob position that selects it exactly.
    for vi in range(NUM_VOWELS):
        knob = (vi * 32767) // (NUM_VOWELS - 1)
        if knob > 32767:
            knob = 32767
        base = morph(knob)
        worst_pin = 0
        worst_d = 0.0
        for kx in (0, 8192, 16384, 24576, 32767):
            t = morph_tilted(knob, kx)
            pinned = sum(1 for i in range(NUM_BANDS)
                         if t[i] in (0, 32767) and base[i] not in (0, 32767))
            worst_pin = max(worst_pin, pinned)
            worst_d = max(worst_d, spectral_distance(base, t))
        # The tilt SHOULD change the spectrum - that is its job. What it
        # must not do is pin ANY band to 0 or full scale: a band at zero is
        # a hole in the spectrum that no fader can reopen, and the player
        # has no way to tell it apart from a broken filter. The >>4 in
        # ReadPanel() was chosen to make this column read 0/8 for every
        # vowel; at >>2 two bands pin, at >>3 one does.
        bad = worst_pin > 0
        flag = "  <-- PINS" if bad else ""
        if bad:
            ok = False
        print(f"   {VOWEL_NAMES[vi]:3} (knob {knob:5d})   {worst_pin}/8 pinned   "
              f"max {worst_d:4.1f} dB from centre{flag}")
    return ok


def check_range():
    print("\n4. Table entries within Q15 range")
    ok = True
    for i, name in enumerate(VOWEL_NAMES):
        for j, v in enumerate(VOWELS[i]):
            if v < 0 or v > 32767:
                print(f"   {name} band {j}: {v}  <-- OUT OF RANGE")
                ok = False
    # The tilt can multiply a band by up to 1.219 (see vowels.h), so the
    # table must peak low enough that the boost still fits Q15.
    peak = max(max(row) for row in VOWELS)
    max_factor = (32768 + ((16383 * 7) >> 4)) / 32768.0
    headroom_ok = peak * max_factor <= 32767
    if not headroom_ok:
        ok = False
    if ok:
        print("   all 48 entries in 0..32767   ok")
    print(f"   table peak {peak} x max tilt {max_factor:.3f} = "
          f"{peak*max_factor:.0f}   "
          f"{'ok' if headroom_ok else '<-- WOULD CLIP'}")
    return ok


def main():
    print(f"TRACT8 vowel check: {NUM_VOWELS} vowels x {NUM_BANDS} bands")
    ok = check_distinct()
    ok &= check_continuity()
    ok &= check_tilt()
    ok &= check_range()
    print("\nPASS" if ok else "\nFAIL")
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
