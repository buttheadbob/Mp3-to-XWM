#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-or-later
# Runs inside the test container: encodes generated signals straight to .xwm at each
# supported format and reports the SNR after decoding with ffmpeg.
set -e
cd /tmp
gcc -std=c2x -O2 -Wall -Wextra -o synth_encode /src/tests/synth_encode.c /src/src/wma_enc.c \
    /src/src/mdct.c /src/src/bitwriter.c /src/src/xwm_writer.c /src/src/platform.c -lm
for sig in ${SIGNALS:-impulse noise sine}; do
    for cfg in "1 48000 2230" "2 48000 2230" "2 96000 4459" "2 192000 8917"; do
        ./synth_encode $sig t.xwm $cfg
        ffmpeg -v error -i t.xwm -f f32le -c:a pcm_f32le -y t.raw
        python3 - "$sig" "$cfg" <<'EOF'
import sys
import numpy as np
sig, cfg = sys.argv[1], sys.argv[2]
ch = int(cfg.split()[0])
o = np.fromfile("t.raw", np.float32).astype(float).reshape(-1, ch)
r = np.fromfile("t.xwm.ref.raw", np.int16).astype(float).reshape(-1, ch) / 32767
r = r[2048:]  # ffmpeg drops one frame more codec delay than Microsoft's decoder
n = min(len(o), len(r))
o, r = o[:n], r[:n]
g = (o * r).sum() / (r * r).sum()
e = o - g * r
print("%-7s ch/rate/align %-14s gain %.3f  SNR %5.1f dB" % (sig, cfg, g, 10 * np.log10((g * g * (r * r).sum()) / (e * e).sum())))
EOF
    done
done
