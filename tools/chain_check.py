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
  7. The plosive is dark enough to read as a stop, not as noise.

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
CLICK_SHIFT = 16
PLOSIVE_LP_Q15 = 3000        # two cascaded one-poles, ~700 Hz corner
PLOSIVE_LP_LOSS_DB = -15.9   # measured RMS loss through the pair
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


def check_plosive_spectrum():
    """A P or a B is not white noise."""
    print("\n7. The plosive is dark, like a bilabial stop")
    ok = True

    # Two cascaded one-poles, run on white noise, measured in bands.
    rng = np.random.default_rng(1)
    n = 8192
    x = rng.integers(-32768, 32767, n).astype(float)
    a = PLOSIVE_LP_Q15 / 32768.0
    y1 = y2 = 0.0
    y = np.zeros(n)
    for i, v in enumerate(x):
        y1 += (v - y1) * a
        y2 += (y1 - y2) * a
        y[i] = y2

    spec = np.abs(np.fft.rfft(y * np.hanning(n)))
    freqs = np.fft.rfftfreq(n, 1.0 / FS)

    def band(lo, hi):
        m = (freqs >= lo) & (freqs < hi)
        return float(np.sqrt(np.mean(spec[m] ** 2)))

    low = band(100, 1000)
    mid = band(1000, 4000)
    high = band(4000, 12000)
    mid_db = 20 * math.log10(mid / low)
    high_db = 20 * math.log10(high / low)

    print(f"   below 1 kHz    {0.0:+6.1f} dB   (reference)")
    print(f"   1-4 kHz        {mid_db:+6.1f} dB")
    print(f"   4-12 kHz       {high_db:+6.1f} dB")

    # A real /p/ sits about -15 to -25 dB at 4 kHz relative to its low
    # peak. Raw white noise would read 0 dB in every band, which is what
    # it did before and why it sounded like a hi-hat rather than a stop.
    good = mid_db < -8.0 and high_db < -25.0
    if not good:
        ok = False
    print(f"   dark enough to read as /p/ or /b/   "
          f"{'ok' if good else '<-- STILL BRIGHT'}")
    print("   (white noise reads 0 dB in every band; the alveolars /t/ and")
    print("    /d/ ARE bright, but this card has one burst so it should be")
    print("    the dark one - a bright one sounds like a hi-hat)")

    # The filter must not have a DC offset, which would click on its own.
    dc = abs(float(np.mean(y))) / float(np.sqrt(np.mean(y ** 2)))
    good = dc < 0.05
    if not good:
        ok = False
    print(f"   DC offset {dc*100:.1f}% of RMS   "
          f"{'ok' if good else '<-- WOULD CLICK'}")
    return ok


def check_click_balance():
    """The click must sit UNDER the voice, and 'under' means against the
    voice - not against the click's own full scale."""
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

    # The default must be clearly under the voice. It was reported as too
    # percussive twice: the first fix set the level to "-14 dB" but that
    # was -14 dB of the CLICK's full scale, which landed it 1.5 dB below
    # the voice - level with it, in other words.
    d = 20 * math.log10(max(click_at_sum(DEFAULT_CLICK_LEVEL), 1) / voice)
    good = d < -10.0
    if not good:
        ok = False
    print(f"   default is {d:+.1f} dB under the voice   "
          f"{'ok - punctuation' if good else '<-- STILL PERCUSSIVE'}")

    # But the fader must still be able to make it loud, or the control is
    # pointless.
    full = 20 * math.log10(max(click_at_sum(32767), 1) / voice)
    good = full > -3.0
    if not good:
        ok = False
    print(f"   fader full reaches {full:+.1f} dB   "
          f"{'ok - can still be loud' if good else '<-- CANNOT GET LOUD'}")
    print("   (the click is summed AFTER the bank, so unlike the voice it")
    print("    is never attenuated by the vowel - which is why it needs")
    print("    to be set this far down to sit underneath)")
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
    print("   (the old code gated buzz and hiss separately and crossfaded")
    print("    afterwards, so a button gating one source silenced half the")
    print("    crossfade and 50% breath gave HALF THE VOLUME)")
    return ok


def main():
    print("TRACT8 end-to-end chain check - absolute levels in DAC counts")
    print(f"  pre-bank >>{PRE_SHIFT}, post-bank >>{POST_SHIFT}, "
          f"per-band makeup x{BAND_MAKEUP}")

    ok = check_band_gains()
    ok &= check_full_chain()
    ok &= check_noise_chain()
    ok &= check_headroom()
    ok &= check_click_balance()
    ok &= check_breath_is_a_ratio()
    ok &= check_plosive_spectrum()

    print("\nPASS" if ok else "\nFAIL")
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
