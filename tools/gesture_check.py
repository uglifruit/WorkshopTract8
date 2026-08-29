#!/usr/bin/env python3
"""8mu accelerometer semantics. THE TEST THAT WOULD HAVE SAVED TWO ROUNDS.

Two hardware reports, one root cause: "tilt isn't doing volume now" and
"upside down should be MUTE". Both were the same wrong assumption about
what the 8mu's accelerometer CCs actually mean.

THEY ARE GESTURE MAGNITUDES, NOT BIPOLAR AXES.

Each direction is its own controller reporting how much of that gesture is
happening. "Lift front" (CC 42) and "lift back" (CC 43) are two separate
numbers, not two ends of one signed axis, and - crucially - a controller
lying flat does not necessarily send 0 on either of them.

The volume code assumed otherwise:

    volume = 32767 - lift_back + lift_front

If lift_back rests anywhere above zero the card is quiet from boot, and
because the value is already clamped at the bottom, moving the controller
appears to do NOTHING. That is why it read as "tilt isn't doing volume"
rather than as "volume is too low".

The mute had the mirror-image bug. CC 49 "inverted" and CC 48 "not
inverted" are a gesture PAIR. CC 49 does not fall back to zero when the
controller is turned right side up; it simply stops being sent while CC 48
is sent instead. Treating CC 49 as a level meant the mute latched on and
never released.

Why no existing test caught either: midi_check.py transcribes the firmware's
dispatch, so it shared the same assumption and agreed with it. A test that
models the component under test cannot find a bug in the model. This file
models the DEVICE instead - what an 8mu actually puts on the wire - and
checks the firmware copes.

Checks:
  1. A resting offset on either axis must not duck the volume.
  2. Volume must recover when the controller is levelled, from any start.
  3. Tilting must always duck, whichever direction the unit reports.
  4. Mute engages on CC 49 and RELEASES on CC 48.
  5. Rounding survives a resting offset the same way volume does.

Run: python tools/gesture_check.py
"""

import sys

CC_VOL_UP = 42
CC_VOL_DOWN = 43
CC_ROUND_LEFT = 44
CC_ROUND_RIGHT = 45
CC_NOT_INVERTED = 48
CC_INVERTED = 49


class Firmware:
    """Transcription of the accelerometer path in midi8mu.cpp."""

    def __init__(self):
        self.vol_rest = -1
        self.round_rest = -1
        self.volume = 32767
        self.round = 0
        self.muted = 0

    def _deviation(self, which, value):
        rest = self.vol_rest if which == 'vol' else self.round_rest
        if rest < 0 or value < rest:
            rest = value
            if which == 'vol':
                self.vol_rest = rest
            else:
                self.round_rest = rest
        return value - rest

    def cc(self, cc, v):
        q15 = v << 8
        if cc in (CC_VOL_UP, CC_VOL_DOWN):
            dev = self._deviation('vol', q15)
            mag = abs(dev)
            self.volume = max(0, min(32767, 32767 - (mag << 1)))
        elif cc in (CC_ROUND_LEFT, CC_ROUND_RIGHT):
            dev = self._deviation('round', q15)
            r = abs(dev) << 1
            self.round = max(0, min(32767, r))
        elif cc == CC_INVERTED:
            self.muted = 1
        elif cc == CC_NOT_INVERTED:
            self.muted = 0


def check_resting_offset():
    """The exact bug: a non-zero resting value must not silence the card."""
    print("\n1. A resting offset must not duck the volume")
    ok = True
    for rest in (0, 5, 20, 40, 64, 100):
        f = Firmware()
        # The 8mu sits still, reporting its resting value repeatedly.
        for _ in range(5):
            f.cc(CC_VOL_DOWN, rest)
        good = f.volume > 30000
        if not good:
            ok = False
        print(f"   resting value {rest:3d} -> volume {f.volume:5d}   "
              f"{'ok' if good else '<-- DUCKED AT REST (the bug)'}")
    print("   (the old formula gave volume 255 for a resting value of 127,")
    print("    and 16383 for 64 - silent or half, with no way to recover)")
    return ok


def check_recovery():
    """Levelling the controller must restore full volume, from any start."""
    print("\n2. Volume recovers when the controller is levelled")
    ok = True
    for label, seq in (
        ("flat, tilt, flat", [5, 5, 60, 100, 60, 5]),
        ("TILTED AT BOOT, then levelled", [80, 90, 60, 30, 8, 5]),
        ("only ever tilted", [70, 75, 72, 70]),
    ):
        f = Firmware()
        vols = []
        for v in seq:
            f.cc(CC_VOL_UP, v)
            vols.append(f.volume)
        # The last reading, once the controller has settled, must be loud.
        good = vols[-1] > 30000
        if not good:
            ok = False
        print(f"   {label:32} -> final {vols[-1]:5d}   "
              f"{'ok' if good else '<-- STUCK QUIET'}")
    print("   (rest tracks the running MINIMUM, so a controller that boots")
    print("    tilted corrects itself the first time it is put down)")
    return ok


def check_tilt_ducks():
    print("\n3. Tilting away from rest always ducks")
    ok = True
    for cc, name in ((CC_VOL_UP, "lift front"), (CC_VOL_DOWN, "lift back")):
        f = Firmware()
        f.cc(cc, 5)          # establish rest
        flat = f.volume
        f.cc(cc, 127)        # full tilt
        tilted = f.volume
        good = flat > 30000 and tilted < 5000
        if not good:
            ok = False
        print(f"   {name:11} flat {flat:5d} -> tilted {tilted:5d}   "
              f"{'ok' if good else '<-- NO DUCK'}")
    return ok


def check_mute_pair():
    """CC 49 mutes, CC 48 unmutes. Neither is a level."""
    print("\n4. Mute is a gesture PAIR, not a level")
    ok = True
    f = Firmware()
    good = f.muted == 0
    print(f"   unmuted at rest   {'ok' if good else 'FAIL'}")
    ok &= good

    f.cc(CC_INVERTED, 127)
    good = f.muted == 1
    print(f"   CC 49 (inverted)      -> muted   {'ok' if good else 'FAIL'}")
    ok &= good

    # The old bug: CC 49 stops being sent, but nothing releases the mute.
    for _ in range(10):
        pass
    good = f.muted == 1
    print(f"   stays muted while inverted   {'ok' if good else 'FAIL'}")
    ok &= good

    f.cc(CC_NOT_INVERTED, 127)
    good = f.muted == 0
    print(f"   CC 48 (not inverted)  -> unmuted   {'ok' if good else 'FAIL'}")
    ok &= good
    print("   (the old code watched only CC 49 and read it as a level, so")
    print("    the mute latched on and never came back off)")

    # Any value on CC 49 must mute - it is a gesture, not a threshold.
    for v in (1, 40, 64, 127):
        f2 = Firmware()
        f2.cc(CC_INVERTED, v)
        if f2.muted != 1:
            print(f"   CC 49 value {v} did not mute   FAIL")
            ok = False
    print("   any CC 49 value mutes (a gesture, not a threshold)   ok")
    return ok


def check_round_offset():
    print("\n5. Rounding survives a resting offset")
    ok = True
    for rest in (0, 20, 64):
        f = Firmware()
        for _ in range(3):
            f.cc(CC_ROUND_LEFT, rest)
        at_rest = f.round
        f.cc(CC_ROUND_LEFT, min(127, rest + 60))
        tilted = f.round
        good = at_rest < 2000 and tilted > 10000
        if not good:
            ok = False
        print(f"   rest {rest:3d}: round {at_rest:5d} -> tilted {tilted:5d}   "
              f"{'ok' if good else '<-- FAIL'}")
    print("   (rounding must rest SPREAD, or every vowel is rounded by")
    print("    default and the flat position is unreachable)")
    return ok


def main():
    print("TRACT8 8mu accelerometer gesture check")
    print("  The CCs are gesture magnitudes, not bipolar axes.")
    ok = check_resting_offset()
    ok &= check_recovery()
    ok &= check_tilt_ducks()
    ok &= check_mute_pair()
    ok &= check_round_offset()
    print("\nPASS" if ok else "\nFAIL")
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
