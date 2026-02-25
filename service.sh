#!/system/bin/sh
# Omni-Shield Service
# Ensures prop file permissions
chmod 644 /data/adb/.omni_data/.identity.cfg 2>/dev/null

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
    printf '%016x' $(( (seed + offset * 2654435761) & 0xFFFFFFFFFFFF )) | head -c 16
}

# Wrapper principal
derive_ssaid() {
    if command -v python3 >/dev/null 2>&1; then
        derive_ssaid_python "$1" "$2"
    else
        derive_ssaid_fallback "$1" "$2"
    fi
}

# Apps objetivo: Snapchat primero (SSAID crítico para ban tracking)
TARGET_PACKAGES=(
    "com.snapchat.android"
    "com.instagram.android"
    "com.tinder.app"
)

if [ ! -f "$SSAID_FILE" ]; then
    exit 0
fi

# Hacer backup del archivo original si no existe ya
[ ! -f "${SSAID_FILE}.omni_bak" ] && cp "$SSAID_FILE" "${SSAID_FILE}.omni_bak"

MODIFIED=0
for i in "${!TARGET_PACKAGES[@]}"; do
    PKG="${TARGET_PACKAGES[$i]}"
    NEW_SSAID=$(derive_ssaid "$MASTER_SEED" "$i")

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
    # Force-stop del SettingsProvider para invalidar caché en memoria
    am force-stop com.android.providers.settings 2>/dev/null
fi

# ============================================================
# FIN PR37 SSAID Injection
# ============================================================
