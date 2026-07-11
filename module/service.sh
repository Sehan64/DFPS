#!/system/bin/sh
MODDIR=${0%/*}
. "$MODDIR/system/bin/dfps_setup.sh"

BIN="$MODDIR/system/bin/dfps"
LOG="$MODDIR/dfps.log"
PIDF="$MODDIR/dfps.pid"

# Stop any previous instance gracefully. The daemon installs a SIGTERM handler
# and tears down cleanly, so prefer TERM and only escalate to KILL if it hangs.
if pgrep -x dfps >/dev/null 2>&1; then
    pkill -TERM -x dfps 2>/dev/null
    i=0
    while pgrep -x dfps >/dev/null 2>&1 && [ "$i" -lt 20 ]; do
        sleep 0.25
        i=$((i + 1))
    done
    pkill -KILL -x dfps 2>/dev/null
fi

# Make sure the data directory / symlinks exist, then launch detached so the
# daemon survives this boot script's shell. Logs go to MODDIR/dfps.log and the
# pid is recorded for clean teardown.
ensure_dfps_data_dir

nohup "$BIN" >"$LOG" 2>&1 &
echo $! >"$PIDF"
