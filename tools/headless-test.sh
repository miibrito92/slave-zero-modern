#!/usr/bin/env bash
# Run the game in an invisible gamescope session, capture screenshots, then exit.
#   tools/headless-test.sh "60,300,900" 1000 [seconds-timeout]
set -uo pipefail
here="$(cd "$(dirname "$0")/.." && pwd)"
prefix="$here/.."
game="$prefix/drive_c/GOG Games/Slave Zero"
shots="${1:-60,300,900}"
exitf="${2:-1000}"
tmo="${3:-180}"

rm -f "$game"/szm_shot_*.bmp "$game/szModern.log"
export WINEPREFIX="$prefix"
export PROTONPATH="$HOME/.local/share/Steam/compatibilitytools.d/GE-Proton11-14-x86_64"
export GAMEID=umu-0 UMU_RUNTIME_UPDATE=0
export SZM_SHOTS="$shots" SZM_EXIT="$exitf" SZM_KEYS="${4:-}"
export PULSE_SERVER=unix:/nonexistent   # no audio during tests
cd "$game"
timeout --kill-after=10 "$tmo" gamescope --backend headless -W 3840 -H 2160 -w 3840 -h 2160 -- \
  umu-run "$game/SlaveZero.exe" > "$here/build/headless.log" 2>&1
echo "exit code $?"
ls -la "$game"/szm_shot_*.bmp 2>/dev/null
echo "--- szModern.log"; cat "$game/szModern.log" 2>/dev/null | head -60
