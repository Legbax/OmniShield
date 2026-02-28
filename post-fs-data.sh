#!/system/bin/sh
MODDIR=${0%/*}

# PR69: Sincronizar configuración ANTES de que Zygisk tome el snapshot
if [ -f "/data/adb/.omni_data/.identity.cfg" ]; then
    cp "/data/adb/.omni_data/.identity.cfg" "$MODDIR/identity.cfg"
    chmod 644 "$MODDIR/identity.cfg"
fi

# Omni-Shield Post-FS-Data
# Setup directory
mkdir -p /data/adb/.omni_data
