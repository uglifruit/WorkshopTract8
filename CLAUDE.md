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

## CV Out 2 became the vowel (v1.24.0)

The load meter did its job and then stopped being worth a jack.

It read ~100% of budget against a model predicting 44.2%, which is what
found the int64 filter bank. After the rewrite it read **3.0 V typical,
3.5 V peak - 60% and 70%**, or 12.5 and 14.6 us against the 20.83 us
deadline, with the sound unchanged. A number that is not going to change
again does not earn an output on a card with two.

CV Out 2 now carries the **openness axis of the vowel cube**, 0-5 V closed
to open, taken after the 8mu takeover, the formant CV and the babble
animation - so it reports the vowel actually being sounded rather than
where any one control sits. In BABBLE and auto-chatter it wanders on its
own, which makes it a modulation source musically RELATED to what is being
heard rather than an unrelated LFO.

The timing code went with it. It was two `time_us_32()` reads and a divide
per sample computing something nothing looked at. To measure again, put it
back around the ISR body - and note the measurement was trustworthy:
`time_us_32()` has only 1 us resolution against a 20.83 us budget, but the
truncation is unbiased and it accumulates over 256 samples.

## CV Out 2 read 5 V: the bank was 64-bit for no reason (v1.23.0)

The measurement the log kept asking for, finally taken, and it said what
WorkshopSpectral's did: **the model was wrong by more than 2x.**
`budget_check.py` predicted 44.2%; CV Out 2 read about 5 V, which is the
clamp - the ISR was using essentially its entire 20.83 us.

The measurement itself was checked before believing it. `time_us_32()` has
1 us resolution against a 20.83 us budget, which sounds far too coarse -
but the truncation is UNBIASED (the delta is floor(T) or ceil(T) depending
where the microsecond boundary falls, averaging to T), and the figure is
accumulated over 256 samples. The scaling checks out at exactly 2047 for
100%. The reading was real.

**The cost was `int64` in the filter bank.** The M0+ has no 64-bit
multiply, so every `(int64_t)a * b` is a call to `__aeabi_lmul` - three per
band, 24 per sample, plus a dozen more in the excitation path.

`PICO_INT64_OPS_IN_RAM=1` was already set and verified (`__wrap___aeabi_lmul`
at 0x2000a514, which is RAM). That was worth 40 points of load and is still
necessary. It just was not enough, because the right answer is not to make
the calls fast but to not make them.

**The width was never needed, and that had to be PROVED rather than
argued.** An int32 that wraps does not clip, it INVERTS, and a resonator
fed its own inverted output is an explosion rather than a distortion. Two
bounds:

- Driving every band at its own centre frequency - the worst case for a
  resonator - plus squares, noise and DC: worst accumulator 2.0e8 against
  int32's 2.1e9. **10.7x margin.**
- The algebraic bound, every term at maximum with the same sign
  simultaneously, which no signal can produce: 1.2e9. **1.79x margin.**

The second needed one extra bit of input headroom, so `excite >>= 3`
became `>>= 4` and the output shift `>>2` became `>>1`. Level is
identical - `chain_check.py` still measures a vowel at 528 counts and the
worst case at 1568.

`filter_check.py` now carries both bounds as a permanent check, because
this is exactly the kind of change that looks fine for months and then
detonates on one unusual input.

**Measured after the rewrite: 3.0 V typical, 3.5 V peak - 60% and 70% of
budget**, or 12.5 and 14.6 us against the 20.83 us deadline. From clamped
at 100% to 6 us of headroom at the worst, with the sound unchanged.

**The rule: `int64` on an M0+ is a function call, not an instruction.**
Reach for it only when a bound has been computed and genuinely exceeds
2^31, and re-derive that bound if the gain staging ever moves.

## The whine was MY soft clipper, and a cubic has no linear region (v1.22.0)

The v1.20.0 fix for the card being too quiet introduced the whine that
four subsequent versions then hunted. Worth stating plainly: this was a
regression I added, and I spent three rounds blaming the hardware for it.

**A cubic soft clipper bends from the very first sample.** There is no
linear region at all - `y = x - x^3/3` is curving at x = 1. So every vowel
was distorted all the time, not merely the peaks: 28 dB of THD at ordinary
playing level.

That is exactly the reported reproduction:

- **Fader 7 (volume) full** drives the level furthest into the curve.
- **Fader 5 (brightness) at SOME heights** decides WHICH band is loudest,
  and therefore where that band's harmonics land. A 700 Hz formant puts
  its 3rd at 2100 Hz and its 5th at 3500 Hz - right at the ~3750 Hz
  reported.
- **Static, not moving** - because harmonic distortion is a property of
  the transfer curve, not of anything changing.

### What identified it

A bisection build with the output stage bypassed - engine intact, volume
multiply and clipper removed - **had no whine**. One flash, and the search
collapsed from the whole card to eight lines.

The complementary build (pure tone, engine bypassed) confirmed it from the
other side: the tester noticed that fader 5 still changed the sound *of a
tone the engine was not generating*, which is only possible if something
downstream of both is coupling them. That was the output stage.

**Three earlier candidates were eliminated the same way and should have
been eliminated sooner:** LED PWM frequency, LED duty, and the knob
sampling beat all survived their own test builds. Each of those was a real
finding, and none was this.

### The fix

A **hard-knee limiter**. Below 1500 counts it is the identity - not
approximately linear, exactly linear, so ordinary playing is bit-for-bit
undistorted. Only genuine peaks bend, with a quadratic landing onto the
rail.

| | vowel at 528 | THD |
|---|---|---|
| original `>>2` | 528 | none |
| v1.20.0 cubic | 1705 | **-28 dB** |
| v1.22.0 limiter | 1320 | **-92 dB** |

### The gain was then set from a scope, not the model (v1.22.1)

x2.5 shipped first and measured +/-2 V typical with momentary peaks near
+/-3 V, against rails of +/-6 V - a third of the range. The model's "a
vowel peaks at 528 counts" is AH with the vowel table at its peak, and
real playing is about **half** that: 273 typical, 409 peak.

**Size the stage from the PEAK, not the average.** At x3.5 a momentary
peak lands at 1431 against the 1500 knee - still exactly linear, 4.2 V of
a possible 6. x4 was tried and puts that peak at 1636, over the knee and
into the landing curve, which is the v1.20.0 mistake in miniature: sizing
for the average and letting the peaks bend.

`chain_check.py` check 11 now asserts against 409, the measured peak,
rather than its own optimistic 528.

**The rule: a saturating curve must have a linear region big enough for
the signal that normally passes through it.** "Soft" is not automatically
gentler - a soft knee that starts at zero distorts everything, where a
hard knee placed above the music distorts nothing.

## The whine was our sampling BEATING against the input mux (v1.21.0)

Reported as *"one cycle every ~280 us (approx), independent of LFO speed
going in"*. That last clause is the whole diagnosis: a tone that does not
track anything patched in is not being modulated by an input, it is being
GENERATED by the firmware's own timing.

ComputerCard drives an external 4-way mux, one state per audio interrupt,
and updates `knobs[mux_state]` from a single shared ADC channel. So a knob
receives a genuinely new value **every 4 samples**, no matter what we do.

`kPanelDiv` was **3**: one knob per sample, round-robin. Three against four
beats. The AGE of the value being read walks 0,1,2,0,1,2 with a period of
lcm(3,4) = 12 samples, and 48000/12 is **4 kHz** - 250 us per cycle,
against a bench reading of ~280 us on scope graduations.

It was intermittent because the knob's own ~60 Hz filter has to have some
ripple left for the beat to turn it into a tone. A dead-still knob on a
quiet ADC is silent; anything keeping the value moving brings it back.

**No amount of downstream smoothing could ever have fixed this**, which is
why four rounds of filtering did not touch it. The tone is created at the
moment of sampling, so it is already inside the signal before any filter
sees it. That is the difference between this and the ADC-jitter bug, which
looked similar and was genuinely a smoothing problem.

`kPanelDiv` is now **4**, and all three knobs are read together on that
one phase. Every read lands at the same point in the mux cycle, so the age
is constant and there is no beat. It is also *cheaper*: three one-poles
every fourth sample is fewer operations than one one-pole every sample.

The `>>5` filter shift is deliberately unchanged. Loosening it to `>>3` to
"compensate" for the slightly lower 12 kHz per-knob rate was tried and put
the residual jitter back from 10 counts to 17 - trading this bug for the
previous one.

**The rule: any control read on a divider must be locked to whatever rate
the hardware actually updates it at.** `chain_check.py` check 12 verifies
all four combinations - panel and morph against the knob and CV muxes -
and demonstrates the old 3-against-4 beating at 4 kHz rather than merely
asserting the new one works.

## The card was ~10 dB too quiet, and the CV was half-wave rectified (v1.20.0)

Bench readings with a scope, which settled two things no simulation had
even been asked about:

> *"a bipolar sine LFO is a -ve voltage - basically half-wave rectified.
> The vowel position FADER 1 and FADER 8 is also adjusting DC offset.
> Maximum I can achieve is a waveform spanning +0.5 to -1.5 V, so very
> quiet."*

### The rectification: Clamp15 floors at 0

`Clamp15()` clamps to **0..32767**, and the formant CV was applied through
it:

```cpp
openness = Clamp15(openness + formant_cv_);
front    = Clamp15(front    - formant_cv_);
```

At mid-travel that is symmetric and fine, which is why it was never
noticed. But faders 1 and 8 set where the axes sit, and with a fader low
the negative half of a bipolar LFO is swallowed whole: the vowel stops
moving for half the cycle and the output jumps between "vowel present" and
"vowel pinned". On a scope that is a half-wave rectified waveform whose
offset moves with fader position - which is precisely what was reported,
including the detail that faders 1 and 8 moved the offset.

The CV is now **scaled into the headroom the faders leave**, so a full
swing always sweeps a full vowel range wherever the faders sit. The fader
chooses the centre; the CV keeps its whole travel around it.

### The quietness: staging sized for a case nobody plays

`return sum >> 2` was sized for the worst case - eight bands wide open with
a coherent input, 1568 counts - while a real vowel uses about three bands
and peaked at 528 of a possible 2047. That is **11.8 dB left on the table**,
and it is half of why the card was called noisy: the noise was not loud,
the voice was quiet, so the floor sat proportionally close to it.

Raising the shift alone clips the coherent case flat, which is why the old
comment forbade it. A cubic **soft knee** takes the headroom back instead:
linear through everything ordinary, rounding over on the rare peak that
would have clipped anyway, flat at the rail. A vowel goes 528 -> 1705
counts, **+10.2 dB**, and the worst case lands at 2047 exactly rather than
3920.

Note the effective small-signal gain is ~3.7x, not the nominal 2.5x: the
cubic's slope at the origin is 3/2 of nominal because the `*3/2` is what
puts unity at the knee. `chain_check.py` check 11 measures linearity
against that slope - comparing against the raw multiply reads the intended
gain as if it were distortion, which is a mistake I made writing the test.

**The lesson: headroom reserved for an unreachable worst case is not
safety, it is lost signal.** A soft limiter converts that reservation back
into level, and only the peaks that were going to clip anyway pay for it.

## The noise was a CONTROL CV read at AUDIO rate (v1.19.0)

The one that was actually reported, found after four fixes that were each
real bugs and none of them this one.

`vol` multiplies the output every sample:

```cpp
if (Connected(Input::Audio2)) vol += (int32_t)AudioIn2() << 4;
out = (int32_t)(((int64_t)out * vol) >> 15);
```

Audio In 2 is an **audio-rate** input. ComputerCard gives it a 12 kHz notch
for the mux interference and nothing else, because an audio input has to
pass audio. Its residual broadband ADC noise is a couple of LSB - nothing
as a signal. But volume is not a signal path, it is a **multiplier**, so
`<<4` turns two LSB into full-depth amplitude modulation of the entire
voice. Broadband, riding on the output, loudest when the card is loudest.

CV In 2 had the same shape at `<<3`, feeding all eight band gains at once.

**The discriminating observation was the user's, not a measurement of
mine:** *"it lessens whenever there is audio into Audio In 1"*. Extra
excitation making noise QUIETER means it is not being added downstream -
it is being masked. That rules out every additive source in one sentence
and points at a multiplier. Then *"it's there when I patch Audio 2 In,
where there is NO throbbing LED"* killed the supply-coupling theory and
named the input outright.

**Why four correct fixes missed it.** The zipper (v1.18.2), the stalling
slew (v1.18.2), the ADC jitter on the KNOBS (v1.18.3) and the local gain
mirror were all on the **gain-target path**. This is on the **output
multiplier**, downstream of all of them. Smoothing the targets could never
touch it, and every simulation I wrote modelled the target path, so every
simulation said the card was clean.

Both control CVs are now filtered with the accumulator held 4 bits above
Q15 - the same Q19 trick as the knobs, for the same reason: `(delta >> n)`
of a small delta is zero and the filter would stall exactly where the noise
lives. 64 counts of step becomes 1, and a real CV move still lands in
3.7 ms.

**Audio In 1 is deliberately NOT smoothed.** It is summed into the
excitation and genuinely carries audio; filtering it would be a bug in the
other direction. `cv_check.py` records that asymmetry so it is not
"tidied up" later.

**The rule: a control that MULTIPLIES needs smoothing that a control which
ADDS does not.** Noise on an addend is noise. Noise on a multiplier is
modulation of everything else.

## The noise was ADC JITTER, and the simulation could never see it (v1.18.3)

High-frequency noise when moving faders 1, 5 or 8, persisting after the
hand stopped and varying with where the fader was left.

**Three wrong theories first, all disproved by measurement:** USB bus
contention jittering sample timing (impossible - AudioOut1 writes a buffer
that ComputerCard clocks out by DMA at a fixed rate); volatile reads costing
RAM bandwidth (mirroring the gains locally moved the ISR's load/store count
from 176 to 175, so that change was reverted rather than shipped); and the
control-rate zipper (real, fixed in v1.18.2, but not the whole story).

**The simulation said moving and static were identical, -33.3 vs -33.4 dB.**
It was right, and it was measuring the wrong thing: every test fed the vowel
blend CLEAN values. A raw RP2040 ADC reading jitters a few LSB continuously,
and `KnobVal() << 3` turns each LSB into 8 counts of Q15. The gain smoother
settles in ~9 ms; a freshly jittered target arrives every 8 ms. So the gains
chase a target that never stops moving, with or without a hand on the knob.

That is both halves of the report at once: a buzz that outlives the gesture,
and a loudness that depends on knob position because ADC noise is not
uniform across the range.

**The filter state must be higher precision than its output.** Filtering in
Q15 barely helped - 24 counts down to 20 - because `(delta >> n)` of a small
delta is zero and the filter stalls exactly where the jitter lives. That is
the same trap as the gain slew, one level up. Keeping the accumulator at Q19
and shifting down on the way out gives 10 counts, settling a real move in
11 ms.

**What settled it was asking two discriminating questions** rather than
theorising further: does it happen with no 8mu (yes), and does CV In 2
provoke it (yes). Both answers moved the search out of USB entirely.

## Smooth at the AUDIO rate, not the control rate (v1.18.2)

A fader move produced high-frequency noise that could persist after the
hand stopped, clearing only when some other message arrived.

The band gains are recomputed at 125 Hz. Applying a new target directly
puts a step in a multiplier every 8 ms, and a step in a multiplier is a
corner in the output - broadband, and heard as a buzz at the update rate.

**My first fix slewed at the control rate and did nothing**, which is the
instructive part. Slewing at 125 Hz replaces one step every 8 ms with
several smaller steps every 8 ms: the amplitude falls but the RATE is
unchanged, and the rate is what is audible. Smoothing has to happen at the
rate the signal is sampled, not the rate the control changes.

**It also stalled.** `(target - cur) >> 2` on a small POSITIVE delta is
zero, so a rising gain stopped a few counts short and sat there while ADC
dither moved the target around it - a buzz that outlives the gesture and
clears only when something shifts the target far enough to matter. That was
the "keeps going until I move a fader again" half of the report, and an
arithmetic shift being asymmetric about zero is the whole cause.

Now a per-sample one-pole in `Voder::Process()` with an explicit
one-count-per-sample floor so it converges exactly in both directions.
Settles in 9.4 ms, largest single-sample step 0.27 dB, costs 1.4% of budget.

`chain_check.py` check 9 demonstrates the old form stalling at 16000
against a target of 16003 rather than merely asserting the new one works.

## A stale device address made the 8mu look alive but do nothing (v1.18.3)

Reported as the 8mu "seemingly working - lights lit, responding to wiggles -
just not making any changes to the sound". That is not a connection failure,
it is the card discarding valid messages.

`tuh_midi_rx_cb()` dropped anything whose address did not match `s_dev_addr`,
and `tuh_midi_mount_cb()` only accepted a device when `s_dev_addr` was zero.
So any divergence between the card's idea of the address and reality - a
spurious umount, a missed mount, a half-completed enumeration - was
PERMANENT: every later mount refused, every message dropped, and the device
itself still lit and sending.

Both now recover. The mount callback always takes the newest device, and the
RX callback adopts whatever address is genuinely sending. Neither can be
wrong: something is talking on a MIDI IN endpoint and this card listens to
exactly one device.

## The 8mu dropping out was a WEDGED ENDPOINT (v1.16.0)

Reported as "the 8mu carries on stopping, faders 1 and 8 stop doing
anything, then it feels like it power cycles and starts reworking".

That last clause is the diagnosis. A dropped message does not power cycle
anything - a re-enumeration does. So the fault was not lost data, it was
USB stopping entirely and the device timing out.

**`tuh_midi_rx_cb()` is invoked from inside `midih_xfer_cb()`, BEFORE that
function re-arms the IN endpoint** with `usbh_edpt_xfer()`. Everything done
in that callback is time the endpoint is not listening. The card was
parsing the entire MIDI stream there - up to 256 bytes, each through a
running-status state machine, inside the driver's transfer callback.

If that takes long enough for the next packet to arrive first, the transfer
errors, the driver's `TU_ASSERT` returns early, and **the endpoint is never
re-armed again**. The host stops polling. The device times out and resets.

Faders 1 and 8 appeared worst affected because they are the vowel axes and
get moved most, so their CCs were most often in flight when it happened.

**The callback now only memcpys into a 2048-byte ring** and returns; all
parsing happens in the core 1 loop after `tuh_task()` returns and the
endpoint is safely re-armed. Overflow drops bytes rather than blocking,
which is the right trade: a dropped CC is one stale fader until its next
message, where a blocked callback is a dead controller.

The driver's own RX FIFO also went from 64 bytes to 512. The default is one
bulk endpoint's worth, and `tu_fifo` drops silently when it overflows.

**Do not put work in a USB callback.** It is not an ordinary event handler;
it runs in the driver's transfer path with the endpoint disarmed.

## Previously: v1.6.0, LIFT gestures understood at last

## Previously: v1.5.0, bipolar tilt axes

## Previously: v1.4.1, accelerometer semantics fixed

## Previously: v1.4.0, the vowel cube

## Previously: v1.3.0, the partials left the interface

**Read the control-model section before changing any mapping.**

## Previously: v1.2.0, works on hardware, playability reworked

**Read the playability section before changing how the faders behave.**

## Previously: v1.1.0, WORKS ON HARDWARE

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
vowels.h      vowel gain vectors               (playable_check)
midi8mu.cpp   8mu CC/note dispatch             (midi_check, gesture_check)
usb_core1.cpp Core 1 USB pump + MIDI parser    (midi_check, checks 6-7)
main.cpp      panel, CV, LEDs, core scheduling (chain_check, cv_check,
                                                babble_check, autochat_check)
```

Each test names its file and each file names its test. **If you change a DSP
file, re-run the matching test — they take seconds.** They need `numpy` and
are run individually; there is no runner, which is deliberate — the output
of each is a table you read, not a green tick.

| Tool | Pins down |
|---|---|
| `filter_check.py` | Band geometry, Q15 quantisation error, IIR stability |
| `excite_check.py` | polyBLEP aliasing vs naive saw, noise whiteness |
| `chain_check.py` | Absolute levels in DAC counts, gain smoothing, output limiter, mux lock |
| `playable_check.py` | The vowel cube, panel reclaim, does the knob still work |
| `midi_check.py` | 8mu dispatch, running status, USB callback handoff |
| `gesture_check.py` | Accelerometer LIFT semantics — models the DEVICE, not us |
| `cv_check.py` | Do random voltages actually babble; control-CV smoothing |
| `babble_check.py` | BABBLE structure and boot-mode detection |
| `autochat_check.py` | Phrase structure — does it have lungs |
| `silence_check.py` | No single jack fault may mute the card |
| `init_check.py` | The filter bank is usable before `main()` runs |
| `budget_check.py` | Cycle estimate — **a prediction, not a measurement** |

**Several of these exist because a bug reached hardware that a proxy
measurement had declared healthy.** They test the property that would sound
wrong, not something correlated with it. `gesture_check.py` is the model
for that: it simulates what an 8mu actually puts on the wire rather than
transcribing what the firmware expects, because *a test that models the
component under test cannot find a bug in the model.*

`tools/budget_check.py` is a **model, not a measurement**, and it says so
loudly. WorkshopSpectral modelled 51% and measured 231% on real silicon.
**CV Out 2 is the authority on this card's cost**, and it has now been
read. Before the v1.23.0 rewrite: ~100% against a predicted 44.2%. After:
**60% typical, 70% peak.** The model still does not know what
`__aeabi_lmul` costs in context, and it was out by more than a factor of
two in the direction that matters; treat its number as a lower bound and
read the jack.

## They are LIFT gestures (v1.6.0)

The fact everything turns on, and it took six versions to establish:

> **Each gesture reads 0 when the device is LEVEL and rises as that side is
> lifted. A level 8mu sends 0 on all four. They are NOT a complementary
> pair adding to 127, and there is NO centre detent at 64.**

So a physical axis is the DIFFERENCE of its pair:

```
axis = lift_front - lift_back        -127 .. +127, zero when level
```

Both halves must be read here, because each carries real information -
unlike a complementary pair, where one half is redundant.

### Why the previous version felt broken

v1.5.0 assumed a detent at 64 and computed `full - (|cc42 - 64|)^2`:

| device | CC 42 | deviation | volume |
|---|---|---|---|
| level | 0 | 64 | **silent** |
| half lifted | 64 | 0 | full |
| fully lifted | 127 | 63 | silent |

Full volume happened only at one specific half-lifted angle, and the
natural resting position was silent. Reported as *"it feels like only a
very specific angle has volume"* - a better description of the fault than
anything the code comments claimed at the time.

### Volume is now a fader plus a bipolar lift

Fader 7 (CC 40) sets the base; the front/back lift swings a full scale
either side of it. A fader at half gives a full swell above and a full duck
below; at the top the card is loud until the back is lifted. Squared
response, so a quarter lift costs 0.6 dB - holding the controller roughly
level is fine, and the expressive travel is at a deliberate lift.

Rounding is the left/right lift, bipolar in the sense that either side
moves toward the rounded (OO) face.

**Mute is disabled** at the player's request; it was confusing the
diagnosis. The CC numbers and the hard-won polarity note survive in
`midi8mu.h` for whenever it comes back.

### Six versions, and the pattern in them

1. `32767 - lift_back + lift_front` - assumed lift_back rests at zero.
2. Running-minimum "rest" plus `abs()` - solved a non-problem, folded the axis.
3. Straight 0-127 levels - a level controller sat at half volume.
4. Mute polarity inverted - muted during normal use.
5. Centre detent at 64 - full volume only at a half-lifted angle.
6. This one: lift differences, which is what the device actually sends.

Five of those six produced a control that appeared **absent** rather than
**wrong**. Counting the silent filter bank and the dead knob, that is six
times on this card. **When a control seems to do nothing at all, suspect
the mapping before the wiring** - and when the mapping is a guess about
someone else's hardware, ask rather than infer. Every one of these was
settled in a sentence once the question was put directly.

## Previously: the tilt axes as a centre detent (v1.5.0, wrong)

The accelerometer pairs add to 127, so a level 8mu sits at **64** on each
axis. That makes 64 the natural neutral point, and both tilt controls are
now bipolar around it:

| Axis | CC | Centre (64) | Either extreme |
|---|---|---|---|
| Front/back | 42 | full volume | silence |
| Left/right | 44 | unrounded | toward OO |

**Direction does not matter, only distance.** That is what makes them
playable - nothing to remember about which way is which.

**The response is squared, to favour the neutral state.** For volume this
is the whole point: a quarter tilt is 0.6 dB down where a linear curve
would be 2.5 dB, so a controller held imperfectly level does not quietly
rob level, while a full tilt still reaches -30 dB. A cubed curve was tried
and clings to full volume too long to read as a fade at all.

### The five attempts this took

Worth listing, because every one failed by inventing semantics instead of
asking what the device sends:

1. `32767 - lift_back + lift_front` - assumed lift_back rests at zero.
2. Running-minimum "rest" plus `abs()` deviation - solved a non-problem,
   and folded the axis so half the travel mirrored the other.
3. Straight 0-127 levels - a level controller sat at half volume.
4. Mute read as high-means-inverted - muted during normal use.
5. This one: bipolar around 64, which is what the pairing implies.

Three of those produced a control that appeared **absent** rather than
**wrong**, which is the hardest kind to diagnose from the bench. Counting
the silent filter bank and the dead knob, that pattern has now hit this
card four times, and every time a clamp or a fold was hiding a bad
assumption underneath. **When a control seems to do nothing at all, suspect
the mapping before the wiring.**

## Previously: the accelerometer is not gesture magnitudes (v1.4.1)

Two hardware reports, one root cause: *"Tilt isn't doing volume now!? And
upside down should be MUTE."* Both were the same wrong assumption.

**Each gesture direction is its own controller reporting how much of that
gesture is happening.** "Lift front" (CC 42) and "lift back" (CC 43) are two
independent numbers, not two ends of one signed axis. And a controller lying
flat does not necessarily send 0 on either.

### Why volume died

```cpp
volume = 32767 - lift_back + lift_front;   // WRONG
```

This assumes `lift_back` rests at 0. If it rests anywhere above zero the
card is quiet or silent from boot - and because the value is already clamped
at the bottom, moving the controller appears to do **nothing at all**. That
is why it read as "tilt isn't doing volume" rather than "volume is too low".

**The fix assumes nothing.** Each axis tracks its own resting value as a
**running minimum** and responds to deviation from it. A gesture magnitude
is non-negative and falls back toward its floor when the controller is
level, so the minimum converges within a second of ordinary handling.

Taking the *first* value instead has a nasty failure mode: if the 8mu is
tilted when the first message arrives - or only transmits once movement
starts, so the first message is already mid-gesture - then "tilted" becomes
the zero point, level becomes a duck, and only a power cycle cures it. The
running minimum recovers on its own the first time the controller is put
down. `gesture_check.py` tests exactly that case.

### Why mute never worked

CC 49 "inverted" and CC 48 "not inverted" are a **gesture pair**. CC 49 does
not fall back to zero when the controller is turned right side up; it simply
stops being sent while CC 48 is sent instead. Reading CC 49 as a level meant
the mute engaged and then never released.

Now CC 49 mutes and CC 48 unmutes, and neither is treated as a level - any
value on CC 49 mutes, because it is a gesture rather than a threshold.

### Why no existing test caught either

`midi_check.py` transcribes the firmware's own dispatch, so it shared the
same assumption and agreed with it. **A test that models the component under
test cannot find a bug in the model.**

`tools/gesture_check.py` is new and models the DEVICE instead - what an 8mu
actually puts on the wire, including resting offsets and the gesture pair -
then checks the firmware copes. That is the shape any future
controller-semantics test should take.

## Previously: the vowel cube (v1.4.0)

Four changes, all from playing, and the first is the interesting one.

### Rounding is a real third axis, and it is what OH needed

v1.3.0 shipped with a 2-D vowel square and OH recorded as an accepted miss
at 6.0 dB. The user asked whether the other tilt directions could "put OH in
a third direction on the cube of vowel sounds" - which is exactly right, and
better than accepting the miss.

OH is unreachable in the square because it is **rounder** than anything on
the OO-AH edge: its F2 (840 Hz) sits BELOW both back corners, so it is not
between them in any direction the square can travel. Lip rounding is a
genuinely independent dimension of vowel space - protruding the lips
lengthens the tract and lowers every formant, hitting F2 hardest.

The cube is two planes of four corners: the existing vowels, and the same
four with F1 x0.88 and F2 x0.62. Those multipliers were fitted by minimising
the worst reachable error across all six named vowels.

**OH goes from 6.0 dB (unreachable) to 2.6 dB.** UH improves to 2.7 dB, and
AW and ER come along free.

### Faders 1 and 8, not 1 and 2

Reported directly: the outermost faders are the ergonomic pair, because the
hand spans the device instead of using one finger twice. The two vowel axes
get played constantly, so they get the outer pair. `midi_check.py` pins the
CC numbers so this cannot be quietly undone.

### Upside down is a hard mute

CC 49 is the 8mu inverted gesture. Turning the controller over is
unmistakable and nobody does it by accident, which is what a panic stop
should be - no aim required. It does not latch, and LED 5 goes dark so the
panel explains the silence. (Silence with no explanation is the failure mode
this card keeps rediscovering.)

Applied after the volume scale and before the clamp, so the CV outs stay
live while muted - useful if you are muting in order to look at something.

### Buttons A and D add breath

Both already did something (gate the buzz, latch freeze) and neither used
its held state for anything, so this is free expression. They ADD to the
fader rather than setting it, so it is a gesture on top of wherever breath
is parked: press one over a voiced sound and it goes breathy without losing
the pitch.

## Previously: the control model (v1.3.0) - the partials are gone

Second round of playing feedback, and it went deeper than the first:

> *"I would prefer the vowel sounds just on three sliders. It's too hard to
> try to manipulate the partials."*

That is not a request for a tweak, it is a verdict on the abstraction. Even
with the v1.2.0 fixes - faders bending rather than replacing, centre
detents, both controls live - **per-band control is the wrong interface for
playing**. A vowel is a position of the mouth, not eight independent
numbers, and asking anyone to operate seven faders as a chord is asking them
to solve the Voder's original problem: the one its operators trained for
months to overcome. Reproducing that is not fidelity, it is just a bad
instrument.

### What replaced it

**The vowel is now a 2-D space, and two faders place a point in it.**

```
             BACK  <----------> FRONT
  CLOSE       OO                 EE
    ^          |                  |
    |          |   UH lives in    |
 OPENNESS      |   here as a      |
    |          |   blend          |
    v          |                  |
   OPEN       AH                 EH
```

Openness is F1 (how far the jaw opens), Front is F2 (where the tongue
sits). The eight band gains are the bilinear blend of the four corner
vowels. The corners come out exact, UH lands inside within 1.7 dB, and
sweeping both faders together gives diphthongs.

**Faders 6-8 are deliberately unassigned.** That is the point of the
change, not an omission. Five controls that each mean something beat eight
that have to be operated as a chord.

### The full mapping

| Fader | Control | | Elsewhere | |
|---|---|---|---|---|
| 1 CC 34 | Openness | | Tilt 42/43 | Volume |
| 2 CC 35 | Front | | Note 36 | Voiced gate |
| 3 CC 36 | Breath | | Note 48 | Noise gate |
| 4 CC 37 | Pitch | | Note 60 | Plosive |
| 5 CC 38 | Brightness | | Note 72 | Freeze |

**Volume moved to the tilt** because it is the one control that wants to be
a gesture. Pitch and vowel get set and left; swelling a phrase is what
holding the controller is for. It rests at UNITY, not at half - splitting
the range around a centre was tried and wasted half of it, since a full back
tilt only reached half volume and the card could not be faded out at all.

**Pitch moved to a fader** because it is the opposite: something you set and
leave, which is exactly what a fader is good at and a tilt is bad at.

### OH is not reachable, and that is accepted

OH is rounder than anything on the OO-AH edge - its F2 (840 Hz) sits below
both back corners - so no bilinear blend of these four reaches it; the
closest is 6.0 dB away. Making OH a corner instead of AH trades that for a
5.1 dB miss on AH, which is a worse deal: AH is the more useful vowel and
the one a player reaches for first. `playable_check.py` records the
tolerance so anyone retuning the corners can see which trade they are making.

### A latent bug found while doing this

The constructor's initialiser list had been spliced into `ReadPanel()` by an
earlier edit, so `gate_seen_`, `ext_hold_` and the diagnostic flags were
being reset 125 times a second. That would have broken the gate latch and
the external-input hold from the silence fix. Removed with the rewrite.

## Previously: the playability rework (v1.2.0)

v1.1.0 was reported as *"not very playable: with faders moving, the main
knob doesn't do anything - and moving 8 faders is very very difficult."*
Both halves of that were my design error, and the second is the more
interesting one.

**The knob was dead** because `faders_touched` latched on first fader use
and the morph then stopped writing bands 1-7 - permanently, for the rest of
the session. I added that latch to stop the 125 Hz morph overwriting fader
moves, which was a real conflict, but the cure was worse than the disease.

**Eight simultaneous faders is the Voder's own problem.** It is the thing
its operators trained for months to overcome. Reproducing it on an
instrument that already puts a whole vowel under one knob is not fidelity to
the original, it is just a bad interface.

### The model now

The knob (or the 8mu's left/right tilt) chooses the vowel. Each fader
**bends one band** around a centre detent at CC 64. Both are live at all
times, a centred fader contributes exactly nothing, and one fader is a
meaningful gesture by itself.

**Boost and cut are deliberately asymmetric:**

- **Boost is additive** (`band_offset`, up to +20000). A quiet band needs a
  real number added to become a formant. Scaling it up would leave it
  proportionally quiet, and the fader would feel dead on exactly the bands a
  player most wants to bring forward.
- **Cut is proportional** (`band_cut`, floored at 3900/32768 = -18 dB).
  Subtracting a fixed amount drove any band below that amount to exactly
  zero - a hole no other control could reopen, which reads as a broken
  filter rather than as shading, and wasted the bottom quarter of the travel
  on the difference between silent and silent.

`tools/playable_check.py` pins all of this down, including the property that
actually broke: **the knob must still move the sound after any number of
faders have moved.** v1.1.0 scores 0.0 dB on that check.

### The vowel table dropped to peak 16000

At 26800 there was only 1.7 dB of headroom above the loudest band, so
boosting a prominent formant did almost nothing - the fader hit the ceiling
exactly where a player would reach for it. At 16000 there is ~6 dB of boost
even on the loudest band. The output shift went `>>4` to `>>2` to give the
level back, which `chain_check.py` measures: a vowel now peaks near 530 DAC
counts against 132 before, with the worst case at 1568 of 2047.

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

Confirmed on hardware over the bench sessions: the card enumerates an 8mu
and CC 34-41 move the right controls; the vowel cube and the square under
it are playable; volume behaves; the output is clean at the current gain.

Genuinely open:

- [x] ~~Read CV Out 2.~~ **Done.** Before the 32-bit rewrite it read
      ~5 V - the clamp, 100% of budget - against a predicted 44.2%.
      After: **3.0 V typical, 3.5 V peak = 60% and 70%**, or 12.5 and
      14.6 us against the 20.83 us deadline. About 6 us spare at the
      worst. `budget_check.py`'s number remains a lower bound and should
      be read as one.
- [ ] Confirm the USB lockup is gone. It took sustained fader plus
      accelerometer traffic to provoke, so it needs deliberate
      reproduction rather than absence of reports. Three separate causes
      have been fixed (unbounded drain, wedged endpoint, stale address)
      and any of them could have been the one being hit.
- [ ] Listen for whether Q=4 is right. It is a guess balancing band
      separation against ringing on plosives, and only ears settle it.
- [ ] Consider whether a continuous F1/F2 model would beat the table.

If the card is ever silent again, hold the switch down and read LEDs 0-3
(see the diagnostic table above) - that identifies which of the four
conditions is at fault without another round trip. If LED 1 is lit with
nothing patched, `kExtGateLevel` is below this unit's ADC noise floor. If
LED 0 is lit, noise on Pulse In 2 is tripping the gate latch and
`gate_seen_` needs a threshold.

## What the bench taught, in one line each

Every one of these cost a flash cycle, and each is a general lesson rather
than a fact about this card:

1. **A DSP test suite can be entirely green while the signal never
   arrives.** Five suites passed while the card was mute.
2. **Static initialisation order is not something to be careful about, it
   is something to design out.**
3. **Do not put work in a USB callback.** It runs with the endpoint
   disarmed.
4. **When a control seems to do nothing at all, suspect the mapping before
   the wiring** - and when the mapping is a guess about someone else's
   hardware, ask rather than infer. Six versions of the accelerometer, all
   settled by one sentence from the player.
5. **Smooth at the rate the signal is sampled, not the rate the control
   changes.**
6. **A filter's state must be higher precision than its output**, or
   `(delta >> n)` of a small delta is zero and it stalls exactly where the
   noise lives.
7. **A control that MULTIPLIES needs smoothing that a control which ADDS
   does not.**
8. **Any control read on a divider must be locked to the rate the hardware
   actually updates it at**, or the two beat.
9. **A saturating curve must have a linear region big enough for the
   signal that normally passes through it.** A cubic has none.
10. **Size an output stage from the measured PEAK, not from a model's
    average.** The model was optimistic by 2x.
11. **The discriminating question beats another theory.** "Does it happen
    with no 8mu?" and "does audio into Audio In 1 change it?" each
    eliminated more than any simulation written that day.
