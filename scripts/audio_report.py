"""Report on a recording of the game's audio output (RECOMP_AUDIO_WAV).

    python scripts/audio_report.py <file.wav> [--from SECONDS] [--to SECONDS]

Says whether the sound is really stereo or two copies of one channel, and
finds what is heard as defects:

- gaps: runs of digital silence (both channels exactly 0) from 5 ms to half
  a second inside sound, which the device plays as dropouts;
- clicks: a sample-to-sample jump far larger than the signal around it;
- clipping: samples at full scale.

The recording holds what the emulated APU produced, before the host device.
Underruns of the device itself are counted live by [AUDIO] lines instead.
Standard library only.
"""

import argparse
import array
import math
import sys
import wave

GAP_MS = 5.0
CLICK_RATIO = 8.0          # jump against the mean jump of the surrounding 10 ms
CLICK_MIN = 6000           # and at least this large, in 16-bit units


def read(path):
    with wave.open(path, "rb") as w:
        if w.getsampwidth() != 2:
            sys.exit(f"{path}: {8 * w.getsampwidth()}-bit samples; expected 16-bit")
        rate, channels, frames = w.getframerate(), w.getnchannels(), w.getnframes()
        data = array.array("h")
        data.frombytes(w.readframes(frames))
    if sys.byteorder == "big":
        data.byteswap()
    return rate, channels, data


def stereo_verdict(left, right):
    """Correlation of the channels and the energy of their difference."""
    n = len(left)
    if not n:
        return None
    sl = sum(left) / n
    sr = sum(right) / n
    cov = vl = vr = side = mid = 0.0
    for a, b in zip(left, right):
        da, db = a - sl, b - sr
        cov += da * db
        vl += da * da
        vr += db * db
        side += (a - b) * (a - b)
        mid += (a + b) * (a + b)
    corr = cov / math.sqrt(vl * vr) if vl > 0 and vr > 0 else 1.0
    side_db = 10 * math.log10(side / mid) if side > 0 and mid > 0 else -math.inf
    return corr, side_db, math.sqrt(vl / n), math.sqrt(vr / n)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("wav")
    ap.add_argument("--from", dest="start", type=float, default=0.0)
    ap.add_argument("--to", dest="end", type=float, default=None)
    args = ap.parse_args()

    rate, channels, data = read(args.wav)
    total = len(data) // channels
    first = int(args.start * rate)
    last = min(total, int(args.end * rate)) if args.end else total
    print(f"{args.wav}: {rate} Hz, {channels} channel(s), {total / rate:.1f} s; analysing {first / rate:.1f}..{last / rate:.1f} s")
    if last <= first:
        return

    if channels >= 2:
        left = data[first * channels:last * channels:channels]
        right = data[first * channels + 1:last * channels:channels]
    else:
        left = right = data[first:last]

    # Stereo, per second, then overall on the seconds with sound.
    stereo_s = mono_s = quiet_s = 0
    worst = []
    for s in range(0, len(left) - rate + 1, rate):
        v = stereo_verdict(left[s:s + rate], right[s:s + rate])
        if v is None:
            continue
        corr, side_db, rms_l, rms_r = v
        if max(rms_l, rms_r) < 30:          # under about -60 dBFS: silence
            quiet_s += 1
        elif side_db < -40 or corr > 0.999:
            mono_s += 1
        else:
            stereo_s += 1
            worst.append((corr, s / rate + args.start))
    heard = stereo_s + mono_s
    if channels < 2:
        print("channels: the file is mono")
    elif not heard:
        print("channels: no sound to judge")
    else:
        kind = "stereo" if stereo_s > mono_s else "mono in both channels"
        print(f"channels: {kind} ({stereo_s} s with distinct channels, {mono_s} s identical, {quiet_s} s silent)")
        if worst:
            worst.sort()
            print("  widest seconds (L/R correlation): "
                  + ", ".join(f"{t:.0f} s ({c:.2f})" for c, t in worst[:5]))

    # Gaps: digital silence inside sound.
    gap_min = int(rate * GAP_MS / 1000)
    gaps = []
    run = 0
    loud_before = False
    for i in range(len(left)):
        if left[i] == 0 and right[i] == 0:
            run += 1
            continue
        # Longer silences are the game's own pauses, not dropouts.
        if gap_min <= run < rate // 2 and loud_before:
            gaps.append(((first + i - run) / rate, run * 1000.0 / rate))
        run = 0
        loud_before = abs(left[i]) > 64 or abs(right[i]) > 64 or loud_before
    print(f"gaps of {GAP_MS:.0f} ms or more inside sound: {len(gaps)}"
          + (f", {sum(g[1] for g in gaps):.0f} ms in all" if gaps else ""))
    for t, ms in gaps[:10]:
        print(f"  at {t:.3f} s: {ms:.1f} ms")

    # Clicks: a jump much larger than the neighbourhood's.
    win = max(1, rate // 100)
    clicks = []
    for ch, name in ((left, "L"), (right, "R")) if channels >= 2 else ((left, "mono"),):
        diffs = [abs(ch[i + 1] - ch[i]) for i in range(len(ch) - 1)]
        acc = sum(diffs[:win])
        for i in range(win, len(diffs)):
            mean = acc / win
            d = diffs[i]
            if d >= CLICK_MIN and d > CLICK_RATIO * max(mean, 1.0):
                clicks.append(((first + i) / rate, name, d))
            acc += diffs[i] - diffs[i - win]
    clicks.sort()
    print(f"clicks: {len(clicks)}")
    for t, name, d in clicks[:10]:
        print(f"  at {t:.3f} s ({name}): jump of {d}")

    clipped = sum(1 for v in left if v in (32767, -32768)) + (
        sum(1 for v in right if v in (32767, -32768)) if channels >= 2 else 0)
    print(f"clipped samples: {clipped}")


if __name__ == "__main__":
    main()
