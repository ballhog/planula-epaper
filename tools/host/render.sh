#!/usr/bin/env bash
# Build lib/panel on this computer, render the sample screens to PNG and run
# the logic tests. Needs a C++17 compiler and Python with Pillow.
#   tools/host/render.sh
set -euo pipefail
cd "$(dirname "$0")"
ROOT=../..
GFX="${GFX_DIR:-$ROOT/.pio/libdeps/epaper/Adafruit GFX Library}"
JSON="${JSON_DIR:-$ROOT/.pio/libdeps/epaper/ArduinoJson/src}"
if [ ! -f "$GFX/Adafruit_GFX.cpp" ]; then
  echo "Adafruit GFX not found. Run 'pio pkg install' (or 'pio run') once first." >&2
  exit 1
fi
mkdir -p out
cc -O1 -c "$ROOT/lib/qrcodegen/src/qrcodegen.c" -o out/qrcodegen.o
c++ -std=c++17 -DARDUINO=200 -DARDUINOJSON_ENABLE_ARDUINO_STRING=0 -DARDUINOJSON_ENABLE_ARDUINO_STREAM=0 -DARDUINOJSON_ENABLE_ARDUINO_PRINT=0 -DARDUINOJSON_ENABLE_PROGMEM=0 -O1 -Wall -Wno-unused-function \
  -Istub -I"$GFX" -I"$JSON" -I"$ROOT/lib/panel/src" -I"$ROOT/lib/qrcodegen/src" -I"$ROOT/lib/sigv4/src" \
  host_main.cpp "$ROOT"/lib/panel/src/*.cpp "$ROOT"/lib/sigv4/src/*.cpp "$GFX/Adafruit_GFX.cpp" out/qrcodegen.o -o out/host
./out/host render
python3 - <<'EOF'
import glob, os
from PIL import Image
for p in sorted(glob.glob('out/*.pbm')):
    im = Image.open(p).convert('L')
    # e-paper look: warm paper, soft black
    im = im.point(lambda v: 236 if v else 28)
    im.save(p[:-4] + '.png')
    os.remove(p)
print('PNG in tools/host/out/')
EOF
