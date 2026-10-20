#!/usr/bin/env python3
"""Assemble the static site for hosting the browser build with game data.

    python3 -I tools/web_site.py --data original/data [--episodes 1] [--out build-web/site]

Copies the page shell and nitemare3d.js/.wasm from the Emscripten build
folder, the game data files into data/ and the pre-rendered tunes into
data/music/, and writes data/manifest.json. The page (web/n3d-web.js) finds
the manifest, downloads the data before showing "Click to play" and streams
the music in the background. Deploy the folder with `wrangler deploy` (see
wrangler.jsonc) or any static host.
"""
import argparse
import hashlib
import json
import os
import shutil
import sys

SHELL = ["index.html", "n3d-web.js", "n3d-web.css", "nitemare3d.js", "nitemare3d.wasm"]
COMMON = ["GAME.PAL", "NITE3D.BSF", "SND.DAT", "UIF.DAT", "DEMO.1", "ENDING.FLI"]
PER_EPISODE = ["MAP.{}", "IMG.{}"]


def find_ci(folder, name):
    """Path of name inside folder, matched case-insensitively (or None)."""
    if not os.path.isdir(folder):
        return None
    for entry in os.listdir(folder):
        if entry.lower() == name.lower():
            return os.path.join(folder, entry)
    return None


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--build", default="build-web", help="Emscripten build folder (default build-web)")
    ap.add_argument("--data", required=True, help="game data folder (MAP.1, IMG.1, SND.DAT, ...)")
    ap.add_argument("--music", help="pre-rendered tuneNN.ogg folder (default <data>/music; 'none' to skip)")
    ap.add_argument("--episodes", default="1,2,3", help="episodes to include, e.g. 1 (default 1,2,3)")
    ap.add_argument("--out", default=None, help="output folder (default <build>/site)")
    args = ap.parse_args()

    out = args.out or os.path.join(args.build, "site")
    if os.path.exists(out):
        shutil.rmtree(out)
    os.makedirs(os.path.join(out, "data", "music"))

    for f in SHELL:
        src = os.path.join(args.build, f)
        if not os.path.isfile(src):
            sys.exit(f"missing {src}: build the browser version first (README, 'Browser (WebAssembly)')")
        shutil.copy2(src, os.path.join(out, f))

    names = list(COMMON)
    for ep in [e.strip() for e in args.episodes.split(",") if e.strip()]:
        names += [n.format(ep) for n in PER_EPISODE]

    manifest = {"files": [], "music": []}
    digest = hashlib.sha256()
    for name in names:
        src = find_ci(args.data, name)
        if not src:
            if name in ("MAP.1", "IMG.1", "SND.DAT", "UIF.DAT"):
                sys.exit(f"missing required data file {name} in {args.data}")
            print(f"skipping {name} (not in {args.data})")
            continue
        dest = name.lower()
        shutil.copy2(src, os.path.join(out, "data", dest))
        size = os.path.getsize(src)
        manifest["files"].append({"name": dest, "size": size})
        with open(src, "rb") as fh:
            digest.update(fh.read())

    music = args.music or os.path.join(args.data, "music")
    if music != "none" and os.path.isdir(music):
        for t in range(1, 100):
            for ext in ("ogg", "wav"):
                src = find_ci(music, f"tune{t:02d}.{ext}")
                if src:
                    dest = f"tune{t:02d}.{ext}"
                    shutil.copy2(src, os.path.join(out, "data", "music", dest))
                    manifest["music"].append({"name": dest, "size": os.path.getsize(src)})
                    break

    manifest["version"] = digest.hexdigest()[:12]
    with open(os.path.join(out, "data", "manifest.json"), "w") as fh:
        json.dump(manifest, fh, indent=1)

    total = sum(f["size"] for f in manifest["files"]) + sum(f["size"] for f in manifest["music"])
    print(f"{out}: {len(manifest['files'])} data files, {len(manifest['music'])} tunes, {total / 1e6:.1f} MB of game data")


if __name__ == "__main__":
    main()
