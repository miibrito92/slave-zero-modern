#!/usr/bin/env bash
# Install the modern renderer into the GOG Slave Zero folder.
#  - copies build/szmodern.dll and (if absent) dist/szModern.ini
#  - makes "Modern" display mode 0 in szHardware.ini (Glide kept as mode 1)
#  - NOPs the Win9x-only CLI/STI pair in SlaveZero.exe's CPU-speed probe (crashes on modern OSes)
# Originals are kept in backup/ (first run only). Undo with tools/uninstall.sh.
set -euo pipefail
here="$(cd "$(dirname "$0")/.." && pwd)"
game="$here/../drive_c/GOG Games/Slave Zero"
backup="$here/backup"
mkdir -p "$backup"
for f in szHardware.ini szOptions.ini SlaveZero.exe; do
  [ -f "$backup/$f" ] || cp -p "$game/$f" "$backup/$f"
done

cp "$here/build/szmodern.dll" "$game/szmodern.dll"
[ -f "$game/szModern.ini" ] || cp "$here/dist/szModern.ini" "$game/szModern.ini"

printf '%s\r\n' '[Main]' 'Num Display Modes=2' \
  '[Display 0]' 'Renderer Name=szmodern.dll' 'Device ID=Slave Zero Modern Renderer' 'Menu Description=Modern' \
  'Device Index=0' 'Width=640' 'Height=480' 'Bpp=16' \
  '[Display 1]' 'Renderer Name=3dfx.dll' 'Device ID=3dfx Glide Device Driver' 'Menu Description=Glide' \
  'Device Index=0' 'Width=640' 'Height=480' 'Bpp=16' > "$game/szHardware.ini"
sed -i 's/^Display Mode Wanted=.*/Display Mode Wanted=0\r/' "$game/szOptions.ini"

python3 -I - "$game/SlaveZero.exe" <<'EOF'
import sys
p = sys.argv[1]
d = bytearray(open(p, 'rb').read())
for off, orig in ((0x1cc49, 0xfa), (0x1cc81, 0xfb)):  # cli / sti at VA 0x41d849 / 0x41d881
    if d[off] not in (orig, 0x90):
        sys.exit(f"unexpected byte {d[off]:#x} at {off:#x}; is this the GOG SlaveZero.exe?")
    d[off] = 0x90
open(p, 'wb').write(d)
EOF
echo "installed into $game"
