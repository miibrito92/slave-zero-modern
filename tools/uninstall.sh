#!/usr/bin/env bash
# Restore the original GOG files saved by tools/install.sh and remove the modern renderer.
set -euo pipefail
here="$(cd "$(dirname "$0")/.." && pwd)"
game="$here/../drive_c/GOG Games/Slave Zero"
backup="$here/backup"
# szOptions.ini is left alone so in-game settings (controls etc.) survive.
for f in szHardware.ini SlaveZero.exe; do
  [ -f "$backup/$f" ] && cp -p "$backup/$f" "$game/$f"
done
rm -f "$game/szmodern.dll"
echo "restored originals (szModern.ini and szModern.log left in place)"
