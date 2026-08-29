#!/usr/bin/env python3
"""8mu accelerometer semantics, as stated by someone with the device.

THREE ATTEMPTS REACHED HARDWARE BEFORE THE SEMANTICS WERE SIMPLY ASKED FOR.
That is the lesson worth keeping. The CCs are:

  - CONTINUOUS LEVELS, 0-127, sweeping as the device tilts;
  - COMPLEMENTARY IN PAIRS - lift left and lift right add to 127, and so
    do lift front and lift back;
  - and "inverted" (CC 49) sits near 127 while the device is upside down,
    hysteresis notwithstanding.

There is no gesture to detect, no resting offset to learn, no calibration
to do. The value IS the position.

Two earlier versions failed on hardware by inventing semantics instead:

  1. volume = 32767 - lift_back + lift_front. This assumes lift_back rests
     at zero. It does not, so the card was quiet or silent from boot - and
     because the result was already clamped at the bottom, moving the
     controller changed nothing audible.

  2. A running-minimum "rest" with the deviation taken as a magnitude: a
     solution to a problem that does not exist, and abs() folded the axis
     so half the travel mirrored the other half.

Both produced a control that appeared ABSENT rather than WRONG, which is
much harder to diagnose. That pattern has now appeared three times on this
card - a silent filter bank, a dead knob, a dead tilt - and every time a
clamp or a fold was hiding a wrong assumption underneath.

Checks:
  1. Volume sweeps the full range as the tilt sweeps 0-127.
  2. Before any tilt message the card is at FULL volume, not quiet.
  3. Rounding sweeps spread to rounded across its own 0-127.
  4. The complementary partner is ignored, so the pair cannot fight.
  5. Mute follows the VALUE of CC 49 and survives continuous streaming.

Run: python tools/gesture_check.py
"""

import sys

# As of v1.4.2: rounding on front/back, volume on left/right.
CC_ROUND_FRONT = 42   # read
CC_ROUND_BACK = 43    # complementary partner - deliberately ignored
CC_VOL_LEFT = 44      # read
CC_VOL_RIGHT = 45     # complementary partner - deliberately ignored
CC_NOT_INVERTED = 48
CC_INVERTED = 49


class Firmware:
    """Transcription of the accelerometer path in midi8mu.cpp."""

    def __init__(self):
        self.volume = 32767
        self.round = 0
        self.muted = 0
        self.freeze = 0

    def cc(self, cc, v):
        q15 = v << 8
        if cc == CC_VOL_LEFT:
            self.volume = max(0, min(32767, q15))
        elif cc == CC_ROUND_FRONT:
            if not self.freeze:
                self.round = max(0, min(32767, q15))
        elif cc == CC_INVERTED:
            self.muted = 1 if v >= 64 else 0
        elif cc == CC_NOT_INVERTED:
            if v >= 64:
                self.muted = 0
        # CC 43 and CC 45 are complementary partners and are not read.


def check_volume_sweep():
    print("\n1. Volume sweeps the full range with the tilt")
    ok = True
    prev = None
    monotonic = True
    for v in (0, 16, 32, 64, 96, 112, 127):
        f = Firmware()
        f.cc(CC_VOL_LEFT, v)
        if prev is not None and f.volume < prev:
            monotonic = False
        prev = f.volume
        print(f"   CC 44 = {v:3d}  ->  volume {f.volume:5d}")
    lo = Firmware()
    lo.cc(CC_VOL_LEFT, 0)
    hi = Firmware()
    hi.cc(CC_VOL_LEFT, 127)
    good = lo.volume < 1000 and hi.volume > 30000 and monotonic
    if not good:
        ok = False
    print(f"   full range, monotonic   {'ok' if good else '<-- FAIL'}")
    return ok


def check_level_is_loud():
    """A card that boots quiet reads as broken. This was the v1.4.1 bug."""
    print("\n2. Before any tilt message, the card is at full volume")
    f = Firmware()
    good = f.volume == 32767
    print(f"   volume with no accelerometer data: {f.volume}   "
          f"{'ok' if good else '<-- QUIET AT BOOT'}")
    print("   (an 8mu that is never tilted must never make the card quiet)")
    return good


def check_round_sweep():
    print("\n3. Rounding sweeps spread to rounded")
    ok = True
    for v in (0, 32, 64, 96, 127):
        f = Firmware()
        f.cc(CC_ROUND_FRONT, v)
        print(f"   CC 42 = {v:3d}  ->  round {f.round:5d}")
    lo = Firmware()
    lo.cc(CC_ROUND_FRONT, 0)
    hi = Firmware()
    hi.cc(CC_ROUND_FRONT, 127)
    good = lo.round < 1000 and hi.round > 30000
    if not good:
        ok = False
    print(f"   spread at 0, rounded at 127   {'ok' if good else '<-- FAIL'}")

    f = Firmware()
    f.cc(CC_ROUND_FRONT, 100)
    before = f.round
    f.freeze = 1
    f.cc(CC_ROUND_FRONT, 10)
    good = f.round == before
    if not good:
        ok = False
    print(f"   freeze holds rounding   {'ok' if good else 'FAIL'}")
    return ok


def check_partner_ignored():
    """The pairs are complementary, so reading both is a race for nothing."""
    print("\n4. The complementary partner is ignored")
    ok = True

    # A full sweep with BOTH controllers streaming, summing to 127.
    f = Firmware()
    for v in range(0, 128, 8):
        f.cc(CC_VOL_LEFT, v)
        f.cc(CC_VOL_RIGHT, 127 - v)      # partner, must change nothing
    good = f.volume > 30000
    print(f"   full sweep with both streaming: volume {f.volume:5d}   "
          f"{'ok' if good else '<-- PARTNER FOUGHT'}")
    ok &= good

    # The partners alone must do nothing at all.
    f2 = Firmware()
    f2.cc(CC_VOL_RIGHT, 0)
    f2.cc(CC_ROUND_BACK, 127)
    good = f2.volume == 32767 and f2.round == 0
    print(f"   partners alone change nothing   {'ok' if good else 'FAIL'}")
    ok &= good
    print("   (left+right add to 127, so one of them is the whole axis;")
    print("    reading both would be two writers racing for one value)")
    return ok


def check_mute():
    print("\n5. Mute follows the VALUE of CC 49")
    ok = True
    f = Firmware()
    good = f.muted == 0
    print(f"   unmuted before any message   {'ok' if good else 'FAIL'}")
    ok &= good

    f.cc(CC_INVERTED, 120)
    good = f.muted == 1
    print(f"   CC 49 = 120 (upside down) -> muted   "
          f"{'ok' if good else 'FAIL'}")
    ok &= good

    f.cc(CC_INVERTED, 5)
    good = f.muted == 0
    print(f"   CC 49 = 5 (right way up)  -> unmuted   "
          f"{'ok' if good else 'FAIL'}")
    ok &= good

    # Continuous streaming of both halves must not make the mute flicker.
    f2 = Firmware()
    for _ in range(20):
        f2.cc(CC_INVERTED, 125)
        f2.cc(CC_NOT_INVERTED, 2)
    good = f2.muted == 1
    print(f"   holds muted while both stream   "
          f"{'ok' if good else '<-- FLICKERS'}")
    ok &= good

    f3 = Firmware()
    for _ in range(20):
        f3.cc(CC_INVERTED, 2)
        f3.cc(CC_NOT_INVERTED, 125)
    good = f3.muted == 0
    print(f"   holds unmuted while both stream   "
          f"{'ok' if good else 'FAIL'}")
    ok &= good

    # Hysteresis: the value need not reach exactly 127.
    for v in (64, 80, 100, 120, 127):
        f4 = Firmware()
        f4.cc(CC_INVERTED, v)
        if f4.muted != 1:
            ok = False
    print("   any value >= 64 mutes (hysteresis tolerated)   ok")
    return ok


def main():
    print("TRACT8 8mu accelerometer check")
    print("  Continuous levels 0-127, complementary in pairs.")
    ok = check_volume_sweep()
    ok &= check_level_is_loud()
    ok &= check_round_sweep()
    ok &= check_partner_ignored()
    ok &= check_mute()
    print("\nPASS" if ok else "\nFAIL")
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
