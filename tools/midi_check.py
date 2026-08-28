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
  9. Volume on the front/back accelerometer axis.
 10. A sustained MIDI flood does not stall the parser (lockup regression).

Run: python tools/midi_check.py
"""

import sys
import math

# --- transcribed from midi8mu.h ------------------------------------------
CC_OPENNESS = 34     # fader 1
CC_FRONT = 35        # fader 2
CC_BREATH = 36       # fader 3
CC_PITCH = 37        # fader 4
CC_BRIGHT = 38       # fader 5
CC_VOL_UP = 42       # tilt front
CC_VOL_DOWN = 43     # tilt back
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
        self.openness_from_midi = 0
        self.front_from_midi = 0
        self.breath_from_midi = 0
        self.pitch_from_midi = 0
        self.bright_from_midi = 0
        self.volume = 32767
        self.volume_from_midi = 0
        self.gate_voiced = 0
        self.gate_noise = 0
        self.freeze = 0
        self.plosive_count = 0


class Dispatch:
    """Transcription of midi8mu.cpp."""

    def __init__(self):
        self.s = State()
        self.vol_up = 0
        self.vol_down = 0
        self.freeze_held = False

    def cc(self, cc, v):
        """Transcription of HandleCc() in midi8mu.cpp."""
        q15 = v << 8
        if cc == CC_OPENNESS:
            # Frozen formants ignore the vowel axes; that is what freeze is.
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
        if cc == CC_VOL_UP:
            self.vol_up = q15
        elif cc == CC_VOL_DOWN:
            self.vol_down = q15
        else:
            return
        vol = 32767 - self.vol_down + self.vol_up
        self.s.volume = max(0, min(32767, vol))
        self.s.volume_from_midi = 1

    def note_on(self, note, vel):
        if vel == 0:
            self.note_off(note)
            return
        if note == NOTE_VOICED:
            self.s.gate_voiced = 1
        elif note == NOTE_NOISE:
            self.s.gate_noise = 1
        elif note == NOTE_PLOSIVE:
            self.s.plosive_count += 1
        elif note == NOTE_FREEZE:
            if not self.freeze_held:
                self.s.freeze ^= 1
                self.freeze_held = True

    def note_off(self, note):
        if note == NOTE_VOICED:
            self.s.gate_voiced = 0
        elif note == NOTE_NOISE:
            self.s.gate_noise = 0
        elif note == NOTE_FREEZE:
            self.freeze_held = False

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

    # Faders 6-8 are deliberately unassigned. Nothing may move.
    dd = Dispatch()
    before = vars(dd.s).copy()
    for cc in (39, 40, 41):
        dd.cc(cc, 100)
    good = vars(dd.s) == before
    if not good:
        ok = False
    print(f"   CC 39,40,41 (faders 6-8) do nothing   "
          f"{'ok' if good else 'FAIL'}")
    print("   (unassigned on purpose - seven faders of partials is what")
    print("    made the card unplayable, see playable_check.py)")

    # Other unmapped CCs likewise.
    dd = Dispatch()
    before = vars(dd.s).copy()
    for cc in (0, 1, 8, 33, 44, 45, 46, 50, 127):
        dd.cc(cc, 64)
    good = vars(dd.s) == before
    if not good:
        ok = False
    print(f"   unmapped CCs leave the state untouched   "
          f"{'ok' if good else 'FAIL'}")

    # Freeze must block the vowel axes but NOT pitch or breath: freezing a
    # formant should not stop you playing a melody through it.
    dd = Dispatch()
    dd.cc(CC_OPENNESS, 100)
    dd.s.freeze = 1
    dd.cc(CC_OPENNESS, 10)
    dd.cc(CC_PITCH, 90)
    good = dd.s.openness == 100 << 8 and dd.s.pitch == 90 << 8
    if not good:
        ok = False
    print(f"   freeze holds the vowel but not pitch   "
          f"{'ok' if good else 'FAIL'}")
    return ok


def check_volume():
    print("\n9. Volume on the front/back tilt")
    ok = True
    d = Dispatch()
    good = d.s.volume == 32767 and d.s.volume_from_midi == 0
    print(f"   rests at unity before any tilt: {d.s.volume}   "
          f"{'ok' if good else 'FAIL'}")
    ok &= good

    d.cc(CC_VOL_UP, 127)
    up = d.s.volume
    d2 = Dispatch()
    d2.cc(CC_VOL_DOWN, 127)
    down = d2.s.volume
    # Back tilt must reach silence, or the card cannot be faded out.
    good = up >= 32767 and down < 1000
    print(f"   tilt front -> {up:5d}, tilt back -> {down:5d}   "
          f"{'ok - full duck to silence' if good else 'FAIL'}")
    ok &= good

    d3 = Dispatch()
    d3.cc(CC_VOL_DOWN, 127)
    d3.cc(CC_VOL_UP, 0)
    good = 0 <= d3.s.volume <= 32767
    print(f"   stays in Q15 range at the extreme: {d3.s.volume}   "
          f"{'ok' if good else 'FAIL'}")
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
    print("TRACT8 8mu MIDI check: faders 34 openness, 35 front, 36 breath,")
    print("  37 pitch, 38 bright; tilt 42/43 volume; notes 36/48/60/72")
    ok = check_faders()
    ok &= check_notes()
    ok &= check_running_status()
    ok &= check_realtime_interleave()
    ok &= check_channel_agnostic()
    ok &= check_volume()
    ok &= check_flood()
    print("\nPASS" if ok else "\nFAIL")
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
