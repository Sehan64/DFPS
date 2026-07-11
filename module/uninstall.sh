#!/system/bin/sh
MODDIR=${0%/*}

# Stop the daemon gracefully if it is running.
if pgrep -x dfps >/dev/null 2>&1; then
    pkill -TERM -x dfps 2>/dev/null
    i=0
    while pgrep -x dfps >/dev/null 2>&1 && [ "$i" -lt 20 ]; do
        sleep 0.25
        i=$((i + 1))
    done
    pkill -KILL -x dfps 2>/dev/null
fi

# Remove runtime artifacts this plugin created inside its own folder.
rm -f "$MODDIR/dfps.log" "$MODDIR/dfps.pid"

# Clean up leftovers from older versions / the data directory.
rm -f /data/local/tmp/tx_code.txt 2>/dev/null
rm -rf /data/local/tmp/dfps

echo "[+] DFPS uninstalled."
