# Mp3 to XWM

Converts MP3 files into the `.xwm` (xWMA) audio format that Space Engineers uses for music and sounds.

A single console program with no external libraries. Its MP3 decoder and WMA encoder are part of the program.

## Use

1. Run `mp3toxwm` (`mp3toxwm.exe` on Windows).
2. On the first run it creates two folders next to the program, `ToConvert` and `Output`.
3. Put the `.mp3` files into `ToConvert`.
4. Choose the quality for stereo files with `1`, `2` or `3` (48, 96 or 192 kbit/s; 48 is the default), then press `S` to start, or `Q` to quit.
5. The converted files are written to `Output` with the same names and the `.xwm` extension.

If `Output` already holds files, the program says so before starting. Files with the same name are overwritten. A file is only replaced once its new version is complete.

When started from a console window (for example by double-clicking it), the program waits for a key before closing so the summary stays readable.

## Output format

The files match the format of the game's own music:

| Field | Value |
|---|---|
| Codec | WMA version 2 (format tag `0x0161`) in an xWMA RIFF file with `fmt`, `dpds` and `data` chunks |
| Sample rate | 44100 Hz. Other MP3 sample rates are resampled. |
| Channels | Same as the MP3: mono or stereo. 3D sounds in the game must be mono. |
| Bit rate | Stereo: 48 kbit/s (2230 byte packets, the game's own music), 96 kbit/s (4459) or 192 kbit/s (8917). Mono: always 48 kbit/s, the highest rate xWMA has for mono at 44.1 kHz. |

96 and 192 kbit/s decode cleanly with Microsoft's WMA decoder but have not yet been tried in the game.

Input: MPEG-1, MPEG-2 and MPEG-2.5 audio, layers I, II and III, including free format streams. ID3v1 and ID3v2 tags are skipped. Files are processed as a stream, so memory use stays small whatever their length.

Gapless MP3s (with a LAME or FFmpeg encoder tag) have their encoder delay and padding removed. With Microsoft's decoder the audio starts on the first sample of the source. The decoder always outputs whole frames plus one final frame, so 46 to 93 ms of silence follow the end.

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

## Tests

See [tests/README.md](tests/README.md).

## Credits

- WMA and MP3 data tables, and the WMA codec parameter rules, from [FFmpeg](https://ffmpeg.org), LGPL-2.1 or later.

Details in [THIRD_PARTY.md](THIRD_PARTY.md).

## Licence

AGPL-3.0-or-later, see [LICENSE](LICENSE).
