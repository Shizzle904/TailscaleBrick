#!/bin/sh
APPDIR="$(dirname "$0")"
cd "$APPDIR" || exit 1
export LD_LIBRARY_PATH="/usr/trimui/lib:$APPDIR/lib:$LD_LIBRARY_PATH"
export SDL_NOMOUSE=1
chmod +x ./tailscale_ui ./bin/tailscale ./bin/tailscaled ./scripts/daemon.sh 2>/dev/null || true

echo "=== launch $(date) ===" >> app.log
./tailscale_ui >> app.log 2>&1
RC=$?
echo "=== exit rc=$RC ===" >> app.log

exit $RC
