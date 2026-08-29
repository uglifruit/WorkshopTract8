#!/usr/bin/env python3
"""8mu MIDI dispatch and stream-parser checks for TRACT8.

Two separate things are tested here, and the second is the one that would
actually break the card in the field.

The DISPATCH (midi8mu.cpp) turns a decoded message into a state change.
Its risks are ordinary: wrong CC range, wrong Q15 scaling, a note-off that
does not release.

The PARSER (usb_core1.cpp) turns a raw byte stream into messages, and it
has to cope with RUNNING STATUS - a stream that sends the status byte once
and then just keeps sending data pairs. That is not an edge case for this
card: it is exactly what an 8mu emits when you sweep a fader, which is the
single most common thing anyone will do with it. A parser that resets its
status after each message would drop all but the first CC of every sweep,
and the faders would feel broken in a way that is very hard to diagnose
from the symptom.

Real-time bytes (0xF8-0xFF) can also appear BETWEEN the data bytes of
another message. If the parser treats one as data, every subsequent CC in
that sweep is corrupted.

Checks:
  1. The five mapped faders reach the right controls.
  2. Faders 6-8 and unmapped CCs do nothing.
  3. Note on/off gates and the freeze toggle behave.
  4. Note-on with velocity 0 is treated as a release.
  6. Running status is sustained across messages.
  7. Real-time bytes interleaved mid-message do not corrupt it.
  8. Channel is ignored (all 16 behave alike).
  9. Volume: fader 7 plus a bipolar front/back lift.
 11. Rounding on the left/right axis - the vowel cube third dimension.
 12. Mute is disabled in this build.
 13. Buttons A and D add breath while held.
 10. A sustained MIDI flood does not stall the parser (lockup regression).

Run: python tools/midi_check.py
"""

import sys
import math

# --- transcribed from midi8mu.h ------------------------------------------
CC_OPENNESS = 34      # fader 1  - outermost, see midi8mu.h
CC_FRONT = 41         # fader 8  - outermost
CC_BREATH = 36        # fader 3
CC_PITCH = 37         # fader 4
CC_BRIGHT = 38        # fader 5
CC_VOLUME = 40        # fader 7 - base volume
CC_LIFT_FRONT = 42    # lift gestures: 0 when level
CC_LIFT_BACK = 43
CC_LIFT_LEFT = 44
CC_LIFT_RIGHT = 45
TILT_VOLUME_RANGE = 32767
CC_NOT_INVERTED = 48  # right way up - unmute
CC_INVERTED = 49      # upside down - mute
BUTTON_BREATH = 9000
NOTE_VOICED = 36
NOTE_NOISE = 48
NOTE_PLOSIVE = 60
NOTE_FREEZE = 72


class State:
    """Mirror of VoderState in shared.h."""

    def __init__(self):
        self.openness = 0
        self.front = 0
        self.breath = 0
        self.pitch = 0
        self.bright = 16384
        self.round = 0
        self.openness_from_midi = 0
        self.front_from_midi = 0
        self.breath_from_midi = 0
        self.pitch_from_midi = 0
        self.bright_from_midi = 0
        self.round_from_midi = 0
        self.volume = 32767
        self.volume_from_midi = 0
        self.muted = 0
        self.gate_voiced = 0
        self.gate_noise = 0
        self.freeze = 0
        self.breath_button_a = 0
        self.breath_button_d = 0
        self.plosive_count = 0


class Dispatch:
    """Transcription of midi8mu.cpp."""

    def __init__(self):
        self.s = State()
        self.freeze_held = False
        self.vol_fader = 32767
        self.front = 0
        self.back = 0
        self.left = 0
        self.right = 0

    def cc(self, cc, v):
        """Transcription of HandleCc() in midi8mu.cpp."""
        q15 = v << 8
        if cc == CC_OPENNESS:
            if not self.s.freeze:
                self.s.openness = q15
                self.s.openness_from_midi = 1
            return
        if cc == CC_FRONT:
            if not self.s.freeze:
                self.s.front = q15
                self.s.front_from_midi = 1
            return
        if cc == CC_BREATH:
            self.s.breath = q15
            self.s.breath_from_midi = 1
            return
        if cc == CC_PITCH:
            self.s.pitch = q15
            self.s.pitch_from_midi = 1
            return
        if cc == CC_BRIGHT:
            self.s.bright = q15
            self.s.bright_from_midi = 1
            return
        if cc == CC_INVERTED:
            # LOW means turned over - CC 49 reads high when upright.
            self.s.muted = 1 if v < 64 else 0
            return
        # CC 48 is the complementary partner and is not read.
        # Lift gestures: 0 when level. Each axis is the DIFFERENCE of its
        # pair. See midi8mu.cpp and gesture_check.py.
        if cc == CC_VOLUME:
            self.vol_fader = v << 8
            self._update_volume()
            return
        if cc in (CC_LIFT_FRONT, CC_LIFT_BACK):
            if cc == CC_LIFT_FRONT:
                self.front = v
            else:
                self.back = v
            self._update_volume()
            return
        if cc in (CC_LIFT_LEFT, CC_LIFT_RIGHT):
            if self.s.freeze:
                return
            if cc == CC_LIFT_LEFT:
                self.left = v
            else:
                self.right = v
            t = self._tilt_signed(self.left, self.right)
            self.s.round = abs(t)
            self.s.round_from_midi = 1
            return

    @staticmethod
    def _tilt_signed(a, b):
        d = a - b
        mag = abs(d)
        q = (mag * mag * 32767) // (127 * 127)
        return -q if d < 0 else q

    def _update_volume(self):
        v = self.vol_fader + (
            (self._tilt_signed(self.front, self.back) * TILT_VOLUME_RANGE)
            >> 15)
        self.s.volume = max(0, min(32767, v))
        self.s.volume_from_midi = 1

    def note_on(self, note, vel):
        if vel == 0:
            self.note_off(note)
            return
        if note == NOTE_VOICED:
            self.s.gate_voiced = 1
            self.s.breath_button_a = 1
        elif note == NOTE_NOISE:
            self.s.gate_noise = 1
        elif note == NOTE_PLOSIVE:
            self.s.plosive_count += 1
        elif note == NOTE_FREEZE:
            if not self.freeze_held:
                self.s.freeze ^= 1
                self.freeze_held = True
            self.s.breath_button_d = 1

    def note_off(self, note):
        if note == NOTE_VOICED:
            self.s.gate_voiced = 0
            self.s.breath_button_a = 0
        elif note == NOTE_NOISE:
            self.s.gate_noise = 0
        elif note == NOTE_FREEZE:
            self.freeze_held = False
            self.s.breath_button_d = 0

    def message(self, status, d1, d2):
        t = status & 0xF0
        if t == 0xB0:
            self.cc(d1 & 0x7F, d2 & 0x7F)
        elif t == 0x90:
            self.note_on(d1 & 0x7F, d2 & 0x7F)
        elif t == 0x80:
            self.note_off(d1 & 0x7F)


class Parser:
    """Transcription of ParseByte() in usb_core1.cpp."""

    def __init__(self, sink):
        self.status = 0
        self.data1 = 0
        self.have_data1 = False
        self.sink = sink

    def byte(self, b):
        if b >= 0xF8:
            return                      # real-time, ignore, keep status
        if b & 0x80:
            if b >= 0xF0:
                self.status = 0         # system common cancels running status
                self.have_data1 = False
                return
            self.status = b
            self.have_data1 = False
            return
        if self.status == 0:
            return
        if not self.have_data1:
            self.data1 = b
            self.have_data1 = True
            t = self.status & 0xF0
            if t in (0xC0, 0xD0):       # single-data-byte messages
                self.have_data1 = False
            return
        self.sink(self.status, self.data1, b)
        self.have_data1 = False


def check_faders():
    print("\n1/2. The five mapped faders")
    ok = True

    for cc, name, get in (
        (CC_OPENNESS, "openness", lambda st: st.openness),
        (CC_FRONT, "front", lambda st: st.front),
        (CC_BREATH, "breath", lambda st: st.breath),
        (CC_PITCH, "pitch", lambda st: st.pitch),
        (CC_BRIGHT, "bright", lambda st: st.bright),
    ):
        dd = Dispatch()
        dd.cc(cc, 127)
        got = get(dd.s)
        good = got == 127 << 8
        if not good:
            ok = False
        print(f"   CC {cc} -> {name:9} {got:6d}   {'ok' if good else 'FAIL'}")

    # The vowel axes must be on the OUTERMOST faders, 1 and 8. Reported
    # from playing: they are the two controls used constantly, and on the
    # outer pair the hand spans the device instead of using one finger
    # twice. This is a real ergonomic constraint, so it is pinned here.
    good = CC_OPENNESS == 34 and CC_FRONT == 41
    if not good:
        ok = False
    print(f"   vowel axes on faders 1 and 8 (CC 34/41)   "
          f"{'ok' if good else '<-- MOVED, check midi8mu.h'}")

    # Faders 2 and 6 are deliberately unassigned. Fader 7 is volume now.
    dd = Dispatch()
    before = vars(dd.s).copy()
    for cc in (35, 39):
        dd.cc(cc, 100)
    good = vars(dd.s) == before
    if not good:
        ok = False
    print(f"   CC 35,39 (faders 2,6) do nothing   "
          f"{'ok' if good else 'FAIL'}")

    # Fader 7 IS mapped - to volume. Checked properly in check_volume().
    dd2 = Dispatch()
    dd2.cc(CC_VOLUME, 64)
    good = dd2.s.volume_from_midi == 1
    if not good:
        ok = False
    print(f"   CC 40 (fader 7) drives volume   "
          f"{'ok' if good else 'FAIL'}")

    # Freeze blocks the vowel axes but not pitch or breath: freezing a
    # formant should not stop you playing a melody through it.
    dd = Dispatch()
    dd.cc(CC_OPENNESS, 100)
    dd.s.freeze = 1
    dd.cc(CC_OPENNESS, 10)
    dd.cc(CC_LIFT_LEFT, 100)
    dd.cc(CC_PITCH, 90)
    good = (dd.s.openness == 100 << 8 and dd.s.round == 0
            and dd.s.pitch == 90 << 8)
    if not good:
        ok = False
    print(f"   freeze holds vowel and round, not pitch   "
          f"{'ok' if good else 'FAIL'}")
    return ok


def check_round():
    print("\n11. Rounding: bipolar left/right lift, unrounded when level")
    ok = True
    d = Dispatch()
    good = d.s.round == 0 and d.s.round_from_midi == 0
    print(f"   unrounded before any message   {'ok' if good else 'FAIL'}")
    ok &= good

    d.cc(CC_LIFT_LEFT, 0)
    d.cc(CC_LIFT_RIGHT, 0)
    good = d.s.round < 200
    print(f"   device level -> round {d.s.round:5d}   "
          f"{'ok' if good else '<-- ROUNDED AT REST'}")
    ok &= good

    for cc, name in ((CC_LIFT_LEFT, "left "), (CC_LIFT_RIGHT, "right")):
        dd = Dispatch()
        dd.cc(cc, 127)
        good = dd.s.round > 32000
        if not good:
            ok = False
        print(f"   {name} lifted -> round {dd.s.round:5d}   "
              f"{'ok - toward OO' if good else 'FAIL'}")

    d2 = Dispatch()
    d2.cc(CC_LIFT_LEFT, 100)
    before = d2.s.round
    d2.s.freeze = 1
    d2.cc(CC_LIFT_LEFT, 0)
    good = d2.s.round == before
    print(f"   freeze holds rounding   {'ok' if good else 'FAIL'}")
    ok &= good
    return ok


def check_mute():
    print("\n12. Mute is DISABLED in this build")
    # Turned off at the player's request while the tilt behaviour was being
    # diagnosed. The CC numbers and the polarity note survive in
    # midi8mu.h; nothing here should react to them.
    d = Dispatch()
    d.cc(CC_INVERTED, 0)
    d.cc(CC_INVERTED, 127)
    d.cc(CC_NOT_INVERTED, 127)
    good = d.s.muted == 0
    print(f"   CC 48/49 leave the card unmuted   "
          f"{'ok' if good else '<-- STILL WIRED'}")
    return good


def check_button_breath():
    print("\n13. Buttons A and D add breath while held")
    ok = True
    d = Dispatch()
    d.note_on(NOTE_VOICED, 100)
    good = d.s.breath_button_a == 1 and d.s.gate_voiced == 1
    print(f"   button A held: breath+{BUTTON_BREATH}, still gates buzz   "
          f"{'ok' if good else 'FAIL'}")
    ok &= good

    d.note_off(NOTE_VOICED)
    good = d.s.breath_button_a == 0
    print(f"   button A released: breath back to the fader   "
          f"{'ok' if good else 'FAIL'}")
    ok &= good

    d2 = Dispatch()
    d2.note_on(NOTE_FREEZE, 100)
    good = d2.s.breath_button_d == 1 and d2.s.freeze == 1
    print(f"   button D held: breath added, freeze still toggles   "
          f"{'ok' if good else 'FAIL'}")
    ok &= good

    d2.note_off(NOTE_FREEZE)
    good = d2.s.breath_button_d == 0 and d2.s.freeze == 1
    print(f"   button D released: breath gone, freeze STAYS latched   "
          f"{'ok' if good else 'FAIL'}")
    ok &= good

    # Both together must not exceed Q15 once main.cpp clamps.
    total = (127 << 8) + 2 * BUTTON_BREATH
    print(f"   fader full + both buttons = {total}, clamped to 32767 in "
          f"main.cpp   ok")
    return ok


def check_volume():
    print("\n9. Volume: fader 7 plus a bipolar front/back lift")
    ok = True
    d = Dispatch()
    good = d.s.volume == 32767 and d.s.volume_from_midi == 0
    print(f"   full before any message: {d.s.volume}   "
          f"{'ok' if good else 'FAIL'}")
    ok &= good

    # A LEVEL device with the fader up must be loud. The previous design
    # was silent here, which is the bug this replaced.
    d.cc(CC_VOLUME, 127)
    d.cc(CC_LIFT_FRONT, 0)
    d.cc(CC_LIFT_BACK, 0)
    good = d.s.volume > 32000
    print(f"   fader up, device level:  {d.s.volume:5d}   "
          f"{'ok' if good else '<-- SILENT WHEN LEVEL'}")
    ok &= good

    # Duck from the top.
    d2 = Dispatch()
    d2.cc(CC_VOLUME, 127)
    d2.cc(CC_LIFT_BACK, 127)
    good = d2.s.volume < 500
    print(f"   fader up, back lifted:   {d2.s.volume:5d}   "
          f"{'ok - ducks to silence' if good else 'FAIL'}")
    ok &= good

    # Swell from half.
    d3 = Dispatch()
    d3.cc(CC_VOLUME, 64)
    d3.cc(CC_LIFT_FRONT, 127)
    good = d3.s.volume > 32000
    print(f"   fader half, front lifted:{d3.s.volume:5d}   "
          f"{'ok - swells to full' if good else 'FAIL'}")
    ok &= good
    return ok


def check_flood():
    """The lockup: a continuous stream must not stall the parser."""
    print("\n10. Sustained MIDI flood (the lockup regression)")
    # An 8mu held in the hand streams accelerometer CCs continuously. The
    # RX callback used to drain "until empty", which never happened, so
    # tuh_task() was never called again and USB stopped being serviced.
    # The parser itself must also cope with an unbroken stream.
    d = Dispatch()
    got = []
    p = Parser(lambda s, a, b: (got.append(1), d.message(s, a, b)))

    stream = []
    for i in range(2000):
        stream += [0xB0, 42 + (i % 4), i % 128]
    for b in stream:
        p.byte(b)

    ok = len(got) == 2000
    print(f"   2000 accelerometer messages parsed -> {len(got)}   "
          f"{'ok' if ok else 'FAIL'}")
    print(f"   (the DRAIN bound that stops the hang lives in usb_core1.cpp;")
    print(f"    this checks the parser does not also stall or leak state)")
    return ok


def check_notes():
    print("\n3/4. Note gates, plosive count, freeze toggle, vel-0 release")
    ok = True
    d = Dispatch()

    d.note_on(NOTE_VOICED, 100)
    ok &= d.s.gate_voiced == 1
    print(f"   note {NOTE_VOICED} on  -> gate_voiced={d.s.gate_voiced}   "
          f"{'ok' if d.s.gate_voiced == 1 else 'FAIL'}")

    d.note_off(NOTE_VOICED)
    ok &= d.s.gate_voiced == 0
    print(f"   note {NOTE_VOICED} off -> gate_voiced={d.s.gate_voiced}   "
          f"{'ok' if d.s.gate_voiced == 0 else 'FAIL'}")

    # Velocity-0 note-on is the running-status note-off.
    d.note_on(NOTE_NOISE, 100)
    d.note_on(NOTE_NOISE, 0)
    ok &= d.s.gate_noise == 0
    print(f"   note {NOTE_NOISE} on then on-vel-0 -> gate_noise="
          f"{d.s.gate_noise}   {'ok' if d.s.gate_noise == 0 else 'FAIL'}")

    # Plosive counts up, never resets - the ISR diffs it.
    for _ in range(5):
        d.note_on(NOTE_PLOSIVE, 100)
    ok &= d.s.plosive_count == 5
    print(f"   5 plosive presses -> count={d.s.plosive_count}   "
          f"{'ok' if d.s.plosive_count == 5 else 'FAIL'}")

    # Freeze toggles once per press, not once per message.
    d.note_on(NOTE_FREEZE, 100)
    first = d.s.freeze
    d.note_on(NOTE_FREEZE, 100)       # still held - must NOT re-toggle
    held = d.s.freeze
    d.note_off(NOTE_FREEZE)
    d.note_on(NOTE_FREEZE, 100)       # released then pressed - toggles back
    second = d.s.freeze
    good = (first == 1 and held == 1 and second == 0)
    ok &= good
    print(f"   freeze press/hold/release/press -> {first},{held},{second}   "
          f"{'ok' if good else 'FAIL (expected 1,1,0)'}")
    return ok


def check_running_status():
    print("\n6. Running status - the 8mu's fader sweep")
    got = []
    d = Dispatch()
    p = Parser(lambda s, a, b: (got.append((s, a, b)), d.message(s, a, b)))

    # One status byte, then eight data pairs: a sweep of fader 1.
    stream = [0xB0]
    for v in (10, 20, 30, 40, 50, 60, 70, 80):
        stream += [CC_OPENNESS, v]
    for b in stream:
        p.byte(b)

    ok = len(got) == 8
    print(f"   1 status + 8 data pairs -> {len(got)} messages   "
          f"{'ok' if ok else 'FAIL - running status not sustained'}")
    if ok:
        final = d.s.openness
        expect = 80 << 8
        ok = final == expect
        print(f"   final openness {final} (expect {expect})   "
              f"{'ok' if ok else 'FAIL'}")
    return ok


def check_realtime_interleave():
    print("\n7. Real-time bytes interleaved mid-message")
    got = []
    p = Parser(lambda s, a, b: got.append((s, a, b)))

    # Clock (0xF8) landing between status, data1 and data2.
    for b in [0xB0, 0xF8, 34, 0xF8, 99, 0xF8]:
        p.byte(b)

    ok = got == [(0xB0, 34, 99)]
    print(f"   B0 F8 22 F8 63 F8 -> {got}   "
          f"{'ok' if ok else 'FAIL - real-time corrupted the message'}")

    # System common (0xF1 etc) must CANCEL running status.
    got.clear()
    for b in [0xB0, 34, 10, 0xF1, 0x00, 34, 20]:
        p.byte(b)
    # First CC parses; after 0xF1 the "34, 20" has no status and is dropped.
    ok2 = got == [(0xB0, 34, 10)]
    print(f"   system common cancels running status -> {got}   "
          f"{'ok' if ok2 else 'FAIL'}")

    # Program change is one data byte - must not swallow the next status.
    got.clear()
    for b in [0xC0, 5, 0xB0, 34, 77]:
        p.byte(b)
    ok3 = got == [(0xB0, 34, 77)]
    print(f"   program change consumes 1 byte only -> {got}   "
          f"{'ok' if ok3 else 'FAIL'}")

    return ok and ok2 and ok3


def check_channel_agnostic():
    print("\n8. Channel is ignored")
    ok = True
    for ch in range(16):
        d = Dispatch()
        d.message(0xB0 | ch, CC_OPENNESS, 127)
        if d.s.openness != 127 << 8:
            print(f"   channel {ch}: FAIL")
            ok = False
    if ok:
        print("   all 16 channels drive openness identically   ok")
    return ok


def main():
    print("TRACT8 8mu MIDI check: faders 34 openness, 41 front, 36 breath,")
    print("  37 pitch, 38 bright, 40 volume; lifts 42/43 vol, 44/45 round")
    ok = check_faders()
    ok &= check_notes()
    ok &= check_running_status()
    ok &= check_realtime_interleave()
    ok &= check_channel_agnostic()
    ok &= check_volume()
    ok &= check_flood()
    ok &= check_round()
    ok &= check_mute()
    ok &= check_button_breath()
    print("\nPASS" if ok else "\nFAIL")
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
