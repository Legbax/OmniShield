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

# Derivar SSAID desde seed (mismo algoritmo que generateRandomId en omni_engine.hpp)
# Usamos awk para aritmética de enteros compatible con sh
derive_ssaid() {
    local seed="$1"
    local offset="$2"
    local s=$((seed + offset))
    local result=""
    for i in $(seq 1 16); do
        s=$(( (s * 6364136223 + 1442695041) & 0x7FFFFFFF ))
        nibble=$(( (s >> 16) & 0xF ))
        result="${result}$(printf '%x' $nibble)"
    done
    echo "$result"
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
    # Offset diferente para cada app (mismo que en omni_engine.hpp generateRandomId)
    NEW_SSAID=$(derive_ssaid "$MASTER_SEED" "$i")

    # Verificar si el paquete tiene entrada en el archivo
    if grep -q "package=\"$PKG\"" "$SSAID_FILE" 2>/dev/null; then
        # Actualizar el value= de la entrada existente del paquete
        # Usar sed con delimitador | para evitar conflictos con /
        sed -i "s|package=\"$PKG\"[^/]*/> |package=\"$PKG\" value=\"$NEW_SSAID\" />|g" "$SSAID_FILE" 2>/dev/null
        MODIFIED=1
    fi
    # Si el paquete no tiene entrada aún, se creará cuando instale la app
    # y el hook JNI actuará como fallback hasta el próximo reinicio
done

# Notificar al sistema que invalide el caché de Settings si se modificó algo
if [ "$MODIFIED" -eq 1 ]; then
    # Force-stop del SettingsProvider para invalidar caché en memoria
    am force-stop com.android.providers.settings 2>/dev/null
fi

# ============================================================
# FIN PR37 SSAID Injection
# ============================================================
