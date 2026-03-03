# Julia.md — OMNI-Shield v13.0 Technical Reference

**Version:** v13.0 (The Void)
**Author:** Legba
**Last updated:** 2026-03-03 (PR98: Patch tun0 VPN leakage via /proc/net/route + if_inet6 tun filter + /proc/self/net/* aliases; PR97: Wipe Google Traces always targets com.android.webview; PR96: location_lat/lon now propagated to native GPS cache)

---

## 1. Architecture Overview

OMNI-Shield is a Zygisk module (C++17, ARM64) that spoofs device identity at the native layer. It intercepts ~55 libc/JNI/HAL functions via Dobby inline hooks, ~6 via Zygisk PLT hooks, and ~40 Java methods via `hookJniNativeMethods`, projecting a complete fake device fingerprint to detection apps, Play Integrity, and DRM systems.

### Spoofing Layers (bottom-up)

| Layer | What | How |
|-------|------|-----|
| 1. Properties | `__system_property_get`, `_read_callback`, `_read`, `_find`, `_foreach`, `SystemProperties.native_get` JNI | 200+ keys mapped to profile fields + `patchPropertyPages()` mremap |
| 2. Global Properties | `writeProfileProps()` → resetprop ~75 keys globally | Eliminates dual readings for forensic tools (PR95) |
| 3. VFS | `openat`, `read`, `fopen` (memfd for `/proc/cpuinfo`) | Fake `/proc/{cpuinfo,version,cmdline,status}`, `/sys/{cpu,battery,thermal,net}` |
| 4. Subprocess | `execve`, `posix_spawn`, `posix_spawnp` | memfd + orig_posix_spawn for intercepted commands |
| 5. JNI Fields | `Build.*`, `Build.VERSION.*` static fields via `SetStaticObjectField` | 25+ fields overwritten in `postAppSpecialize` |
| 6. JNI Methods | `hookJniNativeMethods` on Telephony, Location, Sensor, Network, Camera, MediaCodec | ~40 Java methods hooked |
| 7. HAL/Driver | `eglQueryString`, `glGetString`, `vkGetPhysicalDeviceProperties`, `clGetDeviceInfo` | GPU identity, OpenGL ES version |
| 8. Stealth | `dl_iterate_phdr` filter, `/proc/self/maps` filter, `remapModuleMemory()` | Module invisible in memory maps |
| 9. Kernel | SUSFS `set_uname` + `set_cmdline_or_bootconfig` | Kernel version + /proc/cmdline spoofing |

### Key Files

| File | Lines | Purpose |
|------|-------|---------|
| `jni/main.cpp` | ~5985 | Core module: all hooks, profile loading, companion, patchPropertyPages, writePifProps |
| `jni/omni_engine.hpp` | ~410 | Identity generators: IMEI (Luhn), ICCID, IMSI, MAC, Serial, GPS, UUID, Timezone, Carrier |
| `jni/omni_profiles.h` | ~726 | `DeviceFingerprint` struct (58 fields) + 40 device profiles (8 brands) |
| `jni/include/zygisk.hpp` | - | Zygisk API v3 header |
| `jni/include/dobby.h` | - | Dobby hooking framework header |
| `sepolicy.rule` | 2 | SELinux rules for zygote mount on /proc/cpuinfo |
| `service.sh` | ~187 | Boot service: SSAID injection, proxy auto-start, modem prop stabilizer |
| `boot-completed.sh` | ~82 | 2nd+ boot resetprop application + PIF hijacking |
| `post-fs-data.sh` | ~54 | Pre-Zygote SUSFS uname + cmdline forgery |
| `customize.sh` | ~25 | Magisk/KernelSU install script |
| `proxy_manager.sh` | ~418 | Transparent SOCKS5 proxy via hev-socks5-tunnel + iptables |
| `webroot/js/app.js` | ~1201 | WebUI: config management, proxy control, scope picker |
| `webroot/index.html` | ~802 | WebUI HTML |
| `CMakeLists.txt` | - | Build config: C++17, links log + vulkan |
| `.github/workflows/build.yml` | - | CI: NDK r25c build + tun2socks + ZIP packaging |

### Config Path

```
/data/adb/.omni_data/.identity.cfg
```

Key-value format, read by companion process (root daemon via Zygisk IPC) or direct fallback.

```properties
profile=Samsung Galaxy A12
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
  |-- dlopen(omnishield.so) -> onLoad(Api, JNIEnv)
  |    \-- Save g_api, g_jvm globals. NO hooks here.
  |
  |-- REGISTER_ZYGISK_COMPANION -> companion_handler (root daemon)
  |    |-- Reads .identity.cfg via socket IPC
  |    |-- writeProfileProps() -> ~75 resetprop entries to .profile_props
  |    |-- writePifProps() -> inject profile into /data/adb/pif.prop
  |    |-- system("resetprop -n ...") -> apply props globally
  |    \-- system("killall com.google.android.gms.unstable")
  |
  |-- Fork app process
  |    |-- preAppSpecialize(AppSpecializeArgs*)
  |    |    |-- readConfigViaCompanion() -> fallback readConfig()
  |    |    |-- Parse scoped_apps, match nice_name
  |    |    |-- If NOT target -> DLCLOSE_MODULE_LIBRARY (unload .so)
  |    |    |-- If target -> FORCE_DENYLIST_UNMOUNT
  |    |    \-- MemFD bind mount /proc/cpuinfo (if profile found)
  |    |
  |    \-- postAppSpecialize(const AppSpecializeArgs*)
  |         |-- Install ALL Dobby hooks (~55 native functions)
  |         |-- Install ALL PLT hooks (6 functions across ~100 ELFs)
  |         |-- patchPropertyPages() -> mremap atomic page replacement
  |         |-- Install ALL JNI hooks (Telephony, Location, Sensor, Network, Camera)
  |         |-- Sync Build.* and Build.VERSION.* static fields via JNI
  |         |-- Override System.setProperty("http.agent", spoofedUA)
  |         |-- Override System.setProperty("os.version", spoofedKernel)
  |         \-- remapModuleMemory() -> anonymous mappings
  |
  \-- Fork system_server
       \-- preServerSpecialize -> DLCLOSE_MODULE_LIBRARY (no hooks in system_server)
```

---

## 3. Critical Invariants

### Zygisk ABI

1. **No virtual destructor** in `Module` class. Adding `virtual ~Module()` shifts vtable -> SIGSEGV in `forkSystemServer`.

2. **Never move Dobby hooks to `onLoad`**. Hooks there affect ALL child processes.

3. **`REGISTER_ZYGISK_MODULE(OmniModule)`** -- no manual entry points.

### Dobby Hooks

4. **Always use `DobbySymbolResolver("libc.so", "symbol")`** for libc hooks. Never `nullptr`.

5. **Every hook function must start with `if (!orig_XXX)`** null guard.

6. **Critical libc hooks** (`openat`, `read`, `close`, `lseek`, `stat`, `fstatat`) must fall back to `syscall(__NR_xxx)` when `orig_XXX` is null.

7. **Never hook both `open` and `openat`**. Bionic's `open()` calls `openat()` internally.

### PLT Hooks

7b. **`execve`, `posix_spawn`, `posix_spawnp`, `__system_property_read`** use Zygisk PLT hooks (GOT pointer patching). Too small for Dobby trampoline on aarch64.

7c. **`orig_*` pointers MUST be pre-resolved via `dlsym(RTLD_DEFAULT)`** before PLT hooks. PLT overwrites across ~100 ELFs can corrupt `oldFunc`.

7d. **`__system_property_find` / `__system_property_foreach` Dobby inline + PLT hook are MUTUALLY EXCLUSIVE.** `g_propFindInlineHooked` / `g_propForeachInlineHooked` guards prevent infinite recursion.

7e. **`dlsym` hook MUST be installed AFTER all `dlsym`/`DobbySymbolResolver` calls.**

7f. **`android_dlopen_ext`/`dlopen` hooks re-apply PLT hooks to late-loaded libraries** (PR87/PR89). `reapplyPltHooksForNewLibraries()` re-hooks `__system_property_find`, `_read`, `_foreach`, `execve`, `posix_spawn`, `posix_spawnp`.

### Property Spoofing

8. **`my_system_property_get` must NEVER early-return** before spoofing logic. It's the central hub for `_read_callback` and `native_get`.

9. **`patchPropertyPages()`** uses `mremap(MREMAP_FIXED)` for atomic page replacement. Updates serial field (len << 24) for correct value length reporting.

10. **`shouldHide(key)` operates on VALUES**, not keys. Filters: `mediatek`, `lancelot`, `huaqin`, `mt6769`, `moly.`. Exception for Xiaomi/MediaTek target profiles.

### JNI Safety

11. **Always `ExceptionCheck() / ExceptionClear()`** after `hookJniNativeMethods`.

12. **Always null-check** before `NewStringUTF(ptr)`.

13. **Self-reference check** after `hookJniNativeMethods`: if `orig_XXX` still points to `my_XXX`, the hook failed.

---

## 4. DeviceFingerprint Struct

58 fields total in `jni/omni_profiles.h`:

- **36 strings:** manufacturer, brand, model, device, product, hardware, board, bootloader, fingerprint, buildId, tags, type, radioVersion, incremental, securityPatch, release, boardPlatform, eglDriver, openGlEs, hardwareChipname, zygote, vendorFingerprint, display, buildDescription, buildFlavor, buildHost, buildUser, buildDateUtc, buildVersionCodename, buildVersionPreviewSdk, gpuVendor, gpuRenderer, gpuVersion, screenWidth, screenHeight, screenDensity
- **6 ints:** core_count, ram_gb, pixelArrayWidth/Height (rear), frontPixelArrayWidth/Height
- **13 floats:** accel/gyro/mag ranges+resolutions, sensor physical sizes, focal lengths, apertures (rear+front)
- **3 bools:** hasHeartRateSensor, hasBarometerSensor, hasFingerprintWakeupSensor

40 profiles in static POD array across 8 brands: Xiaomi (12), Samsung (10), OnePlus (4), Google Pixel (4), Motorola (4), Nokia (2), Realme (3), ASUS (1). Zero heap allocation.

---

## 5. Engine Functions (`jni/omni_engine.hpp`)

23 inline functions for deterministic identity generation:

| Function | Purpose |
|----------|---------|
| `generateValidImei(profile, seed)` | IMEI with Luhn checksum + brand-specific TAC pool |
| `generateValidIccid(profile, seed)` | ITU-T E.118 ICCID (19 digits) |
| `generateValidImsi(profile, seed)` | USA carriers: T-Mobile, AT&T, Verizon, Sprint, US Cellular |
| `generatePhoneNumber(profile, seed)` | NANP format (+1 NPA-NXX-XXXX) |
| `generateRandomMac(brand, seed)` | Brand-specific OUI MAC address |
| `generateRandomSerial(brand, seed, patch)` | Brand-specific serial number |
| `generateWidevineId(seed)` | 16-byte hex Widevine device ID |
| `generateLocationForRegion(profile, seed)` | GPS coords in 5 US cities |
| `generateAltitudeForRegion(profile, seed)` | Altitude (meters ASL) |
| `getTimezoneForProfile(seed)` | US timezone (New_York, Los_Angeles, Chicago, Phoenix) |
| `getCarrierNameForImsi(profile, seed)` | Map MCC/MNC to carrier name |
| `getRilVersionForProfile(profile)` | RIL version (MTK, Samsung Exynos, AOSP Qualcomm) |
| `generateTls13CipherSuites(seed)` | Shuffled TLS 1.3 cipher suites |
| `generateTls12CipherSuites(seed)` | Shuffled TLS 1.2 cipher suites |
| `getGlVersionForProfile(fp)` | OpenGL ES version string |
| `generateBatteryTemp/Voltage(seed)` | Realistic battery telemetry |

---

## 6. Hooked Functions Catalog

### Dobby Inline Hooks (~55)

**libc (Properties):** `__system_property_get`, `__system_property_read_callback`, `__system_property_find`, `__system_property_foreach`
**libc (File I/O):** `openat`, `read`, `close`, `lseek`, `lseek64`, `pread`, `pread64`, `fopen`, `readlinkat`, `readlink`, `readdir`
**libc (Stat):** `stat`, `lstat`, `fstatat`, `statfs`, `statvfs`, `access`
**libc (System):** `uname`, `clock_gettime`, `sysinfo`, `getauxval`, `getifaddrs`, `ioctl`, `fcntl`
**libc (FD):** `dup`, `dup2`, `dup3`
**libc (Process):** `posix_spawn`, `posix_spawnp`
**Dynamic Linker:** `dlsym`, `android_dlopen_ext`, `dlopen`
**Stealth:** `dl_iterate_phdr`
**Graphics:** `eglQueryString` (libEGL), `glGetString` (libGLESv2), `vkGetPhysicalDeviceProperties` (libvulkan), `clGetDeviceInfo` (libOpenCL)
**Crypto:** `SSL_CTX_set_ciphersuites`, `SSL_set1_tls13_ciphersuites`, `SSL_set_ciphersuites`, `SSL_set_cipher_list` (libssl)
**Settings:** `SettingsSecure.getString` (4 overloads), `SettingsGlobal.getString` (4 overloads) (libandroid_runtime)
**Sensor:** `Sensor.getName`, `Sensor.getVendor`
**Camera:** `CameraMetadataNative.nativeReadValues`
**Media:** `MediaCodec.native_setup`

### Zygisk PLT Hooks (6, fallback for thin syscall wrappers)

`execve`, `posix_spawn`, `posix_spawnp`, `__system_property_read`, `__system_property_find` (if Dobby inline fails), `__system_property_foreach` (if Dobby inline fails)

### JNI Method Hooks (~40)

**Telephony (6):** getDeviceId, getSubscriberId, getSimSerialNumber, getLine1Number, getImei, getMeid
**Location (9):** getLatitude, getLongitude, getAltitude, getAccuracy, getSpeed, getBearing, getTime, getElapsedRealtimeNanos, isFromMockProvider
**Sensor (16):** getMaximumRange, getResolution, getPower, getMinDelay, getMaxDelay, getVersion, getFifoMaxEventCount, getFifoReservedEventCount, getName, getVendor, getStringType + custom sensor filter via getSensorList
**Network (8):** getType, getSubtype, getExtraInfo, isConnected, isAvailable, isRoaming, isWifiEnabled, startScan
**SystemProperties (1):** native_get
**Kernel (1):** uname (libcore/io/Linux)

---

## 7. Property Spoofing Architecture

### Three-tier approach

| Tier | Mechanism | Scope | Bypass resistance |
|------|-----------|-------|-------------------|
| **Hooks** | `my_system_property_get` + `_read_callback` + `_find` + `_foreach` | Per-process (scoped apps only) | Standard API calls: 100%. Direct syscall reads: 0% |
| **Page Patching** | `patchPropertyPages()` mremap(MREMAP_FIXED) | Per-process shared memory | In-process reads: 100%. Fresh mmap of /dev/__properties__/*: 0% |
| **resetprop** | `writeProfileProps()` -> companion shell loop | System-wide global | All readers including forensic tools: 100% |

### Properties in `my_system_property_get()` (~200+ keys)

**Identity:** ro.product.{model,brand,manufacturer,device,name} + all partition variants (system, vendor, odm, product, system_ext) + for_attestation namespace
**Fingerprints:** ro.build.fingerprint, ro.vendor.build.fingerprint, ro.odm.build.fingerprint, ro.system_ext.build.fingerprint, ro.product.build.fingerprint, ro.system.build.fingerprint
**Build metadata:** display.id, host, user, flavor, id, tags, type, description, incremental, security_patch, release, codename, preview_sdk, date.utc, date (human-readable) -- all partition variants
**Hardware:** ro.hardware, ro.board.platform, ro.boot.hardware, ro.hardware.chipname, ro.product.board, ro.soc.{manufacturer,model}, ro.boot.bootdevice
**HAL:** ro.hardware.{gralloc,hwcomposer,memtrack,camera,keystore,audio,vulkan,egl}
**Security:** ro.secure, ro.debuggable, ro.boot.verifiedbootstate, ro.boot.flash.locked, ro.boot.vbmeta.device_state, ro.secureboot.lockstate
**Telephony:** gsm.{network.type,sim.state,version.baseband,version.ril-impl}, gsm.{sim.operator,operator}.{numeric,iso-country,alpha}, ro.telephony.default_network, ro.carrier
**IMEI/Serial:** ro.serialno, ro.boot.serialno, ril.serialnumber, gsm.device.id, ro.ril.miui.imei{0,1}, ro.ril.oem.imei
**DRM:** ro.mediadrm.device_id, drm.service.enabled
**Display:** ro.sf.lcd_density, ro.product.display_resolution, ro.opengles.version
**Locale:** ro.product.locale, persist.sys.locale, persist.sys.country, persist.sys.language, persist.sys.timezone
**Network:** net.hostname, wifi.*, dhcp.wlan0.*, gsm.network.state
**MediaTek suppression:** ro.mediatek.*, ro.vendor.mediatek.* (suppressed for non-MTK profiles)
**MIUI suppression:** ro.miui.*, ro.vendor.miui.* (suppressed for non-Xiaomi profiles)

### Properties in `writeProfileProps()` (~75 resetprop'd globally)

**Telephony:** gsm.version.baseband, gsm.operator.*, gsm.sim.operator.*, gsm.version.ril-impl
**IMEI:** ro.ril.oem.imei, ro.ril.miui.imei{0,1}
**Hostname:** net.hostname
**Vendor suppression:** vendor.gsm.serial, vendor.gsm.project.baseband, ro.vendor.mediatek.*, ro.mediatek.version.branch, ro.vendor.md_apps.load_verno
**MIUI:** persist.sys.miui_optimization, persist.sys.device_name
**Market names:** ro.product.marketname + 5 partition variants (odm, product, system, system_ext, vendor)
**PR95 Build metadata:** ro.build.{display.id, host, user, flavor, id, version.incremental, version.security_patch}
**PR95 Dates UTC (7 partitions):** ro.{build,vendor.build,odm.build,product.build,system.build,system_ext.build,bootimage.build}.date.utc
**PR95 Dates human-readable (7 partitions):** ro.*.build.date (strftime from buildDateUtc)
**PR95 Build IDs (5 partitions):** ro.{vendor,odm,product,system,system_ext}.build.id
**PR95 Incrementals (5 partitions):** ro.*.build.version.incremental
**PR95 Security patches (4):** ro.{vendor,odm}.build.{security_patch,version.security_patch}
**PR95 Misc:** ro.fota.oem, ro.product.cert, ro.product.mod_device, ro.build.product, ro.boot.product.hardware.sku, ro.boot.rsc, ro.product.locale, ro.baseband, persist.sys.timezone

### `patchPropertyPages()` (PR91)

Atomic page replacement via `mremap(MREMAP_FIXED)`:
1. Phase 1: Iterate ALL properties via `orig_system_property_foreach`. Compare `my_system_property_get(name)` vs real value.
2. Phase 2: For each differing property, allocate temp page, memcpy, mremap atomically. Update serial field (len << 24) for value length.
3. Phase 3: Restore PROT_READ on remapped pages.

### `writePifProps()` (PR93)

Direct C++ injection of PIF keys into `/data/adb/pif.prop`:
- Keys: FINGERPRINT, MANUFACTURER, MODEL, BRAND, PRODUCT, DEVICE, RELEASE, ID, INCREMENTAL, SECURITY_PATCH
- Preserves user settings (spoofBuild, DEBUG, etc.)
- Called from companion_handler after writeProfileProps()

---

## 8. Dual Reading Analysis (Post-PR95)

### Eliminated (48 properties now single-reading)

All build dates (UTC + human-readable, 7 partitions each), build IDs (6), incrementals (6), security patches (4), host, display.id, fota.oem, cert, mod_device, build.product, boot.rsc, boot.product.hardware.sku, locale, baseband, all partition marketnames, timezone.

### Accepted duals (cannot resetprop without breaking system)

| Property | Values | Reason |
|----------|--------|--------|
| `ro.product.{model,brand,manufacturer,device,name}` (all partitions) | Real Xiaomi / Spoofed Samsung | MIUI framework depends on these |
| `ro.odm.build.fingerprint` | Only real Redmi/lancelot... | PR83: crash MIUI Settings |
| `ro.build.description` | lancelot-user / a12sqz-user | PR83: crash MIUI Settings |
| `ro.hardware`, `ro.board.platform`, `ro.boot.hardware` | mt6768 / mt6765 | HAL loading |
| `ro.hardware.egl` | mali / powervr | Graphics driver crash |
| `gsm.version.baseband` | Modem dual | Runtime prop, modem overwrites |
| `/proc/cpuinfo` | MT6769T | MemFD bind mount pending |
| BOOTCLASSPATH | miuisdk@boot.jar, mediatek-*.jar | Env var inherited from Zygote |
| GPU GL_RENDERER | Mali-G52 | Hardware driver |
| `getNetworkCountryIso` | cl (Chile) | Modem/RIL reads real MCC |

---

## 9. Boot Scripts

### `post-fs-data.sh` (pre-Zygote)
- SUSFS `set_uname` with profile-specific kernel version (40 profiles mapped)
- SUSFS `set_cmdline_or_bootconfig` from `.cmdline` file (generated by companion_handler)

### `service.sh` (post-boot)
1. **SSAID Injection** (PR37): Deterministic per-package android_id derived from master_seed + package hash. Python XorShift64* (64-bit) with sed fallback. Patches `/data/system/users/0/settings_ssaid.xml`.
2. **Proxy Auto-start** (PR72): If `proxy_enabled=true`, waits for network then launches `proxy_manager.sh start`.
3. **Modem Prop Stabilizer** (PR86): Loops every 5s resetting `gsm.operator.iso-country` and `gsm.sim.operator.iso-country` to "us" (modem overwrites periodically).

### `boot-completed.sh` (KernelSU 2nd+ boot)
1. Applies all resetprop from `.profile_props` (backup path; companion_handler is primary)
2. Sets `Settings.Global device_name` via `settings put global`
3. PIF Hijacking: Extracts `#PIF_*` comments from `.profile_props` -> writes to `/data/adb/pif.prop`

### `customize.sh` (install)
- Sets permissions on tun2socks, scripts
- Creates `/data/adb/.omni_data`

---

## 10. Transparent Proxy System (PR72)

```
WebUI (app.js) --ksu_exec--> proxy_manager.sh {start|stop|status}
                                    |
                              tun2socks (hev-socks5-tunnel, ARM64 static)
                              TUN: tun0 (172.19.0.1/30, MTU 8500)
                              SOCKS5 proxy
                                    |
                           iptables rules:
                           mangle OUTPUT: mark UID packets -> 0x1337
                           nat OUTPUT: DNAT DNS -> proxy_dns:53
                           ip6tables: DROP all IPv6 (leak prevention)
```

Test suite: `tests/simulate_proxy.sh` -- 57 tests across 9 categories.

---

## 11. Known Limitations

| Vector | Status | Reason |
|--------|--------|--------|
| Identity property duals (model, brand, etc.) | Accepted | MIUI crashes if resetprop'd globally |
| `ro.odm.build.fingerprint` dual | Accepted | PR83: MIUI Settings crash |
| `/proc/cpuinfo` (MT6769T) | Pending | MemFD bind mount + sepolicy.rule added, awaiting test |
| BOOTCLASSPATH / DEX2OATBOOTCLASSPATH | Cannot fix | Env vars set by Zygote pre-fork, contain miuisdk/mediatek jars |
| GPU hardware (Mali vs PowerVR) | Cannot fix | Physical hardware. `glGetString`/`eglQueryString` hooked but GL runtime reveals real GPU |
| `getNetworkCountryIso` | Partial | Modem/RIL reads real MCC from cell tower. `resetprop` loop in service.sh helps but Binder path may bypass |
| `getTypeAllocationCode` | Cannot fix | TAC from real IMEI, modem-level |
| `gsm.version.baseband` dual | Partial | Runtime prop, modem daemon overwrites after resetprop |
| WebView UA | Partial | Chromium caches UA during Zygote init before hooks |
| `Settings.Global.getString("device_name")` | Fixed | PR76: `settings put global device_name` in boot-completed.sh |

---

## 12. Build & Deploy

### CI Pipeline (`.github/workflows/build.yml`)

1. Checkout -> NDK r25c setup
2. CMake build: `arm64-v8a`, `android-30`, `c++_static`
3. Download `hev-socks5-tunnel-linux-arm64` (ELF validation)
4. Package ZIP: `zygisk/arm64-v8a.so`, `module.prop`, scripts, `bin/tun2socks`, `webroot/`
5. Output: `omnishield-v13.0-release.zip`

### Module Structure (installed)

```
/data/adb/modules/omnishield/
|-- zygisk/arm64-v8a.so
|-- bin/tun2socks
|-- webroot/
|-- sepolicy.rule
|-- module.prop
|-- service.sh
|-- post-fs-data.sh
|-- boot-completed.sh
|-- proxy_manager.sh
\-- customize.sh
```

---

## 13. Testing Device

**Real hardware:** Xiaomi Redmi 9 (lancelot), MT6769T (Helio G80), MIUI 12.5, Android 11, KernelSU
**Default spoof target:** Samsung Galaxy A12 (SM-A125F), MT6765 (Helio P35), Android 11

---

## 14. Conventions

- **PR tags in comments:** `// PR95:` -- always reference PR number.
- **Log tag:** `AndroidSystem` (camouflaged as system log).
- **Debug mode:** `g_debugMode` controlled by `debug_mode=true` in config.
- **Tooling:** No `WebFetch` for gists -- use `curl -sL` or `gh gist view`.
- **Shell scripts:** POSIX-compatible for Android mksh. No bashisms.

---

## 15. PR98 — Patch tun0 VPN leakage (2026-03-03)

VdInfo detected `tun0` as an active VPN interface even though hev-socks5-tunnel uses root iptables (not Android VpnService). Three VFS gaps were confirmed:

### Gaps fixed

| Vector | Root cause | Fix |
|--------|------------|-----|
| `/proc/net/route` | No `PROC_NET_ROUTE` enum or handler — kernel adds tun0 route when proxy active | Added synthetic routing table (wlan0 routes in WiFi mode, rmnet_data0 in LTE mode) |
| `/proc/net/if_inet6` WiFi mode | Comment said "filter tun" but `if` only checked `dummy` and `p2p` — tun0 link-local IPv6 (fe80::/10) leaked | Added `line.find("tun") != npos` condition |
| `/proc/self/net/*` aliases | All handlers used `strcmp("/proc/net/...")` — the `/proc/self/net/` kernel alias bypassed every hook | Extended conditions to also match `/proc/self/net/dev`, `/proc/self/net/route`, `/proc/self/net/if_inet6`, `/proc/self/net/tcp[6]`, `/proc/self/net/udp[6]` |

**Files changed:** `jni/main.cpp`

---

## 16. PR96 / PR97 — Location Selector & Wipe Google Traces (2026-03-03)

### PR96: Location selector now propagates to native GPS hooks

**Bug:** `parseConfigString()` read `profile`, `master_seed`, `seed_version` etc. but silently ignored `location_lat`, `location_lon`, `location_alt`. `initLocationCache()` always called `generateLocationForRegion()` from seed, making the UI Location selector cosmetic-only.

**Fix (`jni/main.cpp`):** Added block at the end of `parseConfigString()` that reads `location_lat`/`location_lon`/`location_alt` from config and sets `g_cachedLat`/`g_cachedLon`/`g_cachedAlt` + `g_locationCached = true`. Since this runs AFTER the `seed_version` reset of `g_locationCached`, user-pinned coordinates always win over the generated fallback.

### PR97: Wipe Google Traces — com.android.webview always targeted

**Bug:** `wipeGoogleTraces()` only wiped the dynamically detected `$WV` package (resolved via `cmd webviewupdate current-webview-package`). On devices where the active WebView is `com.google.android.webview` or Chrome, `com.android.webview` (AOSP built-in) kept residual session data. Additionally, if `cmd webviewupdate` returned unexpected multi-line output, `$WV` could be invalid and `pm clear` silently failed for the wrong package.

**Fix (`webroot/js/app.js`):** Added `| tr -d '\n'` to sanitise `$WV`, added `[ -z "$WV" ] && WV=com.google.android.webview` null-guard, and explicitly added `com.android.webview` to all three operations (`am force-stop`, `pm clear`, `rm -rf`) independently of `$WV`.

---

## 17. Update Protocol

When modifying code, update this Julia.md:
1. Add entry to this file if a major PR lands
2. Update line counts if files changed significantly (Section 1)
3. Update hooked function catalog if hooks added/removed (Section 6)
4. Update property lists if properties added/removed (Section 7)
5. Update known limitations if status changes (Section 11)
6. Update critical invariants if new crash patterns discovered (Section 3)
