#!/usr/bin/env bash
# Re-run the Ghidra headless decompilation into ref/decomp/.
# Tools are user-local under ~/.local/opt/sz-tools (JDK 25, Ghidra 12.2, llvm-mingw).
set -euo pipefail
here="$(cd "$(dirname "$0")/.." && pwd)"
tools="$HOME/.local/opt/sz-tools"
game="$here/../drive_c/GOG Games/Slave Zero"

export JAVA_HOME="$(ls -d "$tools"/jdk-* | head -1)"
export PATH="$JAVA_HOME/bin:$PATH"

mkdir -p "$here/ref/bin" "$here/ref/decomp" "$here/build/ghidra"
cp "$game/3dfx.dll" "$game/d3d.dll" "$game/SlaveZero.exe" \
   "$game/Scripts/wHud.dll" "$game/Scripts/wCamera.dll" "$here/ref/bin/"

"$(ls -d "$tools"/ghidra_*/support | head -1)/analyzeHeadless" \
  "$here/build/ghidra" sz \
  -import "$here"/ref/bin/* -overwrite \
  -scriptPath "$here/tools/ghidra_scripts" \
  -postScript ExportDecomp.java "$here/ref/decomp"
