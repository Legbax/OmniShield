#!/system/bin/sh
# ============================================================
# PR72: Download hev-socks5-tunnel for OmniShield
# ============================================================
# Downloads the ARM64 static binary of hev-socks5-tunnel,
# required for the Transparent Proxy feature.
#
# Uses the GitHub "latest release" redirect URL — no API needed.
# Tries multiple download tools for maximum Android compatibility.
# ============================================================

BIN_DIR="/data/adb/modules/omnishield/bin"
TARGET="$BIN_DIR/tun2socks"
DOWNLOAD_URL="https://github.com/heiher/hev-socks5-tunnel/releases/latest/download/hev-socks5-tunnel-linux-aarch64"

# Check if already exists
if [ -x "$TARGET" ]; then
    echo "tun2socks already exists at $TARGET"
    exit 0
fi

mkdir -p "$BIN_DIR"

echo "Downloading hev-socks5-tunnel..."

# Find the best available download tool
# Android's toybox curl/wget are limited; prefer busybox if available
BUSYBOX=""
for bb in /data/adb/ksu/bin/busybox /data/adb/magisk/busybox /system/bin/busybox busybox; do
    if [ -x "$bb" ] 2>/dev/null || command -v "$bb" >/dev/null 2>&1; then
        BUSYBOX="$bb"
        break
    fi
done

DL_OK=0
DL_ERR=""

# Method 1: busybox wget (most reliable on rooted Android)
if [ "$DL_OK" -eq 0 ] && [ -n "$BUSYBOX" ]; then
    echo "Trying busybox wget..."
    DL_ERR=$($BUSYBOX wget --no-check-certificate -q -O "$TARGET" "$DOWNLOAD_URL" 2>&1) && DL_OK=1
fi

# Method 2: system curl with -kL (insecure + follow redirects)
if [ "$DL_OK" -eq 0 ] && command -v curl >/dev/null 2>&1; then
    echo "Trying curl..."
    DL_ERR=$(curl -kL --connect-timeout 15 --max-time 120 -o "$TARGET" "$DOWNLOAD_URL" 2>&1) && DL_OK=1
fi

# Method 3: system wget
if [ "$DL_OK" -eq 0 ] && command -v wget >/dev/null 2>&1; then
    echo "Trying wget..."
    DL_ERR=$(wget --no-check-certificate -q -O "$TARGET" "$DOWNLOAD_URL" 2>&1) && DL_OK=1
fi

# Validate the download (must be a real binary, not an HTML error page)
if [ "$DL_OK" -eq 1 ] && [ -s "$TARGET" ]; then
    # Quick sanity check: ELF binaries start with 0x7f ELF
    if head -c4 "$TARGET" 2>/dev/null | grep -q "ELF"; then
        chmod 755 "$TARGET"
        echo "OK: tun2socks installed at $TARGET"
        exit 0
    else
        echo "ERROR: Downloaded file is not a valid binary (got HTML error page?)"
        rm -f "$TARGET"
        exit 1
    fi
fi

echo "ERROR: All download methods failed"
[ -n "$DL_ERR" ] && echo "Last error: $DL_ERR"
rm -f "$TARGET"
exit 1
