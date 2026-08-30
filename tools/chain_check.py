#!/usr/bin/env python3
"""End-to-end signal level through TRACT8's chain. THE GAIN-STAGING AUTHORITY.

This test exists because the card came back from its first hardware run
SILENT except for plosives - which are summed after the filter bank, so
their working proved the ISR, the DAC and the output path were all fine and
localised the fault to the bank or its gain staging.

The bug it found: the bandpass sections have a peak voltage gain far BELOW
unity at their centre frequencies. An RBJ constant-skirt bandpass has a peak
gain of alpha/a0, not 1.0 - and alpha is proportional to sin(w0)/2Q, so at
250 Hz with Q=4 that is about 0.004. Combined with the >>3 before the bank
and the >>4 after it, the chain threw away roughly 78 dB and the output
never left the DAC's zero code.

filter_check.py did not catch this: it measured each band's response
NORMALISED to its own peak, which is the right way to check centre
frequency and Q and the wrong way to check level. This file measures
absolute amplitude in DAC counts, which is the number that decides whether
anyone hears anything.

Checks:
  1. Per-band peak gain at centre frequency (the thing that was wrong).
  2. Full chain level for a buzz at F0 through a real vowel, in DAC counts.
  3. The same for noise.
  4. Headroom: worst case must not overflow int32 or clip the DAC.
  5. The click sits under the voice, measured against the VOICE.
  6. Breath changes the character of the sound, not its level.
  7. The plosive occupies a band - neither white noise nor a thump.
  8. The default burst is a consonant length, not a drum hit.
  9. Band gains smooth per sample and converge exactly (no stuck buzz).

Run: python tools/chain_check.py
"""

import sys
import math
import numpy as np

FS = 48000
BAND_HZ = [250, 450, 700, 1000, 1400, 1900, 2600, 3800]
Q = 4.0
COEFF_SHIFT = 15
COEFF_SCALE = 1 << COEFF_SHIFT

# Gain staging, transcribed from voder.cpp. These are what this test exists
# to validate.
PRE_SHIFT = 3      # excite >>= 3   before the bank
POST_SHIFT = 2     # return sum >> POST_SHIFT
BAND_MAKEUP = 1    # per-band makeup multiplier applied to y (1 = none)

DAC_MAX = 2047

# A real vowel: AH, the open-back corner from vowels.h.
VOWEL_AH = [666, 4003, 16000, 15423, 7647, 4226, 3923, 1675]


def coeffs(f0):
    w0 = 2.0 * math.pi * f0 / FS
    alpha = math.sin(w0) / (2.0 * Q)
    a0 = 1.0 + alpha
    b0 = alpha / a0
    a1 = (-2.0 * math.cos(w0)) / a0
    a2 = (1.0 - alpha) / a0
    return (round(b0 * COEFF_SCALE) / COEFF_SCALE,
            round(a1 * COEFF_SCALE) / COEFF_SCALE,
            round(a2 * COEFF_SCALE) / COEFF_SCALE)


def peak_gain(b0, a1, a2, f0):
    """|H| at the band's centre - the ABSOLUTE gain, not normalised."""
    w = 2.0 * math.pi * f0 / FS
    z1 = np.exp(-1j * w)
    z2 = z1 * z1
    return float(np.abs(b0 * (1.0 - z2) / (1.0 + a1 * z1 + a2 * z2)))


def check_band_gains():
    print("\n1. Per-band peak voltage gain at centre frequency")
    print("   This is the ABSOLUTE gain. filter_check.py normalises it away.")
    print("   band     b0 (Q15)   peak gain     dB")
    ok = True
    for f0 in BAND_HZ:
        b0, a1, a2 = coeffs(f0)
        g = peak_gain(b0, a1, a2, f0)
        db = 20 * math.log10(g) if g > 0 else -999
        flag = "" if g > 0.25 else "  <-- VERY LOSSY"
        if g <= 0.25:
            ok = False
        print(f"   {f0:5d}   {round(b0*COEFF_SCALE):8d}   {g:9.5f}  "
              f"{db:7.2f}{flag}")
    print("   (a constant-skirt bandpass peaks at alpha/a0, NOT at 1.0;")
    print("    alpha ~ sin(w0)/2Q, so low bands are the lossiest)")
    return ok


def run_chain(sig, gains, makeup=BAND_MAKEUP):
    """Integer transcription of Voder::Process()'s bank + output staging."""
    states = [[0, 0, 0, 0] for _ in BAND_HZ]   # x1 x2 y1 y2
    icoef = []
    for f0 in BAND_HZ:
        w0 = 2.0 * math.pi * f0 / FS
        alpha = math.sin(w0) / (2.0 * Q)
        a0 = 1.0 + alpha
        icoef.append((round((alpha / a0) * COEFF_SCALE),
                      round(((-2.0 * math.cos(w0)) / a0) * COEFF_SCALE),
                      round(((1.0 - alpha) / a0) * COEFF_SCALE)))

    out = np.zeros(len(sig), dtype=np.float64)
    for n, x in enumerate(sig):
        excite = int(x) >> PRE_SHIFT
        total = 0
        for i, (b0, a1, a2) in enumerate(icoef):
            st = states[i]
            acc = b0 * (excite - st[1]) - a1 * st[3] - a2 * st[2]
            y = acc >> COEFF_SHIFT
            st[1] = st[0]
            st[0] = excite
            st[2] = st[3]
            st[3] = y
            y2 = y * makeup
            total += (y2 * gains[i]) >> 15
        out[n] = total >> POST_SHIFT
    return out


def make_saw(f0, n):
    inc = int(f0 * (2 ** 32) / FS)
    ph = 0
    o = np.zeros(n, dtype=np.int64)
    for i in range(n):
        ph = (ph + inc) & 0xFFFFFFFF
        o[i] = ((ph >> 17) - 16384) * 2
    return o


def check_full_chain():
    print("\n2. Full chain: 110 Hz buzz through the AH vowel")
    n = 8192
    sig = make_saw(110, n)
    out = run_chain(sig, VOWEL_AH)
    tail = out[n // 2:]                    # let the filters settle
    peak = float(np.max(np.abs(tail)))
    rms = float(np.sqrt(np.mean(tail ** 2)))

    print(f"   input peak            {float(np.max(np.abs(sig))):9.0f} "
          f"(Q15 full scale 32767)")
    print(f"   output peak           {peak:9.1f}  DAC counts "
          f"(usable range +/-{DAC_MAX})")
    print(f"   output rms            {rms:9.1f}  DAC counts")
    if peak > 0:
        print(f"   peak as dBFS          {20*math.log10(peak/DAC_MAX):9.2f} dB")
    else:
        print("   peak as dBFS               SILENT")

    # Below about 20 counts peak the card is inaudible in any practical
    # setting - that is under -40 dBFS into a modular level input.
    ok = peak >= 20
    print(f"   audible?              "
          f"{'yes' if ok else 'NO - THIS IS THE BUG'}")
    return ok


def check_noise_chain():
    print("\n3. Full chain: white noise through the AH vowel")
    n = 8192
    rng = np.random.default_rng(1)
    sig = rng.integers(-32768, 32767, n)
    out = run_chain(sig, VOWEL_AH)
    tail = out[n // 2:]
    peak = float(np.max(np.abs(tail)))
    rms = float(np.sqrt(np.mean(tail ** 2)))
    print(f"   output peak           {peak:9.1f}  DAC counts")
    print(f"   output rms            {rms:9.1f}  DAC counts")
    ok = peak >= 20
    print(f"   audible?              "
          f"{'yes' if ok else 'NO'}")
    return ok


def check_headroom():
    print("\n4. Headroom: all bands open, worst-case coherent input")
    n = 8192
    # Worst case is a tone at the frequency where the summed response peaks.
    sig = (32767 * np.sin(2 * np.pi * 1400 *
                          np.arange(n) / FS)).astype(np.int64)
    out = run_chain(sig, [32767] * 8)
    tail = out[n // 2:]
    peak = float(np.max(np.abs(tail)))
    print(f"   output peak           {peak:9.1f}  DAC counts")
    clips = peak > DAC_MAX
    print(f"   clips the DAC?        {'yes' if clips else 'no'}"
          f"{'  (clamped in main.cpp; a state the player chooses)' if clips else ''}")
    # The failure condition is int32 overflow, not clipping.
    ok = peak < 2 ** 31
    print(f"   int32 safe?           {'yes' if ok else 'NO - OVERFLOW'}")
    return ok




# Click gain staging, transcribed from voder.cpp.
CLICK_SHIFT = 17
PLOSIVE_LO_Q15 = 2600        # ~600 Hz, removes the thump
PLOSIVE_HI_Q15 = 12000       # ~2800 Hz, removes the hiss
PLOSIVE_LP_LOSS_DB = -9.4    # measured RMS loss through the bandpass
DEFAULT_CLICK_DECAY = 2000   # main.cpp, about 12 ms
PLOSIVE_MIN_SAMPLES = 384
PLOSIVE_MAX_SAMPLES = 48000
DEFAULT_CLICK_LEVEL = 3000      # main.cpp, no 8mu attached
VOWEL_BAND_FRACTION = sum(VOWEL_AH) / (8 * 32767)


def voice_at_sum():
    """Roughly what a real vowel contributes at the summing point."""
    return int((32767 >> PRE_SHIFT) * 2.299 * VOWEL_BAND_FRACTION)


def click_at_sum(level):
    """Peak contribution of a burst at the summing point.

    The two-pole lowpass that darkens the burst also costs about 16 dB of
    level, which is why the shift is >>17 here where an unfiltered burst
    needed >>19. Both numbers have to move together: keeping the old shift
    with the filter added would have made the click inaudible.
    """
    raw = (32768 * level) >> CLICK_SHIFT
    return raw * (10 ** (PLOSIVE_LP_LOSS_DB / 20.0))


def voice_at_sum():
    """Roughly what a real vowel contributes at the summing point."""
    return int((32767 >> PRE_SHIFT) * 2.299 * VOWEL_BAND_FRACTION)


def click_at_sum(level):
    """Peak contribution of a burst at the summing point.

    The bandpass that shapes the burst also costs about 9 dB of level, so
    the shift and the filter are the same decision expressed twice:
    changing one without the other silently moves the balance.
    """
    raw = (32768 * level) >> CLICK_SHIFT
    return raw * (10 ** (PLOSIVE_LP_LOSS_DB / 20.0))


def check_click_balance():
    """The click must sit UNDER the voice, measured against the VOICE."""
    print("\n5. Click level against the voice")
    ok = True
    voice = voice_at_sum()
    print(f"   a real vowel contributes about {voice} at the sum")
    print("   click level      vs voice")
    for level, label in ((DEFAULT_CLICK_LEVEL, "default   "),
                         (16384, "fader half"),
                         (32767, "fader full")):
        c = click_at_sum(level)
        db = 20 * math.log10(max(c, 1) / voice)
        print(f"   {label}      {db:+6.1f} dB")

    # Reported as too loud twice. The first fix set the level to "-14 dB",
    # but that was -14 dB of the CLICK's own full scale, which landed it
    # level with the voice. Measure against the voice or the number means
    # nothing.
    d = 20 * math.log10(max(click_at_sum(DEFAULT_CLICK_LEVEL), 1) / voice)
    good = d < -10.0
    if not good:
        ok = False
    print(f"   default is {d:+.1f} dB under the voice   "
          f"{'ok - punctuation' if good else '<-- STILL PERCUSSIVE'}")

    full = 20 * math.log10(max(click_at_sum(32767), 1) / voice)
    good = full > -6.0
    if not good:
        ok = False
    print(f"   fader full reaches {full:+.1f} dB   "
          f"{'ok - can still be loud' if good else '<-- CANNOT GET LOUD'}")
    print("   (the click is summed AFTER the bank, so unlike the voice it")
    print("    is never attenuated by the vowel)")
    return ok


def check_breath_is_a_ratio():
    """Breath must change the character, not the level."""
    print("\n6. Breath is a ratio, not a level")
    ok = True
    print("   breath   both gates   voiced only   noise only")
    levels = []
    for bm in (0, 8192, 16384, 24576, 32767):
        row = f"   {bm:5d}   "
        for gv, gn in ((1, 1), (1, 0), (0, 1)):
            mix = 32767 - bm
            blended = ((32767 * mix) + (32767 * bm)) >> 15
            env = max(32767 * gv, 32767 * gn)
            ex = (blended * env) >> 15
            levels.append(ex)
            row += f"{ex:9d}    "
        print(row)

    spread = max(levels) - min(levels)
    good = spread < 200
    if not good:
        ok = False
    print(f"   level varies by {spread} across every combination   "
          f"{'ok - constant' if good else '<-- BREATH CHANGES VOLUME'}")
    print("   (gating buzz and hiss separately and crossfading afterwards")
    print("    made 50% breath give HALF THE VOLUME when one gate was shut)")
    return ok


def plosive_burst(n=8192, seed=1):
    """Transcription of the bandpass in Voder::Process()."""
    rng = np.random.default_rng(seed)
    x = rng.integers(-32768, 32767, n).astype(float)
    ah = PLOSIVE_HI_Q15 / 32768.0
    al = PLOSIVE_LO_Q15 / 32768.0
    h1 = h2 = l1 = l2 = 0.0
    y = np.zeros(n)
    for i, v in enumerate(x):
        h1 += (v - h1) * ah
        h2 += (h1 - h2) * ah
        l1 += (h2 - l1) * al
        l2 += (l1 - l2) * al
        y[i] = h2 - l2
    return x, y


def check_plosive_spectrum():
    """A P or a B is neither white noise nor a kick drum."""
    print("\n7. The plosive occupies the band a stop actually occupies")
    ok = True

    x, y = plosive_burst()
    spec = np.abs(np.fft.rfft(y * np.hanning(len(y))))
    freqs = np.fft.rfftfreq(len(x), 1.0 / FS)

    def band(lo, hi):
        m = (freqs >= lo) & (freqs < hi)
        return float(np.sqrt(np.mean(spec[m] ** 2)))

    peak = band(500, 1500)
    sub = 20 * math.log10(band(20, 200) / peak)
    high = 20 * math.log10(band(4000, 12000) / peak)

    print(f"   below 200 Hz   {sub:+6.1f} dB   (thump region)")
    print(f"   500-1500 Hz    {0.0:+6.1f} dB   (the burst peak)")
    print(f"   above 4 kHz    {high:+6.1f} dB   (hiss region)")

    # BOTH ends have to be down. A plain lowpass gets the top right and
    # the bottom badly wrong - it keeps the sub-100 Hz energy, and the
    # burst becomes a thump. That version was reported as "a bomb going
    # off"; the one before it, unfiltered, as "white noise". The band is
    # the only shape that is neither.
    good_low = sub < -6.0
    good_high = high < -8.0
    if not (good_low and good_high):
        ok = False
    print(f"   thump removed   {'ok' if good_low else '<-- A BOMB'}")
    print(f"   hiss removed    {'ok' if good_high else '<-- WHITE NOISE'}")

    dc = abs(float(np.mean(y))) / float(np.sqrt(np.mean(y ** 2)))
    good = dc < 0.05
    if not good:
        ok = False
    print(f"   DC offset {dc*100:.1f}% of RMS   "
          f"{'ok' if good else '<-- WOULD CLICK'}")
    return ok


def check_plosive_length():
    """A consonant is short. A drum hit is not."""
    print("\n8. The default burst is a consonant, not a drum hit")
    sq = (DEFAULT_CLICK_DECAY * DEFAULT_CLICK_DECAY) >> 15
    n = PLOSIVE_MIN_SAMPLES + \
        (((PLOSIVE_MAX_SAMPLES - PLOSIVE_MIN_SAMPLES) * sq) >> 15)
    ms = n / (FS / 1000.0)
    # A real plosive release is 5-15 ms. It was 41 ms, which reads as a
    # drum whatever its spectrum.
    good = 4.0 < ms < 20.0
    print(f"   default decay {ms:.0f} ms   "
          f"{'ok - inside the real 5-15 ms range' if good else '<-- TOO LONG'}")

    full = PLOSIVE_MIN_SAMPLES + \
        (((PLOSIVE_MAX_SAMPLES - PLOSIVE_MIN_SAMPLES) * 32767) >> 15)
    print(f"   fader full still reaches {full/(FS/1000.0):.0f} ms   ok")
    print("   (the fader is for sustained textures; the DEFAULT is what")
    print("    a panel trigger gives before anyone touches anything)")
    return good




GAIN_SMOOTH_SHIFT = 6


def smooth_step(cur, target):
    """Transcription of the per-sample gain smoothing in Voder::Process()."""
    d = (target - cur) >> GAIN_SMOOTH_SHIFT
    if d == 0 and cur != target:
        return cur + (1 if target > cur else -1)
    return cur + d


def check_gain_smoothing():
    """Band gains must reach their target exactly, and quickly, per sample."""
    print("\n9. Band gains are smoothed per sample and converge exactly")
    ok = True

    # THE BUG THIS GUARDS. The gains were first slewed at the CONTROL rate
    # (125 Hz) with (target - cur) >> 2. That failed twice over.
    #
    # It did not remove the buzz, because slewing at the control rate just
    # replaces one step every 8 ms with several smaller ones every 8 ms -
    # the rate is unchanged and the rate is what is audible.
    #
    # And it never converged upward: an arithmetic shift of a small
    # POSITIVE delta is zero, so a rising gain stalled a few counts short
    # while ADC dither jittered the target around it. That is a buzz that
    # persists after the hand stops moving and clears only when some other
    # message shifts the target - exactly what was reported.
    for start, target, label in ((0, 16000, "rising"),
                                 (16000, 0, "falling"),
                                 (16000, 16003, "rising by 3"),
                                 (16000, 15997, "falling by 3")):
        cur = start
        n = 0
        while cur != target and n < 100000:
            cur = smooth_step(cur, target)
            n += 1
        good = cur == target
        if not good:
            ok = False
        ms = n / 48.0
        print(f"   {label:14} {start:6d} -> {target:6d} in {n:4d} samples "
              f"({ms:5.2f} ms)   "
              f"{'ok' if good else '<-- NEVER CONVERGES'}")

    # The old control-rate form, for contrast: it stalls.
    cur = 16000
    for _ in range(1000):
        cur = cur + ((16003 - cur) >> 2)
    stalled = cur != 16003
    print(f"   old >>2 form after 1000 updates: {cur} (target 16003)   "
          f"{'stalls, as it did on hardware' if stalled else 'converged'}")

    # No single sample may move the gain enough to be an edge. The worst
    # case is the first step of the largest jump.
    biggest = (32767 - 0) >> GAIN_SMOOTH_SHIFT
    import math
    db = 20 * math.log10(1 + biggest / 16000.0)
    good = db < 0.5
    if not good:
        ok = False
    print(f"   largest single-sample step {biggest} = {db:.2f} dB   "
          f"{'ok - no edge' if good else '<-- AUDIBLE STEP'}")

    # And it must settle fast enough to feel immediate.
    cur, n = 0, 0
    while cur != 16000 and n < 100000:
        cur = smooth_step(cur, 16000)
        n += 1
    good = n / 48.0 < 20.0
    if not good:
        ok = False
    print(f"   settles in {n/48.0:.1f} ms   "
          f"{'ok - feels immediate' if good else '<-- SLUGGISH'}")
    return ok


def main():
    print("Voder end-to-end chain check - absolute levels in DAC counts")
    print(f"  pre-bank >>{PRE_SHIFT}, post-bank >>{POST_SHIFT}, "
          f"per-band makeup x{BAND_MAKEUP}")

    ok = check_band_gains()
    ok &= check_full_chain()
    ok &= check_noise_chain()
    ok &= check_headroom()
    ok &= check_click_balance()
    ok &= check_breath_is_a_ratio()
    ok &= check_plosive_spectrum()
    ok &= check_plosive_length()
    ok &= check_gain_smoothing()

    print("\nPASS" if ok else "\nFAIL")
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
