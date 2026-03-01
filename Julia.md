# Julia.md — OmniShield v12.9.58 Technical Reference

**Version:** v12.9.60 (The Void)
**Last updated:** 2026-03-01 (PR73b: `__system_property_read` hook, os.version MIUI fix, uname shell wrapper, Dobby diagnostics)

---

## 1. Architecture Overview

OmniShield is a Zygisk module (C++17, ARM64) that spoofs device identity at the native layer. It intercepts ~47 libc/JNI/HAL functions via Dobby inline hooks and Zygisk `hookJniNativeMethods`, projecting a complete fake device fingerprint to detection apps, Play Integrity, and DRM systems.

### Spoofing Layers (bottom-up)

| Layer | What | How |
|-------|------|-----|
| 1. Properties | `__system_property_get`, `__system_property_read_callback`, `__system_property_read`, `SystemProperties.native_get` JNI | 168+ keys mapped to profile fields |
| 2. VFS | `openat`, `read`, `fopen` (memfd for `/proc/cpuinfo`) | Fake `/proc/{cpuinfo,version,cmdline,status}`, `/sys/{cpu,battery,thermal,net}` |
| 3. Subprocess | `execve`, `posix_spawn`, `posix_spawnp` | Emulate `getprop` and `cat /proc/cpuinfo` output in forked child |
| 4. JNI Fields | `Build.*`, `Build.VERSION.*` static fields via `SetStaticObjectField` | 25+ fields overwritten in `postAppSpecialize` |
| 5. JNI Methods | `hookJniNativeMethods` on Telephony, Location, Sensor, Network, Camera, MediaCodec | ~40 Java methods hooked |
| 6. HAL/Driver | `eglQueryString`, `glGetString`, `vkGetPhysicalDeviceProperties`, `clGetDeviceInfo` | GPU identity, OpenGL ES version |
| 7. Stealth | `dl_iterate_phdr` filter, `/proc/self/maps` filter, `remapModuleMemory()` (file-backed → anonymous) | Module invisible in memory maps |

### Key Files

| File | Lines | Purpose |
|------|-------|---------|
| `jni/main.cpp` | ~3600 | Core module: all hooks, profile loading, companion process |
| `jni/omni_engine.hpp` | ~405 | Identity generators: IMEI (Luhn), ICCID, IMSI, MAC, Serial, GPS, UUID |
| `jni/omni_profiles.h` | ~726 | `DeviceFingerprint` struct (58 fields) + 40 device profiles |
| `jni/include/zygisk.hpp` | - | Zygisk API v3 header (AppSpecializeArgs, REGISTER_ZYGISK_MODULE) |
| `jni/include/dobby.h` | - | Dobby hooking framework header |
| `proxy_manager.sh` | ~418 | Transparent SOCKS5 proxy via hev-socks5-tunnel + iptables |
| `service.sh` | ~161 | Boot service: SSAID injection, permissions, proxy auto-start |
| `customize.sh` | ~23 | Magisk/KernelSU install script: chmod, dir creation |
| `webroot/js/app.js` | ~1201 | WebUI: config management, proxy control, scope picker |
| `webroot/index.html` | ~802 | WebUI HTML: tabs for Device, IDs, Telephony, Location, Proxy, Settings |
| `webroot/css/style.css` | ~802 | Dark-themed WebUI styles |
| `tests/simulate_proxy.sh` | ~375 | 57-test simulation suite for proxy lifecycle |
| `CMakeLists.txt` | - | Build config: NDK r25c, arm64-v8a, android-30, c++_static |
| `.github/workflows/build.yml` | - | CI: NDK build + tun2socks download + ZIP packaging |

### Config Path

```
/data/adb/.omni_data/.identity.cfg
```

Key-value format, read by companion process (root daemon via Zygisk IPC) or direct fallback.

```properties
profile=Samsung Galaxy A31
master_seed=1234567890
scoped_apps=com.snapchat.android,com.instagram.android
jitter=true
webview_spoof=true
proxy_enabled=true
proxy_type=socks5
proxy_host=proxy.example.com
proxy_port=1080
proxy_user=user
proxy_pass=pass
proxy_dns=1.1.1.1
```

---

## 2. Zygisk Lifecycle

```
Zygote boot
  ├─ dlopen(omnishield.so) → onLoad(Api, JNIEnv)
  │    └─ Save g_api, g_jvm globals. NO hooks here.
  │
  ├─ REGISTER_ZYGISK_COMPANION → companion_handler (root daemon)
  │    └─ Reads .identity.cfg via socket IPC (bypasses SELinux)
  │
  ├─ Fork app process
  │    ├─ preAppSpecialize(AppSpecializeArgs*)
  │    │    ├─ readConfigViaCompanion() → fallback readConfig()
  │    │    ├─ Parse scoped_apps, match nice_name
  │    │    ├─ If NOT target → DLCLOSE_MODULE_LIBRARY (unload .so)
  │    │    ├─ If target → FORCE_DENYLIST_UNMOUNT
  │    │    └─ WebView spoof check (webview_spoof=true + process contains "webview")
  │    │
  │    └─ postAppSpecialize(const AppSpecializeArgs*)
  │         ├─ Install ALL Dobby hooks (~37 native functions)
  │         ├─ Install ALL JNI hooks (Telephony, Location, Sensor, Network, Camera)
  │         ├─ Sync Build.* and Build.VERSION.* static fields via JNI
  │         ├─ Override System.setProperty("http.agent", spoofedUA)
  │         ├─ Override System.setProperty("os.version", spoofedKernel)
  │         └─ remapModuleMemory() → anonymous mappings
  │
  └─ Fork system_server
       └─ preServerSpecialize → DLCLOSE_MODULE_LIBRARY (no hooks in system_server)
```

---

## 3. Critical Invariants

These rules are derived from past crashes and regressions. Violating any will cause zygote crashes, spoofing failures, or detection.

### Zygisk ABI

1. **No virtual destructor** in `Module` class (`zygisk.hpp`). Adding `virtual ~Module()` shifts the vtable and causes SIGSEGV in `forkSystemServer`. The 5 pure virtuals (`onLoad`, `preAppSpecialize`, `postAppSpecialize`, `preServerSpecialize`, `postServerSpecialize`) must be the only vtable entries.

2. **Never move Dobby hooks to `onLoad`**. `onLoad` runs in the Zygote parent process pre-fork. Hooks installed there affect ALL child processes (including system_server and non-target apps).

3. **`REGISTER_ZYGISK_MODULE(OmniModule)`** — no manual entry points. No `registerModule()` (v2 API).

### Dobby Hooks

4. **Always use `DobbySymbolResolver("libc.so", "symbol")`** for libc hooks. Never `nullptr` (searches VDSO → mprotect on read-only memory → SIGSEGV). Never pass function pointers directly (PLT stubs → wrong address).

5. **Every hook function must start with `if (!orig_XXX)`** null guard. If DobbyHook fails silently, `orig_XXX` stays null.

6. **Critical libc hooks** (`openat`, `read`, `close`, `lseek`, `stat`, `fstatat`) must fall back to `syscall(__NR_xxx)` when `orig_XXX` is null. Return `-1` is fatal for boot.

7. **Never hook both `open` and `openat`**. Bionic's `open()` calls `openat()` internally → infinite recursion. Only hook `openat`. Use `orig_openat(AT_FDCWD, ...)` as terminal in `my_open`.

### Property Spoofing

8. **`my_system_property_get` must NEVER early-return** before the spoofing logic. It's the central hub — `my_system_property_read_callback` and `my_SystemProperties_native_get` both depend on it. If `orig_system_property_get` is null, initialize `value[0] = '\0'` and continue to profile logic.

9. **Three subprocess vectors** are hooked: `execve`, `posix_spawn`, `posix_spawnp`. Android 10+ uses `posix_spawn` for `Runtime.exec()`. All three detect `getprop`, `cat`, `sh -c getprop`, `su -c getprop` patterns.

10. **`shouldHide(key)` operates on VALUES**, not keys. Called at lines 889, 1329, 2449, 2520, 2632. Input is lowercased via `toLowerStr()`. Currently filters: `mediatek`, `lancelot`, `huaqin`, `mt6769`, `moly.`. Exception: allows these through when target profile is Xiaomi/MediaTek.

### JNI Safety

11. **Always call `env->ExceptionCheck() / ExceptionClear()`** after `hookJniNativeMethods` and after any `CallStaticObjectMethod`. Pending JNI exceptions cascade and silently corrupt all subsequent JNI operations.

12. **Always null-check** before `NewStringUTF(ptr)`. Passing `nullptr` → fatal JNI error.

13. **Self-reference check** after `hookJniNativeMethods`: if `orig_XXX` still points to `my_XXX`, the hook failed and calling `orig_XXX` creates infinite recursion.

### Config & Scope

14. **Companion process** (`readConfigViaCompanion`) is primary config reader. Direct file read is fallback only. Never remove the fallback — it's the safety net.

15. **`ksu_exec`** in app.js must use the global `ksu` object (injected by KernelSU via `@JavascriptInterface`), NOT `import('kernelsu')`. The import fails on some ROMs/KernelSU builds.

16. **SSAID derivation** is per-package-name via `cksum` XOR `master_seed`. Position-independent — reordering scope doesn't rotate SSAIDs.

---

## 4. DeviceFingerprint Struct

58 fields total in `jni/omni_profiles.h`:

- **36 strings:** manufacturer, brand, model, device, product, hardware, board, bootloader, fingerprint, buildId, tags, type, radioVersion, incremental, securityPatch, release, boardPlatform, eglDriver, openGlEs, hardwareChipname, zygote, vendorFingerprint, display, buildDescription, buildFlavor, buildHost, buildUser, buildDateUtc, buildVersionCodename, buildVersionPreviewSdk, gpuVendor, gpuRenderer, gpuVersion, screenWidth, screenHeight, screenDensity
- **6 ints:** core_count, ram_gb, pixelArrayWidth/Height (rear), frontPixelArrayWidth/Height
- **13 floats:** accel/gyro/mag ranges+resolutions, sensor physical sizes, focal lengths, apertures (rear+front)
- **3 bools:** hasHeartRateSensor, hasBarometerSensor, hasFingerprintWakeupSensor

40 profiles in static POD array. Zero heap allocation. Thread-safe lazy init via Meyer's Singleton (`getDeviceProfiles()`).

---

## 5. Transparent Proxy System (PR72)

### Components

```
WebUI (app.js) ──ksu_exec──▶ proxy_manager.sh {start|stop|status}
                                    │
                              ┌─────▼──────┐
                              │ tun2socks   │ (hev-socks5-tunnel, ARM64 static binary)
                              │ TUN: tun_omni│ (172.19.0.1/30, MTU 8500)
                              │ SOCKS5 proxy │
                              └─────┬──────┘
                                    │
                           iptables rules:
                           ├─ mangle OUTPUT: mark UID packets → 0x1337
                           ├─ nat OUTPUT: DNAT DNS → proxy_dns:53
                           └─ ip6tables: DROP all IPv6 (leak prevention)
```

### Shell Constraints (Android mksh)

- No `flock`, no `exec N>file` (high FDs). Use `mkdir -p` atomic locking with PID-based stale detection.
- No bash arrays, no `<<<`, no `read -ra`. Use `IFS=','` + positional params.
- `toybox curl` can't follow HTTPS redirects. Binary bundled in CI instead.
- `/dev/net/tun` may not exist. Create with `mknod /dev/net/tun c 10 200`.
- SELinux blocks unsigned binaries. `chcon u:object_r:system_file:s0` on tun2socks.

### Test Suite

`tests/simulate_proxy.sh` — 57 tests across 9 categories: config parsing, YAML generation (with Python YAML validation), lock mechanism, mock daemon, iptables construction, key consistency (JS↔Shell), HTML/JS ID cross-reference, build workflow completeness.

---

## 6. Known Limitations

| Vector | Status | Reason |
|--------|--------|--------|
| WebView UA (`getUserAgentString`) | Partial | `Build.MODEL` is spoofed via `SetStaticObjectField`, but Chromium caches UA during Zygote init before hooks. Fix requires DEX injection (LSPosed/Pine). |
| `Settings.Global.getString("device_name")` | Not hooked | Pure Java method via Binder IPC to ContentProvider. No native symbol in `libandroid_runtime.so` for Dobby. Property-level `device_name` IS hooked. |
| ENV variables (`BOOTCLASSPATH`, `DEX2OATBOOTCLASSPATH`) | Cannot fix | Set by Zygote pre-fork. Contain MediaTek/MIUI jars. Immutable post-init. |
| GPU hardware (Mali vs Adreno) | Cannot fix | Physical hardware. `glGetString`/`eglQueryString` are hooked for string-level spoofing but hardware capabilities differ. |
| Network info (WiFi IP, DNS, speed) | Not hooked | Requires `NetworkCapabilities` + `LinkProperties` hooks. Separate PR. |

---

## 7. Hooked Functions Catalog

### Dobby Native Hooks (~37)

**libc (File I/O):** openat, read, close, lseek, lseek64, pread, pread64, fopen, readlinkat
**libc (Process):** execve, posix_spawn, posix_spawnp
**libc (System):** uname, clock_gettime, access, stat, lstat, fstatat, sysinfo, readdir, getauxval, getifaddrs, ioctl, fcntl, dup, dup2, dup3
**libc (Properties):** __system_property_get, __system_property_read_callback, __system_property_read
**Stealth:** dl_iterate_phdr
**Graphics:** eglQueryString (libEGL), glGetString (libGLESv2), vkGetPhysicalDeviceProperties (libvulkan), clGetDeviceInfo (libOpenCL)
**Crypto:** SSL_CTX_set_ciphersuites, SSL_set1_tls13_ciphersuites, SSL_set_ciphersuites, SSL_set_cipher_list (libssl)
**Hardware:** Sensor.getName, Sensor.getVendor (libsensorservice/libsensors)
**Settings:** SettingsSecure.getString, SettingsSecure.getStringForUser (libandroid_runtime)
**Camera:** CameraMetadataNative.nativeReadValues
**Media:** MediaCodec.native_setup

### JNI Method Hooks

**Telephony (6):** getDeviceId, getSubscriberId, getSimSerialNumber, getLine1Number, getImei, getMeid
**Location (9):** getLatitude, getLongitude, getAltitude, getAccuracy, getSpeed, getBearing, getTime, getElapsedRealtimeNanos, isFromMockProvider
**Sensor (16):** getMaximumRange, getResolution, getPower, getMinDelay, getMaxDelay, getVersion, getFifoMaxEventCount, getFifoReservedEventCount, getName, getVendor, getStringType (×8 + custom sensor filter via getSensorList)
**Network (8):** getType, getSubtype, getExtraInfo, isConnected, isAvailable, isRoaming, isWifiEnabled, startScan
**SystemProperties (1):** native_get
**Kernel (1):** uname (libcore/io/Linux — intercepts android.system.Os.uname())

---

## 8. WebUI Architecture

KernelSU Manager hosts the WebUI in an Android WebView. The global `ksu` object provides root shell access via `ksu.exec(cmd, opts, callback)`.

### Tabs

| Tab | Key Element IDs | Function |
|-----|-----------------|----------|
| Device | `profile-select`, `apply-device` | Profile selection, Build.* preview |
| IDs | `seed-input`, `apply-ids` | IMEI/ICCID/IMSI/MAC/Serial/AndroidID generation |
| Telephony | `carrier-*`, `apply-telephony` | Carrier, IMSI pool, MCC/MNC |
| Location | `lat-input`, `lng-input`, `apply-location` | GPS coordinates, city picker |
| Proxy | `proxy-enabled`, `proxy-host`, `proxy-port`, `proxy-user`, `proxy-pass`, `proxy-dns`, `proxy-status-badge` | SOCKS5 transparent proxy |
| Settings | `scoped-apps`, `webview-spoof`, `destroy-identity` | App scope, WebView toggle, identity wipe |

### State Management

All state lives in the `state` object in app.js. `saveConfig()` writes to `.identity.cfg` via `ksu_exec("printf '%s\\n' ... > file")`. `readConfig()` reads via `ksu_exec("cat file")`. Both verify via read-back.

`smartApply(label)` saves config then `am force-stop` each scoped app for instant identity reload without reboot.

---

## 9. Build & Deploy

### CI Pipeline (`.github/workflows/build.yml`)

1. Checkout → NDK r25c setup
2. CMake build: `arm64-v8a`, `android-30`, `c++_static`
3. Download `hev-socks5-tunnel-linux-arm64` from GitHub releases (ELF validation)
4. Package ZIP: `zygisk/arm64-v8a.so`, `module.prop`, `customize.sh`, `service.sh`, `post-fs-data.sh`, `proxy_manager.sh`, `bin/tun2socks`, `webroot/`

### Manual Deploy

```bash
# Build
adb push dist/omnishield-v12.9.58-release.zip /sdcard/
# Install via KernelSU/Magisk Manager
# Reboot
```

### Module Structure (installed)

```
/data/adb/modules/omnishield/
├── zygisk/
│   └── arm64-v8a.so          ← Compiled Zygisk module
├── bin/
│   └── tun2socks              ← hev-socks5-tunnel ARM64 static binary
├── webroot/                   ← WebUI served by KernelSU Manager
├── module.prop
├── service.sh
├── post-fs-data.sh
├── proxy_manager.sh
└── customize.sh
```

---

## 10. PR Changelog (Condensed)

| PR | Version | Key Changes |
|----|---------|-------------|
| 73b | v12.9.60 | `__system_property_read` hook closes legacy API bypass (Fix1), Dobby diagnostic logging for execve/posix_spawn (Fix2), `os.version` direct `System.props` field access for MIUI (Fix3), `emulate_uname_output` shell wrapper argv parsing (Fix4), `dlsym(RTLD_DEFAULT)` fallback for execve/posix_spawn/posix_spawnp — DobbySymbolResolver fails on some Bionic builds (Fix5), `syscall(__NR_execve)` + `fork+execve` fallback when DobbyHook returns `orig=0x0` from PLT stubs (Fix6) |
| 73 | v12.9.59 | VD Info fixes: toybox/toolbox bypass (Fix1), `uname` subprocess interception + `emulate_uname_output` helper (Fix2), `Os.uname()` JNI hook via `libcore/io/Linux` (Fix3), `shouldHide()` + `"miui"` filter (Fix4) |
| 72-QA | v12.9.58 | VD Info fixes: shell bypass (`sh/su -c getprop`), `os.version` Java cache override, `shouldHide()` expanded (huaqin/mt6769/moly.), bluetooth_name hook, proxy system (tun2socks + iptables + 57 tests) |
| 71h | v12.9.58 | Smart Apply: `am force-stop` scoped apps on config save |
| 71g | v12.9.57 | WebView spoof toggle (separate from scope to avoid Destroy Identity crash) |
| 71f | v12.9.56 | `/proc/cpuinfo` bypass: subprocess `cat` interception + memfd for `fread` |
| 71e | v12.9.55 | **CRITICAL**: `my_system_property_get` early-return killed all 168+ property hooks |
| 71d | v12.9.54 | `http.agent` sync via `System.setProperty` |
| 71c | v12.9.53 | `posix_spawn`/`posix_spawnp` hooks (Android 10+ subprocess vector) |
| 71b | v12.9.52 | `SystemProperties.native_get` JNI hook + full `getprop` dump emulation |
| 71 | v12.9.51 | `execve` hook for `getprop` child processes, 40+ leaked props sealed |
| 70c | v12.9.50 | Companion process (root config reader), `/proc/<pid>/maps` bypass, `dl_iterate_phdr` filter, `remapModuleMemory()`, DLCLOSE for non-target apps, custom scope picker UI |
| 70 | v12.9.49 | `ksu_exec` rewrite (global `ksu` over `import('kernelsu')`), dynamic scope, SSAID per-package derivation |
| 56 | v12.9.36 | Zygisk API v2→v3: `AppSpecializeArgs`, `REGISTER_ZYGISK_MODULE` |
| 55 | v12.9.35 | Defensive: `onLoad` null guard, `g_jvm` global, `this->api` in `preServerSpecialize` |
| 54b | v12.9.34 | **ROOT CAUSE**: VTable shift from `virtual ~Module()` → removed |
| 54 | v12.9.33 | Syscall fallback in 7 critical hooks, DLCLOSE in `preServerSpecialize` |
| 53 | v12.9.32 | Meyer's Singleton for profile map, `debug_mode` flag |
| 51 | v12.9.31 | Fix `open`/`openat` infinite recursion, `readlinkat` null guard |
| 50 | v12.9.30 | `DobbySymbolResolver("libc.so")` to avoid VDSO hooks, `stoll` try-catch, `NewStringUTF` null guards |
| 49 | v12.9.29 | Replace PLT stub DobbyHook with `DobbySymbolResolver` for 9 hooks |
| 48 | v12.9.28 | 23 null guards for all Dobby hooks |
| 47 | v12.9.27 | Anti-crash: null guards for 12 JNI hooks, JNI exception cascade fix (8 sites), self-reference check |
| 38+39 | v12.9.18 | Sensor metadata (8 methods), GPS location spoofing (9 methods), network type (8 methods), sensor list filter, seed version rotation |
| 37 | v12.9.17 | Boot ID, cgroups, SSAID injection, LTE spoofing, `SUPPORTED_ABIS` fix |
| 22 | v12.9 | Tracer nullification (`TracerPid:\t0`), boot integrity props, CPU topology, partition fingerprints |
| 21 | v12.7 | Attestation fortress: `for_attestation` namespace, HAL gralloc/hwcomposer, DTB model, VFS hostname/ostype |

---

## 11. Testing Device

**Real hardware:** Xiaomi Redmi 9 (lancelot), MT6769T, MIUI 12.5, Android 11, KernelSU
**Default spoof target:** Samsung Galaxy A31 (SM-A315F), Exynos 9611, Android 11

---

## 12. Conventions

- **PR tags in comments:** `// PR72-QA Fix2:` — always reference PR number and fix number.
- **Log tag:** `AndroidSystem` (camouflaged as system log).
- **Debug mode:** `g_debugMode` controlled by `debug_mode=true` in config. All `LOGD`/`LOGE` are no-ops in release.
- **Tooling:** No `WebFetch` for gists — use `curl -sL` or `gh gist view`.
- **Shell scripts:** POSIX-compatible for Android mksh. No bashisms.
- **Commit messages:** `fix(scope):`, `feat(proxy):`, `fix(zygisk):` prefixes.

---

## 13. Update Protocol

When modifying code, update this Julia.md:
1. Add entry to PR Changelog table (Section 10)
2. Update line counts if files changed significantly (Section 1)
3. Update hooked function catalog if hooks added/removed (Section 7)
4. Update known limitations if status changes (Section 6)
5. Update critical invariants if new crash patterns discovered (Section 3)
