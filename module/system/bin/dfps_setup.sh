#!/system/bin/sh
# Shared setup helper for the DFPS plugin.
#
# Sourced by customize.sh (install time) and service.sh (every boot) so the
# on-disk data directory the daemon expects is always present and points at
# THIS plugin instance — even after a reinstall that moves MODPATH.
#
# IMPORTANT: dfps always reads its config/modes from /data/local/tmp/dfps/
# and watches that directory for live reloads (see src/config.c / src/touch.c).
# That path is a hard contract baked into the daemon; do not change it here
# without rebuilding the daemon to match.

ensure_dfps_data_dir() {
    # When sourced, $0 is the caller's path (customize.sh / service.sh), whose
    # parent directory is the plugin root = MODDIR.
    MODDIR=${0%/*}
    DATADIR=/data/local/tmp/dfps

    mkdir -p "$DATADIR"

    # Idempotent: -f replaces any stale symlink a previous install may have
    # left pointing at an old MODPATH.
    ln -sf "$MODDIR/system/bin/dfps" "$DATADIR/dfps"
    ln -sf "$MODDIR/modes.map"        "$DATADIR/modes.map"
    ln -sf "$MODDIR/dfps.conf"        "$DATADIR/dfps.conf"
}
