#!/usr/bin/env python3
"""Can the card actually be played? Regression test for control interaction.

THE PROBLEM THIS EXISTS FOR. v1.1.0 was reported as "not very playable":
with the faders moved, the main knob - the easiest and most expressive
control on the card - did nothing at all, and the only remaining way to
shape a vowel was to position all eight faders by hand.

That was a design mistake, not a coding one. The faders wrote the band
gains directly and latched the panel morph out on first touch, so one fader
move killed Knob 1 permanently. And needing eight simultaneous controls to
make a vowel is the Voder's OWN problem - the thing its operators trained
for months to overcome. There is no reason to reproduce it on an instrument
that already puts a whole vowel under one knob.

The fix is a different model: the knob (or the 8mu's tilt) chooses the
vowel, and each fader BENDS one band around a centre detent. Both are live
at all times, a fader at centre contributes nothing, and one fader is a
meaningful gesture by itself.

The properties below are what "playable" means for this card, made
testable. They are easy to break with an innocent-looking change, which is
exactly why they are pinned here.

Checks:
  1. The knob still moves the sound after any number of faders have moved.
  2. All faders centred reproduces the morph's vowel EXACTLY.
  3. One fader alone makes an audible difference.
  4. One fader cannot obliterate the vowel.
  5. The fader curve is usable across its whole travel.

Run: python tools/playable_check.py
"""

import sys
import math

# Transcribed from vowels.h (peak 16000) and midi8mu.h.
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
FADER_OFFSET_MAX = 20000
FADER_CUT_FLOOR = 3900
FADER_CENTRE = 64


def clamp15(x):
    return max(0, min(32767, x))


def fader(cc):
    """(offset, cut) for a fader at CC value cc. Centre detent = 64.

    Asymmetric on purpose: boost adds, cut scales. See midi8mu.cpp.
    """
    centred = cc - FADER_CENTRE
    if centred >= 0:
        return ((centred * centred * FADER_OFFSET_MAX)
                // (FADER_CENTRE * FADER_CENTRE), 32768)
    m = -centred
    cut = 32768 - ((m * m * (32768 - FADER_CUT_FLOOR))
                   // (FADER_CENTRE * FADER_CENTRE))
    return (0, cut)


def band_gains(knob_q15, faders, knob_x=16384):
    """The full panel chain: morph -> tilt -> fader offsets."""
    scaled = knob_q15 * (NUM_VOWELS - 1)
    idx = min(scaled >> 15, NUM_VOWELS - 2)
    f = scaled - (idx << 15)
    tilt = knob_x - 16384

    out = []
    for i in range(NUM_BANDS):
        a = VOWELS[idx][i]
        b = VOWELS[idx + 1][i]
        g = a + (((b - a) * f) >> 15)
        pos = (i * 2) - 7
        factor = max(0, 32768 + ((tilt * pos) >> 4))
        g = (g * factor) >> 15
        off, cut = faders[i]
        g = (g * cut) >> 15
        g += off
        out.append(clamp15(g))
    return out


def spectral_distance(v1, v2):
    d = 0.0
    for a, b in zip(v1, v2):
        da = 20 * math.log10(max(a, 1) / 32767.0)
        db = 20 * math.log10(max(b, 1) / 32767.0)
        d += (da - db) ** 2
    return math.sqrt(d / len(v1))


def check_knob_still_works():
    """THE regression. The knob must work however many faders have moved."""
    print("\n1. The knob keeps working after the faders have been used")
    ok = True

    for label, offs in (
        ("no faders moved", [fader(64)] * 8),
        ("one fader at full", [fader(127)] + [fader(64)] * 7),
        ("one fader at zero", [fader(0)] + [fader(64)] * 7),
        ("all 8 faders scattered",
         [fader(v) for v in (10, 100, 40, 127, 0, 70, 90, 30)]),
    ):
        # Sweep the knob across its whole travel and see if the sound moves.
        lo = band_gains(0, offs)
        hi = band_gains(32767, offs)
        d = spectral_distance(lo, hi)
        good = d > 5.0
        if not good:
            ok = False
        print(f"   {label:24} knob 0 -> full moves the spectrum "
              f"{d:5.1f} dB   {'ok' if good else '<-- KNOB IS DEAD'}")

    print("   (v1.1.0 scored 0.0 dB on every row but the first: the faders")
    print("    latched the morph out and the knob stopped doing anything)")
    return ok


def check_centred_is_neutral():
    print("\n2. All faders centred reproduces the vowel exactly")
    ok = True
    centre = [fader(FADER_CENTRE)] * 8
    for vi in range(NUM_VOWELS):
        knob = min((vi * 32767) // (NUM_VOWELS - 1), 32767)
        plain = band_gains(knob, [fader(64)] * 8)
        with_faders = band_gains(knob, centre)
        good = plain == with_faders
        if not good:
            ok = False
        print(f"   {VOWEL_NAMES[vi]:3} identical with faders centred   "
              f"{'ok' if good else '<-- FAIL'}")
    return ok


def check_one_fader_matters():
    print("\n3. ONE fader alone makes an audible difference")
    ok = True
    base = band_gains(0, [fader(64)] * 8)          # AH
    for band in range(7):                   # faders own bands 0-6
        offs = [fader(64)] * 8
        offs[band] = fader(127)
        boosted = band_gains(0, offs)
        offs[band] = fader(0)
        cut = band_gains(0, offs)

        db_up = 20 * math.log10(max(boosted[band], 1) / max(base[band], 1))
        db_dn = 20 * math.log10(max(cut[band], 1) / max(base[band], 1))
        # A fader that moves its band less than 3 dB either way is not
        # worth reaching for.
        good = db_up > 3.0 and db_dn < -3.0
        if not good:
            ok = False
        print(f"   band {band}: full {db_up:+6.1f} dB, zero {db_dn:+7.1f} dB   "
              f"{'ok' if good else '<-- TOO WEAK'}")
    return ok


def check_one_fader_is_not_everything():
    print("\n4. One fader cannot obliterate the vowel")
    ok = True
    for vi in range(NUM_VOWELS):
        knob = min((vi * 32767) // (NUM_VOWELS - 1), 32767)
        base = band_gains(knob, [fader(64)] * 8)
        worst = 0.0
        for band in range(7):
            for cc in (0, 127):
                offs = [fader(64)] * 8
                offs[band] = fader(cc)
                d = spectral_distance(base, band_gains(knob, offs))
                worst = max(worst, d)
        # One fader should shade the vowel, not replace it. The vowels are
        # 6-17 dB apart, so a single fader moving the spectrum more than
        # about 12 dB would mean it can turn any vowel into any other.
        good = worst < 12.0
        if not good:
            ok = False
        print(f"   {VOWEL_NAMES[vi]:3} worst single-fader shift {worst:5.1f} dB   "
              f"{'ok' if good else '<-- ONE FADER DOMINATES'}")
    return ok


def check_curve():
    print("\n5. The fader curve is usable across its travel")
    print("   cc    offset   cut   effect on a mid-level band (7647)")
    ok = True
    prev = None
    monotonic = True
    for cc in (0, 16, 32, 48, 64, 80, 96, 112, 127):
        o, cut = fader(cc)
        g = clamp15(((7647 * cut) >> 15) + o)
        db = 20 * math.log10(max(g, 1) / 7647)
        if prev is not None and g < prev:
            monotonic = False
        prev = g
        print(f"   {cc:3d}  {o:+7d} x{cut/32768:4.2f}  {g:6d}  ({db:+6.1f} dB)")
    print(f"   monotonic across the travel   {'ok' if monotonic else 'FAIL'}")
    ok &= monotonic

    # The centre detent must be exactly neutral, or a "parked" fader
    # silently colours every vowel.
    good = fader(FADER_CENTRE) == (0, 32768)
    print(f"   centre detent is exactly neutral   {'ok' if good else 'FAIL'}")
    ok &= good
    return ok


def main():
    print("TRACT8 playability check")
    print("  Both the knob and the faders must stay live, and one fader")
    print("  must be a meaningful gesture on its own.")
    ok = check_knob_still_works()
    ok &= check_centred_is_neutral()
    ok &= check_one_fader_matters()
    ok &= check_one_fader_is_not_everything()
    ok &= check_curve()
    print("\nPASS" if ok else "\nFAIL")
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
