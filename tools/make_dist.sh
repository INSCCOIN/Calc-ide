#!/bin/sh
# Build every firmware variant and the release zip into dist/.
#   PICO_SDK_PATH=/path/to/pico-sdk ./tools/make_dist.sh
set -e
cd "$(dirname "$0")/.."
: "${PICO_SDK_PATH:?set PICO_SDK_PATH to your pico-sdk}"
V=0.1
mkdir -p dist
build() {   # dir board sdboot
    cmake -S . -B "$1" -DPICO_BOARD="$2" -DCALCIDE_SDBOOT="$3" -DCMAKE_BUILD_TYPE=Release >/dev/null
    cmake --build "$1" -j >/dev/null
}
build build-pico2        pico2 OFF
build build-pico         pico  OFF
build build-pico2-sdboot pico2 ON
build build-pico-sdboot  pico  ON
cp build-pico2/calcide.uf2        dist/calcide-$V-rp2350.uf2
cp build-pico/calcide.uf2         dist/calcide-$V-rp2040.uf2
cp build-pico2-sdboot/calcide.bin dist/calcide-$V-rp2350-sdboot.bin
cp build-pico-sdboot/calcide.bin  dist/calcide-$V-rp2040-sdboot.bin
rm -f dist/calcide-$V.zip
tmp=$(mktemp -d)
git archive --format=tar --prefix=calcide-$V/ HEAD | tar -x -C "$tmp"
mkdir -p "$tmp/calcide-$V/dist"
cp dist/*.uf2 dist/*.bin "$tmp/calcide-$V/dist/"
(cd "$tmp" && zip -qr calcide-$V.zip calcide-$V)
mv "$tmp/calcide-$V.zip" dist/
rm -rf "$tmp"
ls -l dist
