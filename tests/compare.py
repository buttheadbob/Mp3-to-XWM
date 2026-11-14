# SPDX-License-Identifier: AGPL-3.0-or-later
"""Compares a decoded .xwm against the reference audio.

Usage: compare.py reference.wav decoded.wav
Finds the delay (within +-8192 samples) by cross correlation, then reports gain, SNR and the
segmental SNR. Exits 1 if the result is below the limits given in the
environment (MIN_SNR, default 10 dB) or the gain is off by more than 1 dB.
"""
import os
import sys
import wave

import numpy as np


def read(path):
    w = wave.open(path)
    a = np.frombuffer(w.readframes(w.getnframes()), np.int16)
    return a.reshape(-1, w.getnchannels()).astype(np.float64) / 32768.0, w.getframerate()


def main():
    ref, rr = read(sys.argv[1])
    out, ro = read(sys.argv[2])
    if rr != ro or ref.shape[1] != out.shape[1]:
        print(f"format mismatch: {rr} Hz x{ref.shape[1]} vs {ro} Hz x{out.shape[1]}")
        return 1
    mono_r, mono_o = ref.mean(axis=1), out.mean(axis=1)
    n = min(len(mono_r), len(mono_o))
    size = 1 << int(np.ceil(np.log2(2 * n)))
    corr = np.fft.irfft(np.fft.rfft(mono_o[:n], size) * np.conj(np.fft.rfft(mono_r[:n], size)), size)
    # delays of up to 8192 samples either way (negative: the decoder dropped leading samples)
    window = np.concatenate([corr[-8192:], corr[:8193]])
    delay = int(np.argmax(np.abs(window))) - 8192
    if delay >= 0:
        length = min(len(ref), len(out) - delay)
        r, o = ref[:length], out[delay:delay + length]
    else:
        length = min(len(ref) + delay, len(out))
        r, o = ref[-delay:-delay + length], out[:length]
    gain = float((r * o).sum() / max((r * r).sum(), 1e-20))
    err = o - gain * r
    snr = 10 * np.log10((gain * gain * (r * r).sum()) / max((err * err).sum(), 1e-20))
    seg = []
    block = 2048
    for i in range(0, length - block, block):
        s = (r[i:i + block] ** 2).sum()
        if s < 1e-6 * block:
            continue
        e = ((o[i:i + block] - r[i:i + block]) ** 2).sum()
        seg.append(10 * np.log10(s / max(e, 1e-20)))
    segsnr = float(np.mean(np.clip(seg, -10, 60))) if seg else float("nan")
    tail = len(out) - delay - len(ref)
    print(f"delay {delay}  length ref {len(ref)} out {len(out)} (extra {tail})  gain {gain:.3f} "
          f"({20 * np.log10(max(gain, 1e-9)):+.2f} dB)  SNR {snr:.1f} dB  segSNR {segsnr:.1f} dB")
    min_snr = float(os.environ.get("MIN_SNR", "10"))
    ok = snr >= min_snr and abs(20 * np.log10(max(gain, 1e-9))) <= 1.0
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
