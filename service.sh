#!/system/bin/sh
# Omni-Shield Service
# Ensures prop file permissions
chmod 644 /data/adb/.omni_data/.identity.cfg 2>/dev/null
# PR70c: Set SELinux context so zygote can read the config (fallback path)
chcon u:object_r:system_data_file:s0 /data/adb/.omni_data 2>/dev/null
chcon u:object_r:system_data_file:s0 /data/adb/.omni_data/.identity.cfg 2>/dev/null

# ============================================================
# PR37: SSAID Injection — OmniShield
# ============================================================
# El SSAID (Settings.Secure "android_id") en Android 8+ es
# POR-APP: derivado de (base_android_id + package_name + signing_key).
# Se almacena en /data/system/users/0/settings_ssaid.xml.
# El hook JNI en libandroid_runtime.so NO intercepta este valor
# porque el path real pasa por Binder IPC a system_server.
# La única solución fiable es modificar el archivo en disco.
# El valor inyectado usa el mismo master_seed que el módulo nativo
# para garantizar coherencia entre ambas capas.
# ============================================================

SSAID_FILE="/data/system/users/0/settings_ssaid.xml"
OMNI_CONFIG="/data/adb/.omni_data/.identity.cfg"

# Leer master_seed del config
MASTER_SEED=""
if [ -f "$OMNI_CONFIG" ]; then
    MASTER_SEED=$(grep "^master_seed=" "$OMNI_CONFIG" | cut -d'=' -f2 | tr -d ' \r\n')
fi

# Si no hay seed definido no modificar
[ -z "$MASTER_SEED" ] && exit 0

# Implementación Python (precisión 64-bit completa)
derive_ssaid_python() {
    local seed="$1"
    local offset="$2"
    python3 -c "
s = ($seed + $offset)
result = ''
for i in range(16):
    s = (s * 6364136223846793005 + 1442695040888963407) & 0xFFFFFFFFFFFFFFFF
    result += '0123456789abcdef'[((s >> 33) ^ s) & 0xF]
print(result)
" 2>/dev/null
}

# Fallback sin python3: consistente entre reboots pero no coincide con el hook JNI.
# El hook JNI es el canal primario; este es best-effort para el path Binder.
derive_ssaid_fallback() {
    local seed="$1"
    local offset="$2"
    printf '%016x' $(( (seed + offset * 2654435761) & 0xFFFFFFFFFFFFFFFF )) | head -c 16
}

# Wrapper principal
derive_ssaid() {
    if command -v python3 >/dev/null 2>&1; then
        derive_ssaid_python "$1" "$2"
    else
        derive_ssaid_fallback "$1" "$2"
    fi
}

# Read scoped_apps from config (comma-separated)
SCOPED_APPS=""
if [ -f "$OMNI_CONFIG" ]; then
    SCOPED_APPS=$(grep "^scoped_apps=" "$OMNI_CONFIG" | cut -d'=' -f2-)
fi

# POSIX sh compatible: split by comma into positional params ($1, $2, ...)
if [ -n "$SCOPED_APPS" ]; then
    OLDIFS="$IFS"
    IFS=','
    set -- $SCOPED_APPS
    IFS="$OLDIFS"
else
    set --
fi

if [ ! -f "$SSAID_FILE" ]; then
    exit 0
fi

# Hacer backup del archivo original si no existe ya
[ ! -f "${SSAID_FILE}.omni_bak" ] && cp "$SSAID_FILE" "${SSAID_FILE}.omni_bak"

MODIFIED=0
for PKG in "$@"; do
    # Derive SSAID from package name hash (deterministic, order-independent)
    PKG_HASH=$(printf '%s' "$PKG" | cksum | cut -d' ' -f1)
    PKG_SEED=$(( (MASTER_SEED ^ PKG_HASH) & 0xFFFFFFFFFFFFFFFF ))
    NEW_SSAID=$(derive_ssaid "$PKG_SEED" "0")

    if grep -q "package=\"$PKG\"" "$SSAID_FILE" 2>/dev/null; then
        if command -v python3 >/dev/null 2>&1; then
            # Python: manejo robusto del XML sin asumir orden de atributos ni single-line.
            # FastXmlSerializer de Android no garantiza orden ni formato de línea.
            python3 -c "
import re
content = open('$SSAID_FILE').read()
pkg = '$PKG'
new_val = '$NEW_SSAID'
# Caso 1: package= aparece antes de value=
p1 = r'(<setting[^>]+package=\"' + re.escape(pkg) + r'\"[^>]+value=\")[^\"]*(\")'
# Caso 2: value= aparece antes de package=
p2 = r'(<setting[^>]+value=\")[^\"]*(\")[^>]+package=\"' + re.escape(pkg) + r'\"'
new_content = re.sub(p1, r'\g<1>' + new_val + r'\g<2>', content)
if new_content == content:
    new_content = re.sub(p2, lambda m: m.group(0).replace(
        m.group(1) + m.group(2), m.group(1) + new_val + '\"'), content)
open('$SSAID_FILE', 'w').write(new_content)
" 2>/dev/null && MODIFIED=1
        else
            # Fallback a sed básico si python3 no está disponible.
            # Asume formato estándar de una línea (funciona en la mayoría de dispositivos).
            sed -i "s|package=\"$PKG\"\(.*\)value=\"[^\"]*\"|package=\"$PKG\"\1value=\"$NEW_SSAID\"|g" \
                "$SSAID_FILE" 2>/dev/null
            MODIFIED=1
        fi
    fi
done

# Notificar al sistema que invalide el caché de Settings si se modificó algo
if [ "$MODIFIED" -eq 1 ]; then
    # PR40 (Gemini BUG-C1-01): Restaurar propietario y contexto SELinux.
    # Python y sed pueden dejar el archivo sin owner system:system o sin contexto
    # u:object_r:system_data_file:s0, causando crash en system_server al leerlo.
    chown system:system "$SSAID_FILE"
    chmod 600 "$SSAID_FILE"
    restorecon "$SSAID_FILE" 2>/dev/null

    # Force-stop del SettingsProvider para invalidar caché en memoria
    am force-stop com.android.providers.settings 2>/dev/null
fi

# ============================================================
# FIN PR37/PR40 SSAID Injection
# ============================================================

# ============================================================
# PR72: Auto-start Transparent Proxy if enabled
# ============================================================
PROXY_SCRIPT="/data/adb/modules/omnishield/proxy_manager.sh"
if [ -f "$PROXY_SCRIPT" ] && grep -q "^proxy_enabled=true" "$OMNI_CONFIG" 2>/dev/null; then
    # Wait for network availability (max 30s)
    _net_ok=0
    for _i in $(seq 1 30); do
        ping -c1 -W1 8.8.8.8 >/dev/null 2>&1 && { _net_ok=1; break; }
        sleep 1
    done
    if [ "$_net_ok" -eq 1 ]; then
        sh "$PROXY_SCRIPT" start &
    fi
fi
# ============================================================
# FIN PR72
# ============================================================
