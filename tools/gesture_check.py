#!/usr/bin/env python3
"""8mu accelerometer semantics, as stated by someone holding the device.

THE SEMANTICS, ESTABLISHED THE HARD WAY:

  - The CCs are CONTINUOUS LEVELS, 0-127, sweeping as the device tilts.
  - They come in COMPLEMENTARY PAIRS that add to 127, so a level device
    sits at 64 on each axis.
  - CC 49, despite its "inverted" label, reads HIGH while the device is the
    right way up and falls when it is turned over.

Because a level device sits at 64, both tilt axes are BIPOLAR WITH A CENTRE
DETENT: the effect is the deviation from 64 in either direction.

  VOLUME  (front/back, CC 42)  64 = full volume, either extreme = silent
  ROUND   (left/right, CC 44)  64 = unrounded,   either extreme = OO

The response is SQUARED so that it favours the neutral state - the card
stays audible through most of the travel and only falls away near the ends,
so an imperfectly level controller does not quietly rob level.

FOUR VERSIONS OF THIS REACHED HARDWARE BEFORE IT WAS RIGHT, and every one
failed by inventing semantics rather than asking:

  1. volume = 32767 - lift_back + lift_front. Assumes lift_back rests at
     zero; it does not, so the card was quiet from boot and - being already
     clamped at the bottom - moving the controller changed nothing audible.
  2. A running-minimum "rest" with the deviation as a magnitude: a solution
     to a problem that does not exist, and abs() folded the axis so half
     the travel mirrored the other.
  3. Straight 0-127 levels, which made a level controller sit at half
     volume and only reach full at one extreme.
  4. Mute read as high-means-inverted, which muted the card during normal
     use and unmuted it only when turned over.

Three of those four produced a control that appeared ABSENT rather than
WRONG - the hardest kind to diagnose from the bench, and a pattern this
card has now hit four times counting the silent filter bank and the dead
knob. Every time, a clamp or a fold was hiding a bad assumption underneath.

Checks:
  1. Volume is full at the centre detent and silent at both extremes.
  2. The volume curve favours ON - shallow near centre, steep at the ends.
  3. Rounding is unrounded at centre and fully round at both extremes.
  4. The complementary partner is ignored, so the pair cannot fight.
  5. Mute polarity: CC 49 is HIGH when upright, LOW when turned over.

Run: python tools/gesture_check.py
"""

import sys
import math

CC_VOLUME = 42          # read
CC_VOLUME_PARTNER = 43  # complementary partner - deliberately ignored
CC_ROUND = 44           # read
CC_ROUND_PARTNER = 45   # complementary partner - deliberately ignored
CC_NOT_INVERTED = 48
CC_INVERTED = 49
TILT_CENTRE = 64


class Firmware:
    """Transcription of the accelerometer path in midi8mu.cpp."""

    def __init__(self):
        self.volume = 32767
        self.round = 0
        self.muted = 0
        self.freeze = 0

    @staticmethod
    def _deviation(v):
        d = abs(v - TILT_CENTRE)
        if d > TILT_CENTRE:
            d = TILT_CENTRE
        return (d * d * 32767) // (TILT_CENTRE * TILT_CENTRE)

    def cc(self, cc, v):
        if cc == CC_VOLUME:
            self.volume = 32767 - self._deviation(v)
        elif cc == CC_ROUND:
            if not self.freeze:
                self.round = self._deviation(v)
        elif cc == CC_INVERTED:
            self.muted = 1 if v < 64 else 0
        # CC 43, 45 and 48 are complementary partners and are not read.


def check_volume_detent():
    print("\n1. Volume: full at centre, silent at both extremes")
    ok = True
    f = Firmware()
    f.cc(CC_VOLUME, 64)
    centre = f.volume
    good = centre == 32767
    if not good:
        ok = False
    print(f"   CC 42 = 64 (level)      -> volume {centre:5d}   "
          f"{'ok - FULL' if good else '<-- NOT FULL AT CENTRE'}")

    for v in (0, 127):
        f = Firmware()
        f.cc(CC_VOLUME, v)
        good = f.volume < 1200
        if not good:
            ok = False
        print(f"   CC 42 = {v:3d} (extreme)   -> volume {f.volume:5d}   "
              f"{'ok - silent' if good else '<-- NOT SILENT'}")

    # Symmetric: equal deviation either side must give equal volume.
    for d in (8, 16, 32, 48):
        a = Firmware()
        a.cc(CC_VOLUME, 64 - d)
        b = Firmware()
        b.cc(CC_VOLUME, 64 + d)
        good = a.volume == b.volume
        if not good:
            ok = False
        print(f"   +/-{d:2d} from centre       -> {a.volume:5d} / {b.volume:5d}   "
              f"{'ok - symmetric' if good else '<-- ASYMMETRIC'}")
    return ok


def check_favours_on():
    """The curve must be shallow near centre and steep at the ends."""
    print("\n2. The curve favours ON")
    ok = True
    print("   CC 42   volume      dB")
    for v in (64, 80, 96, 112, 120, 127):
        f = Firmware()
        f.cc(CC_VOLUME, v)
        db = 20 * math.log10(max(f.volume, 1) / 32767)
        print(f"    {v:3d}    {f.volume:5d}   {db:6.1f}")

    # A quarter tilt must still be essentially full volume; a linear curve
    # would be 2.5 dB down there, which is an audible loss for a controller
    # that is merely being held imperfectly level.
    q = Firmware()
    q.cc(CC_VOLUME, 64 + 16)
    q_db = 20 * math.log10(max(q.volume, 1) / 32767)
    good = q_db > -1.5
    if not good:
        ok = False
    print(f"   quarter tilt is {q_db:.1f} dB down   "
          f"{'ok - still on' if good else '<-- TOO LOSSY'}")

    # And the end must genuinely be silent, not merely quiet.
    e = Firmware()
    e.cc(CC_VOLUME, 127)
    e_db = 20 * math.log10(max(e.volume, 1) / 32767)
    good = e_db < -25
    if not good:
        ok = False
    print(f"   full tilt is {e_db:.1f} dB down   "
          f"{'ok - silent' if good else '<-- NOT SILENT ENOUGH'}")
    return ok


def check_round_detent():
    print("\n3. Rounding: unrounded at centre, OO at both extremes")
    ok = True
    f = Firmware()
    f.cc(CC_ROUND, 64)
    good = f.round < 200
    if not good:
        ok = False
    print(f"   CC 44 = 64 (level)    -> round {f.round:5d}   "
          f"{'ok - unrounded' if good else '<-- ROUNDED AT REST'}")

    for v in (0, 127):
        f = Firmware()
        f.cc(CC_ROUND, v)
        good = f.round > 30000
        if not good:
            ok = False
        print(f"   CC 44 = {v:3d} (extreme) -> round {f.round:5d}   "
              f"{'ok - toward OO' if good else '<-- NOT ROUNDED'}")

    # Freeze must hold the vowel, rounding included.
    f = Firmware()
    f.cc(CC_ROUND, 100)
    before = f.round
    f.freeze = 1
    f.cc(CC_ROUND, 64)
    good = f.round == before
    if not good:
        ok = False
    print(f"   freeze holds rounding   {'ok' if good else 'FAIL'}")
    return ok


def check_partner_ignored():
    print("\n4. The complementary partner is ignored")
    ok = True
    f = Firmware()
    for v in range(0, 128, 8):
        f.cc(CC_VOLUME, 64)              # hold level
        f.cc(CC_VOLUME_PARTNER, 127 - v)  # partner sweeps, must do nothing
    good = f.volume == 32767
    print(f"   partner sweep leaves volume at {f.volume:5d}   "
          f"{'ok' if good else '<-- PARTNER FOUGHT'}")
    ok &= good

    f2 = Firmware()
    f2.cc(CC_VOLUME_PARTNER, 0)
    f2.cc(CC_ROUND_PARTNER, 127)
    good = f2.volume == 32767 and f2.round == 0
    print(f"   partners alone change nothing   {'ok' if good else 'FAIL'}")
    ok &= good
    print("   (each pair adds to 127, so one controller is the whole axis;")
    print("    reading both would be two writers racing for one value)")
    return ok


def check_mute():
    print("\n5. Mute: CC 49 reads high when upright, low when turned over")
    ok = True
    for v in (127, 120, 100, 80, 64):
        f = Firmware()
        f.cc(CC_INVERTED, v)
        if f.muted != 0:
            ok = False
    print(f"   CC 49 high (64-127, upright)   -> NOT muted   "
          f"{'ok' if ok else '<-- BACKWARDS'}")

    inv_ok = True
    for v in (0, 5, 20, 40, 63):
        f = Firmware()
        f.cc(CC_INVERTED, v)
        if f.muted != 1:
            inv_ok = False
    print(f"   CC 49 low  (0-63, turned over) -> muted       "
          f"{'ok' if inv_ok else '<-- FAIL'}")
    ok &= inv_ok

    f = Firmware()
    good = f.muted == 0
    print(f"   unmuted before any message   {'ok' if good else 'FAIL'}")
    ok &= good

    f2 = Firmware()
    for _ in range(20):
        f2.cc(CC_INVERTED, 5)
        f2.cc(CC_NOT_INVERTED, 122)
    good = f2.muted == 1
    print(f"   holds muted while both stream   "
          f"{'ok' if good else '<-- FLICKERS'}")
    ok &= good
    return ok


def main():
    print("TRACT8 8mu accelerometer check")
    print("  Bipolar axes with a centre detent at 64; squared response.")
    ok = check_volume_detent()
    ok &= check_favours_on()
    ok &= check_round_detent()
    ok &= check_partner_ignored()
    ok &= check_mute()
    print("\nPASS" if ok else "\nFAIL")
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
