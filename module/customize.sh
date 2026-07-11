# customize.sh
# Compiled at install time (sourced by the AxManager installer). Resolves the
# device's display refresh-rate -> mode-id map from SurfaceFlinger, writes a
# default config, then wires up the data directory the daemon expects.
ui_print "[*] Querying SurfaceFlinger display configurations..."
DUMPSYS_OUT=$(dumpsys SurfaceFlinger 2>/dev/null)
if [ -n "$DUMPSYS_OUT" ]; then
    echo "$DUMPSYS_OUT" | while read -r line; do
        case "$line" in
            # Match layout: {id=0, hwcId=0, ... refreshRate=60.00 Hz}
            *id=[0-9]*refreshRate=[0-9]*)
                first_part=${line%%,*}
                id=$(echo "$first_part" | tr -cd '0-9')
                rate=$(echo "$line" | sed -nE 's/.*refreshRate=([0-9]+).*/\1/p')
                [ -n "$rate" ] && [ -n "$id" ] && echo "$rate $id"
                ;;
            # Match layout: id=1, ... 120Hz
            *id=[0-9]*Hz*|*id=[0-9]*fps*)
                first_part=${line%%,*}
                id=$(echo "$first_part" | tr -cd '0-9')
                rate=$(echo "$line" | sed -nE 's/.*[^0-9]([0-9]+)(\.[0-9]+)?(Hz|fps).*/\1/p')
                [ -n "$rate" ] && [ -n "$id" ] && echo "$rate $id"
                ;;
            # Match layout: 0: 1080x2400, ... refresh=60.000000
            *[0-9]:*refresh=[0-9]*|*[0-9]:*fps=[0-9]*)
                first_part=${line%%,*}
                id=$(echo "$first_part" | cut -d: -f1 | tr -cd '0-9')
                rate=$(echo "$line" | sed -nE 's/.*(refresh|fps)=([0-9]+).*/\2/p')
                [ -n "$rate" ] && [ -n "$id" ] && echo "$rate $id"
                ;;
        esac
    done | sort -u -k1,1n > "$MODPATH/modes.map"

    ui_print "[+] Resolved Display Modes (RefreshRate -> ID):"
    while read -r hz id; do
        ui_print "    - ${hz}Hz -> ID ${id}"
    done < "$MODPATH/modes.map"
else
    ui_print "[-] Warning: Failed to query SurfaceFlinger. Root mode might require manual ID mapping."
fi

# Extract minimum and maximum refresh rates from modes.map for the fallback config
MIN_RATE=60
MAX_RATE=120
if [ -f "$MODPATH/modes.map" ] && [ -s "$MODPATH/modes.map" ]; then
    MIN_RATE=$(awk '{print $1}' "$MODPATH/modes.map" | sort -n | head -n1)
    MAX_RATE=$(awk '{print $1}' "$MODPATH/modes.map" | sort -n | tail -n1)
    ui_print "[+] Dynamic fallback rates set: Idle $MIN_RATE Hz / Active $MAX_RATE Hz"
fi

# Define a function containing the default configuration
write_default_config() {
    cat << EOF
# ==========================================
# Dynamic FPS Controller Configuration
# ==========================================

# --- Global Parameters ---
# Enable debug logging in logcat
DEBUG = false

# Delay (in ms) before reverting from Active to Idle rate after touch
touchSlackMs = 2000

# Optional SurfaceFlinger policy override for devices that block cross-group switching
enableFrameRateFlex = false

# --- Brightness Controls ---
# Clamp to min rate and stop touch-boosting on low brightness
enableMinBrightness = false

# Brightness percentage to trigger the clamp (0-100%)
minBrightnessThreshold = 10

# --- Battery Saver ---
# Enable battery saving features (true/false)
batterySaver = false

# Battery percentage to trigger power save mode
lowBatteryThreshold = 10

# Max refresh rate when power save is on (Hz)
powerSaveMaxRate = 60

# --- System Rules ---
# Default rates used when an app has no specific rule
defaultIdle = $MIN_RATE
defaultActive = $MAX_RATE

# Refresh rate forced when the display turns off (-1 to do nothing)
offscreenRate = $MIN_RATE

# --- App-Specific Rules ---
# Format: packageName = idleRate activeRate
# Use -1 to inherit the default rates above.

# Games (Locked for thermals and consistent frame pacing)
com.miHoYo.Yuanshen = $MAX_RATE $MAX_RATE
com.hypergryph.arknights = $MAX_RATE $MAX_RATE
com.mobile.legends = $MAX_RATE $MAX_RATE

# Launcher
com.android.launcher3 = 60 60
EOF
}

# Write default configuration directly into the module directory
write_default_config > "$MODPATH/dfps.conf"

# Make the shipped binaries executable (covers dfps + the setup helper).
set_perm_recursive "$MODPATH/system/bin" 0 2000 0755 0755

# Wire the /data/local/tmp/dfps data directory + symlinks via the shared helper.
. "$MODPATH/system/bin/dfps_setup.sh"
ensure_dfps_data_dir
ui_print "[+] DFPS data directory ready at /data/local/tmp/dfps"
