#!/bin/sh
# Build a self-contained macOS app bundle: dist/Nitemare3D.app
#
#   tools/make_macos_app.sh [ICON_1024.png] [HDPACK_DIR]
#
# Bundles the release binary, SDL2, the game data (original/data incl. the
# pre-rendered music/), an HD replacement pack and an icon. The app launches
# fullscreen at desktop resolution with anisotropic 16x filtering and scale6x
# upscaling; extra arguments in N3D_ARGS are appended.
#
# ICON defaults to an icon built from the HD title screen in the pack.
# HDPACK_DIR defaults to a fresh `re/upscale/pack.py` build of the splash and
# main-menu art only (HDPACK_ONLY=id,id overrides; needs numpy+Pillow).
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
ICON=${1:-}
PACK=${2:-}
PY=${PYTHON:-python3}
APP="$ROOT/dist/Nitemare3D.app"
TMP=$(mktemp -d)

cmake -S "$ROOT" -B "$ROOT/build" -G Ninja -DCMAKE_BUILD_TYPE=Release > /dev/null
ninja -C "$ROOT/build" nitemare3d

if [ -z "$PACK" ]; then
  PACK="$TMP/hdpack"
  # Only the splash (ui_03) and main menu (ui_06) art is replaced; the rest of
  # the game keeps its original pixels (plus the runtime upscaler).
  (cd "$ROOT/re/upscale" && "$PY" pack.py "$PACK" --only "${HDPACK_ONLY:-ui_03,ui_06}")
fi

if [ -z "$ICON" ]; then
  ICON="$TMP/icon_1024.png"
  "$PY" - "$PACK" "$ICON" <<'EOF'
import sys
import numpy as np
from PIL import Image, ImageDraw, ImageFilter
pack, out = sys.argv[1], sys.argv[2]
line = [l.split() for l in open(pack + '/index.txt') if l.split()[-1] == 'ui_03'][0]
W, H = int(line[2]) * int(line[4]), int(line[3]) * int(line[4])
a = np.fromfile(pack + '/ui_03.rgba', dtype='<u4').reshape(W, H).T
title = Image.fromarray(np.stack([(a >> 16) & 255, (a >> 8) & 255, a & 255], -1).astype(np.uint8))
side = H  # square crop: logo + house on the left of the title screen
tile = title.crop((0, 0, side, side)).resize((824, 824), Image.LANCZOS).convert('RGBA')
mask = Image.new('L', (824, 824), 0)
ImageDraw.Draw(mask).rounded_rectangle((0, 0, 823, 823), radius=185, fill=255)
tile.putalpha(mask)
canvas = Image.new('RGBA', (1024, 1024), (0, 0, 0, 0))
sm = Image.new('L', (1024, 1024), 0)
ImageDraw.Draw(sm).rounded_rectangle((100, 112, 923, 935), radius=185, fill=110)
shadow = Image.new('RGBA', (1024, 1024), (0, 0, 0, 0))
shadow.putalpha(sm.filter(ImageFilter.GaussianBlur(14)))
canvas = Image.alpha_composite(canvas, shadow)
canvas.alpha_composite(tile, (100, 100))
canvas.save(out)
EOF
fi

rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Frameworks" "$APP/Contents/Resources"

# Icon
ICONSET="$TMP/AppIcon.iconset"
mkdir -p "$ICONSET"
for s in 16 32 128 256 512; do
  sips -z $s $s "$ICON" --out "$ICONSET/icon_${s}x${s}.png" > /dev/null
  d=$((s * 2))
  sips -z $d $d "$ICON" --out "$ICONSET/icon_${s}x${s}@2x.png" > /dev/null
done
iconutil -c icns "$ICONSET" -o "$APP/Contents/Resources/AppIcon.icns"

# Binary + SDL2 (named -bin: the bundle filesystem is usually case-insensitive,
# so "nitemare3d" would collide with the "Nitemare3D" launcher)
cp "$ROOT/build/nitemare3d" "$APP/Contents/MacOS/nitemare3d-bin"
SDL=$(otool -L "$ROOT/build/nitemare3d" | awk '/libSDL2/ {print $1}')
cp -L "$SDL" "$APP/Contents/Frameworks/libSDL2-2.0.0.dylib"
chmod u+w "$APP/Contents/Frameworks/libSDL2-2.0.0.dylib"
install_name_tool -id @rpath/libSDL2-2.0.0.dylib "$APP/Contents/Frameworks/libSDL2-2.0.0.dylib"
install_name_tool -change "$SDL" @executable_path/../Frameworks/libSDL2-2.0.0.dylib "$APP/Contents/MacOS/nitemare3d-bin"

# Game data, music pack, HD pack
mkdir -p "$APP/Contents/Resources/data"
for f in "$ROOT"/original/data/*; do
  case "$(basename "$f")" in music-*) continue ;; esac
  cp -R "$f" "$APP/Contents/Resources/data/"
done
cp -R "$PACK" "$APP/Contents/Resources/hdpack"

# Launcher
cat > "$APP/Contents/MacOS/Nitemare3D" <<'EOF'
#!/bin/sh
C="$(cd "$(dirname "$0")/.." && pwd)"
exec "$C/MacOS/nitemare3d-bin" --data "$C/Resources/data" --hd-pack "$C/Resources/hdpack" \
  --res desktop --fullscreen --no-spaces --filter aniso --aniso 16 --upscale scale6x $N3D_ARGS "$@"
EOF
chmod +x "$APP/Contents/MacOS/Nitemare3D"

cat > "$APP/Contents/Info.plist" <<'EOF'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleName</key><string>Nitemare-3D</string>
  <key>CFBundleDisplayName</key><string>Nitemare-3D</string>
  <key>CFBundleIdentifier</key><string>com.local.nitemare3d</string>
  <key>CFBundleVersion</key><string>1.6</string>
  <key>CFBundleShortVersionString</key><string>1.6</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>CFBundleExecutable</key><string>Nitemare3D</string>
  <key>CFBundleIconFile</key><string>AppIcon</string>
  <key>LSMinimumSystemVersion</key><string>11.0</string>
  <key>LSApplicationCategoryType</key><string>public.app-category.action-games</string>
  <key>NSHighResolutionCapable</key><true/>
</dict>
</plist>
EOF

# Ad-hoc sign (inside-out) so Gatekeeper treats the local build as one unit.
codesign --force -s - "$APP/Contents/Frameworks/libSDL2-2.0.0.dylib"
codesign --force -s - "$APP/Contents/MacOS/nitemare3d-bin"
codesign --force -s - "$APP"
rm -rf "$TMP"
du -sh "$APP"
echo "built $APP"
