#!/system/bin/sh
# ============================================================
# PR72: Download hev-socks5-tunnel for OmniShield
# ============================================================
# Downloads the ARM64 static binary of hev-socks5-tunnel,
# required for the Transparent Proxy feature.
#
# Uses the GitHub "latest release" redirect URL — no API needed.
# ============================================================

BIN_DIR="/data/adb/modules/omnishield/bin"
TARGET="$BIN_DIR/tun2socks"
# Direct download URL — GitHub redirects /latest/download/ to the actual asset
DOWNLOAD_URL="https://github.com/heiher/hev-socks5-tunnel/releases/latest/download/hev-socks5-tunnel-linux-aarch64"

echo "OmniShield — tun2socks Downloader"
echo "=================================="

# Check if already exists
if [ -x "$TARGET" ]; then
    echo "tun2socks already exists at $TARGET"
    exit 0
fi

mkdir -p "$BIN_DIR"

echo "Downloading hev-socks5-tunnel..."

# Try curl first (most common on Android), then wget as fallback
DL_OK=0
if command -v curl >/dev/null 2>&1; then
    curl -sL --connect-timeout 15 --max-time 120 -o "$TARGET" "$DOWNLOAD_URL" && DL_OK=1
fi
if [ "$DL_OK" -eq 0 ] && command -v wget >/dev/null 2>&1; then
    wget -q -T 15 -O "$TARGET" "$DOWNLOAD_URL" && DL_OK=1
fi

if [ "$DL_OK" -eq 0 ] || [ ! -s "$TARGET" ]; then
    echo "ERROR: Download failed"
    rm -f "$TARGET"
    exit 1
fi

chmod 755 "$TARGET"
echo "OK: tun2socks installed at $TARGET"
