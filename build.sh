#!/usr/bin/env bash
# Build szmodern.dll (32-bit Windows DLL) with the user-local llvm-mingw toolchain.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
tc="$(ls -d "$HOME"/.local/opt/sz-tools/llvm-mingw-* | head -1)"
cxx="$tc/bin/i686-w64-mingw32-clang++"
imgui="$here/launcher/third_party/imgui"
fonts="$here/launcher/third_party/fonts"
obj="$here/build/obj"
gen="$here/build/gen"
mkdir -p "$here/build" "$obj" "$gen"
python3 -I "$here/launcher/tools/embed.py" "$gen/fonts.h" "kFontExo2=$fonts/Exo2.ttf" "kFontOrbitron=$fonts/Orbitron.ttf"

# Dear ImGui (shared with the launcher), compiled once
for f in imgui imgui_draw imgui_tables imgui_widgets backends/imgui_impl_win32 backends/imgui_impl_dx11; do
  o="$obj/$(basename "$f").o"
  if [ ! -f "$o" ] || [ "$imgui/$f.cpp" -nt "$o" ]; then
    "$cxx" -O2 -std=c++17 -fno-exceptions -fno-rtti -DIMGUI_DISABLE_WIN32_DEFAULT_IME_FUNCTIONS \
      -I"$imgui" -c "$imgui/$f.cpp" -o "$o"
  fi
done

flags=(-O2 -std=c++17 -fno-exceptions -fno-rtti -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers
       -DIMGUI_DISABLE_WIN32_DEFAULT_IME_FUNCTIONS -I"$imgui" -I"$imgui/backends" -I"$gen")
"$cxx" "${flags[@]}" -c "$here/src/szmodern.cpp" -o "$obj/szmodern.o"
"$cxx" "${flags[@]}" -c "$here/src/overlay.cpp" -o "$obj/overlay.o"
"$cxx" -shared -static -o "$here/build/szmodern.dll" "$obj/szmodern.o" "$obj/overlay.o" \
  "$obj"/imgui.o "$obj"/imgui_draw.o "$obj"/imgui_tables.o "$obj"/imgui_widgets.o \
  "$obj"/imgui_impl_win32.o "$obj"/imgui_impl_dx11.o \
  -ld3d11 -ldxgi -ld3dcompiler_47 -luuid -lwinmm -lpsapi -ldwmapi -lgdi32
echo "built $here/build/szmodern.dll"
