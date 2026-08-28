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
  1. Fader CCs map to the right band with the right Q15 scaling.
  2. Out-of-range CCs are ignored.
  3. Note on/off gates and the freeze toggle behave.
  4. Note-on with velocity 0 is treated as a release.
  5. Freeze blocks fader writes.
  6. Running status is sustained across messages.
  7. Real-time bytes interleaved mid-message do not corrupt it.
  8. Channel is ignored (all 16 behave alike).

Run: python tools/midi_check.py
"""

import sys

# --- transcribed from midi8mu.h ------------------------------------------
CC_FADER_FIRST = 34
CC_FADER_LAST = 41
CC_TILT_UP = 42
CC_TILT_DOWN = 43
NOTE_VOICED = 36
NOTE_NOISE = 48
NOTE_PLOSIVE = 60
NOTE_FREEZE = 72


class State:
    """Mirror of VoderState in shared.h."""

    def __init__(self):
        self.band_gain = [0] * 8
        self.pitch_bend = 0
        self.gate_voiced = 0
        self.gate_noise = 0
        self.freeze = 0
        self.plosive_count = 0


class Dispatch:
    """Transcription of midi8mu.cpp."""

    def __init__(self):
        self.s = State()
        self.tilt_up = 0
        self.tilt_down = 0
        self.freeze_held = False

    def cc(self, cc, v):
        if CC_FADER_FIRST <= cc <= CC_FADER_LAST:
            if not self.s.freeze:
                self.s.band_gain[cc - CC_FADER_FIRST] = v << 8
            return
        if cc == CC_TILT_UP:
            self.tilt_up = v << 8
        elif cc == CC_TILT_DOWN:
            self.tilt_down = v << 8
        else:
            return
        self.s.pitch_bend = self.tilt_up - self.tilt_down

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
    print("\n1/2. Fader CC mapping and Q15 scaling")
    ok = True
    d = Dispatch()
    for i, cc in enumerate(range(CC_FADER_FIRST, CC_FADER_LAST + 1)):
        d.cc(cc, 127)
        expect = 127 << 8
        got = d.s.band_gain[i]
        good = got == expect
        if not good:
            ok = False
        print(f"   CC {cc} -> band {i}   value {got:6d}  "
              f"{'ok' if good else 'FAIL expected %d' % expect}")

    # Full-scale fader should be within a hair of Q15 max.
    top = 127 << 8
    print(f"   full-scale fader = {top} of 32767 "
          f"({(32767-top)/32767*100:.2f}% short - by design, see midi8mu.cpp)")

    # Out-of-range CCs must not touch the bands.
    before = list(d.s.band_gain)
    for cc in (0, 1, 8, 33, 42, 50, 127):
        d.cc(cc, 64)
    if d.s.band_gain != before:
        print("   FAIL: an out-of-range CC modified a band gain")
        ok = False
    else:
        print("   CCs 0,1,8,33,42,50,127 leave band gains untouched   ok")
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


def check_freeze_blocks():
    print("\n5. Freeze blocks fader writes")
    d = Dispatch()
    d.cc(34, 100)
    before = d.s.band_gain[0]
    d.s.freeze = 1
    d.cc(34, 7)
    after = d.s.band_gain[0]
    ok = before == after
    print(f"   band 0 {before} -> {after} while frozen   "
          f"{'ok' if ok else 'FAIL - freeze did not hold'}")
    return ok


def check_running_status():
    print("\n6. Running status - the 8mu's fader sweep")
    got = []
    d = Dispatch()
    p = Parser(lambda s, a, b: (got.append((s, a, b)), d.message(s, a, b)))

    # One status byte, then eight data pairs: a sweep of fader 1.
    stream = [0xB0]
    for v in (10, 20, 30, 40, 50, 60, 70, 80):
        stream += [34, v]
    for b in stream:
        p.byte(b)

    ok = len(got) == 8
    print(f"   1 status + 8 data pairs -> {len(got)} messages   "
          f"{'ok' if ok else 'FAIL - running status not sustained'}")
    if ok:
        final = d.s.band_gain[0]
        expect = 80 << 8
        ok = final == expect
        print(f"   final band 0 gain {final} (expect {expect})   "
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
        d.message(0xB0 | ch, 34, 127)
        if d.s.band_gain[0] != 127 << 8:
            print(f"   channel {ch}: FAIL")
            ok = False
    if ok:
        print("   all 16 channels drive band 0 identically   ok")
    return ok


def main():
    print("TRACT8 8mu MIDI check: factory mapping, CC 34-41 / notes 36,48,60,72")
    ok = check_faders()
    ok &= check_notes()
    ok &= check_freeze_blocks()
    ok &= check_running_status()
    ok &= check_realtime_interleave()
    ok &= check_channel_agnostic()
    print("\nPASS" if ok else "\nFAIL")
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
