# TRACT8

*A Voder you play with your hands.*

An eight-band reimplementation of the 1939 Bell Labs Voder for the Music Thing
Modular Workshop System Computer, played from a [Music Thing
8mu](https://www.musicthing.co.uk/8mu.html) over USB MIDI Host — one fader per
formant band — or from the card's own three knobs.

## What it does

The [Voder](https://en.wikipedia.org/wiki/Voder) was the first machine that
could talk. It had no vocabulary and no text input. It made two raw sounds — a
buzz and a hiss — and an operator shaped them into speech in real time using
ten finger keys, a wrist bar and a foot pedal. Operators trained for months.

TRACT8 is that instrument, with eight bands instead of ten:

- **Two excitation sources.** A bandlimited buzz at F0 for voiced sounds, white
  noise for sibilants, crossfaded continuously rather than switched.
- **Eight fixed bandpass filters** at 250, 450, 700, 1000, 1400, 1900, 2600 and
  3800 Hz, each with its own gain. **The gains are the performance.**
- **Plosive bursts** for the stop consonants — p, t, k — that the original had
  dedicated keys for.
- **A vowel morph** on Knob 1 for when you have no controller attached.

It is not a vocoder and not a text-to-speech engine. Nothing analyses an input
signal and nothing knows what a word is. What comes out is whatever your hands
put in.

## What to feed it

Nothing, if you like — the card generates its own excitation and drones on
boot. Turn Knob 1 and it says vowels.

Patch **Audio In 1** and your signal replaces the buzz and hiss entirely, at
which point the card is an eight-band formant filter you play by hand. Drums
through it are worth the patch cable.

A good first patch: nothing plugged in at all, Knob 3 fully anticlockwise
(all buzz), and Knob 1 swept slowly. That is the card at its most Voder-like.

## Controls

### With an 8mu (front USB-C jack)

Plug it in and it works. Nothing to configure — TRACT8 reads the 8mu's factory
mapping, so a device straight out of the box drives it.

| 8mu control | Factory message | Effect |
|---|---|---|
| Faders 1–8 | CC 34–41 | **Filter band gains 1–8** |
| Tilt front / back | CC 42 / 43 | F0 pitch bend, ±1 octave |
| Button 1 | Note 36 (C2) | Gate the voiced buzz |
| Button 2 | Note 48 (C3) | Gate the unvoiced noise |
| Button 3 | Note 60 (C4) | Plosive burst |
| Button 4 | Note 72 (C5) | Toggle formant freeze |

Channel-agnostic — set the 8mu to any channel. The other five accelerometer
gestures (CC 44–49), pitch bend, sysex and program change are ignored.

The 8mu only transmits when something changes, so a band stays where you left
it until you move that fader again.

### On the panel

| Switch | Knob 1 (Main) | Knob 2 (X) | Knob 3 (Y) |
|---|---|---|---|
| **Middle / Down** | Vowel morph | Mouth openness | Source mix, buzz ↔ noise |
| **Up** | F0, 50–500 Hz | — | Source mix |

Switch **Down** also fires a plosive on each flick — the nearest the panel has
to a stop key.

The panel and the 8mu both write the same eight band gains, and neither takes
priority. There is no mode switch: move a fader or turn a knob, whichever is
under your hand.

### Jacks

| Jack | Function |
|---|---|
| Audio In 1 | External excitation — replaces buzz and hiss when patched |
| CV In 1 | 1V/oct F0 |
| Pulse In 1 | Plosive trigger |
| Pulse In 2 | Glottal gate |
| Audio Out 1 / 2 | Mono output, same signal on both |
| CV Out 1 | Formant energy envelope |
| CV Out 2 | Measured DSP load |
| Pulse Out 1 | High while voiced |
| Pulse Out 2 | High while frozen |

### LEDs

0–3 show band energy in pairs, low to high. LED 4 lights when an 8mu is
mounted. LED 5 shows voiced (bright) versus unvoiced (dim), or full brightness
while frozen.

## Why the vowel morph is not alphabetical

Knob 1 walks the table in the order **AH → OH → OO → UH → EH → EE**, which
looks arbitrary and is not. Adjacent entries are what the knob crossfades
between, so two similar vowels sitting next to each other would give a stretch
of travel where nothing audible happens.

AH and UH are the closest pair in the set — only 2.3 dB apart, because
acoustically they really are neighbours — and an early version of the table had
them adjacent. The order above was chosen by searching all 720 permutations for
the shortest path around the vowel quadrilateral among those whose worst
adjacent step still exceeds 6 dB. It happens to trace open-back → close-back →
central → front → close-front, which is a lap a real mouth could make.

Sweeping it slowly is the closest this card gets to a diphthong.

## Reading the DSP load

CV Out 2 outputs measured microseconds per sample against the 20.83 µs budget,
0 V to about 5 V for 0–100%. This is a measurement, not an estimate. If it
reads above about 4 V the card is close to dropping samples.

## What formant freeze actually does

It latches the eight band gains where they are. Faders and Knob 1 stop having
any effect; the excitation keeps running. It holds exactly the vowel that was
there, which is the point — it is not a reverb or a sustain, and if you freeze
during a plosive you get the plosive's spectrum held indefinitely, which sounds
odd because it is.

Press Button 4 again to release.

## Building

```bash
cmake -B build -G Ninja
cmake --build build
```

Then drag `build/tract8.uf2` onto the Pico in bootloader mode. A released
binary is committed at `UF2/tract8.uf2`.

The build reports 12 warnings, all from the Pico SDK's `pwm.h` inlined into
the vendored `ComputerCard.h`. None come from TRACT8's own sources, and they
are present in every ComputerCard project.

## Testing

Seven host-side scripts under `tools/`, run individually. They need `numpy`.

```bash
python tools/filter_check.py   # band geometry, Q15 quantisation, STABILITY
python tools/excite_check.py   # polyBLEP aliasing, noise whiteness
python tools/vowel_check.py    # vowel distinctness, morph continuity, tilt
python tools/midi_check.py     # 8mu dispatch AND the running-status parser
python tools/chain_check.py     # ABSOLUTE output level in DAC counts
python tools/silence_check.py   # no jack fault may mute the card
python tools/budget_check.py    # cycle estimate (prediction, not measurement)
```

Two caught real bugs before hardware; the last two were written *because* of
the hardware silence bug, and exist to stop it recurring. See `CLAUDE.md`.

## Status

**v1.0.1 — the first hardware run was silent, and the fix is not yet
confirmed.**

v1.0.0 made no sound except a plosive click. Two lines gated the excitation on
`Connected()`, and because ComputerCard forces a disconnected input to zero, a
misdetected jack fed the filter bank silence. Both now fall back to making
sound rather than muting. See `CLAUDE.md` for the full account.

If this build is still silent, **hold the switch down** — LEDs 0–3 become a
diagnostic display that says which condition is at fault:

| LED | Meaning if lit |
|---|---|
| 0 | Excitation gated shut |
| 1 | External input has taken over |
| 2 | Formant freeze latched |
| 3 | Band gains all near zero |

Everything else above about how it *sounds* remains a prediction. The DSP load
figure is a model, and models of this hardware have been wrong by a factor of
four; CV Out 2 is the authority.

## Credits

- **Homer Dudley** and Bell Labs, for the Voder, 1939.
- **rppicomidi** — the `usb_midi_host` driver, MIT, vendored unmodified.
- **Chris Johnson** — the ComputerCard library and its `midi_host` example,
  which this card's USB scheduling follows.
- **Tom Whitwell / Music Thing Modular** — the Workshop System and the 8mu.

## License

MIT. See `LICENSE`, which records the vendored components and their terms.
