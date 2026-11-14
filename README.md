# Mp3 to XWM

Converts MP3 files into the `.xwm` sound files Space Engineers plays for music and sounds: xWMA for music, or MS ADPCM for sounds that loop.

A single console program with no external libraries. Its MP3 decoder and its WMA and ADPCM encoders are part of the program.

Version 1.0.0. Source: https://github.com/buttheadbob/Mp3-to-XWM

## Download

Releases are on the repository's Releases page:

- `mp3toxwm-1.0.0-windows-x64.zip`: `mp3toxwm.exe` for 64-bit Windows 10 and 11.
- `mp3toxwm-1.0.0-linux-x64.tar.gz`: `mp3toxwm` for 64-bit Linux, statically linked.

Each comes with a `.sha256` file holding its SHA-256 checksum. Both packages also hold this README, LICENSE and THIRD_PARTY.md.

## Use

1. Run `mp3toxwm` (`mp3toxwm.exe` on Windows).
2. On the first run it creates two folders next to the program, `ToConvert` and `Output`.
3. Put the `.mp3` files into `ToConvert`.
4. Choose the output format with `X` (XWM, the default) or `A` (MS ADPCM). Press `S` to start, or `Q` to quit.
5. The converted files are written to `Output` with the same names and the `.xwm` extension.

If `Output` already holds files, the program says so before starting. Files with the same name are overwritten. A file is only replaced once its new version is complete.

When started from a console window (for example by double-clicking it), the program waits for a key before closing so the summary stays readable.

## Which format

| | XWM (xWMA) | MS ADPCM |
|---|---|---|
| Best for | Music | Sounds that loop (engines, hums, ambience, alarms) and short effects |
| Size | Small: about 360 KB a minute | About 7-8 times larger than XWM (about 2.7 MB a minute in stereo) |
| Looping | A short gap of about 0.08 s where the loop starts again | No gap |
| In the game | The format of the game's own music | The format of the game's own looping sounds |

Why the choice exists: xWMA is a transform codec. Its decoder always outputs whole frames of 2048 samples, plus one final frame that fades to silence, and the game gives no way to cut playback short for xWMA files. So a sound set to loop has a short silence each time it starts over. MS ADPCM stores samples directly in blocks of 512, so a sound whose length is a whole number of blocks loops without any gap. Both formats use the `.xwm` name, as the game does for its own ADPCM sounds; the game tells them apart by their contents.

To loop without a gap, an ADPCM file must be a whole number of 512-sample blocks long. The program resamples the whole sound so its length comes out at the nearest whole number of blocks (at most 256 samples more or fewer), instead of padding the end with silence, and writes into the file the sample rate that plays those samples in the sound's own time, for example 44083 Hz instead of 44100 Hz. Pitch and speed stay as they were; the game sets each sound's playback rate from its file, as it does for its own looping sounds, which use rates from 43641 to 44414 Hz. The resampling treats the sound as a loop, reading its start after its end and its end before its start, so the loop point comes out as clean as any other point. The program prints the change and the rate for each file. A sound so short (under about 3 ms) that the rate would pass 200000 Hz, the most XAudio2 plays, is converted unchanged and padded with silence to the next block instead, and the program says so.

ADPCM conversion holds the whole sound in memory while it fits the length (about 21 MB per stereo minute at 44.1 kHz, 23 MB at 48 kHz). Sounds longer than 10 minutes are not held: they are converted as they come and padded with silence to the next block, so they will not loop seamlessly, and the program says so. Use XWM for long music.

## Output formats

XWM files match the format of the game's own music:

| Field | Value |
|---|---|
| Codec | WMA version 2 (format tag `0x0161`) in an xWMA RIFF file with `fmt`, `dpds` and `data` chunks |
| Sample rate | 44100 Hz. Other MP3 sample rates are resampled. |
| Channels | Same as the MP3: mono or stereo. 3D sounds in the game must be mono. |
| Bit rate | 48 kbit/s with 2230 byte packets, as the game's own music |

Stereo files at 96 and 192 kbit/s (4459 and 8917 byte packets) decoded cleanly with Microsoft's WMA decoder but did not play properly in the game, so the program writes 48 kbit/s only.

MS ADPCM files match the game's own looping sounds: a RIFF WAVE file with `fmt` and `data` chunks, format tag 2, 4 bits per sample, 512 samples per block (262 byte blocks for mono, 524 for stereo), the 7 standard coefficient pairs, mono or stereo as the MP3. The sample rate is 44100 Hz adjusted by the length fit as above (44100 Hz for sounds that are padded). Windows' own MS ADPCM codec decodes the files to exactly the expected samples, and a test loop played in the game without a gap.

Input: MPEG-1, MPEG-2 and MPEG-2.5 audio, layers I, II and III, including free format streams. ID3v1 and ID3v2 tags are skipped. Files are processed as a stream, so for XWM output memory use stays small whatever their length; ADPCM output holds the sound while it fits the loop, up to 10 minutes, as above.

Gapless MP3s (with a LAME or FFmpeg encoder tag) have their encoder delay and padding removed. With Microsoft's decoder the audio starts on the first sample of the source.

## Build

Needs a C23 compiler: clang 19 or newer, or gcc 13 or newer. No other tools or libraries.

Windows (uses clang from PATH or from `C:\Program Files\LLVM`, or gcc from MinGW-w64):

```
build.bat
```

Linux:

```
sh build.sh
```

The program is written to `build/`.

`release.bat` (Windows) and `release.sh` (Linux) build the program and pack the release files above into `release/`, named after the version in `src/version.h`.

## Tests

See [tests/README.md](tests/README.md).

## Credits

- WMA and MP3 data tables, and the WMA codec parameter rules, from [FFmpeg](https://ffmpeg.org), LGPL-2.1 or later.

Details in [THIRD_PARTY.md](THIRD_PARTY.md).

## Licence

AGPL-3.0-or-later, see [LICENSE](LICENSE).
