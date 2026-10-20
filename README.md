# Nitemare-3D, rebuilt for modern computers

Nitemare-3D is a 1994 first-person horror game for Windows 3.1 by David P.
Gray. This project makes the original game run on today's Mac, Linux and
Windows computers, and in a web browser.

It is still the original game. A program reads the old Windows 3.1
executable and converts it into modern code, so the levels, enemies,
weapons, menus and save games all behave exactly as they did in 1994. On top
of that you get:

- any screen size, including widescreen and ultrawide
- smooth 60 fps movement (the original ran at 25)
- sharper textures
- WASD and mouse look
- gamepad support
- full-screen play in a browser

**Play it in your browser:** https://nitemare3d.everydaypreview.com

## You need your own copy of the game

This repository has none of the game's files. You need the original Windows
installer, `win3_Nite3d3x.zip`. The game files and the converted game code
are made from that file on your own computer and never go into this
repository.

## Build and play (Mac)

1. Install the tools:

   ```bash
   brew install sdl2 cmake ghidra openjdk@21
   ```

2. Unpack the game files from your installer:

   ```bash
   python3 tools/extract_installer.py /path/to/win3_Nite3d3x.zip original
   ```

3. Convert the game:

   ```bash
   GHIDRA_INSTALL_DIR=/opt/homebrew/opt/ghidra/libexec JAVA_HOME=/opt/homebrew/opt/openjdk@21 tools/build_gen.sh
   ```

4. Build it:

   ```bash
   cmake -S . -B build && cmake --build build -j
   ```

5. Play:

   ```bash
   ./build/nitemare3d --data original/data
   ```

You can build on Linux and Windows too. See [docs/TECHNICAL.md](docs/TECHNICAL.md) for how.

## Controls

| Key | Action |
| --- | --- |
| W A S D or arrow keys | move and turn |
| Mouse | look around |
| Left click or Ctrl | fire |
| Right click or Space | open doors, use things |
| 1–4 | pick a weapon |
| Esc | menu |
| Tab | status |
| F9 / F10 | maps |
| F5 / F4 | quick save / quick load |
| Alt+Enter | full screen |

Gamepads work as well: left stick moves, right stick turns, right trigger
fires, A uses.

## Useful options

| Option | What it does |
| --- | --- |
| `--fullscreen` | start in full screen |
| `--classic` | the original 320×200 look |
| `--soundfont FILE.sf2` | better music, using any General MIDI SoundFont |
| `--fm` | the 1994 Sound Blaster music sound |

Put an option at the end of the play command, for example
`./build/nitemare3d --data original/data --classic`. Every other option is
listed in [docs/TECHNICAL.md](docs/TECHNICAL.md).

## How it works

[docs/TECHNICAL.md](docs/TECHNICAL.md) explains how the conversion works,
along with the web build, music rendering, the renderer and the debugging
tools.

## Legal

Nitemare-3D is © David P. Gray / Gray Design Associates. This repository
contains only new code and some open-source libraries (Nuked OPL3,
TinySoundFont, stb_vorbis). It does not include the game itself. Don't
commit game files or converted game code to it.
