#!/system/bin/sh
MODDIR=${0%/*}
. "$MODDIR/system/bin/dfps_setup.sh"

BIN="$MODDIR/system/bin/dfps"
LOG="$MODDIR/dfps.log"
PIDF="$MODDIR/dfps.pid"

stop_daemon() {
    if pgrep -x dfps >/dev/null 2>&1; then
        pkill -TERM -x dfps 2>/dev/null
        i=0
        while pgrep -x dfps >/dev/null 2>&1 && [ "$i" -lt 20 ]; do
            sleep 0.25
            i=$((i + 1))
        done
        pkill -KILL -x dfps 2>/dev/null
    fi
    rm -f "$PIDF"
}

start_daemon() {
    ensure_dfps_data_dir
    nohup "$BIN" >"$LOG" 2>&1 &
    echo $! >"$PIDF"
}

case "${1:-status}" in
    start)
        start_daemon
        echo "[+] DFPS started."
        ;;
    stop)
        stop_daemon
        echo "[+] DFPS stopped."
        ;;
    *)
        if pgrep -x dfps >/dev/null 2>&1; then
            echo "Dynamic FPS is RUNNING."
            echo "--------------------------"
            echo "Current configuration:"
            cat "/data/local/tmp/dfps/dfps.conf"
        else
            echo "Dynamic FPS is STOPPED. Initializing..."
            start_daemon
            echo "[+] Started."
        fi
        ;;
esac
