#!/bin/sh
# Runs the tests (native, no car needed), then builds PorscheTool.exe (32-bit Windows, static) with
# mingw-w64 on macOS/Linux, the portable command-line tool and the mock adapter for end-to-end tests.
# 32-bit on purpose: most J2534 drivers (e.g. Tactrix op20pt32.dll) are 32-bit DLLs.
set -e
cd "$(dirname "$0")"
mkdir -p build
CXX="${CXX:-c++}"
$CXX -std=c++17 -O2 -Wall -Isrc tests/test_core.cpp -o build/test_core && ./build/test_core
PACK=""; for f in data/G1_de.json data/G1.json; do [ -f "$f" ] && PACK="$f" && break; done
if [ -n "$PACK" ]; then
  $CXX -std=c++17 -O2 -Wall -Isrc tests/test_pack.cpp -o build/test_pack && ./build/test_pack "$PACK" | tail -2
  $CXX -std=c++17 -O2 -Wall -Isrc tests/test_engine.cpp -o build/test_engine && ./build/test_engine "$PACK" build | tail -1
else
  echo "(no data pack in data/ - pack and engine tests skipped; build one with tools/make_packs.py)"
fi
$CXX -std=c++17 -O2 -Wall -Isrc src/cli.cpp -o build/porschetool-cli
if command -v i686-w64-mingw32-g++ >/dev/null 2>&1; then
  i686-w64-mingw32-g++ -O2 -static -std=c++17 -mwindows -Wall -Wno-unused-function -Isrc \
      src/porschetool.cpp -o PorscheTool.exe -luser32 -lgdi32 -lkernel32 -ladvapi32 -lshell32
  i686-w64-mingw32-g++ -shared -static -O2 -std=c++17 -Isrc tests/mock_j2534.cpp -o build/op20pt32.dll -Wl,--kill-at
  ls -la PorscheTool.exe build/op20pt32.dll
else
  echo "(mingw-w64 not found - Windows app not built)"
fi
ls -la build/porschetool-cli
