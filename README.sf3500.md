# Water Closet for the Datafrog SF3500

This branch (`sf3500`) is Parallel Realities' Water Closet at upstream tag `v1.0`
(commit `565749a`) plus a port to the Datafrog SF3500 handheld (MIPS32r2, no GPU).
The game's rules, levels and data are upstream's. The commits on top of the tag are:

1. **SF3500 platform backend** (`sf3500/` and changes to `src/system/draw.c`, `init.c`,
   `input.c`, `sound.c` and a few small edits in `src/game/` and the headers):
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
3. This file.

## Controls (SF3500)

D-Pad moves and navigates menus, A jumps, B interacts, X creates a clone and resets, Y
restarts the stage, Start pauses, Select + Start exits.

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
