#!/bin/sh
# Build OpenDiag.exe (32-bit Windows, static) with mingw-w64 on macOS/Linux, and run the core tests.
# 32-bit on purpose: most J2534 drivers (e.g. Tactrix op20pt32.dll) are 32-bit DLLs.
set -e
cd "$(dirname "$0")"
c++ -std=c++17 -Wall -Isrc tests/test_core.cpp -o tests/test_core && ./tests/test_core
i686-w64-mingw32-g++ -O2 -static -std=c++17 -mwindows -Wall -Wno-unused-function \
    src/opendiag.cpp -o OpenDiag.exe -luser32 -lgdi32 -lkernel32 -ladvapi32
ls -la OpenDiag.exe
