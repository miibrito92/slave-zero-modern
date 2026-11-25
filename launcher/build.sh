#!/usr/bin/env bash
# Build the launcher for Linux (AppImage) and Windows (.exe).
#   ./build.sh            both
#   ./build.sh linux      Linux AppImage only
#   ./build.sh windows    Windows exe only
# Requires: g++, SDL3 headers/libs (Linux); ~/.local/opt/sz-tools/llvm-mingw-* (Windows);
#           ~/.local/opt/sz-tools/appimagetool (AppImage). The renderer must be built first (../build.sh).
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
root="$here/.."
out="$here/build"
gen="$out/gen"
imgui="$here/third_party/imgui"
what="${1:-all}"
mkdir -p "$gen"

[ -f "$root/build/szmodern.dll" ] || "$root/build.sh"
python3 -I "$here/tools/embed.py" "$gen/payload.h" "kSzModernDll=$root/build/szmodern.dll"
python3 -I "$here/tools/embed.py" "$gen/fonts.h" \
  "kFontExo2=$here/third_party/fonts/Exo2.ttf" "kFontOrbitron=$here/third_party/fonts/Orbitron.ttf"

# icon (generated, not game art)
if [ ! -f "$gen/icon-256.png" ]; then
  magick -size 256x256 xc:none -fill '#0b100c' -draw 'roundrectangle 8,8 247,247 40,40' \
    -stroke '#6fe04a' -strokewidth 6 -fill none -draw 'roundrectangle 14,14 241,241 36,36' \
    -font "$here/third_party/fonts/Orbitron.ttf" -pointsize 118 -fill '#6fe04a' -stroke none \
    -gravity center -annotate +0+4 'SZ' "$gen/icon-256.png"
  magick "$gen/icon-256.png" -define icon:auto-resize=256,64,48,32,16 "$gen/icon.ico"
fi

common_src=("$here/src/app.cpp" "$here/src/ui_style.cpp" "$imgui/imgui.cpp" "$imgui/imgui_draw.cpp"
            "$imgui/imgui_tables.cpp" "$imgui/imgui_widgets.cpp")
inc=(-I"$here/src" -I"$gen" -I"$imgui" -I"$imgui/backends")

if [ "$what" = all ] || [ "$what" = linux ]; then
  mkdir -p "$out/linux"
  g++ -O2 -std=c++17 -Wall -Wno-unused-parameter "${inc[@]}" \
    "${common_src[@]}" "$here/src/platform_linux.cpp" "$here/src/main_sdl.cpp" \
    "$imgui/backends/imgui_impl_sdl3.cpp" "$imgui/backends/imgui_impl_opengl3.cpp" \
    -o "$out/linux/slavezero-launcher" -static-libstdc++ -static-libgcc \
    -Wl,-rpath,'$ORIGIN/../lib' $(pkg-config --cflags --libs sdl3) -lGL -ldl
  echo "built $out/linux/slavezero-launcher"

  appdir="$out/linux/AppDir"
  rm -rf "$appdir"
  mkdir -p "$appdir/usr/bin" "$appdir/usr/lib"
  cp "$out/linux/slavezero-launcher" "$appdir/usr/bin/"
  cp -L "$(pkg-config --variable=libdir sdl3)/libSDL3.so.0" "$appdir/usr/lib/"
  cp "$gen/icon-256.png" "$appdir/slavezero-launcher.png"
  cat > "$appdir/slavezero-launcher.desktop" <<'EOF'
[Desktop Entry]
Type=Application
Name=Slave Zero Launcher
Comment=Configure and launch Slave Zero with the modern renderer
Exec=slavezero-launcher
Icon=slavezero-launcher
Categories=Game;
EOF
  cat > "$appdir/AppRun" <<'EOF'
#!/bin/sh
HERE="$(dirname "$(readlink -f "$0")")"
export APPDIR="${APPDIR:-$HERE}"
exec "$HERE/usr/bin/slavezero-launcher" "$@"
EOF
  chmod +x "$appdir/AppRun"
  ARCH=x86_64 "$HOME/.local/opt/sz-tools/appimagetool" --no-appstream "$appdir" "$out/SlaveZero-Launcher-x86_64.AppImage" \
    > "$out/linux/appimagetool.log" 2>&1 || { cat "$out/linux/appimagetool.log"; exit 1; }
  echo "built $out/SlaveZero-Launcher-x86_64.AppImage"
fi

if [ "$what" = all ] || [ "$what" = windows ]; then
  tc="$(ls -d "$HOME"/.local/opt/sz-tools/llvm-mingw-* | head -1)"
  mkdir -p "$out/windows"
  printf 'APPICON ICON "%s"\n' "$gen/icon.ico" > "$gen/launcher.rc"
  "$tc/bin/x86_64-w64-mingw32-windres" "$gen/launcher.rc" -O coff -o "$out/windows/launcher.res"
  "$tc/bin/x86_64-w64-mingw32-clang++" -O2 -std=c++17 -municode -mwindows -static \
    -Wall -Wno-unused-parameter "${inc[@]}" \
    "${common_src[@]}" "$here/src/platform_win.cpp" "$here/src/main_win32.cpp" \
    "$imgui/backends/imgui_impl_win32.cpp" "$imgui/backends/imgui_impl_dx11.cpp" "$out/windows/launcher.res" \
    -o "$out/SlaveZero-Launcher.exe" -ld3d11 -ldxgi -ld3dcompiler -ldwmapi -lole32 -lshell32 -luuid
  echo "built $out/SlaveZero-Launcher.exe"
fi
