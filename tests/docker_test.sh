#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-or-later
# Runs inside the test container with the project mounted read-only at /src.
# Builds with each available compiler, converts generated MP3s and checks the
# decoded result against the source with ffmpeg's WMA decoder.
set -e
cp -r /src /w
cd /w
rm -rf build

for cc in gcc gcc-14 clang-19; do
    command -v $cc >/dev/null 2>&1 || continue
    printf '%-8s ' "$cc"
    CC=$cc sh build.sh >/tmp/build-$cc.log 2>&1 || { echo "build FAILED"; cat /tmp/build-$cc.log; exit 1; }
    if grep -q warning /tmp/build-$cc.log; then echo "built with warnings"; cat /tmp/build-$cc.log; else echo "built clean"; fi
done

cd build
mkdir -p ToConvert
gen() { ffmpeg -v error -y "$@"; }
# Broadband test signals, encoded the way MP3s usually are.
gen -f lavfi -i "anoisesrc=d=8:c=pink:a=0.3,highpass=f=40" -f lavfi -i "sine=f=440:d=8" \
    -filter_complex "[0][1]amix=inputs=2,volume=2,aformat=channel_layouts=stereo" -ar 44100 -b:a 192k ToConvert/pink_tone.mp3
gen -f lavfi -i "aevalsrc=0.4*sin(2*PI*(100+1500*t)*t)|0.4*sin(2*PI*(150+1200*t)*t):d=8:s=44100" -b:a 256k ToConvert/chirp.mp3
gen -f lavfi -i "anoisesrc=d=6:c=pink:a=0.4" -ac 1 -ar 44100 -b:a 128k ToConvert/mono_noise.mp3
gen -f lavfi -i "anoisesrc=d=6:c=brown:a=0.5" -ac 2 -ar 48000 -b:a 192k ToConvert/rate48k.mp3
gen -f lavfi -i "anoisesrc=d=6:c=pink:a=0.4" -ac 2 -ar 22050 -b:a 96k ToConvert/rate22k.mp3
gen -f lavfi -i "anullsrc=r=44100:cl=stereo:d=3" -f lavfi -i "sine=f=300:d=3" \
    -filter_complex "[0][1]concat=n=2:v=0:a=1,aformat=channel_layouts=stereo" -b:a 128k ToConvert/silence_then_tone.mp3
gen -f lavfi -i "aevalsrc=0.6*sin(2*PI*220*t)*exp(-8*mod(t\,0.5)):d=6:s=44100,aformat=channel_layouts=stereo" \
    -b:a 192k ToConvert/plucks.mp3

cp ToConvert/plucks.mp3 "ToConvert/曲 Ünïcødé – тест.mp3"  # non-ASCII file name
echo s | ./mp3toxwm

fail=0
for f in ToConvert/*.mp3; do
    name=$(basename "$f" .mp3)
    printf '%-20s ' "$name"
    if ! ffmpeg -v error -i "Output/$name.xwm" -f wav -c:a pcm_s16le -y "/tmp/$name.out.wav"; then
        echo "DECODE FAILED"; fail=1; continue
    fi
    # Reference: the MP3 decoded and resampled the same way the game will play it.
    ffmpeg -v error -i "$f" -ar 44100 -c:a pcm_s16le -y "/tmp/$name.ref.wav"
    python3 /src/tests/compare.py "/tmp/$name.ref.wav" "/tmp/$name.out.wav" || fail=1
done
exit $fail
