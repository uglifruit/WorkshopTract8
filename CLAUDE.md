# TRACT8 — working notes for Claude Code

A program card for the **Music Thing Modular Workshop System Computer**
(RP2040), built on the header-only **ComputerCard** library. Sibling project to
`../WorkshopSpectral`, `../WorkshopZX`, `../WorkshopBio`,
`../WorkshopNibbleDrum`, `../Workshop2D2` — reuse their conventions and
structure where they fit.

**TRACT8 is an eight-band reimplementation of the 1939 Bell Labs Voder**
(Homer Dudley), played from a Music Thing 8mu over USB MIDI Host. It is an
original implementation: the Voder was relays and vacuum tubes, so there is no
code to port and the debt is conceptual.

## Current status: v1.1.0, WORKS ON HARDWARE

The card makes sound. Everything below about silence is history, kept
because the lessons generalise.

Three findings from the first real playing session, all fixed:

1. **Lockup under heavy MIDI.** `tuh_midi_rx_cb` drained its endpoint in a
   `while (true)` that exited only when the read returned 0. An 8mu held in
   the hand streams accelerometer CCs continuously and can refill the
   endpoint as fast as the loop empties it, so the callback never returned,
   `tuh_task()` was never called again, and USB stopped entirely. The drain
   is now bounded to 256 bytes per callback; the remainder simply arrives on
   the next one. **Never write an unbounded drain loop inside a callback the
   task pump is waiting on.**
2. **The faders barely changed the sound.** They were linear, and a real
   vowel has ~27 dB between its loudest and quietest band — so a linear
   fader at any ordinary hand position produces a near-flat spectrum, which
   is a filter sweep, not a voice. Squared now: ~36 dB across the throw.
   Cubed was tried at ~63 dB and is too much, the fader feels dead until the
   last third.
3. **The vowel morph is better than the faders**, which is worth taking
   seriously as design feedback: it is the control that carries the
   character. It moved to the accelerometer's left/right axis, breath took
   fader 8, and band 8 gave up its fader.

### The fader/morph conflict, which nearly shipped

The panel morph writes all eight band gains at 125 Hz. Once the faders own
bands 1–7 that would overwrite every fader move within 8 ms and the faders
would look dead — the *same symptom* as the linear-curve problem but a
completely different cause. `g_state.faders_touched` latches on first fader
use and the morph then skips bands 0–6. It still drives band 7, which no
fader claims.

## Previously: v1.0.2, two silence bugs found on hardware

`build/tract8.uf2` — **1.98% flash, 18.79% RAM**. All seven test suites pass.

Two hardware runs, both silent, **two entirely different causes**:

- **v1.0.0** — silent. Suspected jack-detection, fixed it (see below). That
  fix was correct and worth keeping, but it was *not* the reason the card was
  mute.
- **v1.0.1** — still silent, but now the LEDs tracked the knobs and faders.
  That new information identified the real bug: **static initialisation
  order**. The filter bank was running with all-zero coefficients.

**v1.0.2 is not yet confirmed on hardware.**

## Four corrections to the original brief

These were checked against the repo and the 8mu documentation before any code
was written, and all four were confirmed with the user. Do not silently revert
any of them.

1. **The core split is inverted from the brief.** The brief asked for DSP on
   Core 1 and USB polling on Core 0. `ComputerCard::Run()` installs the 48 kHz
   audio interrupt on *whichever core calls it*, and every working USB-host
   card on this hardware — the official ComputerCard `midi_host` example
   included — puts audio on Core 0 and the USB pump on Core 1. We follow that.
   `tuh_task()` has no deadline of its own; the audio ISR has a hard 20.83 µs
   one.
2. **8mu faders are CC 34–41, not CC 1–8.** Confirmed on musicthing.co.uk and
   in three independent in-repo drivers. Accelerometer gestures are CC 42–49.
   The brief's CC 1–8 would require the user to re-map their 8mu in the web
   editor first, which defeats the point.
3. **Cards live in `releases/<NN>_<Name>/`, not `src/programs/`.** The latter
   does not exist in the Workshop_Computer repo. 106 is free.
4. **`33_drumdrum`'s `midi_host.cpp` is NOT reusable.** It is by Moses Hoyt,
   not us. Its `info.yaml` declares MIT but the directory ships no LICENSE
   file and the source carries no copyright notice, so MIT's retention clause
   cannot be satisfied by copying it. We vendor **rppicomidi/usb_midi_host**
   instead — MIT with its header intact, and what the official example and six
   released cards already use.

## The real silence bug (v1.0.1): static initialisation order

**This is the one that actually muted the card.** Read it before touching
anything to do with construction order.

```cpp
VoderCard card;          // file-scope global -> ctor runs BEFORE main()
  -> voder_.Init()       // copies band_b0[] etc. into bank_[]

int main() {
  VoderInit(0);          // ...which is where band_b0[] gets COMPUTED
}
```

Every biquad was initialised from **still-zeroed** coefficient tables. A
biquad with `b0 = a1 = a2 = 0` outputs zero forever, whatever you feed it and
whatever gain you apply.

### The symptom was a fingerprint

The second hardware report was *"LEDs change when I turn knob/faders — but no
sound except the same click"*, and that pair of facts localised it precisely:

- **LEDs responded** → the panel, the MIDI path, `g_state.band_gain` and the
  LED path all worked. The LEDs display `band_gain`, **not** the filter
  output.
- **Plosives sounded** → the ISR, the DAC and the output path all worked.
  Plosives are summed *after* the bank.

Everything on both sides of the filter bank was proven good. Only the bank
itself was left.

### The fix

`Voder::Init()` now calls `VoderInit()` itself rather than trusting that
`main()` got there first, and `VoderInit()` is idempotent so the second call
from `main()` is harmless. There is also a **mute guard**: if `b0` is still
zero after init, the bank falls back to pass-through — which sounds wrong,
but wrong is diagnosable from the panel and silence is not.

**Static initialisation order is not something to be careful about. It is
something to design out.** Do not add anything to `VoderCard`'s constructor
that depends on work done in `main()`.

### Why no test caught it

`filter_check.py` computes the coefficients in Python and checks the maths —
and the maths was always right. It never modelled *when* the C++ runs. The
coefficients simply were not there yet. `tools/init_check.py` now covers this.

## The first silence bug (v1.0.0): jack detection

Fixed before the real cause was known. The fix stands on its own merits —
both lines genuinely did fail in the silencing direction — but it was **not**
why the card was mute.

The first hardware run produced no sound at all except the plosive click.
That symptom is worth more than it looks: plosives are summed **after** the
filter bank, so their working proved the ISR ran, the DAC worked and the
output path was intact. Everything upstream of the bank was suspect and
everything downstream was cleared, in one observation.

Two lines were responsible, both with the same shape:

```cpp
ext_gate = Connected(Input::Pulse2) ? PulseIn2() : true;   // gate path
use_ext  = Connected(Input::Audio1);                       // excitation path
```

ComputerCard **forces a disconnected input's value to zero**. So if the
normalisation probe reports a jack as connected when nothing is patched:

- **Pulse2 misdetected** → `PulseIn2()` reads low forever → both excitation
  levels gate to zero → silence.
- **Audio1 misdetected** → the internal excitation is replaced by a constant
  zero → the bank is fed silence → silence.

Either alone mutes the card. Both fail in the *silencing* direction, which is
the worst possible direction for a fault nobody can see from the panel.

**The fix removes the dependency on `Connected()` for both.** The gate now
latches on having actually *seen* Pulse In 2 go high, and the external input
takes over only when there is genuine signal above `kExtGateLevel`, held for
`kExtHoldSamples` so zero crossings do not chatter it. A misdetected jack now
leaves the card droning — recoverable and obvious — instead of mute.

`tools/silence_check.py` enforces the rule going forward: **no single
jack-detection fault may reduce the card to silence.**

### Why every existing test passed while the card was silent

All five original suites test **DSP**: filter geometry, aliasing, vowel
distinctness, MIDI decoding, cycle cost. None of them modelled the **control
flow** that decides what reaches the DSP. `chain_check.py` was written during
this investigation to measure absolute level end to end (it cleared the maths
— 221 DAC counts, perfectly audible), and `silence_check.py` to model the
routing. The lesson generalises: *a DSP test suite can be entirely green
while the signal never arrives.*

### The diagnostic display

Because every candidate cause looked identical from the front panel, **switch
down now turns LEDs 0–3 into a diagnostic** (it still fires plosives):

| LED | Meaning if lit |
|---|---|
| 0 | Excitation gated shut — no buzz, no noise |
| 1 | External input has taken over from the internal sources |
| 2 | Formant freeze is latched |
| 3 | All eight band gains are near zero |

If the card is ever silent again, that display says which of the four it is
without another round trip.

## The two bugs the tests caught before hardware

Both were caught before the code ever ran, and neither would have been obvious
from listening.

### Q13 coefficients put the lowest band 74 cents flat

The first version of `voder.cpp` stored biquad coefficients as Q13, reasoning
that `a1` reaches −1.99 and so cannot fit Q15's nominal ±1.0.

`tools/filter_check.py` measured the 250 Hz band peaking at **239.2 Hz — a
4.31% error, 74 cents** — and losing 0.6 dB of level. The cause is that `a1`
approaches −2.0 as the centre frequency falls, so the coefficient needs its
resolution exactly where Q13 has least to give.

Q15 works because these are `int32_t`, not `int16_t`: the range is ±65536, not
±1.0. The stored values reach 65234. Error drops to **0.08%, about 1.4 cents.**

**Do not reduce `COEFF_SHIFT` below 15.** Re-run `tools/filter_check.py` if you
touch it.

### The Knob-2 tilt could not work additively, and testing one vowel hid it

"Mouth openness" adds a signed offset across the bands. The first version was
additive and `tools/vowel_check.py` passed — because it tested AH alone.

Sweeping all six vowels failed four of them immediately. The band at risk of
pinning **differs by vowel**: AH pins at the bottom (its 250 Hz gain is only
1364, so a downward tilt drives it negative), while OO and EE pin at the top
(their 250 Hz gain is the peak). No single additive strength works: the offset
needed to brighten a loud band audibly exceeds a quiet band's entire value.

A band pinned at 0 is a hole in the spectrum no fader can reopen, and it reads
to a player as a broken filter.

The fix is **multiplicative** tilt — each band scaled by a factor rather than
offset — plus **capping the vowel table's peak at 26800 instead of 32767**, so
the maximum 1.219× boost still fits Q15. `26800 × 1.219 = 32662`.

**Do not "use the full Q15 range" in `vowels.h`.** It reintroduces the bug.

## The vowel table's row order is load-bearing

Knob 1 crossfades between **adjacent** rows, so two similar vowels sitting next
to each other give a stretch of travel where nothing audible happens.

AH and UH are the closest pair in the set at 2.3 dB — acoustically they really
are neighbours — and the first table had them adjacent. `vowel_check.py`
caught the dead zone.

The order was chosen by searching all 720 permutations for the shortest
articulatory path in the F1/F2 plane among those whose worst adjacent step
exceeds 6 dB:

```
AH -> OH -> OO -> UH -> EH -> EE     worst step 6.1 dB, mean 9.5 dB
```

which traces open-back → close-back → central → front → close-front. A lap of
the vowel quadrilateral, and a path a real mouth could make.

Note the test distinguishes *closest pair anywhere* from *closest adjacent
pair*. Only the second matters; two similar vowels at opposite ends of the
table are never compared by the ear.

## Do NOT `#define CFG_TUH_MIDI 1`

It looks like the obvious way to enable MIDI host support. It breaks the build:

```
usbh.c:1686: error: 'AUDIO_SUBCLASS_CONTROL' undeclared
```

TinyUSB 0.18 has no MIDI host class driver at all. The macro gates a fragment
in `usbh.c` that references audio-class constants without including the audio
header. The upstream ComputerCard `midi_host` example carries the same warning
in its own `tusb_config.h`, and `20_reverb` repeats it. rppicomidi's driver
does its own descriptor parsing and does not need the macro.

## The MIDI parser must handle running status

`tuh_midi_stream_read()` returns a **byte stream, not aligned messages**.
Running status — a status byte sent once, followed by a run of bare data pairs
— is not an edge case here: it is exactly what an 8mu emits when you sweep a
fader, which is the single most common thing anyone will do with this card.

A parser that reset its status after each message would drop all but the first
CC of every sweep, and the faders would feel broken in a way that is very hard
to diagnose from the symptom.

Real-time bytes (0xF8–0xFF) can also land *between* the data bytes of another
message and must be skipped without disturbing the parse. Program change and
channel aftertouch are single-data-byte messages and must be consumed as such,
or the parser eats the following status byte.

All four cases are in `tools/midi_check.py` checks 6 and 7.

## Gotchas already handled — do not undo these

1. **Plosive triggers are counted, not flagged.** `g_state.plosive_count` only
   ever increments; the ISR keeps its own copy and fires once per increment it
   observes. A flag set on Core 1 can be missed entirely or fired twice
   depending on interleaving.
2. **The vowel morph is rate-limited to 125 Hz** (`kMorphDiv = 384`). At full
   rate it was 700 cycles, ~28% of the per-sample budget, to recompute a value
   that only changes when a hand moves. Rate-limiting took predicted load from
   61.6% to 44.2%.
3. **Written tables are plain `.cpp` globals**, never header statics —
   `band_b0/a1/a2` land in BSS. A header static lands in flash where writes are
   silently discarded, and the filters would produce nothing.
4. **`__not_in_flash_func` goes on the definition only**, never repeated in the
   header. GCC emits two section names and silently ignores one.
5. **Boot mute of 0.5 s** with all outputs held at zero, plus `sleep_ms(150)`
   on Core 1 before touching USB. The working USB-host cards all do the
   settling delay; without it the controller does not reliably come up after
   reset.
6. **`PICO_XOSC_STARTUP_DELAY_MULTIPLIER=64`** is required for this crystal.
   Without it the card fails to boot cold but works from a warm reset, which
   is exactly what makes it confusing.
7. **Plosive bursts are summed *after* the filter bank, not through it.** The
   Voder's stop keys bypassed the chain too. A plosive release is a broadband
   click; filtering it turns it into a thud that reads as a vowel onset.
8. **Unplugging the 8mu leaves the band gains where they are.** Slamming every
   formant shut mid-phrase is worse than holding.

## Things that look wrong but are not

- **The output sits well below full scale.** With all eight bands wide open
  and a worst-case input, `filter_check.py` measures a peak of 588 against the
  DAC's ±2048 — about 11 dB of headroom left on the table. That is deliberate,
  not a mistake in the gain staging: the `>>3` before the bank and `>>4` after
  it are sized so that eight bands summing *coherently* cannot overflow int32,
  and the coherent case is far louder than any real vowel. A vowel uses maybe
  three bands at once. If the card turns out too quiet in practice the honest
  fix is to reduce the output shift to `>>3` and re-run the test, not to
  remove the clamp in `main.cpp`.
- **Band Q drifts to 4.17 at 3800 Hz** (4.3% high). That is the bilinear
  transform's frequency warping near Nyquist, not a coefficient error. It is
  within the 15% tolerance and audibly irrelevant.
- **The build emits 12 warnings.** All from the Pico SDK's `pwm.h` inlined into
  the vendored `ComputerCard.h` at lines 874–875. **Zero come from TRACT8's own
  sources** — verified by grepping the build log. The same lines exist in
  WorkshopSpectral's copy, so this is pre-existing across the cards.
- **A full-scale 8mu fader gives 32512, not 32767** (`v << 8` of 127). 0.78%
  short, 0.07 dB. Special-casing 127 would cost a branch per CC to fix
  something nobody can hear.

## Structure

```
voder.cpp     excitation + 8-band filter bank  (filter_check, excite_check)
vowels.h      vowel gain vectors               (vowel_check)
midi8mu.cpp   8mu CC/note dispatch             (midi_check)
usb_core1.cpp Core 1 USB pump + MIDI parser    (midi_check, checks 6-7)
main.cpp      panel, CV, LEDs, core scheduling
```

Each test names its file and each file names its test. **If you change a DSP
file, re-run the matching test — they take seconds.**

`tools/budget_check.py` is a **model, not a measurement**, and it says so
loudly. WorkshopSpectral modelled 51% and measured 231% on real silicon. **CV
Out 2 is the authority on this card's cost.** Once TRACT8 has run on hardware,
replace the prediction with the reading.

## Design notes from playing it

- **Takeover is one-way and latched on first use**, never on the 8mu merely
  being connected. `vowel_from_midi`, `breath_from_midi` and
  `faders_touched` all work this way. A controller sitting plugged in but
  untouched must not seize a control the player has a hand on.
- **Band 8 was the right one to sacrifice** for breath: 3800 Hz carries
  sibilance rather than vowel identity, and the noise source feeds it
  plenty. It stays reachable from the morph and from Knob 2's tilt.
- **Breath is linear where the band faders are squared.** It is a balance,
  not a level — the midpoint of the fader should be half and half.

## Still to do

- [ ] Confirm the lockup is actually gone — it took sustained fader plus
      accelerometer traffic to provoke, so try to reproduce it deliberately.
- [ ] Read CV Out 2 for the real DSP load. Still unmeasured.
- [ ] If anything is ever silent again,
      hold the switch down and read LEDs 0–3 (see the diagnostic table
      above) — that identifies which condition is at fault immediately.
- [ ] If LED 1 is lit with nothing patched, `kExtGateLevel` is too low for
      this unit's ADC noise floor; raise it.
- [ ] If LED 0 is lit, the gate latch is being tripped by noise on Pulse
      In 2; `gate_seen_` needs a debounce or a threshold.
- [ ] Read CV Out 2 and replace the 44.2% prediction with the measurement.
- [ ] Confirm an actual 8mu enumerates and that CC 34–41 move the right bands.
- [ ] Listen for whether Q=4 is right. It is a guess balancing band separation
      against ringing on plosives, and only ears can settle it.
- [ ] Consider whether the six vowels want to be more, or whether a
      continuous F1/F2 model would beat a table.
