# Third party material

Mp3 to XWM (https://github.com/buttheadbob/Mp3-to-XWM) is licensed under the GNU AGPL version 3 or later; see LICENSE.
It contains the material below from other projects.

All of it comes from FFmpeg (https://ffmpeg.org), commit `c02ac4a9e071d5cfbea82318fa8d9f6cb55b353e`, which is licensed
under the GNU Lesser General Public License version 2.1 or later
(https://www.gnu.org/licenses/old-licenses/lgpl-2.1.html).

## Data tables

`src/wma_tables.h`, generated from:

| File | Content | Notice in the file |
|---|---|---|
| `libavcodec/wmadata.h` | WMA coefficient Huffman tables, run/level tables, exponent band tables | copyright (c) 2002 The FFmpeg Project |
| `libavcodec/aactab.c` | the scale factor Huffman table, which WMA uses for exponent coding | Copyright (c) 2005-2006 Oded Shimon, Copyright (c) 2006-2007 Maxim Gavrilov |
| `libavcodec/wma_freqs.c` | critical band edge frequencies | no copyright line |

`src/mp3_tables.h`, generated from (these hold the tables of ISO/IEC 11172-3 and 13818-3):

| File | Content | Notice in the file |
|---|---|---|
| `libavcodec/mpegaudiodec_common.c` | layer III Huffman tables, band sizes, scale factor tables | copyright (c) 2002 Fabrice Bellard |
| `libavcodec/mpegaudiodsp_data.c` | the synthesis window | no copyright line |
| `libavcodec/mpegaudiotabs.h` | bit rates | copyright (c) 2002 Fabrice Bellard |
| `libavcodec/mpegaudiodata.c` | layer II bit allocation and quantization tables | copyright (c) 2002 Fabrice Bellard |

## Code that follows FFmpeg

- In `src/wma_enc.c`, the derivation of the codec parameters from the format follows `libavcodec/wma.c`
  (Copyright (c) 2002-2007 The FFmpeg Project): `exponent_bands`, the block size, noise coding, coefficient table and
  byte offset choices in `ctx_init`, `total_gain_to_bits` and `av_log2`.
- In `src/mp3dec.c`, the choice of the layer II allocation table in `decode_layer2` follows `ff_mpa_l2_select_table`
  in `libavcodec/mpegaudio.c` (Copyright (c) 2001, 2002 Fabrice Bellard).

The rest of the encoder, the MP3 decoder and the other code are written for this project.

## Test only: minimp3

`tests/compare_minimp3.c` compares this project's MP3 decoder with minimp3 (https://github.com/lieff/minimp3, public
domain, CC0). It is a test tool: minimp3 is not in this repository, is not part of the program and is not shipped.
Whoever runs the comparison supplies `minimp3.h` themselves.

## How the licences combine

This project is licensed under the GNU AGPL version 3 or later. The FFmpeg material above is LGPL-2.1-or-later:

1. Section 3 of the LGPL version 2.1 allows a copy of LGPL material to be placed under the GNU GPL version 2 or any
   later version instead.
2. That makes it usable under the GNU GPL version 3.
3. Section 13 of the GNU GPL version 3 allows GPLv3 material to be combined with material under the GNU AGPL
   version 3 into one work, which is conveyed under the AGPL.

The FFmpeg notices listed above are kept in the files that contain the material.
