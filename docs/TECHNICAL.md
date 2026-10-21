# Technical notes

How the recompilation works, every option, and developer helpers. For the short version, see the [README](../README.md).

## Overview

This project turns the original 16-bit Windows 3.1 executable `NITE3W.EXE`
(Nitemare-3D v1.6, © 1994 David P. Gray / Gray Design Associates) into
portable C that runs natively on modern 64-bit macOS, Linux and Windows.

It is a **static recompilation**, not an emulator and not a rewrite:

1. **Ghidra** disassembles and analyses the NE executable (segments, imports,
   jump tables, functions that are only reachable through MFC vtables).
2. `tools/recomp.py` translates every instruction's Ghidra **p-code** into C.
   Flags, 32-bit 386 instructions, string ops, segment arithmetic and
   `__AHINCR` huge pointers keep their exact semantics, so all game logic —
   renderer, AI, doors, weapons, menus, save games — is the original code.
3. A small runtime (`src/rt/`) supplies what Windows 3.1 used to: segmented
   memory, the Win16 API (KERNEL/USER/GDI), **WinG**, MMSYSTEM wave/MIDI/MCI,
   DOS `INT 21h` file I/O and the VGA retrace port.
4. `src/host_sdl.c` replaces the MFC shell: an SDL2 window, keyboard/mouse,
   audio, and the idle loop that drives the game's own `OnIdle` tick.

The pipeline also writes Ghidra's readable decompilation of the game
(`re/nite3w.c`, annotated with data-segment strings and recovered names from
`re/names.txt`) as a local reference for hand-cleanup of individual functions.

**This repository contains no game code or data.** Everything derived from
`NITE3W.EXE` (`src/gen/`, `re/nite3w.c`) is generated locally from your own
copy of the game; see "Game data" and "Regenerating `src/gen/`" below.

## Building

Requirements: CMake ≥ 3.16, a C99 compiler, SDL2 development files.

| Platform | Install SDL2 | Build |
| --- | --- | --- |
| macOS | `brew install sdl2 cmake` | `cmake -S . -B build && cmake --build build -j` |
| Linux (Debian/Ubuntu) | `sudo apt install libsdl2-dev cmake gcc` | same |
| Windows (MSVC) | `vcpkg install sdl2` | `cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake && cmake --build build --config Release` |
| Windows (MinGW cross from Linux) | SDL2 `-mingw` dev package | `cmake -S . -B build -DCMAKE_SYSTEM_NAME=Windows -DCMAKE_C_COMPILER=x86_64-w64-mingw32-gcc -DSDL2_DIR=<sdl>/x86_64-w64-mingw32/lib/cmake/SDL2` |

The build needs the generated sources in `src/gen/`, which are not in the repository (see below). On Windows,
put `SDL2.dll` next to `nitemare3d.exe`.

## Game data

Data files are not included. Extract them from your copy of the original
installer (`win3_Nite3d3x.zip`, the Wise installer `INSTALL.EXE` + `INSTALL.W02`):

```bash
python3 tools/extract_installer.py /path/to/win3_Nite3d3x.zip original
```

This writes `original/NITE3W.EXE` (input to the recompiler) and
`original/data/` (`MAP.1-3`, `IMG.1-3`, `SND.DAT`, `UIF.DAT`, `GAME.PAL`,
`NITE3D.BSF`, `DEMO.1`, `ENDING.FLI`). File names come from the installer's
own script.

## Regenerating `src/gen/` from the executable

Needs Ghidra 12.x and a JDK 21 (`brew install ghidra openjdk@21`):

```bash
GHIDRA_INSTALL_DIR=/opt/homebrew/opt/ghidra/libexec JAVA_HOME=/opt/homebrew/opt/openjdk@21 tools/build_gen.sh
```

Pipeline: `tools/redecomp.sh` (Ghidra headless: `FindMissingFuncs.java`,
`ApplyNames.java`, `ExportDecomp.java`, `ExportAsm.java`, `ExportRecomp.java`)
→ `tools/ne_relocs.py` (imported KERNEL constants) → `tools/recomp.py`
(p-code → C) → `tools/gen_data.py` (relocated segment images + import table).

## Running

```bash
./build/nitemare3d --data original/data
```

Options: `--user DIR` (saves/config; default is the per-user app-data folder),
`--scale N`, `--fullscreen`, `--soundfont FILE.sf2`, `--fm`, and `-- ARGS`
to pass the original command-line switches (e.g. `-- -b` for the debug stats).

Controls are the original game's: arrow keys move/turn, **Ctrl** fires,
**Space** opens/uses, **Left Shift** gives finer turning control, **1–4** weapons, **Esc** menu,
**Tab** status, **F2** music, **F3** sound, **F4** quick load, **F5** quick
save, **F9/F10** automaps. **Alt+Enter** toggles fullscreen (handled by the
host instead of the original DISPDIB "special mode").

Like the original, the game pauses and mutes while its window is inactive.

### Music

The Windows version sent its General MIDI tunes to whatever MIDI synth
Windows had. Two backends are provided:

* **SoundFont (recommended):** drop any General MIDI `.sf2` into the data or
  user folder, or pass `--soundfont FILE.sf2` / set `N3D_SOUNDFONT`. Played
  with TinySoundFont — closest to a mid-90s wavetable card.
* **FM (fallback, `--fm`):** the Sound Blaster's Yamaha OPL chip, emulated
  with Nuked OPL3, using the General MIDI FM instrument bank that ships
  inside `SND.DAT`. This is the Sound Blaster/AdLib sound. `--sb MODEL` picks
  the card (see "Sound Blaster models" below).

## Layout

```
src/host_sdl.c        SDL2 shell: window, input, gamepad, audio, main loop (desktop and browser)
web/                  page shell for the WebAssembly build
src/music.c           MIDI sequencer -> SoundFont or OPL2; pre-rendered tune playback
src/dls.c             DLS (gm.dls) -> SoundFont conversion
tools/render_music.c  n3d-render-music: SND.DAT tunes -> tuneNN.wav/.ogg
src/rt/               runtime: memory/CPU state, Win16+WinG+MMSYSTEM, DOS
src/gen/              generated recompiled code + data image (local only, not in git)
tools/                extraction, Ghidra scripts, recompiler, helpers
re/names.txt          recovered function names and prototypes (input to Ghidra)
third_party/          Nuked OPL3 (LGPL-2.1), TinySoundFont (MIT), stb_vorbis (public domain/MIT)
```

## Debug helpers

Environment variables: `N3D_SHOTS=dir` (save presented frames as BMP,
`N3D_SHOTS_EVERY=n`), `N3D_KEYS="ms:VKhex[:hold],..."` (scripted input),
`N3D_WAVOUT=file.wav` (record audio), `N3D_HEADLESS=1` (no dialogs, ignore
focus), `N3D_DEBUG=1` (audio log). Configure with `-DN3D_TRACE=ON` to log
every recompiled function call to `rt_trace.txt`.

## Not implemented / known differences

* DISPDIB/mode-X "special mode" full screen (replaced by host fullscreen).
* The original joystick driver path (gamepads are handled by the host instead, see "Gamepad"), printing the order form, MFC About/Order dialogs, WinHelp.
* Music timing on the SoundFont path is accurate to one MIDI tick; FM drum
  sounds are approximations (the IBK bank has no percussion kit).

## Legal

Nitemare-3D is © David P. Gray / Gray Design Associates. This repository
contains only new code (tools, runtime, host, web shell) and vendored
third-party libraries. It does not contain the game's executable, data or
anything derived from them: `original/`, `src/gen/`, `re/nite3w.*`,
`decomp/` and the Ghidra project are produced locally and are ignored by git.
Don't commit them.

## Modern resolutions and controls

### Any resolution, widescreen and ultrawide

By default the game now renders at the window's size (in pixels, HiDPI
aware) with a native high-resolution 3D view; the original look is one
switch away.

```bash
./build/nitemare3d --data original/data --res 3440x1440
./build/nitemare3d --data original/data --res 1920x1080 --fullscreen
./build/nitemare3d --data original/data --res desktop --fullscreen
./build/nitemare3d --data original/data --classic        # original 320x200 frame, scaled
```

| Option | Meaning |
| --- | --- |
| `--res WxH` | fixed render resolution (the window scales it, letterboxed) |
| `--res window` | default: render resolution follows the window size |
| `--res desktop` | render at the desktop resolution |
| `--classic` (= `--res 320x200`) | the original frame only, shown at 4:3 |
| `--fps N` | hi-res frame rate during play (default `0` = one frame per display refresh, vsync-paced; a fixed rate that doesn't divide the refresh rate judders) |
| `--no-spaces` | macOS: fullscreen without a separate Space; fixes a horizontal tear in fullscreen on some displays |
| `--renderer metal\|opengl` | SDL render backend (default: SDL's choice) |
| `--filter nearest\|bilinear\|trilinear\|aniso` | texture filtering (default `aniso`) |
| `--aniso N` | anisotropic taps, 1–16 (default 16) |
| `--upscale none\|scale2x\|scale3x\|scale4x\|scale6x` | pixel-art upscaling of textures and sprites (default `scale4x`) |
| `--texture-quality off\|high` | `off` = nearest + no upscaling (the original texels), `high` = aniso 16 + scale4x |

How it works (`src/hd.c`, hooks in `src/rt/overrides.c`):

* The original renderer still runs at 320x200 every frame — its walk over
  the visible walls and objects also drives game logic (automap reveal, wall
  animation, which enemy is under the crosshair) — and then a native
  renderer draws the same world (wall vectors, doors, textures, objects and
  projectiles read from the game's own memory) at the output resolution.
  It uses the original projection constants, so walls and sprites have
  exactly the original proportions (checked against the original frame with
  `N3D_HDCMP`, below); a wider screen shows more of the world to the sides
  (Hor+ field of view: 81° at 4:3, ~100° at 16:9, ~116° at 21:9).
* The HUD, menus and loading screens are the original 320x200 art, scaled
  with the original 4:3 pixel aspect and pillarboxed.
* What the game draws over the view each frame (weapon, frame gargoyles,
  messages) is captured as overlay layers; the corner gargoyles stick to the
  edges of the widened view. Objects that only the wider view shows are
  animated the same way the original animates the ones it draws (not during
  demo playback, so demos stay exact). Walls and objects show the animation
  frame the original draws in the same frame (it advances animations after
  drawing). Sprite and texture pixels are the game's own (checked byte for
  byte with `N3D_HDCHECK`). Palette entries 0-9 and 246-255 are the Windows
  static system colours (the game uses them, e.g. the whites and greys of
  Hugo's coat), so the host palette starts out with them.
* Shrinking the view (smaller than the maximum) keeps it at its 4:3 place.

### Texture filtering and upscaling

The hi-res view is drawn in true colour (`src/hd_tex.c`); the HUD and menus
stay the original 320x200 art.

* **Upscaling** (`--upscale`) works on the 8-bit palette indices with the
  Scale2x / Scale3x rules (the EPX / AdvMAME family; 4x = 2x twice, 6x = 3x
  then 2x), on walls, sprites and the weapon / frame-decoration overlays.
  Every output texel is one of the original texels, so diagonals get smoothed
  while palette effects keep working and sprite cut-outs stay exact. (xBRZ
  blends colours, which would have to be redone for every palette step and is
  GPL; the Scale2x/3x rules are implemented from their published description,
  no third-party code.)
* **Colours**: each texture's mip chain (premultiplied alpha, box filtered)
  is built from the *current* palette and the DS:8084 darkening remap (walls
  that use it get their own copy) and rebuilt, in parallel, when either
  changes — fades, flashes and dark rooms look as in the original.
* **Filtering** (`--filter`): nearest; bilinear (nearest mip); trilinear;
  anisotropic — for each wall or sprite column the pixel footprint (texels
  across the column vs. down it) sets the number of trilinear taps along the
  longer axis, up to `--aniso` (16), at the mip level of the shorter one, so
  walls seen at grazing angles stay sharp without shimmering.
* **Sprites** use premultiplied alpha with a steepened alpha ramp at the
  edges, so filtered sprites keep their cut-out look without dark or coloured
  halos.
* Columns are drawn on worker threads (`N3D_THREADS=n` overrides the count).
  Cost per 60 fps frame at 3440x1440 on one core, aniso 16 + scale4x: about
  14–17 ms for the view plus 5 ms for compositing, spread over the cores of
  an Apple Silicon Mac. `--texture-quality off` is ~4 ms + 5 ms.

### WASD and mouse look

| Input | Action |
| --- | --- |
| **W / S** | forward / back |
| **A / D** | strafe left / right |
| mouse left/right | turn, smoothly (see below) |
| left mouse button | fire (same as Ctrl) |
| right mouse button | open / use (same as Space) |
| Right Shift | run (original), Left Shift = fine control (original) |

The original keys keep working: arrows move/turn, **Alt+←/→** strafes (the
game always had it), Ctrl fires, Space uses. The modern controls go through
the game's own movement routine (`FUN_1010_8a08`: collision, doors, pickups)
from a wrapper around the player tick, and are ignored while a demo is
played back or recorded. Options: `--sensitivity F` (degrees per mouse
count, default 0.12), `--no-mouselook`.

Mouse look uses relative mouse motion only, never the cursor position. The
mouse is captured from the moment the window opens: the pointer is hidden,
confined to the window and read through SDL's relative mode, so you can keep
moving the mouse in the same direction without hitting a screen edge. During
play the motion turns the view; in menus it moves an invisible virtual cursor
(menus are also fully keyboard driven). The capture is released only when the
window loses focus (Alt+Tab / Cmd+Tab) and comes back as soon as the window
is focused again. `--no-mouselook` turns capturing off entirely.

### Browser (WebAssembly)

The same sources build to WebAssembly with Emscripten (SDL2 comes from
Emscripten's port):

```bash
brew install emscripten          # or the emsdk
emcmake cmake -S . -B build-web -G Ninja
cmake --build build-web
python3 -m http.server 8093 --directory build-web   # then open http://localhost:8093
```

`build-web/` then holds a static site: `index.html`, `n3d-web.js`,
`n3d-web.css` (from `web/`), `nitemare3d.js` and `nitemare3d.wasm` (~6.5 MB,
~0.9 MB gzipped). Any static host works.

* **Game data** is not part of the build. The page asks the player once for
  the data files of their own copy (file picker, folder picker or drag and
  drop; a `.sf2` SoundFont and `music/tuneNN.ogg` are taken too) and keeps
  them in the browser's IndexedDB. Saves and `NITE3W.INI` are kept there as
  well. For a private build, `-DN3D_WASM_DATA=original/data` bundles a data
  folder into the build instead (don't publish that).
* **Options** come from the URL: `?classic`, `?fm`, `?res=1280x720`,
  `?filter=trilinear`, `?sensitivity=0.2`, `?pad-turn=250`, `?hidpi`
  (render at device pixels; off by default because the renderer is
  single-threaded here), `?game=-b` (original switches), `?args=...` (raw).
* **Threads:** `-DN3D_WASM_THREADS=ON` runs the hi-res renderer on Web
  Workers. The page must then be cross-origin isolated (served with
  `Cross-Origin-Opener-Policy: same-origin` and
  `Cross-Origin-Embedder-Policy: require-corp`).
* **Mouse:** the page takes the pointer lock from the Play click, with raw
  (unaccelerated) input where the browser offers it. Browsers release the
  lock on Esc and usually swallow that key; the game is then sent its Esc, so
  Esc still opens the menu. Clicking the game re-captures the mouse without
  firing. F11 toggles full screen; in full screen the page also takes the
  keyboard lock where supported, so Esc and Ctrl+W go to the game (hold Esc
  to leave). Outside full screen, closing the tab asks for confirmation,
  because Ctrl (fire) + W is the browser's close-tab shortcut.
* **Sound** plays through an AudioWorklet instead of SDL's audio (which mixes
  on the main thread in the browser): the game renders ~60 ms ahead into it,
  also from inside the game's own blocking loops (the half-second palette
  fades between screens, level loads, tune decoding), so the sound doesn't
  skip when the screen changes.
* Known difference: those palette fades run inside one call of the game's
  code, and a browser only shows a frame once that call returns, so the
  screen holds the last frame for the fade instead of fading.

### Hosting on Cloudflare

The live site is https://nitemare3d.everydaypreview.com (a Cloudflare
Worker that only serves static assets, configured in `wrangler.jsonc`).

```bash
emcmake cmake -S . -B build-web -G Ninja && cmake --build build-web
python3 -I tools/web_site.py --data original/data      # -> build-web/site
CLOUDFLARE_ACCOUNT_ID=<account id> wrangler deploy
```

`tools/web_site.py` copies the page and `nitemare3d.js/.wasm`, the game data
into `data/` and the pre-rendered tunes into `data/music/`, and writes
`data/manifest.json`. When the page finds the manifest it downloads the game
data (~9 MB) before showing "Click to play" and streams the tunes (~25 MB)
in the background; a tune that hasn't arrived yet plays on the synth.
`--episodes 1` limits the site to episode 1, `--music none` leaves the tunes
out. Without a manifest the page falls back to asking for the player's own
files. The account id is needed because the logged-in Cloudflare user has
more than one account.

### Gamepad

Any controller SDL knows (Xbox, PlayStation, Switch Pro, …, and in the
browser any pad with the standard mapping) works in the desktop builds and in
the browser. The first connected pad is used.

| Pad | During play | In menus |
| --- | --- | --- |
| Left stick | move / strafe | up / down |
| Right stick | turn (smooth, like mouse look; `--pad-turn F` = degrees per second at full deflection, default 200) | |
| RT | fire (short rumble) | |
| LT | run (Right Shift) | |
| A, X | use (Space) | A = select (Enter) |
| B | | back (Esc) |
| LB / RB | previous / next weapon | |
| Y | automap (F9) | |
| D-pad | arrow keys | arrow keys |
| Start / Back | Esc (menu) / Tab (status) | same |

### Smooth motion at 60 fps

The game itself runs at 25 frames a second: each frame the player moves 10
units (a sixth of a map cell) or turns 5 degrees, doors slide, enemies and
projectiles step. With the hi-res renderer the screen is shown at a steady 60
fps (`--fps`), and every one of those frames is drawn with everything placed
between the game's last two frames by the time elapsed since the newest one:
the camera (position and keyboard turning), doors, enemies, items and
projectiles all glide instead of stepping. This costs one game frame (40 ms)
of display delay for movement; mouse turning is not delayed. Mouse motion is
kept as a fraction of a degree: the game turns by whole degrees on its tick
and the view adds the remaining fraction, so turning follows the mouse
continuously. Teleports, new levels, loaded games and pauses snap instead of
gliding. Demos are smoothed the same way (display only; the game state is
untouched). Menus and other 320x200 screens are shown as the game draws
them, and so are palette fades that the game runs as a loop of presents
(e.g. the fade-in at the start of a level), paced by the display as in the
original. In `--classic` mode everything is the original 25 fps.

The game's own *Mouse* option (Configure game) steers by the cursor's offset
from the view centre, with the right button to use; with mouse look active
that cursor is held at the centre, so both can be left on.

### Debug helpers for the renderer

`N3D_SHOTS` saves frames at the render resolution. `N3D_HDCMP=dir`
(`N3D_HDCMP_EVERY=n`) renders the native view at the original 320x200
geometry next to the original renderer and dumps both (raw 320x200 index
images + palette, 2x64000+768 bytes); `N3D_HDPROF=1` prints the native
render time. `N3D_HDCHECK=1` verifies the native texture/sprite pixels and
the angle convention against the game's own loaded data. `N3D_TURNLOG=1`
logs each game-frame snapshot and the camera of every displayed frame. `N3D_MOUSE="ms:dx,ms:L:hold,ms:R:hold"` scripts mouse motion
and buttons like `N3D_KEYS`.

### Overrides

`src/rt/overrides.txt` lists functions replaced by C in
`src/rt/overrides.c`. A line `seg:off wrap` keeps the recompiled body as
`f_SSSS_OOOO_orig` so the override can call it. Regenerate with
`python3 -I tools/recomp.py` after editing the list.

## Pre-rendered music

The music can be rendered once to audio files and played back from them
instead of the live synth: sample-exact loops, no synth CPU cost, and any
bank you like baked in.

### Rendering (`n3d-render-music`)

Built next to the game (`ninja -C build` / `cmake --build build`):

```bash
# the original FM sound: OPL2 emulation + the GM instrument bank inside SND.DAT
./build/n3d-render-music --data original/data

# a General MIDI bank of your own: .sf2 SoundFont or .dls (DLS level 1)
./build/n3d-render-music --data original/data --soundfont /path/to/bank.sf2 --out original/data/music-sc55
```

It extracts the 15 MIDI tunes from `SND.DAT` (entry 0 is the IBK bank,
entries 1–15 are Standard MIDI Files), plays each once through the same
sequencer the game uses (`src/music.c`) and writes `tuneNN.wav` (44.1 kHz,
16-bit stereo) and `tuneNN.ogg`. Each file is one pass of the song followed
by the release tail of its last notes. The loop is the song itself (from frame
0 to the end of the pass). It is stored in a WAV `smpl` chunk and as
`LOOPSTART`/`LOOPLENGTH` Vorbis comments (the RPG Maker convention, so other
players loop it too).

| Option | Meaning |
| --- | --- |
| `--soundfont FILE` | `.sf2` or `.dls` bank (default: FM with the game's IBK bank) |
| `--sb MODEL` | Sound Blaster model for the FM synth: `sb1`, `sb2`, `sbpro1`, `sbpro2`, `sb16` (default) |
| `--out DIR` | output folder (default `<data>/music`, where the game looks) |
| `--format wav\|ogg\|both` | default `both`; OGG uses `oggenc` or `ffmpeg` (libvorbis) from `PATH`, WAV only if neither is installed |
| `--quality Q` | Vorbis quality, default 6 |
| `--tunes 1,4,7` | render only these tunes |
| `--gain DB` | SoundFont output gain. By default the tool picks one gain for all tunes so the loudest one just doesn't clip, and the tunes keep their relative levels |
| `--tail S` | longest release tail kept after the song (default 8 s) |
| `--export-sf2 OUT` | with `--soundfont X.dls`: just write the converted `.sf2` and exit |

DLS banks are converted to a SoundFont in memory (`src/dls.c`): instruments,
GS variation banks and drum kits, key/velocity regions, sample loops, tuning,
volume envelope, pan, gain and LFO vibrato/tremolo. The game's
`--soundfont` option accepts `.dls` too.

### Playback

At startup the game looks for `tuneNN.ogg` or `tuneNN.wav` in `--music DIR`
(or `N3D_MUSIC`). By default that is `<data>/music`, then `./music`. When the
game starts a tune, the host matches the MIDI bytes the game hands to MCI
against the `SND.DAT` entries, and if the folder has a file for that tune it
plays the file instead of the synth. Tunes without a file still use the synth
(SoundFont or FM, as before), and `--music none` turns file playback off.
The game's music volume, the **F2** music toggle and the music on/off setting
work the same way as with the synth. Looping is seamless: at the loop end
playback jumps back to the loop start and the release tail is mixed over the
start of the next pass, which is what the live synth does when it restarts
the song. Files at other sample rates or in mono are converted on load.

### Which bank sounds "original"?

**Recommended:** a Roland GS bank, the sound of Windows' own GM synth. On
macOS, render with the bank the system already includes and put the files in the
default folder:

```bash
./build/n3d-render-music --data original/data \
    --soundfont /System/Library/Components/CoreAudio.component/Contents/Resources/gs_instruments.dls --gain -14
```

On Windows, use `--soundfont C:\Windows\System32\drivers\gm.dls` instead.
The FM options below are kept as alternatives.

* **The 1994 sound for most players** was FM: a Sound Blaster or AdLib
  OPL2/OPL3 driven through the Windows MIDI Mapper. The default render (no
  `--soundfont`) uses the GM FM instrument bank that ships in `SND.DAT`, so
  it is built from the game's own data.
* **Wavetable / Windows GM synth:** Windows' "Microsoft GS Wavetable Synth"
  uses `gm.dls`, a Roland GS sound set (Sound Canvas samples). If you have a
  Windows installation, copy `C:\Windows\System32\drivers\gm.dls` and pass
  it with `--soundfont gm.dls`. macOS includes a similar Roland GS bank at
  `/System/Library/Components/CoreAudio.component/Contents/Resources/gs_instruments.dls`,
  which works the same way.
* **Roland SC-55 / Sound Canvas:** the reference GM/GS synth of the era. Use
  an SC-55 SoundFont you have the rights to, made from your own unit or
  bought or licensed. Many "SC-55" SoundFonts online are unlicensed sample
  rips.

No sound banks are bundled. The rendered files are made from your own copy
of the game data, so keep them private like the rest of `original/`.

### Sound Blaster models

The FM synth emulates the FM chip of a real Sound Blaster with
[Nuked OPL3](https://github.com/nukeykt/Nuked-OPL3), the cycle-accurate
emulator DOSBox Staging uses. It is set up the way each card was built, and
the card's analog output filter comes after it, with the values DOSBox
Staging uses (`opl_filter = auto`). The output is also AC-coupled, like the
card's line out, which removes the chip's small DC offset.

| `--sb` | Card | FM chip | Voices | Stereo | Output filter |
| --- | --- | --- | --- | --- | --- |
| `sb1` | Sound Blaster 1.0 | OPL2 | 9 | mono | 1st order, 12 kHz |
| `sb2` | Sound Blaster 2.0 | OPL2 | 9 | mono | 1st order, 12 kHz |
| `sbpro1` | Sound Blaster Pro | 2 × OPL2 (left/right) | 9 | hard left / centre / right | 1st order, 8 kHz |
| `sbpro2` | Sound Blaster Pro 2.0 | OPL3 | 18 | left / centre / right | 1st order, 8 kHz |
| `sb16` | Sound Blaster 16 (default) | OPL3 | 18 | left / centre / right | none |

MIDI pan (CC 10) below 43 plays on the left only, above 85 on the right
only, and anything in between on both sides. The game and the renderer both
take `--sb`:

```bash
./build/n3d-render-music --data original/data --sb sbpro1 --out original/data/music-sbpro
./build/nitemare3d --data original/data --fm --sb sb2 --music none
```

## macOS app bundle

`tools/make_macos_app.sh [ICON_1024.png] [HDPACK_DIR]` builds a self-contained
`dist/Nitemare3D.app`: the release binary, SDL2, the game data from
`original/data` (including a pre-rendered `music/` folder), an HD pack with only
the splash and main-menu art (`HDPACK_ONLY=id,id` to change), and an icon made
from the HD title screen. It launches fullscreen at the desktop resolution with
`--no-spaces --filter aniso --aniso 16 --upscale scale6x`; pass extra options
with `open dist/Nitemare3D.app --args ...`. The bundle contains the game data,
so don't redistribute it unless you may.
