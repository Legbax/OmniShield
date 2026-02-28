#!/system/bin/sh
# ============================================================
# PR72: Download hev-socks5-tunnel for OmniShield
# ============================================================
# This script downloads the ARM64 static binary of
# hev-socks5-tunnel, required for the Transparent Proxy feature.
#
# Run this ON the device (requires internet access):
#   sh /data/adb/modules/omnishield/bin/download_tun2socks.sh
#
# Or manually download from:
#   https://github.com/heiher/hev-socks5-tunnel/releases
#   → hev-socks5-tunnel-linux-aarch64
# ============================================================

BIN_DIR="/data/adb/modules/omnishield/bin"
TARGET="$BIN_DIR/tun2socks"
REPO="heiher/hev-socks5-tunnel"
ARCH="linux-aarch64"

echo "OmniShield — tun2socks Downloader"
echo "=================================="

# Check if already exists
if [ -x "$TARGET" ]; then
    echo "tun2socks already exists at $TARGET"
    echo "Delete it first if you want to re-download."
    exit 0
fi

# Try to get latest release URL
echo "Fetching latest release info..."
RELEASE_URL=""

# Method 1: gh CLI (if available)
if command -v gh >/dev/null 2>&1; then
    RELEASE_URL=$(gh release view --repo "$REPO" --json assets -q ".assets[] | select(.name | contains(\"$ARCH\")) | .url" 2>/dev/null | head -1)
fi

# Method 2: curl + GitHub API
if [ -z "$RELEASE_URL" ] && command -v curl >/dev/null 2>&1; then
    RELEASE_URL=$(curl -sL "https://api.github.com/repos/$REPO/releases/latest" 2>/dev/null | \
        grep "browser_download_url.*$ARCH" | head -1 | sed 's/.*"\(http[^"]*\)".*/\1/')
fi

if [ -z "$RELEASE_URL" ]; then
    echo "ERROR: Could not determine download URL."
    echo ""
    echo "Manual download:"
    echo "  1. Visit: https://github.com/$REPO/releases"
    echo "  2. Download: hev-socks5-tunnel-$ARCH"
    echo "  3. Copy to: $TARGET"
    echo "  4. Run: chmod 755 $TARGET"
    exit 1
fi

echo "Downloading from: $RELEASE_URL"
curl -sL -o "$TARGET" "$RELEASE_URL"

if [ $? -ne 0 ] || [ ! -f "$TARGET" ]; then
    echo "ERROR: Download failed."
    rm -f "$TARGET"
    exit 1
fi

chmod 755 "$TARGET"
echo "OK: tun2socks installed at $TARGET"
echo "You can now enable the proxy in the OmniShield WebUI."
