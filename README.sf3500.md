# Water Closet for the Datafrog SF3500

This branch (`sf3500`) is Parallel Realities' Water Closet at upstream tag `v1.0`
(commit `565749a`) plus a port to the Datafrog SF3500 handheld (MIPS32r2, no GPU).
The game's rules, levels and data are upstream's. The commits on top of the tag are:

1. **SF3500 platform backend** (`sf3500/` and changes to `src/system/draw.c`, `init.c`,
   `input.c`, `sound.c` and a few small edits in `src/game/` and the headers; later commits below
   rework the drawing, text, sound and input parts):
   - the 1280x720 game canvas is rendered by SDL's software renderer into a 640x360 surface
     and presented through the device's display driver (the device has no GPU);
   - a small software mixer (`sf3500/sf3500_mixer.*`, Ogg through stb_vorbis) feeds the
     device's audio driver instead of SDL_mixer;
   - the gamepad is read directly from the device (`sf3500/joykey.*`);
   - SDL_ttf is compiled in from source;
   - level saves are written unformatted (`cJSON_PrintUnformatted`), since they happen on
     every stage load and the file is only ever parsed.
   The display and audio driver come from `/tmp/tfdevice.env`.
2. **Ogg copies of three MP3 sounds** (the mixer decodes Ogg only): `music/puzzle-1-a.ogg`,
   `music/puzzle-1-b.ogg` and the water-spray sound. Same credit as the originals.
3. **Half-size drawing.** The screen is 640x360, so the pictures are reduced to half size once
   at load (`src/system/atlas.c`) and drawn 1:1 into the RGB565 screen buffer, which is handed to
   the display driver as it is. Opaque map tiles are copied straight onto it, glows and sparks are
   blended by a small loop of their own (`blitGlow`), translucent black rectangles darken the pixels
   directly, sprites that face left use pre-mirrored copies (SDL's flipped copy puts a picture in the
   wrong place when it is letterboxed), and a paused screen (a tip, the pause menu) is drawn once and
   then only shown again.
4. **Cached text.** Lines of text are rendered once into a small glyph atlas and drawn as a block.
5. **Sound.** Streamed music is declared with its real sample rate and converted to the output rate
   with a windowed-sinc resampler (`sf3500/resample.hpp`), the mixer works a configurable number of
   milliseconds ahead of the clock, a sound can play several times at once, and an optional
   `music_pack` folder replaces tracks by file name.
6. **Input and controls.** Buttons act like key events (set when pressed, cleared when released) so
   a tap is one press; the D-pad axes do the same. The button mapping is read from `controls.txt`
   (`src/system/controls.c`), L2 and R2 are supported, the game's text shows button names instead
   of key names, and the Controls screen in Options is read-only. Each jump needs a fresh press
   unless `jump_repeat=1`.
7. **Frame limit, settings and timing.** The game does one logic step per frame, so the frame rate
   is the game speed: `fps` sets it (default 60, the designed speed), and `draw_every` (default 2 on this device) draws a
   picture only every 2nd frame while the logic and buttons keep the full rate, which keeps the speed up in the busier stages. Other settings are read from the
   environment, which `run.sh` fills from `watercloset.txt`. A timing and audio line is written to
   the log every 120 frames.
8. This file.

## Controls (SF3500)

D-Pad moves and navigates menus, A jumps, B interacts, X creates a clone and resets, Y
restarts the stage, Start pauses, A, B or X confirm in menus, Select + Start exits. Every one of
these can be changed in `controls.txt` (action=BUTTON, e.g. `jump=L1`); see the setup guide.

## Settings

All read by `run.sh` from `watercloset.txt` into `WC_*` environment variables: `fps`, `draw_every`,
`lights`, `particles`, `background`, `sharpness`, `saturation`, `contrast`, `nice`, `music`,
`audio_lead`, `jump_repeat`, `perf_log`. `WC_CONTROLS` overrides the path of `controls.txt` (for
testing on a PC).

## Building

```sh
make -f Makefile.sf3500 SF3500_TC=/path/to/toolchain/host SDL2_TTF_SRC=/path/to/SDL2_ttf-2.0.15
```

The toolchain is a MIPS32r2 little-endian GCC (tested with Codescape MIPS MTI GCC 6.3.0)
whose sysroot has SDL2, SDL2_image and freetype. It writes `waterCloset.mipsel`; run it
from the game folder so `data/`, `gfx/`, `music/` and `sound/` are found.

## Licences

As stated upstream: source code (`src`, `tools`) GPL 3.0; graphics (`gfx`) and data (`data`)
Creative Commons BY-NC-SA 3.0 (copyright Parallel Realities); music, sound effects and the
font under the licences listed in `data/misc/credits.txt`. Three sounds the game uses are not
in that list: `25879__acclivity__drip1.ogg` is CC BY-NC 4.0 (freesound.org, acclivity),
`398032__swordofkings128__splash.ogg` is CC0 (freesound.org, swordofkings128), and the origin of
`plunge.ogg` is not stated. `mortice_key_drop_on_concrete_floor.ogg` is credited only as
"Copyright www.freesfx.co.uk". Note that `legalcode.txt` is the CC BY-NC-SA 4.0 text while the
README and the in-game credits say 3.0.
