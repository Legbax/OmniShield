#!/system/bin/sh
# OmniShield — Boot Completed
# Runs AFTER BRENE's boot-completed.sh (alphabetically: brene < omnishield).
# Calls resetprop for all profile-specific properties to eliminate dual values.
# VDInfo reads the property area memory directly, bypassing OmniShield's hooks.
# resetprop modifies the actual property area values so even direct reads match.

PROPS_FILE="/data/adb/.omni_data/.profile_props"

# If .profile_props doesn't exist yet, the companion hasn't run.
# Wait briefly for first scoped app launch (companion generates the file).
if [ ! -f "$PROPS_FILE" ]; then
    for _i in 1 2 3 4 5; do
        sleep 2
        [ -f "$PROPS_FILE" ] && break
    done
fi

[ ! -f "$PROPS_FILE" ] && exit 0

# Apply resetprop for each property.
# resetprop -n = non-persistent (doesn't survive reboot, correct since we re-apply each boot)
while IFS='=' read -r key value; do
    # Skip empty lines and comments
    [ -z "$key" ] && continue
    case "$key" in \#*) continue ;; esac
    resetprop -n "$key" "$value" 2>/dev/null
done < "$PROPS_FILE"
