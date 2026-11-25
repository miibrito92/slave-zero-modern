#!/usr/bin/env bash
# Build szmodern.dll (32-bit Windows DLL) with the user-local llvm-mingw toolchain.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
tc="$(ls -d "$HOME"/.local/opt/sz-tools/llvm-mingw-* | head -1)"
cxx="$tc/bin/i686-w64-mingw32-clang++"
mkdir -p "$here/build"
"$cxx" -O2 -std=c++17 -shared -static -fno-exceptions -fno-rtti \
  -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers \
  -o "$here/build/szmodern.dll" "$here/src/szmodern.cpp" \
  -ld3d11 -ldxgi -ld3dcompiler_47 -luuid
echo "built $here/build/szmodern.dll"
