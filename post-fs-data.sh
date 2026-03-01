#!/system/bin/sh
# OmniShield Post-FS-Data
# Runs BEFORE Zygote → ART VM caches the correct os.version from kernel uname.
mkdir -p /data/adb/.omni_data

# ── susfs set_uname integration ─────────────────────────────────────────
# Sets kernel-level uname via ksu_susfs so ALL processes (including those
# outside OmniShield scope) report the profile-correct kernel version.
# Must run before Zygote to ensure System.getProperty("os.version") is correct.
SUSFS_BIN=/data/adb/ksu/bin/ksu_susfs
CFG=/data/adb/.omni_data/.identity.cfg

# Only proceed if susfs binary exists and config is present
[ -x "$SUSFS_BIN" ] || exit 0
[ -f "$CFG" ] || exit 0

PROFILE=$(grep "^profile=" "$CFG" | cut -d'=' -f2-)
[ -z "$PROFILE" ] && exit 0

# Shell mirror of getSpoofedKernelVersion() — jni/main.cpp:644-686
# Groups profiles by boardPlatform → kernel release string.
case "$PROFILE" in
  # ── Special case (hardcoded in C++) ──
  "Redmi 9")
    KV="4.14.186-perf+" ;;

  # ── Google Pixel (brand=google, platform-specific hashes) ──
  "Pixel 5"|"Pixel 4a 5G")
    KV="4.19.113-g820a424c538c-ab7336171" ;;
  "Pixel 4a")
    KV="4.14.150-g62a62a5a93f7-ab7336171" ;;
  "Pixel 3a XL")
    KV="4.9.189-g5d098cef6d96-ab6174032" ;;

  # ── MediaTek (boardPlatform contains "mt6") ──
  "Redmi 10X 4G"|"POCO M3 Pro 5G"|"Galaxy A32 5G"|"Galaxy A12"|\
  "Galaxy A31"|"Realme 8")
    KV="4.14.141-perf+" ;;

  # ── Qualcomm Kona / Lahaina ──
  "Mi 10T"|"Mi 11"|"POCO F3"|"Galaxy S20 FE"|"OnePlus 8T"|\
  "OnePlus 8"|"Moto Edge Plus"|"ASUS ZenFone 7")
    KV="4.19.157-perf+" ;;

  # ── Qualcomm Atoll / Lito ──
  "Redmi Note 10 Pro"|"Redmi Note 9 Pro"|"POCO X3 NFC"|"Mi 11 Lite"|\
  "Galaxy A52"|"Galaxy A72"|"OnePlus Nord"|"Moto Edge"|\
  "Nokia 8.3 5G"|"Realme 8 Pro")
    KV="4.19.113-perf+" ;;

  # ── Qualcomm Bengal / Holi / SM6350 ──
  "Moto G Power 2021"|"Moto G Stylus 2021"|"OnePlus N10 5G"|"Nokia 5.4")
    KV="4.19.157-perf+" ;;

  # ── Qualcomm SM7325 ──
  "Realme GT Master")
    KV="5.4.61-perf+" ;;

  # ── Samsung Exynos 9611 ──
  "Galaxy A51"|"Galaxy M31")
    KV="4.14.113-25145160" ;;

  # ── Samsung Exynos 9825 ──
  "Galaxy F62")
    KV="4.14.113-22911262" ;;

  # ── Samsung Exynos 850 ──
  "Galaxy A21s")
    KV="4.19.113-25351273" ;;

  # ── Default fallback (unrecognized platform) ──
  *)
    KV="4.14.186-perf+" ;;
esac

# Call susfs to set kernel-level uname (before Zygote reads it)
$SUSFS_BIN set_uname "$KV" '#1 SMP PREEMPT' 2>/dev/null
