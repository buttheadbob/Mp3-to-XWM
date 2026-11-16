# Tests

None of these are needed to build or use the converter.

## Linux container (ffmpeg as reference decoder)

The scripts run in a container with ffmpeg, gcc 13, gcc 14, clang 19, Python 3 and NumPy, with the
project mounted read-only at `/src`. For example, from `ubuntu:24.04`:

```
apt-get install ffmpeg gcc gcc-14 clang-19 libc6-dev python3 python3-numpy
```

- `docker_test.sh` builds with each compiler (warnings are reported), generates test MP3s with ffmpeg
  (noise, tones, a chirp, mono, 48 kHz and 22.05 kHz sources, leading silence), converts them with the
  console program, decodes the results with ffmpeg and checks them with `compare.py`.
- `synth_check.sh` encodes generated signals straight to `.xwm` (no MP3 step) at mono 48 kbit/s and
  stereo 48, 96 and 192 kbit/s, and reports gain and SNR after decoding with ffmpeg. The program
  writes mono 48 kbit/s only; stereo and the other rates check the encoder itself.
- `compare.py` aligns two WAV files, then reports delay, gain, SNR and segmental SNR. A stereo
  reference against a mono result is mixed down to the mean of left and right, as the program does.

ffmpeg drops one more frame (2048 samples) of codec delay at the start than Microsoft's decoder,
so its output is 2048 samples early. The scripts allow for this.

## Windows (Microsoft's decoders)

Build with clang:

```
clang -std=c23 -O2 -D_CRT_SECURE_NO_WARNINGS -o build\mf_decode.exe tests\mf_decode.c -lole32 -lmfplat -lmfuuid -lwmcodecdspuuid
clang -std=c23 -O2 -D_CRT_SECURE_NO_WARNINGS -o build\xaudio2_check.exe tests\xaudio2_check.c -lole32 -lxaudio2
clang -std=c23 -O2 -D_CRT_SECURE_NO_WARNINGS -o build\synth_encode.exe tests\synth_encode.c src\wma_enc.c src\mdct.c src\bitwriter.c src\xwm_writer.c src\platform.c
```

- `mf_decode in.xwm out.wav` decodes with the Media Foundation WMA decoder, offline. It reports packet
  errors and compares the decoded size with the file's `dpds` table.
- `xaudio2_check file.xwm` plays the file through XAudio2 2.9, the API the game uses, at zero volume,
  and checks that every sample plays without a voice error. It plays in real time. It needs an audio
  device but opens no window. On the development machine its result varies from run to run, for the
  game's own `.xwm` files as well (some runs play every sample, others end at once with no error), so
  a failed run alone does not show a fault in the file. Opening the audio device can wake it, for
  example a Bluetooth headset.
- `synth_encode signal out.xwm [channels] [bitrate] [block_align] [piece]` writes a test file from a
  generated impulse, noise or sine, plus the raw source next to it. It feeds the encoder `piece`
  samples at a time (default 777) to exercise the streaming paths.

## MS ADPCM

- `acm_decode in.xwm out.wav` (Windows) decodes an MS ADPCM file with Windows' own MS ADPCM codec
  through the Audio Compression Manager, offline, and checks that it gives exactly 512 samples per
  block and that every sample matches the reference decoder in the test with the prediction rounded
  down (an arithmetic shift, as the encoder predicts). It prints the count for rounding toward zero
  too: on the game's own ArcBlockRafineryLoop2d.xwm Windows differs from that in 588733 samples and
  from rounding down in none. Build: `clang -std=c23 -O2 -D_CRT_SECURE_NO_WARNINGS -o build\acm_decode.exe tests\acm_decode.c -lmsacm32 -lwinmm`.
- `loop_check.c` builds 10 s loops of a tone and of noise, from 44.1 kHz and from 48 kHz sources,
  whose length is not a whole number of blocks. It sends them through the loop fit (whole-loop
  resampling to whole blocks) and the ADPCM encoder, decodes them again with its own MS ADPCM
  decoder, and checks that the length is whole blocks, that the loop point is as clean as the
  middle of the loop, and that the header rate plays the samples in the source's own time (pitch
  within 0.1 cent). `loop_check out_dir`:

```
clang -std=c23 -O2 -o build/loop_check tests/loop_check.c src/adpcm_enc.c src/loopfit.c src/wav_writer.c src/platform.c src/resample.c
```

## Hostile input

`test_fuzz.c` feeds the decoder and the conversion path input built to break them: free format frames
far over the legal size with the largest bit reservoir offset, a layer III frame whose largest gain and
linbits give samples far out of range, NaN and infinite samples straight into the encoder, random data
with frame headers sprinkled in, and streams of well formed headers with random contents for every
version, layer, rate and channel mode, plus NaN, infinite and huge samples in pieces of every size
through the ADPCM length fit and encoder, which must always give whole blocks, held or (past a
small hold limit) streamed. Build it with AddressSanitizer so any out-of-bounds access stops
the run:

```
clang -std=c23 -g -O1 -fsanitize=address -D_CRT_SECURE_NO_WARNINGS -Isrc -o build/test_fuzz tests/test_fuzz.c src/mp3dec.c src/mp3_input.c src/resample.c src/wma_enc.c src/mdct.c src/bitwriter.c src/xwm_writer.c src/platform.c src/convert.c src/adpcm_enc.c src/loopfit.c src/wav_writer.c
build/test_fuzz <scratch folder>
```

## MPEG audio test vectors

`vectors_check.c` decodes a test vector (`.bit`) and compares it with its reference output (`.pcm`,
16-bit little endian, interleaved), after finding the best alignment within two frames. It reports SNR
and the largest difference in 16-bit steps. The vectors used are minimp3's set (ISO/IEC 11172-3 and
13818-3 conformance streams and edge cases), commit `ea99364f61c14656440e8d77e9c233ccf3124633`,
folder `vectors`; they are not part of this repository.

```
clang -std=c23 -O2 -D_CRT_SECURE_NO_WARNINGS -o build/vectors_check tests/vectors_check.c src/mp3dec.c
build/vectors_check file.bit file.pcm
```

## MP3 decoder against minimp3 (test only)

`compare_minimp3.c` decodes an MP3 with this project's decoder and with minimp3
(https://github.com/lieff/minimp3, public domain), lines the two up and reports SNR and the largest
difference. minimp3 is not part of this repository or of the program, and is not shipped: put `minimp3.h` (commit
`ea99364f61c14656440e8d77e9c233ccf3124633` was used) in a folder of your choice and pass it with `-I`.
Set `FRAMES=1` to list the granules where the decoders differ.

```
clang -std=c23 -O2 -D_CRT_SECURE_NO_WARNINGS -Isrc -I<minimp3 folder> -o build/compare_minimp3 tests/compare_minimp3.c src/mp3dec.c
build/compare_minimp3 file.mp3
```

## MDCT

`test_mdct.c` compares the fast MDCT with the direct formula for every block size:

```
clang -std=c23 -O2 -o build/test_mdct tests/test_mdct.c src/mdct.c
```
