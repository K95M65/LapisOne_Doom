#!/bin/sh
# Copyright (C) 2026 K95M65. Licensed under the GNU GPL v2 (see LICENSE and NOTICE).
# Cross-build lapis-doom for the Lapis One (aarch64, fully static) and make an install bundle.
# Needs: aarch64-linux-gnu-gcc (Debian/Ubuntu: apt install gcc-aarch64-linux-gnu), make, tar.
set -e
ROOT=$(cd "$(dirname "$0")" && pwd)
CC=${CC:-aarch64-linux-gnu-gcc}
make -C "$ROOT/engine" -j"$(nproc)" CC="$CC"
OUT="$ROOT/dist/lapis-doom"; rm -rf "$OUT"; mkdir -p "$OUT/bin" "$OUT/share"
cp "$ROOT/engine/lapis-doom" "$OUT/bin/"
$CC -O2 -static -pthread -I"$ROOT/engine" -o "$OUT/bin/lapis-usbpad-test" "$ROOT/lapis/tools/usbpad_cli.c" "$ROOT/engine/usbpad.c"
cp "$ROOT"/package/bin/* "$OUT/bin/"; cp "$ROOT/package/share/dock.bin" "$OUT/share/"
cp "$ROOT/package/install.sh" "$ROOT/package/uninstall.sh" "$ROOT/package/fetch-shareware.sh" "$ROOT/README.md" "$ROOT/LICENSE" "$ROOT/NOTICE" "$OUT/"
chmod +x "$OUT"/bin/* "$OUT"/*.sh
tar -C "$ROOT/dist" -czf "$ROOT/dist/lapis-doom-aarch64.tar.gz" lapis-doom
echo "Built $ROOT/dist/lapis-doom-aarch64.tar.gz"
