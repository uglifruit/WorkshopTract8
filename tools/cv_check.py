#!/usr/bin/env python3
"""The jack inputs, and whether random voltages actually make it babble.

THE USE CASE THIS EXISTS FOR: "I want to be able to feed in random voltages
and gates and get chattering." That is a different requirement from "the CV
inputs work", and it is the one worth testing, because a card can track CV
perfectly and still be useless fed from a sample-and-hold.

Three properties make the difference, and none of them is obvious from the
wiring:

  1. THE CVs ADD, they do not replace. A random voltage should wander the
     sound around wherever the controls are parked, so the patch never has
     to supply a sensible absolute value. An input that replaced its
     control would make the knobs dead the moment a cable went in.
  2. THE FORMANT CV MOVES DIAGONALLY. Sweeping one axis of the vowel cube
     mostly travels between two corners; moving openness up as front moves
     down crosses the middle, where the distinct vowels are. One random
     voltage then walks through recognisably different vowels rather than
     shading one of them.
  3. GATES MUST CHATTER WITHOUT CLICKING. The glottal ramp is 2 ms, so a
     gate stream stays clean up to a few hundred Hz and then degrades into
     amplitude modulation rather than into clicks - which is a usable
     texture rather than a fault.

Jacks:
  Audio In 1  breath CV     Audio In 2  volume CV
  CV In 1     pitch 1V/oct  CV In 2     formant, bipolar
  Pulse In 1  click         Pulse In 2  glottal gate

Checks:
  1. Every CV adds to its control rather than replacing it.
  2. A random formant CV visits genuinely different vowels.
  3. The breath CV spans the whole buzz/noise balance.
  4. The volume CV ducks and swells around the base level.
  5. Gate streams chatter cleanly across a useful frequency range.

Run: python tools/cv_check.py
"""

import sys
import math

# Transcribed from vowels.h.
NUM_BANDS = 8
SPREAD = {
    'OO': [16000, 8369, 6043, 6689, 3106, 2657, 2533, 1271],
    'AH': [666, 4003, 16000, 15423, 7647, 4226, 3923, 1675],
    'EE': [16000, 4167, 864, 929, 2824, 8556, 12114, 5952],
    'EH': [1518, 16000, 12137, 3653, 7991, 14057, 10935, 3136],
}
ROUND = {
    'OO': [16000, 8053, 4419, 1387, 1337, 2189, 1977, 978],
    'AH': [569, 7483, 16000, 6101, 1596, 2496, 2808, 1089],
    'EE': [16000, 2170, 1193, 4525, 8724, 7189, 6062, 3677],
    'EH': [2308, 16000, 8610, 8916, 8276, 6071, 5253, 1959],
}

GATE_RAMP = 96          # samples, from voder.h
FS = 48000


def clamp15(x):
    return max(0, min(32767, x))


def blend(openness, front, round_amt=0):
    out = []
    for i in range(NUM_BANDS):
        sb = SPREAD['OO'][i] + (((SPREAD['AH'][i] - SPREAD['OO'][i]) * openness) >> 15)
        sf = SPREAD['EE'][i] + (((SPREAD['EH'][i] - SPREAD['EE'][i]) * openness) >> 15)
        spread = sb + (((sf - sb) * front) >> 15)
        rb = ROUND['OO'][i] + (((ROUND['AH'][i] - ROUND['OO'][i]) * openness) >> 15)
        rf = ROUND['EE'][i] + (((ROUND['EH'][i] - ROUND['EE'][i]) * openness) >> 15)
        rounded = rb + (((rf - rb) * front) >> 15)
        out.append(spread + (((rounded - spread) * round_amt) >> 15))
    return out


def spectral_distance(v1, v2):
    d = 0.0
    for a, b in zip(v1, v2):
        da = 20 * math.log10(max(a, 1) / 32767.0)
        db = 20 * math.log10(max(b, 1) / 32767.0)
        d += (da - db) ** 2
    return math.sqrt(d / len(v1))


def formant_positions(base_open, base_front, cv):
    """Transcription of the CV In 2 diagonal in ReadPanel()."""
    return (clamp15(base_open + cv), clamp15(base_front - cv))


def check_cvs_add():
    print("\n1. Every CV adds to its control rather than replacing it")
    ok = True

    # Formant CV at zero must leave the controls exactly where they are.
    for base in (0, 8000, 16384, 32767):
        o, f = formant_positions(base, base, 0)
        good = o == base and f == base
        if not good:
            ok = False
        print(f"   formant CV 0 at base {base:5d} -> ({o:5d},{f:5d})   "
              f"{'ok' if good else '<-- REPLACED'}")

    # And a CV must move it from wherever it was, not to a fixed place.
    a = formant_positions(4000, 4000, 8000)
    b = formant_positions(20000, 20000, 8000)
    good = a != b
    if not good:
        ok = False
    print(f"   same CV from two bases gives {a} and {b}   "
          f"{'ok - relative' if good else '<-- ABSOLUTE'}")
    print("   (a patch should never have to supply a sensible absolute")
    print("    value; the knobs stay live with a cable plugged in)")
    return ok


def check_formant_diagonal():
    """A random CV must visit genuinely different vowels."""
    print("\n2. A random formant CV visits different vowels")
    ok = True

    # Walk the CV across its range from a centred base and measure how far
    # apart the extremes are, and how much ground is covered on the way.
    base = 16384
    seen = []
    for cv in range(-16384, 16385, 2048):
        o, f = formant_positions(base, base, cv)
        seen.append(blend(o, f, 0))

    span = spectral_distance(seen[0], seen[-1])
    good = span > 8.0
    if not good:
        ok = False
    print(f"   full CV sweep spans {span:5.1f} dB   "
          f"{'ok' if good else '<-- TOO NARROW'}")

    # Consecutive steps must differ audibly, or the sweep has dead zones
    # where a slow random would sit and do nothing.
    steps = [spectral_distance(seen[i], seen[i + 1])
             for i in range(len(seen) - 1)]
    worst = min(steps)
    good = worst > 0.3
    if not good:
        ok = False
    print(f"   smallest step along the sweep {worst:4.2f} dB   "
          f"{'ok - no dead zones' if good else '<-- DEAD ZONE'}")

    # The diagonal must beat moving one axis alone, which is the reason
    # for opposing the two.
    diag = spectral_distance(blend(0, 32767, 0), blend(32767, 0, 0))
    single = spectral_distance(blend(0, 16384, 0), blend(32767, 16384, 0))
    good = diag > single
    if not good:
        ok = False
    print(f"   diagonal {diag:5.1f} dB vs single axis {single:5.1f} dB   "
          f"{'ok - diagonal is wider' if good else '<-- NO BENEFIT'}")
    return ok


def check_breath_cv():
    print("\n3. The breath CV spans the whole balance")
    ok = True
    # Audio In 1 is <<3, so +/-2048 becomes +/-16384.
    for cv, label in ((-2048, "-5V"), (0, "0V"), (2047, "+5V")):
        for base, bl in ((0, "buzz"), (16384, "mid"), (32767, "noise")):
            mix = clamp15(base + (cv << 3))
            if base == 16384:
                print(f"   {label:3} from {bl:5} -> {mix:5d}")
    lo = clamp15(16384 + (-2048 << 3))
    hi = clamp15(16384 + (2047 << 3))
    # The positive rail is 2047, not 2048, so the top lands 7 counts short
    # of full scale - 0.002 dB, which is not a real limit.
    good = lo == 0 and hi > 32700
    if not good:
        ok = False
    print(f"   from centre, +/-5V reaches {lo} and {hi}   "
          f"{'ok - full span' if good else '<-- CANNOT REACH THE ENDS'}")
    return ok


def check_volume_cv():
    print("\n4. The volume CV ducks and swells around the base")
    ok = True
    # Audio In 2 is <<4, so +/-2048 becomes +/-32768.
    base = 32767          # no 8mu: base is full
    duck = clamp15(base + (-2048 << 4))
    good = duck == 0
    if not good:
        ok = False
    print(f"   -5V from full -> {duck:5d}   "
          f"{'ok - ducks to silence' if good else 'FAIL'}")

    half = 16384
    up = clamp15(half + (2047 << 4))
    down = clamp15(half + (-2048 << 4))
    good = up == 32767 and down == 0
    if not good:
        ok = False
    print(f"   from half: +5V {up:5d}, -5V {down:5d}   "
          f"{'ok - swells and ducks' if good else 'FAIL'}")
    print("   (an envelope here articulates a phrase without the patch")
    print("    having to control absolute level)")
    return ok


def check_gate_chatter():
    """Fast gates must chatter, not click and not stall."""
    print("\n5. Gate streams chatter cleanly")
    ok = True
    step = 32768 // GATE_RAMP
    print("   gate Hz   envelope reaches   behaviour")
    for hz in (5, 20, 50, 100, 200, 400, 800):
        half = FS // (hz * 2)
        reached = min(32767, half * step)
        pct = reached * 100 // 32767
        if pct == 100:
            note = "full gate"
        elif pct > 40:
            note = "amplitude modulation - usable texture"
        else:
            note = "tremolo, very shallow"
        print(f"     {hz:4d}         {pct:3d}%           {note}")

    # Up to a few hundred Hz the gate must fully open, or a gate sequence
    # would sound quieter than a held note.
    half200 = FS // 400
    good = min(32767, half200 * step) == 32767
    if not good:
        ok = False
    print(f"   fully opens at 200 Hz   "
          f"{'ok' if good else '<-- GATES LOSE LEVEL'}")

    # And the 2 ms ramp must be long enough to prevent a click. A step
    # discontinuity at 48 kHz is broadband; 96 samples of ramp puts the
    # fastest edge below about 500 Hz of equivalent slew.
    ramp_ms = GATE_RAMP * 1000.0 / FS
    good = ramp_ms >= 1.0
    if not good:
        ok = False
    print(f"   ramp is {ramp_ms:.1f} ms   "
          f"{'ok - no click' if good else '<-- CLICKS'}")
    return ok


def main():
    print("TRACT8 CV and gate check")
    print("  Fed from random voltages, does it babble usefully?")
    ok = check_cvs_add()
    ok &= check_formant_diagonal()
    ok &= check_breath_cv()
    ok &= check_volume_cv()
    ok &= check_gate_chatter()
    print("\nPASS" if ok else "\nFAIL")
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
