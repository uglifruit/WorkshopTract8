# TRACT8 — implementation notes

Developer-facing. For how to *play* the card, see `README.md`; for the
do-not-undo list and the bug history, see `CLAUDE.md`.

## Stages

| Stage | File | Verified by |
|---|---|---|
| Excitation (buzz, noise, gates) | `voder.cpp` | `tools/excite_check.py` |
| Filter bank (8 × 2-pole IIR) | `voder.cpp` | `tools/filter_check.py` |
| Vowel table and morph | `vowels.h`, `main.cpp` | `tools/vowel_check.py` |
| 8mu dispatch | `midi8mu.cpp` | `tools/midi_check.py` |
| MIDI stream parser | `usb_core1.cpp` | `tools/midi_check.py` 6–7 |
| Panel, CV, LEDs, scheduling | `main.cpp` | — |
| Cost | all | `tools/budget_check.py` (a model) |

## The signal chain

```
  buzz osc (F0, polyBLEP saw) ──┐
                                ├─→ crossfade ──→ >>3 ──┐
  noise (xorshift32) ───────────┘                       │
                                                        │
  Audio In 1 (when patched, replaces both) ─────────────┤
                                                        ▼
                     ┌──────────────────────────────────────────┐
                     │  8 parallel 2-pole bandpass, Q = 4       │
                     │  250 450 700 1000 1400 1900 2600 3800 Hz │
                     │   ×g1  ×g2  ×g3  ×g4  ×g5  ×g6  ×g7  ×g8 │
                     └──────────────────┬───────────────────────┘
                                        │  sum
                     plosive burst ─────┤  (post-bank, deliberately)
                                        ▼
                                      >>4 ──→ clamp ──→ Audio Out 1+2
```

## Fixed point

Everything on the hot path is integer. Floats appear in exactly one place —
`VoderInit()`, which runs once at boot before the audio interrupt starts, and
uses `double`, `sin()` and `cos()` to derive the biquad coefficients. There is
no FPU; a soft-float op costs roughly 360 ns, which is fine once and fatal at
48 kHz.

| Quantity | Format | Notes |
|---|---|---|
| Band gains, knobs, mix | Q15 | 0..32767 |
| Biquad coefficients | Q15 | `int32_t`, range ±65536 — `a1` reaches −1.99 |
| Biquad state | Q15 | `int32_t` |
| Oscillator phase | Q32 | `uint32_t`, wraps for free |
| Phase increment | Q32 | derived from milli-Hz via a Q16 constant |
| Vowel table | Q15 | peak capped at 26800, see below |

Three rules, all learned the hard way on this hardware:

1. **Tables written at init are plain `.cpp` globals**, so they land in BSS.
   A header `static` lands in flash, where writes are silently discarded.
2. **`__not_in_flash_func` goes on the definition only.** Repeating it in the
   header makes GCC emit two section names and quietly ignore one.
3. **64-bit products need `PICO_INT64_OPS_IN_RAM=1`.** Each is a call to
   `__aeabi_lmul`, which lives in flash by default. See below.

## The filter bank

Standard RBJ constant-skirt bandpass, one biquad per band, Direct Form I:

```
y[n] = b0·(x[n] − x[n−2]) − a1·y[n−1] − a2·y[n−2]
```

`b1` is always 0 and `b2` always `−b0` for a bandpass, so neither is stored.

**Why Q15 and not Q13.** `a1 = −2·cos(w0)/a0` approaches −2.0 as the centre
frequency falls, which looks like it needs more integer headroom than Q15
allows. It does not: these are `int32_t`, so Q15 spans ±65536, not ±1.0. Q13
was tried first and put the 250 Hz band at 239 Hz — a 74-cent error — because
the coefficient's resolution is worst exactly where it matters most. Q15 brings
that to 1.4 cents. `tools/filter_check.py` measures this.

**Why the accumulator is `int64_t`.** A single product reaches
`65234 × ~20000 ≈ 1.3e9`, which fits `int32_t`. Three of them sum to
`4.0e9`, which does not. The 64-bit accumulator is not defensive
over-engineering; it is required, and `filter_check.py` check 6 asserts it.

**Stability.** Pole radii run from 0.9420 (3800 Hz) to 0.9959 (250 Hz). The
lowest band has only 0.004 of margin, which is why `filter_check.py` checks
pole radius explicitly rather than assuming quantisation is harmless. Anything
that changes Q or the centre frequencies must re-run it.

## Excitation

**Buzz.** A sawtooth from the top bits of a `uint32_t` phase accumulator, with
polyBLEP correction at the wrap. Without it, a 110 Hz saw folds energy from
above Nyquist straight into the 2600 and 3800 Hz bands, where no amount of
filtering can remove it — the aliases are already inside the passband.
polyBLEP costs two multiplies on the one sample in ~436 where the wrap
happens, and buys about 5–6 dB of alias rejection below 5 kHz
(`tools/excite_check.py`).

**Noise.** xorshift32, top 16 bits (a shift-register PRNG's low bits are the
least well distributed). Measured flat to 0.07 dB across the spectrum.

**Gates.** Linear ramps over `kGateRamp = 96` samples (2 ms). A one-pole would
take ~10 ms to settle and slur consonant attacks; a hard edge clicks.

## The vowel table

Six vowels × eight bands. Each entry is derived rather than invented: the
first three formants come from Peterson & Barney (1952), each gets a relative
amplitude falling with formant number, and each band's gain is the energy those
formants deposit in it under a log-frequency gaussian 0.42 octaves wide. A
−26 dB floor is added before normalising, because real vowels have no silent
regions.

Two properties of the table are load-bearing:

- **The row order** — Knob 1 crossfades between adjacent rows, so similar
  vowels must not be neighbours. See `CLAUDE.md`.
- **The peak is 26800, not 32767** — Knob 2's tilt multiplies by up to 1.219,
  and `26800 × 1.219 = 32662` still fits Q15. Raising the table to "use the
  full range" makes the tilt clip its loudest band.

## Knob 2's tilt is multiplicative

It scales each band by `1 + tilt·pos/K` rather than adding an offset. Additive
tilt cannot be made to work: the offset needed to brighten a loud band audibly
exceeds a quiet band's entire value, so one end of the spectrum always pins.
With AH, whose 250 Hz gain is 1364, an additive tilt strong enough to hear
drove band 0 negative. A band pinned at zero is a hole no fader can reopen.

`>>4` gives ±22% swing at the outermost bands, about 1.2 dB, with zero pinning
across all six vowels at both knob extremes.

## Cores

**Core 0** runs `ProcessSample()` in the 48 kHz audio interrupt. **Core 1**
runs `tuh_task()` in a bare loop and nothing else.

This is the opposite of what the original brief specified, and it is
deliberate: `ComputerCard::Run()` installs the audio interrupt on whichever
core calls it, and every working USB-host card on this hardware — including the
official ComputerCard `midi_host` example — arranges it this way. `tuh_task()`
has no deadline of its own; the audio ISR has a hard 20.83 µs one.

Cross-core state is a single `volatile VoderState` (`shared.h`). Every field is
a single byte or a naturally-aligned 32-bit word, so accesses are atomic on the
M0+ and no mutex is needed. Plosive triggers are **counted, not flagged** —
a flag can be missed or double-read depending on interleaving.

## USB MIDI host

TinyUSB 0.18 (Pico SDK 2.2.0) ships **no MIDI host class driver**. The driver
is rppicomidi's `usb_midi_host`, vendored unmodified under MIT with its
copyright header intact, registered through TinyUSB's `usbh_app_driver_get_cb()`
extension point.

**Do not `#define CFG_TUH_MIDI 1`.** It gates a fragment in `usbh.c` that
references audio-class constants without including the audio header, and the
build fails. `tusb_config.h` records this at length.

`tuh_midi_stream_read()` returns a **byte stream**, so `usb_core1.cpp` carries
a small running-status parser. That is not optional: running status is exactly
what an 8mu emits during a fader sweep.

## Cost

Predicted **44.2%** of the per-sample budget at 192 MHz — filter bank 84%,
excitation 11%, everything else 5%. The vowel morph is rate-limited to 125 Hz
(`kMorphDiv = 384`); at full rate it alone was 28% of budget.

**This prediction is not to be trusted.** WorkshopSpectral modelled 51% and
measured 231%. **CV Out 2 reports the real figure** — measured microseconds
against the 20.83 µs budget — and it is the authority. `budget_check.py` exists
to rank what to attack if the measurement comes back high, not to be believed
on its own.

If the bank does turn out too expensive, the known fallback is `MulQ15`-style
32-bit split multiplies in place of the 64-bit accumulator, already proven in
`../WorkshopSpectral/fft.h`.

## Development tools

All under `tools/`, pure Python plus numpy, run individually:

- **`filter_check.py`** — pole radii, centre frequencies, Q15 quantisation
  error, measured Q, inter-band coverage, int32 headroom. The most important
  of the five; it caught the Q13 bug.
- **`excite_check.py`** — polyBLEP vs naive saw aliasing across F0, sawtooth
  range, xorshift uniformity and period, noise whiteness.
- **`vowel_check.py`** — pairwise and adjacent vowel distinctness, morph
  continuity across segment boundaries, tilt pinning across every vowel,
  Q15 range with tilt headroom. It caught both the dead zone and the tilt bug.
- **`midi_check.py`** — CC/note dispatch, Q15 scaling, freeze gating, and the
  stream parser: running status, interleaved real-time bytes, system common,
  single-data-byte messages, channel agnosticism.
- **`budget_check.py`** — cycle model. Prints no PASS/FAIL because it measures
  nothing.

The response evaluation in `filter_check.py` is **analytic**, not a simulation
of the C++ filter. A test that ran the implementation to check the
implementation would agree with itself however wrong both were.
