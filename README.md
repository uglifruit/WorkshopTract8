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
| **Fader 1** | CC 34 | **Openness** — close ↔ open (vowel F1) |
| **Fader 8** | CC 41 | **Front** — back ↔ front (vowel F2) |
| **Fader 2** | CC 35 | **Click decay** — short hit → steady tone |
| Fader 3 | CC 36 | Breath — buzz ↔ noise |
| Fader 4 | CC 37 | Pitch — F0, 50–500 Hz |
| Fader 5 | CC 38 | Brightness — spectral tilt |
| Faders 2, 6, 7 | — | unused, on purpose |
| Tilt front / back | CC 42 | **Volume** — level is full, either extreme silent |
| Tilt left / right | CC 44 | **Rounding** — level is unrounded, either extreme toward OO |
| **Turned over** | CC 49 | **Mute** — CC 49 reads high when upright |
| **Button 1** | Note 36 (C2) | **Trigger** — fires the click and holds it open |
| Button 2 | Note 48 (C3) | Gate the unvoiced noise |
| Button 3 | Note 60 (C4) | Plosive burst |
| Button 4 | Note 72 (C5) | Toggle formant freeze, **+ breath while held** |

Channel-agnostic — set the 8mu to any channel.

### The vowel cube

Three axes, and they are the three things a mouth actually does:

```
              ROUND = 0 (spread)          ROUND = full
      CLOSE    OO ---------- EE            OOr -------- EEr
                |            |              |            |
      OPEN     AH ---------- EH            AHr -------- EHr
              back        front           back        front
```

**Openness** is how far the jaw opens (F1). **Front** is where the tongue
sits (F2). **Rounding** is how far the lips protrude — it lowers F2 hard and
F1 a little, which is why it is a genuinely separate direction rather than a
slide along the other two.

The vowel axes are on **faders 1 and 8**, the outermost pair, so the two
controls you play constantly span the hand rather than needing one finger
twice. Faders 2 and 6 are unassigned on purpose: an earlier version gave
each fader one filter band and it was unplayable.

**OH needs the third axis.** With only openness and front it sat 6.0 dB
outside the reachable space — it is rounder than anything on the OO–AH edge,
its F2 (840 Hz) below both back corners. Tilt the controller and it appears.
AW and ER come with it.

**Rounding is the left/right lift.** Lifting either side moves toward the
rounded (OO) face of the cube — direction does not matter there, only how
far. A level controller is unrounded.

**Mute is disabled in this build.** It was turned off while the tilt
behaviour was being sorted out, to keep the diagnosis clean.

**Buttons 1 and 4 add breath while held**, on top of wherever fader 3 is
parked — a voiced sound with a whisper of noise under it reads as breathy
rather than buzzy. Both already did something and neither used its held
state, so the expression is free.

Every control takes over from its panel equivalent only when you actually
move it — an 8mu plugged in but untouched never seizes a knob your hand is on.

### On the panel

| Switch | Knob 1 (Main) | Knob 2 (X) | Knob 3 (Y) |
|---|---|---|---|
| **Middle / Down** | Openness | Front | Breath |
| **Up** | Pitch | Brightness | Rounding |

Between the two pages the panel reaches **every** parameter the 8mu can, so
the card is fully playable on its own.

**Unplug the 8mu and the knobs take over again.** Each control follows the
controller only while it is connected *and* has actually been moved — so an
idle 8mu never seizes a knob under your hand, and pulling the cable
mid-session hands everything straight back to the panel.

Switch **Down** also fires a plosive on each flick — the nearest the panel has
to a stop key.

The panel and the 8mu both write the same eight band gains, and neither takes
priority. There is no mode switch: move a fader or turn a knob, whichever is
under your hand.

### Jacks

| Jack | Function |
|---|---|
| Audio In 1 | **External exciter** — summed with the internal buzz and noise |
| Audio In 2 | **Volume CV** — swells and ducks around the base level |
| CV In 1 | **Pitch**, 1V/oct |
| CV In 2 | **Formant CV**, bipolar — sweeps the vowel cube diagonally |
| Pulse In 1 | **Click** trigger |
| Pulse In 2 | **Glottal gate** |
| Audio Out 1 / 2 | Mono output, same signal on both |
| CV Out 1 | Formant energy envelope |
| CV Out 2 | Measured DSP load |
| Pulse Out 1 | High while voiced |
| Pulse Out 2 | High while frozen |

### Feeding it random voltages

The card is built to babble. **Every CV adds to its control rather than
replacing it**, so a random voltage wanders the sound around wherever the
knobs and faders are parked — the patch never has to supply a sensible
absolute value, and nothing goes dead when a cable goes in.

**CV In 2 is the one to reach for first.** It moves openness up as front
moves down, sweeping the vowel cube along its diagonal, which crosses the
middle where the distinct vowels live. A sample-and-hold here walks through
recognisably different vowels; sweeping a single axis instead would mostly
travel between two corners. Measured: the diagonal spans 17 dB against
10 dB for one axis alone.

**Gates chatter without clicking.** The glottal ramp is 2 ms, so Pulse In 2
opens fully up to about 200 Hz and above that degrades into amplitude
modulation rather than into clicks — a usable texture rather than a fault.
Pulse In 1 retriggers the click on every rising edge, so a fast gate stream
stutters it.

A good first patch for this: slow random into CV In 2, a clock divided a few
ways into both pulse inputs, and an envelope into Audio In 2.

**Audio In 1 sums, it does not replace.** Patch a drum loop or a bassline in
and it goes through the same eight bands the card is speaking with — so you
can talk over it, rather than the internal voice disappearing while a cable
is in. It comes in level with the internal sources, so whatever you patch can
carry the sound; set the balance at the source.

Breath has no CV input, because it has three physical routes already
(fader 3, Knob 3, and buttons 1 and 4) while external audio had none.

### LEDs

0–3 show band energy in pairs, low to high. LED 4 lights when an 8mu is
mounted. LED 5 shows voiced (bright) versus unvoiced (dim), or full brightness
while frozen.

## BABBLE mode (alt-boot)

**Hold the switch down at power-on.** The boot LEDs light odd instead of
even to confirm it.

Each knob then drives several things at once, so the card talks from a
single sustained gate with nothing else patched:

**Page 1** (switch middle) is *what is said*:

| Knob | Controls |
|---|---|
| **Main** | Vowel diagonal **and** pitch — one sweep walks through vowels, rising as it goes |
| **X** | Chatter rate **and** brightness — clockwise is faster and brighter |
| **Y** | Breath **against** rounding — a rounded hum at one end, a breathy whisper at the other |

**Page 2** (switch up) is *who is saying it*:

| Knob | Controls |
|---|---|
| **Main** | **Voice size** — pitch, brightness and vowel placement together, from a small bright voice to a large dark one |
| **X** | **Animation** — how much the voice wanders between syllables, from a monotone to muttering |
| **Y** | **Consonants** — how many syllables get a plosive, and how loud, from legato to heavily articulated |

Set a voice on page 2, then play it on page 1.

### Auto-chatter

**Hold the momentary switch down for two seconds** and the card starts
talking by itself — no gate needed. The LEDs fill as a progress bar while
you hold, so you can see the gesture land. **Tap the switch to stop.**

It generates phrases rather than an even stream: 2–7 syllables of differing
lengths, then a pause long enough to read as taking a breath. Some syllables
get a consonant, most do not. LED 5 pulses with each syllable so the panel
shows it is running.

Deliberately asymmetric — starting something that then plays by itself takes
effort, stopping it does not.

Hold a gate on either pulse input and it chatters by itself, 2 Hz to 20 Hz
across Knob X. Each syllable is voiced for the first third of its period, so
they always separate instead of running together.

**The chatter makes no clicks of its own** — it is a voice, not a drum. If
you want consonants on top, patch them into **Pulse In 1**, which triggers
clicks in every mode. That way the percussion is a choice rather than
something the mode imposes on every syllable.

Normal mode is one knob per parameter, which is right for playing
deliberately. This is for getting a texture going in seconds.

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

## Breath is a ratio, not a level

Breath changes how *breathy* the voice is, not how loud — sweep it from end
to end and the level stays put while the character moves from a clear hum to
a whisper. It behaves the same whether both source buttons are gating, one
is, or neither: the gates decide the envelope and breath decides what is
inside it.

That makes it duck and swell with everything else. Pull the volume down with
the tilt or fader 7 and the breathy component comes down with it, in
proportion — which is what a voice does.

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

Eight host-side scripts under `tools/`, run individually. They need `numpy`.

```bash
python tools/filter_check.py   # band geometry, Q15 quantisation, STABILITY
python tools/excite_check.py   # polyBLEP aliasing, noise whiteness
python tools/midi_check.py     # 8mu dispatch AND the running-status parser
python tools/chain_check.py     # ABSOLUTE output level in DAC counts
python tools/silence_check.py   # no jack fault may mute the card
python tools/init_check.py      # the bank is usable before main() runs
python tools/playable_check.py  # knob and faders both stay live
python tools/budget_check.py    # cycle estimate (prediction, not measurement)
```

Two caught real bugs before hardware; the last two were written *because* of
the hardware silence bug, and exist to stop it recurring. See `CLAUDE.md`.

## Status

**v1.1.0 — it makes sound. Playability changes from the first real session,
plus a lockup fix.**

The card works. Three things came out of playing it:

- **It could lock up under heavy MIDI.** The USB receive callback drained
  its endpoint in a `while (true)` that only exited when the stream ran dry
  — and an 8mu held in the hand streams accelerometer CCs continuously, so
  it never did. `tuh_task()` stopped being called and USB died. The drain is
  now bounded per callback.
- **The faders were too subtle.** Now squared; see above.
- **The vowel morph and breath moved onto the 8mu**, since those turned out
  to be the controls worth having under your hands.

### Earlier: two silence bugs (v1.0.0–v1.0.2)

v1.0.0 was silent; jack detection was suspected and fixed, correctly but
irrelevantly. v1.0.1 was still silent — but the LEDs now tracked the knobs,
and that identified the real cause: **static initialisation order**. The card
is a file-scope global, so its constructor ran before `main()` computed the
filter coefficients, and every biquad was initialised to all zeros. A
zero-coefficient biquad is silent no matter what you feed it, while the LEDs
(which read the band gains, not the filter output) carried on working
perfectly. See `CLAUDE.md` for the full account.

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
