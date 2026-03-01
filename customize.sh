#!/system/bin/sh
# OmniShield — Magisk/KernelSU module install script
# Runs during module installation to set correct permissions.

ui_print "Installing OmniShield..."

# Ensure tun2socks binary is executable
if [ -f "$MODPATH/bin/tun2socks" ]; then
    chmod 755 "$MODPATH/bin/tun2socks"
    ui_print "  tun2socks binary: OK"
else
    ui_print "  WARNING: tun2socks binary not found in package"
fi

# Ensure scripts are executable
chmod 755 "$MODPATH/service.sh" 2>/dev/null
chmod 755 "$MODPATH/post-fs-data.sh" 2>/dev/null
chmod 755 "$MODPATH/proxy_manager.sh" 2>/dev/null

# Create data directory
mkdir -p /data/adb/.omni_data

ui_print "OmniShield installed successfully"
