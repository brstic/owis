#!/bin/sh
# Build PorscheTool.exe (32-bit Windows, static) with mingw-w64 on macOS/Linux, and run the tests.
# 32-bit on purpose: most J2534 drivers (e.g. Tactrix op20pt32.dll) are 32-bit DLLs.
set -e
cd "$(dirname "$0")"
c++ -std=c++17 -O2 -Wall -Isrc tests/test_core.cpp -o tests/test_core && ./tests/test_core
if [ -f data/G1_de.json ] || [ -f data/G1.json ]; then
  c++ -std=c++17 -O2 -Wall -Isrc tests/test_pack.cpp -o tests/test_pack
  ./tests/test_pack "$( [ -f data/G1_de.json ] && echo data/G1_de.json || echo data/G1.json )" | tail -3
fi
i686-w64-mingw32-g++ -O2 -static -std=c++17 -mwindows -Wall -Wno-unused-function \
    src/porschetool.cpp -o PorscheTool.exe -luser32 -lgdi32 -lkernel32 -ladvapi32
ls -la PorscheTool.exe
