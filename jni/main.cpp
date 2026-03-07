#include <sys/time.h>
#include <android/log.h>
#include <sys/system_properties.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <fstream>
#include <thread>
#include <vector>
#include <map>
#include <string>
#include <cstring>
#include <mutex>
#include <jni.h>
#include <cstdio>
#include <algorithm>
#include <sstream>
#include <chrono>
#include <random>
#include <time.h>
#include <iomanip>
#include <sys/utsname.h>
#include <ifaddrs.h>
#include <linux/if_packet.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <linux/if_arp.h>
#include <errno.h>
#include <cstdlib>
#include <cmath>
#include <vulkan/vulkan.h>
#include <sys/sysinfo.h>
#include <sys/vfs.h>
#include <sys/statvfs.h>
#include <dirent.h>
#include <sys/auxv.h>
#include <asm/hwcap.h>
#include <link.h>
#include <dlfcn.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/mount.h>
#include <set>              // Fix8: deduplicar ELFs en PLT hooks
#include <unordered_map>   // PR84: fake prop_info map
#include <sys/sysmacros.h>  // Fix8: makedev()

#include "zygisk.hpp"
#include "dobby.h"
#include "omni_profiles.h"
#include "omni_engine.hpp"

#define LOG_TAG "AndroidSystem"
#define LOGD(...) do { if (g_debugMode) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__); } while(0)
#define LOGE(...) do { if (g_debugMode) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__); } while(0)

// Globals
static std::map<std::string, std::string> g_config;
static std::string g_currentProfileName = "Redmi 9";
static long g_masterSeed = 0;
static bool g_enableJitter = true;
static uint64_t g_configGeneration = 0;
static bool g_spoofMobileNetwork = false;  // network_type=lte en .identity.cfg
static bool g_debugMode = true;  // PR53: activar con debug_mode=true en .identity.cfg

// PR38+39: GPS cache — coordenadas generadas una vez por sesión desde g_masterSeed
static double g_cachedLat       = 0.0;
static double g_cachedLon       = 0.0;
static double g_cachedAlt       = 0.0;

// PR105: Atómicos thread-safe para el Binder hook de Location.
// g_cachedLat/Lon siguen siendo las variables primarias.
// g_cachedLatBits/LonBits son espejos atómicos para uso en my_ipc_transact.
#include <atomic>
static std::atomic<int64_t> g_cachedLatBits{0};
static std::atomic<int64_t> g_cachedLonBits{0};

static bool   g_locationCached  = false;

// PR38+39: Sensor chip statics — valores del perfil activo para los hooks de Sensor
// Definidos aquí para linkage correcto (los hooks son structs locales en postAppSpecialize)
static float  g_sensorAccelMax  = 78.4532f;   // Default LSM6DSO
static float  g_sensorAccelRes  = 0.0023946f;
static float  g_sensorGyroMax   = 34.906586f;
static float  g_sensorGyroRes   = 0.001064f;
static float  g_sensorMagMax    = 4912.0f;

// PR38+39: Seed version — para detectar rotación de seed desde la UI
static long   g_seedVersion        = 0;
// PR38+39: Sensor presence flags — leídos del perfil activo en postAppSpecialize
// Usados por SensorListHook para filtrar sensores ausentes en el perfil emulado
static bool   g_sensorHasHeartRate = false;
static bool   g_sensorHasBarometer = false;

// PR121: Firma runtime de identidad/ubicación aplicada a procesos persistentes.
// Si cambia (perfil, seed o pin de ubicación), reiniciamos stack de ubicación
// para forzar que Maps/GMS relancen procesos con el config nuevo.
static std::string g_lastRuntimeIdentitySig;

// PR134: Current process name for per-process hook decisions.
static std::string g_currentProcessName;

// PR44: Camera2 — globals ópticos cargados desde findProfile() en postAppSpecialize
// Rear camera (siempre activo)
static float   g_camPhysicalWidth   = 6.40f;
static float   g_camPhysicalHeight  = 4.80f;
static int32_t g_camPixelWidth      = 8000;
static int32_t g_camPixelHeight     = 6000;
static float   g_camFocalLength     = 4.74f;
static float   g_camAperture        = 1.8f;
// Front camera (activo vía isFrontCameraMetadata — LENS_FACING oracle)
static float   g_camFrontPhysWidth  = 3.84f;
static float   g_camFrontPhysHeight = 2.88f;
static int32_t g_camFrontPixWidth   = 6528;
static int32_t g_camFrontPixHeight  = 4896;
static float   g_camFrontFocLen     = 2.20f;
static float   g_camFrontAperture   = 2.2f;

// PR38+39: Inicializar caché de GPS (llamar en postAppSpecialize tras readConfig)
static void initLocationCache() {
    if (!g_locationCached && !g_currentProfileName.empty() && g_masterSeed != 0) {
        omni::engine::generateLocationForRegion(g_currentProfileName, g_masterSeed,
                                                g_cachedLat, g_cachedLon);
        g_cachedAlt    = omni::engine::generateAltitudeForRegion(g_currentProfileName, g_masterSeed);
        g_locationCached = true;
    }
    // PR105: Propagar coordenadas al Binder hook de forma thread-safe.
    // Debe ejecutarse siempre, tanto si se generaron coords nuevas como si
    // g_locationCached ya era true (coords cargadas por parseConfigString).
    {
        int64_t lb, lob;
        memcpy(&lb,  &g_cachedLat, 8);
        memcpy(&lob, &g_cachedLon, 8);
        g_cachedLatBits.store(lb,  std::memory_order_release);
        g_cachedLonBits.store(lob, std::memory_order_release);
    }
}

struct CachedContent {
    std::string content;
    uint64_t generation;
};

// FD Tracking
enum FileType {
    NONE = 0, PROC_VERSION, PROC_CPUINFO, USB_SERIAL, WIFI_MAC,
    BATTERY_TEMP, BATTERY_VOLT, PROC_MAPS, PROC_UPTIME, BATTERY_CAPACITY,
    BATTERY_STATUS, PROC_OSRELEASE, PROC_MEMINFO, PROC_MODULES, PROC_MOUNTS,
    SYS_CPU_FREQ, SYS_SOC_MACHINE, SYS_SOC_FAMILY, SYS_SOC_ID, SYS_FB0_SIZE,
    PROC_ASOUND, PROC_INPUT, SYS_THERMAL,
    PROC_CMDLINE, PROC_STAT, SYS_BLOCK_MODEL, SYS_CPU_GOVERNORS,
    BT_MAC, BT_NAME,
    PROC_NET_ARP, PROC_NET_DEV,
    PROC_NET_ROUTE,       // PR98: /proc/net/route — IPv4 routing table (exposed tun0 when proxy active)
    PROC_HOSTNAME,
    PROC_OSTYPE,
    PROC_DTB_MODEL,
    PROC_ETH0_MAC,
    PROC_SELF_STATUS, SYS_CPU_TOPOLOGY, BAT_TECHNOLOGY, BAT_PRESENT,
    SYS_BLOCK_SIZE, PROC_NET_TCP, PROC_NET_UDP,
    PROC_BOOT_ID,         // /proc/sys/kernel/random/boot_id — UUID único por device
    SYS_CPU_FREQ_AVAIL,   // scaling_available_frequencies — firma de plataforma CPU
    PROC_SELF_CGROUP,     // /proc/self/cgroup — puede revelar namespace KernelSU
    BATTERY_CHARGE_FULL,  // PR42: /sys/class/power_supply/battery/charge_full[_design]
    PROC_NET_IF_INET6,    // PR42: /proc/net/if_inet6 — expone interfaces IPv6 activas
    PROC_NET_IPV6_ROUTE   // PR43: /proc/net/ipv6_route
};
static std::map<int, FileType> g_fdMap;
static std::map<int, size_t> g_fdOffsetMap; // Thread-safe offset tracking
static std::map<int, CachedContent> g_fdContentCache; // Cache content for stable reads
static std::mutex g_fdMutex;
static std::set<int> g_ashmemLoggedFds;
static std::set<int> g_ashmemFds;
static std::set<int> g_nonAshmemFds;
static std::atomic<bool> g_pr120MmapPulseLogged{false};
static std::atomic<bool> g_pr120Mmap64PulseLogged{false};

// Original Pointers
static int (*orig_system_property_get)(const char *key, char *value);
static void (*orig_system_property_read_callback)(const prop_info *pi, void (*callback)(void *cookie, const char *name, const char *value, uint32_t serial), void *cookie);
static int (*orig_system_property_read)(const prop_info *pi, char *name, char *value);  // PR73b-Fix1
static const prop_info* (*orig_system_property_find)(const char *name);  // PR84: close direct shared-memory read vector
static int (*orig_open)(const char *pathname, int flags, mode_t mode);
static int (*orig_openat)(int dirfd, const char *pathname, int flags, mode_t mode);
static ssize_t (*orig_read)(int fd, void *buf, size_t count);
static int (*orig_close)(int fd);
static off_t (*orig_lseek)(int fd, off_t offset, int whence);
static off64_t (*orig_lseek64)(int fd, off64_t offset, int whence);
static ssize_t (*orig_pread)(int fd, void* buf, size_t count, off_t offset);
static ssize_t (*orig_pread64)(int fd, void* buf, size_t count, off64_t offset);
static void* (*orig_mmap)(void* addr, size_t length, int prot, int flags, int fd, off_t offset);
static void* (*orig_mmap64)(void* addr, size_t length, int prot, int flags, int fd, off64_t offset);
static int (*orig_stat)(const char*, struct stat*);
static int (*orig_lstat)(const char*, struct stat*);
static int (*orig_fstatat)(int dirfd, const char *pathname, struct stat *statbuf, int flags);
static FILE* (*orig_fopen)(const char*, const char*);

// Phase 2 Originals
#define EGL_VENDOR 0x3053
static const char* (*orig_eglQueryString)(void* display, int name);
static int (*orig_clock_gettime)(clockid_t clockid, struct timespec *tp);
static int (*orig_uname)(struct utsname *buf);
static int (*orig_access)(const char *pathname, int mode);
static int (*orig_getifaddrs)(struct ifaddrs **ifap);

// Phase 3 Originals
static ssize_t (*orig_readlinkat)(int dirfd, const char *pathname, char *buf, size_t bufsiz);
static ssize_t (*orig_readlink)(const char *pathname, char *buf, size_t bufsiz);
static int (*orig_statfs)(const char *path, struct statfs *buf);
static int (*orig_statvfs)(const char *path, struct statvfs *buf);

// PR71: execve hook — intercept getprop exec to prevent property leak via child process
static int (*orig_execve)(const char *pathname, char *const argv[], char *const envp[]);

// PR71b: posix_spawn / posix_spawnp hooks — Android 10+ uses posix_spawn instead of execve
// for Runtime.exec() and ProcessBuilder, bypassing our execve hook entirely.
static int (*orig_posix_spawn)(pid_t *pid, const char *path,
                               const void *file_actions,
                               const void *attrp,
                               char *const argv[], char *const envp[]);
static int (*orig_posix_spawnp)(pid_t *pid, const char *file,
                                const void *file_actions,
                                const void *attrp,
                                char *const argv[], char *const envp[]);

// PR44: Camera2 — CameraMetadataNative.nativeReadValues(int tag) : byte[]
// Firma AOSP Android 11: instance method, (I)[B — SIN parámetro ptr explícito
static jbyteArray (*orig_nativeReadValues)(JNIEnv*, jobject, jint);

// PR44: MediaCodec — native_setup(String name, boolean nameIsType, boolean encoder)
// Firma AOSP Android 11: instance method, (Ljava/lang/String;ZZ)V
static void (*orig_native_setup)(JNIEnv*, jobject, jstring, jboolean, jboolean);

// Phase 4 Originals (v11.9.9)
static int (*orig_sysinfo)(struct sysinfo *info);
static struct dirent* (*orig_readdir)(DIR *dirp);
static unsigned long (*orig_getauxval)(unsigned long type);

// PR70: dl_iterate_phdr — hide module from ELF object enumeration
static int (*orig_dl_iterate_phdr)(int (*callback)(struct dl_phdr_info *, size_t, void *), void *data);

// PR41: dup family — cerrar bypass de caché VFS
static int (*orig_dup)(int oldfd);
static int (*orig_dup2)(int oldfd, int newfd);
static int (*orig_dup3)(int oldfd, int newfd, int flags);

// PR43: fcntl hook — cerrar F_DUPFD bypass de caché VFS
static int (*orig_fcntl)(int fd, int cmd, ...);

// PR42: ioctl hook para blindaje de MAC frente a llamadas directas al kernel
static int (*orig_ioctl)(int, unsigned long, void*) = nullptr;

// PR84: __system_property_find hook — fake prop_info for spoofed properties
// When detection apps call __system_property_find() and read the returned prop_info
// struct's value field directly (raw memory dereference, no API call), our hooks on
// __system_property_get / __system_property_read / __system_property_read_callback
// are completely bypassed. This fake prop_info approach returns a struct we control
// so even direct memory reads see the spoofed value.
struct FakePropInfo {
    uint32_t serial;                    // offset 0: atomic serial (matches bionic layout)
    char value[PROP_VALUE_MAX];         // offset 4: property value (92 bytes)
    char name[256];                     // name storage — large enough for MIUI long names
};
static std::unordered_map<std::string, FakePropInfo*> g_fakePropMap;
static std::set<const void*> g_fakePropPtrs;           // O(log n) lookup for fake detection
static std::mutex g_fakePropMutex;
static thread_local bool g_inPropertyFind = false;      // recursion guard
static bool g_realDeviceIsMiui = false;                  // PR91-fix: true if real device runs MIUI (preserve framework props)
static bool g_pagesPatched = false;                      // PR91: true after patchPropertyPages succeeds — skip FakePropInfo
static bool g_propFindInlineHooked = false;              // PR86: true if Dobby inline hook on __system_property_find succeeded
static bool g_propForeachInlineHooked = false;           // PR88: true if Dobby inline hook on __system_property_foreach succeeded
static bool g_spawnInlineHooked = false;                 // PR90: true if Dobby inline hooks on posix_spawn/posix_spawnp succeeded

// PR86: dlsym hook — intercept dynamic symbol resolution for property functions.
// When detection apps call dlsym(RTLD_DEFAULT, "__system_property_find") to get a raw
// function pointer, they bypass both PLT hooks AND inline hooks (if the app's .so was
// loaded after hooks were installed). This hook returns our spoofing functions instead.
static void* (*orig_dlsym)(void* handle, const char* symbol) = nullptr;
static thread_local bool g_inDlsym = false;  // recursion guard for dlsym hook

// PR87: android_dlopen_ext / dlopen hooks — re-apply PLT hooks to late-loaded libraries.
// When apps call System.loadLibrary() or dlopen(), the dynamic linker loads the .so
// and writes real function addresses to its GOT using internal routines (NOT dlsym).
// Our initial PLT hooks only covered libraries loaded during postAppSpecialize.
// By hooking the load event, we re-apply PLT hooks to newly loaded libraries.
static void* (*orig_android_dlopen_ext)(const char*, int, const void*) = nullptr;
static void* (*orig_dlopen_hook)(const char*, int) = nullptr;
static thread_local int g_dlopenReapplyDepth = 0;  // recursion guard (nested dlopen for deps)
static std::mutex g_reapplyMutex;                   // thread safety for pltHookRegister/Commit

// PR88: __system_property_foreach hook — intercept property enumeration.
// Detection apps call __system_property_foreach(callback, cookie) to iterate ALL properties
// in the shared memory area. The callback receives real prop_info* pointers directly,
// bypassing __system_property_find (which we hook). Our wrapper callback reads the property
// name from each real prop_info, then passes a FakePropInfo (with spoofed value) to the
// user's callback for properties that should be spoofed.
static int (*orig_system_property_foreach)(
    void (*propfn)(const prop_info* pi, void* cookie), void* cookie) = nullptr;

// PR81: NetworkInterface hooks — DISABLED (PR83), see postAppSpecialize comment.

// SSL Pointers
typedef struct ssl_ctx_st SSL_CTX;
typedef struct ssl_st SSL;
static int (*orig_SSL_CTX_set_ciphersuites)(SSL_CTX *ctx, const char *str);
static int (*orig_SSL_set1_tls13_ciphersuites)(SSL *ssl, const char *str);
static int (*orig_SSL_set_cipher_list)(SSL *ssl, const char *str);
static int (*orig_SSL_set_ciphersuites)(SSL *ssl, const char *str);

// GL Pointers
typedef unsigned char GLubyte;
typedef unsigned int GLenum;
#define GL_VENDOR 0x1F00
#define GL_RENDERER 0x1F01
#define GL_VERSION 0x1F02
static const GLubyte* (*orig_glGetString)(GLenum name);

// OpenCL
typedef int cl_int;
typedef void* cl_device_id;
typedef unsigned int cl_device_info;
#define CL_DEVICE_NAME 0x102B
#define CL_DEVICE_VENDOR 0x102C
#define CL_DRIVER_VERSION 0x102D
#define CL_DEVICE_VERSION 0x102F
static cl_int (*orig_clGetDeviceInfo)(cl_device_id device, cl_device_info param_name, size_t param_value_size, void *param_value, size_t *param_value_size_ret);

// Vulkan
static void (*orig_vkGetPhysicalDeviceProperties)(VkPhysicalDevice physicalDevice, VkPhysicalDeviceProperties* pProperties);

// Settings.Secure
static jstring (*orig_SettingsSecure_getString)(JNIEnv*, jstring);
static jstring (*orig_SettingsSecure_getString3)(JNIEnv*, jobject, jstring);
static jstring (*orig_SettingsSecure_getStringForUser)(JNIEnv*, jstring, jint);
static jstring (*orig_SettingsSecure_getStringForUser4)(JNIEnv*, jobject, jstring, jint);

// Settings.Global
static jstring (*orig_SettingsGlobal_getString)(JNIEnv*, jstring);
static jstring (*orig_SettingsGlobal_getString3)(JNIEnv*, jobject, jstring);
static jstring (*orig_SettingsGlobal_getStringForUser)(JNIEnv*, jstring, jint);
static jstring (*orig_SettingsGlobal_getStringForUser4)(JNIEnv*, jobject, jstring, jint);

// Helper
inline std::string toLowerStr(const char* s) {
    if (!s) return "";
    std::string res = s;
    std::transform(res.begin(), res.end(), res.begin(), [](unsigned char c){ return std::tolower(c); });
    return res;
}

// Config
// PR70c: Shared config parser — used by both direct file read and companion IPC
static void parseConfigString(const std::string& content) {
    g_config.clear();
    std::istringstream stream(content);
    std::string line;
    while (std::getline(stream, line)) {
        size_t eq = line.find('=');
        if (eq != std::string::npos) {
            std::string key = line.substr(0, eq);
            std::string val = line.substr(eq + 1);
            key.erase(0, key.find_first_not_of(" \t\r"));
            key.erase(key.find_last_not_of(" \t\r") + 1);
            val.erase(0, val.find_first_not_of(" \t\r"));
            val.erase(val.find_last_not_of(" \t\r") + 1);
            g_config[key] = val;
        }
    }
    if (g_config.count("profile"))       g_currentProfileName = g_config["profile"];
    if (g_config.count("master_seed"))   try { g_masterSeed = std::stol(g_config["master_seed"]); } catch(...) {}
    if (g_config.count("jitter"))        g_enableJitter = (g_config["jitter"] == "true");
    if (g_config.count("network_type"))  g_spoofMobileNetwork = (g_config["network_type"] == "lte" || g_config["network_type"] == "mobile");
    if (g_config.count("debug_mode"))    g_debugMode = (g_config["debug_mode"] == "true");
    if (g_config.count("seed_version")) {
        long newSeedVersion = 0;
        try { newSeedVersion = std::stol(g_config["seed_version"]); } catch(...) {}
        if (omni::engine::isSeedVersionChanged(newSeedVersion, g_seedVersion)) {
            g_locationCached = false;
            g_seedVersion    = newSeedVersion;
        }
    }
    // PR96: Load user-pinned location from config (set via UI Location selector).
    // initLocationCache() uses generateLocationForRegion() which ignores these fields.
    // By setting g_cachedLat/Lon here and marking g_locationCached=true we prevent
    // initLocationCache() from overwriting with the profile-generated coordinates.
    if (g_config.count("location_lat") && g_config.count("location_lon")) {
        try {
            g_cachedLat      = std::stod(g_config["location_lat"]);
            g_cachedLon      = std::stod(g_config["location_lon"]);
            g_locationCached = true;
            if (g_config.count("location_alt")) {
                try { g_cachedAlt = std::stod(g_config["location_alt"]); } catch(...) {}
            }
        } catch(...) {}
    }
    // PR105: Propagar location pinneada por el usuario a los atómicos del Binder hook.
    if (g_locationCached) {
        int64_t lb, lob;
        memcpy(&lb,  &g_cachedLat, 8);
        memcpy(&lob, &g_cachedLon, 8);
        g_cachedLatBits.store(lb,  std::memory_order_release);
        g_cachedLonBits.store(lob, std::memory_order_release);
    }
}

// Direct file read — fallback when companion is unavailable
void readConfig() {
    std::ifstream file("/data/adb/.omni_data/.identity.cfg");
    if (!file.is_open()) {
        LOGD("[scope] readConfig: FAILED to open /data/adb/.omni_data/.identity.cfg (SELinux?)");
        return;
    }
    std::string content((std::istreambuf_iterator<char>(file)),
                         std::istreambuf_iterator<char>());
    LOGD("[scope] readConfig: file opened OK (%zu bytes)", content.size());
    parseConfigString(content);
    LOGD("[scope] readConfig: parsed %zu keys, scoped_apps=%s",
         g_config.size(),
         g_config.count("scoped_apps") ? g_config["scoped_apps"].c_str() : "(none)");
}

// PR70c: Companion-based config reader — bypasses SELinux restrictions.
// The companion runs as root daemon in u:r:su:s0 context and can read any file.
static bool readConfigViaCompanion(zygisk::Api *api) {
    if (!api) return false;
    int fd = api->connectCompanion();
    if (fd < 0) {
        LOGD("[scope] companion: connect failed (fd=%d)", fd);
        return false;
    }
    uint32_t len = 0;
    ssize_t n = read(fd, &len, sizeof(len));
    if (n != (ssize_t)sizeof(len) || len == 0 || len > 65536) {
        LOGD("[scope] companion: bad header (n=%zd len=%u)", n, len);
        close(fd);
        return false;
    }
    std::string buf(len, '\0');
    ssize_t total = 0;
    while (total < (ssize_t)len) {
        ssize_t r = read(fd, &buf[total], len - total);
        if (r <= 0) break;
        total += r;
    }
    close(fd);
    if (total != (ssize_t)len) {
        LOGD("[scope] companion: incomplete read (%zd/%u)", total, len);
        return false;
    }
    LOGD("[scope] companion: read OK (%u bytes)", len);
    parseConfigString(buf);
    LOGD("[scope] companion: parsed %zu keys, scoped_apps=%s",
         g_config.size(),
         g_config.count("scoped_apps") ? g_config["scoped_apps"].c_str() : "(none)");
    return true;
}

// PR121: Construye una firma de los campos que afectan spoof de ubicación.
static std::string buildRuntimeIdentitySignature(const std::map<std::string, std::string>& cfg) {
    static const char* kKeys[] = {
        "profile",
        "master_seed",
        "seed_version",
        "location_lat",
        "location_lon",
        "location_alt",
        "jitter",
        nullptr
    };
    std::ostringstream oss;
    for (int i = 0; kKeys[i]; ++i) {
        const char* key = kKeys[i];
        auto it = cfg.find(key);
        oss << key << '=';
        if (it != cfg.end()) oss << it->second;
        oss << ';';
    }
    return oss.str();
}

// PR121: Reinicia procesos críticos de ubicación para evitar coordenadas stale
// en procesos persistentes de GMS/Maps (que pueden seguir vivos tras cambios).
static void restartLocationRuntime() {
    system("sh -c '"
           "am force-stop com.google.android.apps.maps 2>/dev/null; "
           "am force-stop com.google.android.gms 2>/dev/null; "
           "killall com.google.android.gms.unstable 2>/dev/null; "
           "killall com.google.android.gms.persistent 2>/dev/null; "
           "killall com.google.android.gms.location.history 2>/dev/null; "
           "killall com.android.location.fused 2>/dev/null; "
           "killall com.xiaomi.location.fused 2>/dev/null; "
           "killall com.mediatek.location.lppe.main 2>/dev/null; "
           "killall com.mediatek.location.ppe.main 2>/dev/null"
           "' &");
    LOGE("[PR121] Restarted Maps/GMS location stack after identity/location change");
}

bool shouldHide(const char* key) {
    if (!key || key[0] == '\0') return false;
    std::string s = toLowerStr(key);
    if (g_currentProfileName == "Redmi 9" && s.find("lancelot") != std::string::npos) return false;
    const DeviceFingerprint* fp_ptr = findProfile(g_currentProfileName);
    if (fp_ptr) {
        const auto& fp = *fp_ptr;
        if (toLowerStr(fp.brand).find("xiaomi") != std::string::npos ||
            toLowerStr(fp.hardware).find("mt") != std::string::npos) {
            // PR72-QA Fix3: When target profile is Xiaomi/MediaTek, allow
            // MediaTek ecosystem strings through (they're expected for this target)
            // PR73-Fix4: also allow "miui" through for Xiaomi targets
            if (s.find("mediatek") != std::string::npos ||
                s.find("huaqin")   != std::string::npos ||
                s.find("mt6769")   != std::string::npos ||
                s.find("moly.")    != std::string::npos ||
                s.find("miui")     != std::string::npos) return false;
        }
    }
    // PR72-QA Fix3: Expanded filters — hide ODM manufacturer (huaqin),
    // SoC identifier (mt6769), and modem prefix (moly.) from property values.
    // PR73-Fix4: Added "miui" — hides ro.miui.*, ro.vendor.miui.* and any
    // MIUI-specific value when the target profile is not Xiaomi/MIUI.
    // PR91-fix: On real MIUI devices, don't suppress "miui" properties — the MIUI
    // framework in every process depends on them for permissions, settings, etc.
    bool hit = s.find("mediatek") != std::string::npos ||
               s.find("lancelot") != std::string::npos ||
               s.find("huaqin")   != std::string::npos ||
               s.find("mt6769")   != std::string::npos ||
               s.find("moly.")    != std::string::npos;
    if (!hit && !g_realDeviceIsMiui) {
        hit = s.find("miui") != std::string::npos;
    }
    return hit;
}

// -----------------------------------------------------------------------------
// Hooks: OpenCL
// -----------------------------------------------------------------------------
cl_int my_clGetDeviceInfo(cl_device_id device, cl_device_info param_name, size_t param_value_size, void *param_value, size_t *param_value_size_ret) {
    if (!orig_clGetDeviceInfo) return -1;
    cl_int ret = orig_clGetDeviceInfo(device, param_name, param_value_size, param_value, param_value_size_ret);
    const DeviceFingerprint* fp_ptr = findProfile(g_currentProfileName);
    if (ret == 0 && fp_ptr) {
        const auto& fp = *fp_ptr;
        std::string egl = toLowerStr(fp.eglDriver);

        // En my_clGetDeviceInfo, añadir constantes y lógica de driver:
        #define CL_DEVICE_VERSION 0x102F
        #define CL_DRIVER_VERSION 0x102D

        if (egl == "adreno") {
            if (param_name == CL_DEVICE_VENDOR) strncpy((char*)param_value, "Qualcomm", param_value_size);
            else if (param_name == CL_DEVICE_NAME) strncpy((char*)param_value, fp.gpuRenderer, param_value_size);
            else if (param_name == CL_DEVICE_VERSION || param_name == CL_DRIVER_VERSION)
                strncpy((char*)param_value, "OpenCL 2.0 QUALCOMM build", param_value_size);
        }
    }
    return ret;
}

// -----------------------------------------------------------------------------
// Phase 2 Hooks
// -----------------------------------------------------------------------------

static inline bool isHiddenPath(const char* path) {
    if (!path || path[0] == '\0') return false;

    static const char* HIDDEN_TOKENS[] = {
        "omnishield", "omni_data", "vortex",
        "magisk", "kernelsu", "susfs",
        "android_cache_data",
        nullptr
    };

    for (int i = 0; HIDDEN_TOKENS[i] != nullptr; ++i) {
        if (strcasestr(path, HIDDEN_TOKENS[i])) return true;
    }
    return false;
}

// PR70: Match /proc/<digits>/<suffix> — detection apps use /proc/<getpid()>/maps
// instead of /proc/self/maps to bypass self-only filtering.
static inline bool isProcPidPath(const char* path, const char* suffix) {
    if (!path || strncmp(path, "/proc/", 6) != 0) return false;
    const char* p = path + 6;
    if (*p < '1' || *p > '9') return false;
    while (*p >= '0' && *p <= '9') p++;
    if (*p != '/') return false;
    return strcmp(p + 1, suffix) == 0;
}

// PR70: Remap module .so memory from file-backed to anonymous.
// After hooks are installed, the .so code/data stays at the same virtual
// addresses but /proc/self/maps shows device 00:00 inode 0 (anonymous)
// instead of the file path.  This hides the module from maps readers even
// if they bypass our openat/read hooks (e.g. direct syscall).
static void remapModuleMemory() {
    // PR105: Usar syscall directo para evitar pasar por my_fopen,
    // que filtraría las entradas del propio módulo y haría esta función un no-op.
    int _maps_fd = (int)syscall(__NR_openat, AT_FDCWD, "/proc/self/maps",
                                O_RDONLY | O_CLOEXEC);
    if (_maps_fd < 0) return;
    FILE *fp = fdopen(_maps_fd, "re");
    if (!fp) { close(_maps_fd); return; }
    char line[512];
    while (fgets(line, sizeof(line), fp)) {
        if (!isHiddenPath(line)) continue;
        uintptr_t start, end;
        char perms[5];
        if (sscanf(line, "%lx-%lx %4s", &start, &end, perms) != 3) continue;
        size_t size = end - start;
        if (size == 0) continue;
        int prot = 0;
        if (perms[0] == 'r') prot |= PROT_READ;
        if (perms[1] == 'w') prot |= PROT_WRITE;
        if (perms[2] == 'x') prot |= PROT_EXEC;
        // Allocate anonymous memory with original perms (so code pages stay
        // executable after mremap — the function itself lives in this .so)
        void *copy = mmap(nullptr, size, prot | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
        if (copy == MAP_FAILED) continue;
        // Ensure original is readable so we can copy
        if (!(prot & PROT_READ)) mprotect((void*)start, size, PROT_READ);
        memcpy(copy, (void*)start, size);
        // Atomically replace file-backed mapping with anonymous copy
        void *result = mremap(copy, size, size, MREMAP_MAYMOVE | MREMAP_FIXED, (void*)start);
        if (result != MAP_FAILED) {
            // Strip PROT_WRITE from segments that shouldn't have it (e.g. r-xp code)
            mprotect((void*)start, size, prot);
        } else {
            munmap(copy, size);
        }
    }
    fclose(fp);
}

// Fix8: Parsear /proc/self/maps para obtener (dev, inode) de cada .so cargada.
// Usado por installPltHooks() para registrar PLT hooks en todas las librerías.
struct ElfEntry { dev_t dev; ino_t inode; };

static std::vector<ElfEntry> enumerateLoadedElfs() {
    std::vector<ElfEntry> result;
    std::set<std::pair<unsigned int, unsigned long>> seen;

    FILE *fp = fopen("/proc/self/maps", "re");
    if (!fp) return result;

    char line[512];
    while (fgets(line, sizeof(line), fp)) {
        unsigned long addr_s, addr_e, offset, ino;
        unsigned int dev_maj, dev_min;
        char perms[5] = {}, path[256] = {};

        int n = sscanf(line, "%lx-%lx %4s %lx %x:%x %lu %255s",
                        &addr_s, &addr_e, perms, &offset, &dev_maj, &dev_min, &ino, path);
        if (n < 7 || ino == 0) continue;
        if (n >= 8 && strstr(path, ".so")) {
            // Fix9: Skip our own module to prevent self-referential PLT hooks.
            if (isHiddenPath(path)) continue;
            // Fix11b: Selective /apex/ filter — include ONLY libjavacore.so.
            // libjavacore.so (/apex/com.android.art/lib64/) is where
            // ProcessBuilder.start() calls posix_spawnp() → must be PLT-hooked
            // to intercept subprocess commands (getprop, uname, cat /proc/...).
            // All other /apex/ ELFs are excluded: they don't call the functions
            // we hook (posix_spawn, execve, __system_property_read), and
            // including them causes crashes on MIUI/MTK ROMs where pltHookCommit
            // fails on those ELFs (SIGBUS or xhook internal error).
            if (strstr(path, "/apex/") && !strstr(path, "libjavacore.so")) continue;
            dev_t dev = makedev(dev_maj, dev_min);
            auto key = std::make_pair((unsigned int)dev, ino);
            if (seen.insert(key).second)
                result.push_back({dev, (ino_t)ino});
        }
    }
    fclose(fp);
    return result;
}

int my_stat(const char* pathname, struct stat* statbuf) {
    if (!orig_stat) { errno = ENOSYS; return -1; }
    if (isHiddenPath(pathname)) { errno = ENOENT; return -1; }
    return orig_stat(pathname, statbuf);
}
int my_lstat(const char* pathname, struct stat* statbuf) {
    if (!orig_lstat) { errno = ENOSYS; return -1; }
    if (isHiddenPath(pathname)) { errno = ENOENT; return -1; }
    return orig_lstat(pathname, statbuf);
}

int my_fstatat(int dirfd, const char *pathname, struct stat *statbuf, int flags) {
    if (!orig_fstatat) { errno = ENOSYS; return -1; }
    // Resolver path absoluto si es relativo con AT_FDCWD
    if (pathname && pathname[0] != '/' && dirfd == AT_FDCWD) {
        char cwd[512] = {};
        if (getcwd(cwd, sizeof(cwd)) != nullptr) {
            std::string full = std::string(cwd) + "/" + pathname;
            if (isHiddenPath(full.c_str())) { errno = ENOENT; return -1; }
        }
    } else if (isHiddenPath(pathname)) {
        errno = ENOENT;
        return -1;
    }
    return orig_fstatat(dirfd, pathname, statbuf, flags);
}
// Forward declaration — defined at ~line 744, needed here for getCachedCpuInfo
std::string generateMulticoreCpuInfo(const DeviceFingerprint& fp);

// PR71f: Cached cpuinfo content — avoids regenerating the full string on every
// fopen/cat call (some scanners read /proc/cpuinfo in tight loops).
// Invalidated when g_configGeneration changes (profile switch / Destroy Identity).
static std::string g_cpuinfoCached;
static uint64_t g_cpuinfoCachedGen = 0;

static const std::string& getCachedCpuInfo(const DeviceFingerprint& fp) {
    if (g_cpuinfoCached.empty() || g_cpuinfoCachedGen != g_configGeneration) {
        g_cpuinfoCached = generateMulticoreCpuInfo(fp);
        g_cpuinfoCachedGen = g_configGeneration;
    }
    return g_cpuinfoCached;
}

FILE* my_fopen(const char* pathname, const char* mode) {
    if (!orig_fopen) { errno = ENOENT; return nullptr; }
    if (isHiddenPath(pathname)) { errno = ENOENT; return nullptr; }

    // PR71f: Serve faked files via memfd — immune to fread() bypassing read() hook.
    // bionic's fread uses internal __sread which may do direct syscalls, skipping
    // our Dobby hook on read(). memfd sidesteps this entirely: all FILE* ops
    // (fread, fgets, fgetc) read from the memfd's kernel buffer directly.
    if (pathname) {
        const DeviceFingerprint* fp_ptr = findProfile(g_currentProfileName);
        if (fp_ptr) {
            const char* content_ptr = nullptr;
            size_t content_len = 0;
            std::string tmp_content;

            if (strstr(pathname, "/proc/cpuinfo")) {
                const std::string& cached = getCachedCpuInfo(*fp_ptr);
                content_ptr = cached.c_str();
                content_len = cached.size();
            }

            if (content_ptr && content_len > 0) {
                // MFD_CLOEXEC (0x0001): FD won't leak to child processes via exec
                int mfd = syscall(__NR_memfd_create, "vfs", 0x0001u);
                if (mfd >= 0) {
                    write(mfd, content_ptr, content_len);
                    lseek(mfd, 0, SEEK_SET);
                    FILE* f = fdopen(mfd, mode);
                    if (f) return f;
                    close(mfd);
                }
            }
        }
    }
    return orig_fopen(pathname, mode);
}

// 1. EGL Spoofing
#define EGL_EXTENSIONS_ENUM 0x3055

const char* my_eglQueryString(void* display, int name) {
    if (!orig_eglQueryString) return nullptr;
    const DeviceFingerprint* fp_ptr = findProfile(g_currentProfileName);
    if (fp_ptr) {
        const auto& fp = *fp_ptr;

        if (name == EGL_VENDOR) return fp.gpuVendor;

        if (name == EGL_EXTENSIONS_ENUM) {
            const char* orig_ext = orig_eglQueryString(display, name);
            if (!orig_ext) return orig_ext;

            std::string egl = toLowerStr(fp.eglDriver);
            if (egl == "adreno") {
                static thread_local std::string ext_cache;
                ext_cache = orig_ext;

                // Erase ARM/Mali extensions — NO replace (crear EGL_QCOM_* falsos es más detectable)
                static const char* ARM_EXTS[] = {"EGL_ARM_", "EGL_MALI_", "EGL_IMG_", nullptr};
                for (int i = 0; ARM_EXTS[i]; ++i) {
                    size_t pos;
                    while ((pos = ext_cache.find(ARM_EXTS[i])) != std::string::npos) {
                        size_t end = ext_cache.find(' ', pos);
                        if (end == std::string::npos) ext_cache.erase(pos);
                        else ext_cache.erase(pos, end - pos + 1);
                    }
                }
                while (ext_cache.find("  ") != std::string::npos)
                    ext_cache.replace(ext_cache.find("  "), 2, " ");

                return ext_cache.c_str();
            }
        }
    }
    return orig_eglQueryString(display, name);
}

// 2. Uptime Spoofing
int my_clock_gettime(clockid_t clockid, struct timespec *tp) {
    if (!orig_clock_gettime) return -1;
    int ret = orig_clock_gettime(clockid, tp);
    // Solo modificamos relojes de uptime de sistema
    if (ret == 0 && (clockid == CLOCK_BOOTTIME || clockid == CLOCK_MONOTONIC)) {
        // Offset determinista: Base de 3 días (259200s) + hasta 12 días extra según semilla
        long added_uptime_seconds = 259200 + (g_masterSeed % 1036800);
        tp->tv_sec += added_uptime_seconds;
    }
    return ret;
}

// 3. Kernel Identity

// PR72-QA Fix2: Helper to compute spoofed kernel version string.
// Extracted from my_uname() so it can also be used in postAppSpecialize
// to override the ART VM cached System.getProperty("os.version").
std::string getSpoofedKernelVersion() {
    std::string kv = "4.14.186-perf+";
    if (const DeviceFingerprint* kfp_ptr = findProfile(g_currentProfileName)) {
        const auto& kfp = *kfp_ptr;
        std::string plat = toLowerStr(kfp.boardPlatform);
        std::string brd  = toLowerStr(kfp.brand);

        if (brd == "google") {
            if (plat.find("lito") != std::string::npos)
                kv = "4.19.113-g820a424c538c-ab7336171";
            else if (plat.find("atoll") != std::string::npos)
                kv = "4.14.150-g62a62a5a93f7-ab7336171";
            else if (plat.find("sdm670") != std::string::npos)
                kv = "4.9.189-g5d098cef6d96-ab6174032";
            else
                kv = "4.19.113-g820a424c538c-ab7336171";
        } else if (plat.find("mt6") != std::string::npos) {
            // Samsung MTK uses numeric kernel suffix, not Qualcomm-style -perf+
            if (brd == "samsung") {
                if (plat.find("mt6769") != std::string::npos)
                    kv = "4.14.113-23424440";  // Galaxy A32 4G / A22 4G
                else
                    kv = "4.14.113-25267920";  // Galaxy A31 (MT6768)
            } else {
                kv = "4.14.186-perf+";
            }
        } else if (plat.find("kona") != std::string::npos || plat.find("lahaina") != std::string::npos) {
            kv = "4.19.157-perf+";
        } else if (plat.find("atoll") != std::string::npos || plat.find("lito") != std::string::npos) {
            kv = "4.19.113-perf+";
        } else if (plat.find("sdm670") != std::string::npos) {
            kv = "4.9.189-perf+";
        } else if (plat.find("bengal") != std::string::npos || plat.find("holi") != std::string::npos ||
                   plat.find("sm6350") != std::string::npos) {
            kv = "4.19.157-perf+";
        } else if (plat.find("sm7325") != std::string::npos) {
            kv = "5.4.61-perf+";
        }
        // PR41: Kernels Samsung Exynos — sufijo numérico (NO -perf+ que es Qualcomm)
        else if (plat.find("exynos9611") != std::string::npos) {
            kv = "4.14.113-25145160";
        } else if (plat.find("exynos9825") != std::string::npos) {
            kv = "4.14.113-22911262";
        } else if (plat.find("exynos850") != std::string::npos) {
            kv = "4.19.113-25351273";
        }
    }
    return kv;
}

int my_uname(struct utsname *buf) {
    if (!orig_uname) return -1;
    int ret = orig_uname(buf);
    if (ret == 0 && buf != nullptr) {
        strcpy(buf->machine, "aarch64"); strcpy(buf->nodename, "localhost");
        std::string kv = getSpoofedKernelVersion();
        strcpy(buf->release, kv.c_str());
        strcpy(buf->version, "#1 SMP PREEMPT");
    }
    return ret;
}

// PR73-Fix3: Hook JNI android.system.Os.uname() via libcore.io.Linux.uname().
// android.system.Os.uname() delegates through libcore to Linux.uname() which
// invokes the uname(2) syscall DIRECTLY — bypassing the Dobby libc hook above.
// This JNI hook is registered via hookJniNativeMethods("libcore/io/Linux").
// StructUtsname constructor order: (sysname, nodename, release, version, machine)
static jobject my_Linux_uname(JNIEnv* env, jobject /*thiz*/) {
    std::string kv = getSpoofedKernelVersion();
    jclass cls = env->FindClass("android/system/StructUtsname");
    if (!cls) { if (env->ExceptionCheck()) env->ExceptionClear(); return nullptr; }
    jmethodID ctor = env->GetMethodID(cls, "<init>",
        "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;"
        "Ljava/lang/String;Ljava/lang/String;)V");
    if (!ctor) {
        env->DeleteLocalRef(cls);
        if (env->ExceptionCheck()) env->ExceptionClear();
        return nullptr;
    }
    jobject result = env->NewObject(cls, ctor,
        env->NewStringUTF("Linux"),
        env->NewStringUTF("localhost"),
        env->NewStringUTF(kv.c_str()),
        env->NewStringUTF("#1 SMP PREEMPT"),
        env->NewStringUTF("aarch64"));
    env->DeleteLocalRef(cls);
    if (env->ExceptionCheck()) env->ExceptionClear();
    return result;
}

// PR42: Intercepta ioctl SIOCGIFHWADDR para wlan0/eth0
// Las apps nativas en C/C++ evitan getifaddrs y leen la MAC directamente del kernel.
// Este hook retorna 02:00:00:00:00:00 (MAC de privacidad AOSP) en ambas interfaces.

#ifndef ARPHRD_ETHER
#define ARPHRD_ETHER 1
#endif

int my_ioctl(int fd, unsigned long request, void* arg) {
    if (!orig_ioctl) return -1;
    int ret = orig_ioctl(fd, request, arg);
    if (ret == 0 && arg != nullptr) {
        if (request == SIOCGIFHWADDR) {
            struct ifreq* ifr = static_cast<struct ifreq*>(arg);
            if (strcmp(ifr->ifr_name, "wlan0") == 0 || strcmp(ifr->ifr_name, "eth0") == 0) {
                unsigned char static_mac[6] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x00};
                memcpy(ifr->ifr_hwaddr.sa_data, static_mac, 6);
                ifr->ifr_hwaddr.sa_family = ARPHRD_ETHER;
            }
        }
        // PR84: Filter wlan0/dummy0 from SIOCGIFCONF response.
        // NetworkInterface.getAll() → native enumIPv4Interfaces → ioctl(SIOCGIFCONF)
        // returns the full interface list. Detection apps compare this against the
        // claimed network type (LTE) to detect WiFi usage.
        else if (request == SIOCGIFCONF) {
            struct ifconf* ifc = static_cast<struct ifconf*>(arg);
            if (ifc->ifc_req && ifc->ifc_len > 0) {
                struct ifreq* ifr = ifc->ifc_req;
                int num_ifaces = ifc->ifc_len / (int)sizeof(struct ifreq);
                int valid = 0;
                for (int i = 0; i < num_ifaces; i++) {
                    const char* ifname = ifr[i].ifr_name;
                    // Filter WiFi and virtual interfaces that reveal WiFi connectivity
                    if (strcmp(ifname, "wlan0") == 0 || strcmp(ifname, "wlan1") == 0 ||
                        strncmp(ifname, "dummy", 5) == 0 || strncmp(ifname, "p2p", 3) == 0 ||
                        strcmp(ifname, "ap0") == 0) {
                        continue;  // skip this interface
                    }
                    // Keep valid interface — compact the array in-place
                    if (valid != i) {
                        ifr[valid] = ifr[i];
                    }
                    valid++;
                }
                ifc->ifc_len = valid * (int)sizeof(struct ifreq);
            }
        }
    }
    return ret;
}

// PR43: fcntl hook (Duplicate FD propagation)
// Intercepta fcntl(F_DUPFD) para propagar la caché VFS al nuevo descriptor.
// Argumento 'arg' es long para cubrir tanto int (F_DUPFD) como punteros (F_GETLK).
int my_fcntl(int fd, int cmd, long arg) {
    if (!orig_fcntl) return -1;
    int ret = orig_fcntl(fd, cmd, arg);
    if (ret >= 0 && (cmd == F_DUPFD || cmd == F_DUPFD_CLOEXEC)) {
        // Propagar caché VFS al nuevo FD clonado
        std::lock_guard<std::mutex> lock(g_fdMutex);
        auto it = g_fdMap.find(fd);
        if (it != g_fdMap.end()) {
            g_fdMap[ret] = it->second;
            g_fdOffsetMap[ret] = 0;
            auto cache_it = g_fdContentCache.find(fd);
            if (cache_it != g_fdContentCache.end())
                g_fdContentCache[ret] = cache_it->second;
        }
    }
    return ret;
}

// 4. Deep VFS (Root Hiding)
int my_access(const char *pathname, int mode) {
    if (!orig_access) return -1;
    if (isHiddenPath(pathname)) { errno = ENOENT; return -1; }
    return orig_access(pathname, mode);
}

// 5. Network Interfaces (Layer 2)
int my_getifaddrs(struct ifaddrs **ifap) {
    if (!orig_getifaddrs) return -1;
    int ret = orig_getifaddrs(ifap);
    if (ret == 0 && ifap != nullptr && *ifap != nullptr) {
        struct ifaddrs *curr = *ifap;
        struct ifaddrs *prev = nullptr;
        while (curr) {
            bool remove = false;
            if (curr->ifa_name) {
                std::string name = curr->ifa_name;
                // PR43: Stealth filtering (eth0, p2p0, tun, dummy)
                if (name == "eth0" || name == "p2p0" ||
                    name.find("dummy") == 0 || name.find("tun") == 0) {
                    remove = true;
                }
                // PR43: En modo LTE spoofing, ocultar wlan0 también
                if (g_spoofMobileNetwork && name == "wlan0") {
                    remove = true;
                }
            }

            if (remove) {
                // Desvincular nodo de la lista
                if (prev) {
                    prev->ifa_next = curr->ifa_next;
                    curr = curr->ifa_next;
                } else {
                    *ifap = curr->ifa_next; // Nuevo head
                    curr = curr->ifa_next;
                }
            } else {
                // Mantener nodo, aplicar spoofing si es wlan0 (y no estamos en modo LTE)
                if (curr->ifa_addr && curr->ifa_addr->sa_family == AF_PACKET &&
                    curr->ifa_name && strcmp(curr->ifa_name, "wlan0") == 0) {
                    struct sockaddr_ll *s = (struct sockaddr_ll*)curr->ifa_addr;
                    // Static MAC 02:00:00:00:00:00 for AOSP privacy
                    unsigned char static_mac[] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x00};
                    memcpy(s->sll_addr, static_mac, 6);
                }
                prev = curr;
                curr = curr->ifa_next;
            }
        }
    }
    return ret;
}

// -----------------------------------------------------------------------------
// Phase 3 Hooks & Helpers
// -----------------------------------------------------------------------------

static std::string getArmFeatures(const std::string& platform) {
    // PR43 FIX: mt6769 (Helio G80/G85) y exynos850 son Cortex-A55 (ARMv8.2) → soportan lrcpc/dcpop.
    // Solo mt6765 (P35, Cortex-A53) y exynos9611 (A73/A53) deben reportar features ARMv8.0.
    if (platform.find("mt6765") != std::string::npos ||
        platform.find("exynos9611") != std::string::npos)
        return "fp asimd evtstrm aes pmull sha1 sha2 crc32 atomics fphp asimdhp cpuid asimdrdm";
    return "fp asimd evtstrm aes pmull sha1 sha2 crc32 atomics fphp asimdhp cpuid asimdrdm lrcpc dcpop asimddp";
}

std::string generateMulticoreCpuInfo(const DeviceFingerprint& fp) {
    std::string out;
    std::string platform = toLowerStr(fp.boardPlatform);
    std::string features = getArmFeatures(platform);

    if (platform.find("mt6768") != std::string::npos ||
        platform.find("mt6769") != std::string::npos ||
        platform.find("mt6833") != std::string::npos ||
        platform.find("mt6853") != std::string::npos) {
        // Familia A55+A75/A76 big.LITTLE
        int bigCoreStart = fp.core_count - 2;
        for(int i = 0; i < fp.core_count; ++i) {
            bool isBig = (i >= bigCoreStart);
            out += "processor\t: " + std::to_string(i) + "\n";
            out += "BogoMIPS\t: " + std::string(isBig ? "52.00" : "26.00") + "\n";
            out += "Features\t: " + features + "\n";
            out += "CPU implementer\t: 0x41\n";
            out += "CPU architecture: 8\n";
            out += "CPU variant\t: 0x0\n";
            out += "CPU part\t: " + std::string(isBig ? "0xd0a" : "0xd05") + "\n";
            out += "CPU revision\t: " + std::string(isBig ? "2" : "1") + "\n\n";
        }
    } else if (platform.find("mt6785") != std::string::npos) {
        // Helio G95: 8x Cortex-A55 homogéneo (NO tiene A75)
        for(int i = 0; i < fp.core_count; ++i) {
            out += "processor\t: " + std::to_string(i) + "\n";
            out += "BogoMIPS\t: 26.00\n";
            out += "Features\t: " + features + "\n";
            out += "CPU implementer\t: 0x41\n";
            out += "CPU architecture: 8\n";
            out += "CPU variant\t: 0x0\n";
            out += "CPU part\t: 0xd05\n";
            out += "CPU revision\t: 1\n\n";
        }
    } else if (platform.find("mt6765") != std::string::npos) {
        // Helio P35: 8x Cortex-A53 homogéneo
        for(int i = 0; i < fp.core_count; ++i) {
            out += "processor\t: " + std::to_string(i) + "\n";
            out += "BogoMIPS\t: 26.00\n";
            out += "Features\t: " + features + "\n";
            out += "CPU implementer\t: 0x41\n";
            out += "CPU architecture: 8\n";
            out += "CPU variant\t: 0x0\n";
            out += "CPU part\t: 0xd03\n"; // Cortex-A53
            out += "CPU revision\t: 0\n\n";
        }
    } else {
        // Fallback: Qualcomm big.LITTLE A77/A78+A55 vs Exynos/otros (A55 homogeneous)
        std::string brandLower = toLowerStr(fp.brand);
        bool isQualcomm = (brandLower == "google") ||
            (platform.find("msmnile") != std::string::npos) ||
            (platform.find("kona")    != std::string::npos) ||
            (platform.find("lahaina") != std::string::npos) ||
            (platform.find("atoll")   != std::string::npos) ||
            (platform.find("lito")    != std::string::npos) ||
            (platform.find("bengal")  != std::string::npos) ||
            (platform.find("holi")    != std::string::npos) ||
            (platform.find("trinket") != std::string::npos) ||
            (platform.find("sdm670")  != std::string::npos) ||
            (platform.find("sm6150")  != std::string::npos) ||
            (platform.find("sm6350")  != std::string::npos) ||
            (platform.find("sm7325")  != std::string::npos);
        std::string bogomips = isQualcomm ? "38.40" : "26.00";

        // Qualcomm: big.LITTLE topology o A55 homogéneo según familia
        // bengal y trinket = A55 homogéneo. Resto = big(A77/A78) + LITTLE(A55)
        bool isHomogeneous = (platform.find("bengal") != std::string::npos);

        // Parámetros del núcleo big según plataforma Qualcomm
        const char* bigPart    = "0xd0d";  // Cortex-A77 (default)
        const char* bigVariant = "0x1";
        const char* bigRev     = "0";
        if (platform.find("kona") != std::string::npos ||
            platform.find("msmnile") != std::string::npos) {
            bigPart = "0xd0d"; bigVariant = "0x4"; bigRev = "0";
        } else if (platform.find("lahaina") != std::string::npos) {
            bigPart = "0xd44"; bigVariant = "0x1"; bigRev = "0"; // Cortex-A78
        } else if (platform.find("lito") != std::string::npos) {
            bigPart = "0xd0d"; bigVariant = "0x3"; bigRev = "0";
        } else if (platform.find("sdm670") != std::string::npos) {
            bigPart = "0xd0a"; bigVariant = "0x2"; bigRev = "1"; // Cortex-A75
        } else if (platform.find("sm6150") != std::string::npos) {
            bigPart = "0xd0b"; bigVariant = "0x1"; bigRev = "0"; // Cortex-A76 (Kryo 460)
        } else if (platform.find("atoll") != std::string::npos) {
            bigPart = "0xd0b"; bigVariant = "0x1"; bigRev = "0"; // Cortex-A76 (Kryo 470 Gold)
        } else if (platform.find("holi") != std::string::npos) {
            bigPart = "0xd0b"; bigVariant = "0x1"; bigRev = "0"; // Cortex-A76 (Kryo 460 Gold)
        } else if (platform.find("sm7325") != std::string::npos) {
            bigPart = "0xd44"; bigVariant = "0x1"; bigRev = "0"; // Cortex-A78 (Kryo 670)
        }

        int bigCoreStart = isHomogeneous ? fp.core_count : fp.core_count - 2;

        for(int i = 0; i < fp.core_count; ++i) {
            bool isBig = isQualcomm && !isHomogeneous && (i >= bigCoreStart);
            out += "processor\t: " + std::to_string(i) + "\n";
            out += "BogoMIPS\t: " + bogomips + "\n";
            out += "Features\t: " + features + "\n";
            out += "CPU implementer\t: 0x41\n";
            out += "CPU architecture: 8\n";
            if (isQualcomm) {
                // ARM64 Qualcomm: incluir variant, part y revision (obligatorios)
                out += "CPU variant\t: " + std::string(isBig ? bigVariant : "0x0") + "\n";
                out += "CPU part\t: "    + std::string(isBig ? bigPart    : "0xd05") + "\n";
                out += "CPU revision\t: " + std::string(isBig ? bigRev   : "5") + "\n";
            }
            out += "\n";
        }
    }
    out += "Hardware\t: " + std::string(fp.hardware) + "\n";
    out += "Revision\t: 0000\n";
    out += "Serial\t\t: 0000000000000000\n";
    return out;
}

ssize_t my_readlinkat(int dirfd, const char *pathname, char *buf, size_t bufsiz) {
    if (!orig_readlinkat) { errno = ENOSYS; return -1; }
    if (isHiddenPath(pathname)) { errno = ENOENT; return -1; }
    return orig_readlinkat(dirfd, pathname, buf, bufsiz);
}

ssize_t my_readlink(const char *pathname, char *buf, size_t bufsiz) {
    if (!orig_readlink) { errno = ENOSYS; return -1; }
    if (isHiddenPath(pathname)) { errno = ENOENT; return -1; }
    return orig_readlink(pathname, buf, bufsiz);
}

// -----------------------------------------------------------------------------
// statfs/statvfs — Consistencia de tamaño de disco con perfil
// android.os.StatFs usa la syscall statfs() internamente.
// Sin este hook, StatFs reporta el tamaño real del disco aunque /sys/block/size
// esté spoofed, creando una incoherencia detectable.
// -----------------------------------------------------------------------------
static long getProfileStorageSectors() {
    const DeviceFingerprint* fp = findProfile(g_currentProfileName);
    if (!fp) return 268435456L;  // 128GB default
    if (fp->ram_gb <= 4)       return 134217728L;   // 64 GB
    else if (fp->ram_gb <= 10) return 268435456L;   // 128 GB
    else                       return 536870912L;    // 256 GB
}

int my_statfs(const char *path, struct statfs *buf) {
    if (!orig_statfs) { errno = ENOSYS; return -1; }
    if (isHiddenPath(path)) { errno = ENOENT; return -1; }
    int ret = orig_statfs(path, buf);
    if (ret != 0) return ret;

    // Solo spoofear particiones de datos internos (/data, /storage, /sdcard)
    if (path && (strstr(path, "/data") == path ||
                 strstr(path, "/storage") == path ||
                 strstr(path, "/sdcard") == path)) {
        long targetSectors = getProfileStorageSectors();
        // Convertir sectores de 512B a bloques del filesystem
        unsigned long targetBytes = (unsigned long)targetSectors * 512UL;
        if (buf->f_bsize > 0) {
            unsigned long targetBlocks = targetBytes / buf->f_bsize;
            // Mantener la proporción usado/libre del sistema real
            if (buf->f_blocks > 0) {
                double usedRatio = 1.0 - ((double)buf->f_bfree / (double)buf->f_blocks);
                buf->f_blocks = targetBlocks;
                buf->f_bfree  = (unsigned long)(targetBlocks * (1.0 - usedRatio));
                buf->f_bavail = buf->f_bfree;
            }
        }
    }
    return ret;
}

int my_statvfs(const char *path, struct statvfs *buf) {
    if (!orig_statvfs) { errno = ENOSYS; return -1; }
    if (isHiddenPath(path)) { errno = ENOENT; return -1; }
    int ret = orig_statvfs(path, buf);
    if (ret != 0) return ret;

    if (path && (strstr(path, "/data") == path ||
                 strstr(path, "/storage") == path ||
                 strstr(path, "/sdcard") == path)) {
        long targetSectors = getProfileStorageSectors();
        unsigned long targetBytes = (unsigned long)targetSectors * 512UL;
        if (buf->f_frsize > 0) {
            unsigned long targetBlocks = targetBytes / buf->f_frsize;
            if (buf->f_blocks > 0) {
                double usedRatio = 1.0 - ((double)buf->f_bfree / (double)buf->f_blocks);
                buf->f_blocks = targetBlocks;
                buf->f_bfree  = (unsigned long)(targetBlocks * (1.0 - usedRatio));
                buf->f_bavail = buf->f_bfree;
            }
        }
    }
    return ret;
}

// PR37: UUID v4 determinístico desde seed para boot_id
static std::string generateBootId(long seed) {
    long s = seed ^ 0xDEADBEEFL;
    auto nextNibble = [&]() -> char {
        s = s * 6364136223846793005LL + 1442695040888963407LL;
        return "0123456789abcdef"[((s >> 33) ^ s) & 0xF];
    };
    std::string u;
    u.reserve(36);
    for (int i = 0; i < 8;  ++i) u += nextNibble(); u += '-';
    for (int i = 0; i < 4;  ++i) u += nextNibble(); u += '-';
    u += '4';
    for (int i = 0; i < 3;  ++i) u += nextNibble(); u += '-';
    s = s * 6364136223846793005LL + 1442695040888963407LL;
    u += "89ab"[((s >> 33) ^ s) & 0x3];
    for (int i = 0; i < 3;  ++i) u += nextNibble(); u += '-';
    for (int i = 0; i < 12; ++i) u += nextNibble();
    return u;
}

// -----------------------------------------------------------------------------
// Hooks: System Properties
// -----------------------------------------------------------------------------
int my_system_property_get(const char *key, char *value) {
    if (shouldHide(key)) { if(value) value[0] = '\0'; return 0; }
    // PR71e: Never short-circuit when orig is null — the spoofing logic below
    // must run regardless, because my_system_property_read_callback and
    // my_SystemProperties_native_get both depend on this function for ALL
    // property spoofing.  When Dobby fails to hook __system_property_get
    // (inlined, already hooked, etc.), orig stays null and the old early-return
    // killed the ENTIRE spoofing chain: read_callback got 0 → passed real value,
    // native_get got 0 → returned default.
    int ret = 0;
    if (orig_system_property_get) {
        ret = orig_system_property_get(key, value);
    } else if (value) {
        value[0] = '\0';
    }

    const DeviceFingerprint* fp_ptr = findProfile(g_currentProfileName);
    if (fp_ptr) {
        const auto& fp = *fp_ptr;
        std::string k = key;
        std::string dynamic_buffer; // Use local buffer instead of static

        if (k == "ro.product.model") dynamic_buffer = fp.model;
        else if (k == "ro.product.brand") dynamic_buffer = fp.brand;
        else if (k == "ro.product.manufacturer") dynamic_buffer = fp.manufacturer;
        else if (k == "ro.product.device") dynamic_buffer = fp.device;
        else if (k == "ro.product.name") dynamic_buffer = fp.product;
        else if (k == "ro.hardware") dynamic_buffer = fp.hardware;
        else if (k == "ro.board.platform") dynamic_buffer = fp.boardPlatform;
        else if (k == "ro.build.fingerprint") dynamic_buffer = fp.fingerprint;
        else if (k == "ro.build.id") dynamic_buffer = fp.buildId;
        else if (k == "ro.serialno" || k == "ro.boot.serialno") {
            dynamic_buffer = omni::engine::generateRandomSerial(fp.brand, g_masterSeed, fp.securityPatch);
        }
        // PR32: ril.serialnumber — serial de radio propietario Samsung (delataba hardware real)
        else if (k == "ril.serialnumber") {
            std::string brandLower = toLowerStr(fp.brand);
            if (brandLower == "samsung") {
                dynamic_buffer = omni::engine::generateRandomSerial(fp.brand, g_masterSeed + 7, fp.securityPatch);
            }
        }
        else if (k == "ro.build.display.id") dynamic_buffer = fp.display;
        else if (k == "ro.build.tags") dynamic_buffer = fp.tags;
        else if (k == "ro.build.version.sdk") {
             if (strcmp(fp.release, "11") == 0) dynamic_buffer = "30";
             else if (strcmp(fp.release, "10") == 0) dynamic_buffer = "29";
             else if (strcmp(fp.release, "12") == 0) dynamic_buffer = "31";
             else dynamic_buffer = "30";
        }
        else if (k == "ro.secure" || k == "ro.build.selinux") dynamic_buffer = "1";
        else if (k == "ro.debuggable" || k == "sys.oem_unlock_allowed") dynamic_buffer = "0";
        // MIUI framework crash fix: AppOpsUtils.isXOptMode() reads this property
        // via SystemProperties.getBoolean(). When enabled (default on non-debug builds),
        // Activity.requestPermissions() routes to com.lbe.security.miui which may not
        // exist → ActivityNotFoundException. Force "false" so MIUI uses standard AOSP
        // permission controller (com.android.permissioncontroller).
        else if (g_realDeviceIsMiui && k == "persist.sys.miui_optimization") dynamic_buffer = "false";
        else if (g_realDeviceIsMiui && k == "ro.miui.build.region") dynamic_buffer = "global";
        else if (k == "ro.hardware.chipname") dynamic_buffer = fp.hardwareChipname;
        else if (k == "ro.product.board") dynamic_buffer = fp.board;
        else if (k == "ro.sf.lcd_density") {
            dynamic_buffer = fp.screenDensity;
        } else if (k == "ro.product.display_resolution") {
            dynamic_buffer = std::string(fp.screenWidth) + "x" + std::string(fp.screenHeight);
        } else if (k == "gsm.device.id" || k == "ro.ril.miui.imei0" || k == "ro.ril.miui.meid" || k == "ro.ril.oem.imei") {
            dynamic_buffer = omni::engine::generateValidImei(g_currentProfileName, g_masterSeed);
        } else if (k == "ro.ril.miui.imei1") {
            dynamic_buffer = omni::engine::generateValidImei(g_currentProfileName, g_masterSeed + 1);
        }
        else if (k == "ro.build.version.incremental")       dynamic_buffer = fp.incremental;
        else if (k == "ro.build.version.security_patch")    dynamic_buffer = fp.securityPatch;
        else if (k == "ro.build.description"       ||
                 k == "ro.vendor.build.description" ||
                 k == "ro.odm.build.description")            dynamic_buffer = fp.buildDescription;
        else if (k == "ro.build.flavor")                    dynamic_buffer = fp.buildFlavor;
        else if (k == "ro.build.host")                      dynamic_buffer = fp.buildHost;
        else if (k == "ro.build.user")                      dynamic_buffer = fp.buildUser;
        else if (k == "ro.build.date.utc")                  dynamic_buffer = fp.buildDateUtc;
        else if (k == "ro.build.version.codename")          dynamic_buffer = fp.buildVersionCodename;
        else if (k == "ro.build.version.preview_sdk")       dynamic_buffer = fp.buildVersionPreviewSdk;
        else if (k == "ro.build.type")                      dynamic_buffer = fp.type;
        else if (k == "ro.build.version.release")           dynamic_buffer = fp.release;
        else if (k == "ro.build.version.min_supported_target_sdk") dynamic_buffer = "23"; // Forzado para API 30 (Android 11)
        else if (k == "ro.vendor.build.fingerprint")        dynamic_buffer = fp.vendorFingerprint;
        else if (k == "ro.bootloader" || k == "ro.boot.bootloader") dynamic_buffer = fp.bootloader;
        else if (k == "ro.zygote")                          dynamic_buffer = fp.zygote;
        else if (k == "ro.product.system.model" ||
                 k == "ro.product.vendor.model" ||
                 k == "ro.product.odm.model")               dynamic_buffer = fp.model;
        // --- PR20: Namespaces system/vendor/odm (Android 11 lee estos antes del raíz) ---
        else if (k == "ro.product.system.brand"        ||
                 k == "ro.product.vendor.brand"        ||
                 k == "ro.product.odm.brand")               dynamic_buffer = fp.brand;
        else if (k == "ro.product.system.manufacturer" ||
                 k == "ro.product.vendor.manufacturer" ||
                 k == "ro.product.odm.manufacturer")        dynamic_buffer = fp.manufacturer;
        else if (k == "ro.product.system.device"       ||
                 k == "ro.product.vendor.device"       ||
                 k == "ro.product.odm.device")              dynamic_buffer = fp.device;
        else if (k == "ro.product.system.name"         ||
                 k == "ro.product.vendor.name"         ||
                 k == "ro.product.odm.name")               dynamic_buffer = fp.product;
        // --- PR20: CPU ABI lists (consultadas por Play Integrity) ---
        else if (k == "ro.product.cpu.abilist"         ||
                 k == "ro.product.system.cpu.abilist")      dynamic_buffer = "arm64-v8a,armeabi-v7a,armeabi";
        else if (k == "ro.product.cpu.abilist64")           dynamic_buffer = "arm64-v8a";
        else if (k == "ro.product.cpu.abilist32")           dynamic_buffer = "armeabi-v7a,armeabi";
        // PR32: Expansión ABI vendor/odm — coherencia entre particiones
        else if (k == "ro.vendor.product.cpu.abilist")      dynamic_buffer = "arm64-v8a,armeabi-v7a,armeabi";
        else if (k == "ro.vendor.product.cpu.abilist64")    dynamic_buffer = "arm64-v8a";
        else if (k == "ro.vendor.product.cpu.abilist32")    dynamic_buffer = "armeabi-v7a,armeabi";
        else if (k == "ro.odm.product.cpu.abilist")         dynamic_buffer = "arm64-v8a,armeabi-v7a,armeabi";
        // --- PR20: Build characteristics (brand-aware) ---
        else if (k == "ro.build.characteristics") {
            std::string br = toLowerStr(fp.brand);
            if (br == "samsung")      dynamic_buffer = "phone";
            else if (br == "google")  dynamic_buffer = "nosdcard";
            else                      dynamic_buffer = "default";
        }
        // --- PR20: Estado de cifrado (Android 11 espera siempre file-based encryption) ---
        else if (k == "ro.crypto.state")                    dynamic_buffer = "encrypted";
        else if (k == "ro.crypto.type")                     dynamic_buffer = "file";
        // --- PR20: Locale coherente con región del perfil ---
        else if (k == "ro.product.locale" || k == "persist.sys.locale") {
            dynamic_buffer = "en-US";  // PR41: Solo USA
        }
        else if (k == "ro.product.first_api_level") {
            int release_api = 0;
            try { release_api = std::stoi(fp.release); } catch (...) {}
            dynamic_buffer = (release_api >= 11) ? "30" : "29";
        }
        else if (k == "ro.build.version.base_os")           dynamic_buffer = "";
        // --- PR22: Boot Integrity & Hardware Boot (Defensa profunda TEE) ---
        else if (k == "ro.boot.verifiedbootstate")    dynamic_buffer = "green";
        // PR32: Ruta del controlador de almacenamiento coherente con plataforma del perfil
        else if (k == "ro.boot.bootdevice") {
            std::string plat = toLowerStr(fp.boardPlatform);
            if (plat.find("mt6") != std::string::npos) {
                dynamic_buffer = "bootdevice";                // MediaTek: path genérico MTK
            } else if (plat.find("exynos") != std::string::npos ||
                       plat.find("s5e")    != std::string::npos) {
                dynamic_buffer = "soc/11120000.ufs";          // Samsung Exynos: UFS 2.1
            } else if (plat.find("bengal")  != std::string::npos ||
                       plat.find("holi")    != std::string::npos ||
                       plat.find("trinket") != std::string::npos) {
                dynamic_buffer = "soc/4744000.sdhci";             // PR33: SM6115/SM4350 = eMMC 5.1
            } else {
                dynamic_buffer = "soc/1d84000.ufshc";             // Qualcomm UFS (kona/lahaina/lito/msmnile/etc.)
            }
        }
        else if (k == "ro.boot.flash.locked")         dynamic_buffer = "1";
        else if (k == "ro.boot.vbmeta.device_state")  dynamic_buffer = "locked";
        else if (k == "ro.boot.veritymode")           dynamic_buffer = "enforcing";
        // PR71: ro.secureboot.lockstate leakeaba "unlocked" en VD-Infos
        else if (k == "ro.secureboot.lockstate")      dynamic_buffer = "locked";
        else if (k == "ro.secureboot.devicelock")     dynamic_buffer = "1";
        else if (k == "ro.boot.hardware")             dynamic_buffer = fp.hardware;
        else if (k == "ro.boot.hardware.platform")    dynamic_buffer = fp.boardPlatform;
        // --- PR22: Partition Fingerprints (Play Integrity v3+) ---
        else if (k == "ro.product.system.fingerprint" ||
                 k == "ro.product.vendor.fingerprint" ||
                 k == "ro.product.odm.fingerprint")       dynamic_buffer = fp.fingerprint;
        // --- PR21: for_attestation namespace (Play Integrity / Firebase hardened) ---
        // Google Play Services lee estos antes de la hardware-backed attestation.
        // Sin estos hooks, el sistema filtra el identity real del Redmi 9.
        else if (k == "ro.product.model.for_attestation")        dynamic_buffer = fp.model;
        else if (k == "ro.product.brand.for_attestation")        dynamic_buffer = fp.brand;
        else if (k == "ro.product.manufacturer.for_attestation") dynamic_buffer = fp.manufacturer;
        else if (k == "ro.product.name.for_attestation")         dynamic_buffer = fp.product;
        else if (k == "ro.product.device.for_attestation")       dynamic_buffer = fp.device;
        // --- PR21: release_or_codename (Firebase Auth / Play Services Android 11+) ---
        else if (k == "ro.build.version.release_or_codename")    dynamic_buffer = fp.release;
        // --- PR21: board.first_api_level (Widevine L1 / Play Integrity) ---
        // Derivar del release del perfil. Android 11 = API 30.
        else if (k == "ro.board.first_api_level") {
            int api = 0;
            try { api = std::stoi(fp.release); } catch(...) {}
            dynamic_buffer = (api >= 11) ? "30" : "29";
        }
        // --- PR21: ODM + system_ext fingerprints (Widevine L1 cross-validation) ---
        // Widevine valida que estos fingerprints coincidan con ro.build.fingerprint.
        // Si devuelven el fingerprint real del Redmi 9, la discrepancia es detectable.
        // PR82: ro.product.build.fingerprint añadido — no estaba en el hook chain y
        // exponía el fingerprint real del dispositivo (Redmi/lancelot_global/lancelot:...).
        else if (k == "ro.odm.build.fingerprint"         ||
                 k == "ro.system_ext.build.fingerprint"  ||
                 k == "ro.product.build.fingerprint")         dynamic_buffer = fp.fingerprint;
        // --- PR21: ro.product.cpu.abi singular (Snapchat / Instagram / Firebase) ---
        // ro.product.cpu.abilist ya está hooked pero la forma singular no lo estaba.
        // Snapchat e Instagram leen la forma singular específicamente.
        else if (k == "ro.product.cpu.abi")                     dynamic_buffer = "arm64-v8a";
        // --- PR21: HAL gralloc + hwcomposer (Widevine HAL coherence check) ---
        // camera/vulkan/keystore/audio/egl ya están hooked. Gralloc y hwcomposer
        // completaban el set HAL que Widevine usa para detectar quimeras de hardware.
        else if (k == "ro.hardware.gralloc"    ||
                 k == "ro.hardware.hwcomposer" ||
                 k == "ro.hardware.memtrack")                    dynamic_buffer = fp.boardPlatform;
        // --- PR21: persist.sys.country / language (Snapchat / Instagram / Tinder) ---
        // persist.sys.locale ya está hooked (PR20) pero country y language por separado
        // no lo estaban. Las apps sociales leen las tres para detectar inconsistencias.
        else if (k == "persist.sys.country") {
            dynamic_buffer = "US";  // PR41: Solo USA
        }
        else if (k == "persist.sys.language") {
            dynamic_buffer = "en";  // PR41: Solo USA
        }
        // --- v12.10: Chronos & Command Shield ---
        else if (k == "ro.opengles.version")          dynamic_buffer = fp.openGlEs;
        else if (k == "sys.usb.state" || k == "sys.usb.config") dynamic_buffer = "mtp";
        else if (k == "persist.sys.timezone") {
            // PR42: Timezone derivado de la MISMA ciudad que GPS (seed+7777)
            // Coherencia garantizada: si GPS=Phoenix → TZ=America/Phoenix siempre
            dynamic_buffer = omni::engine::getTimezoneForProfile(g_masterSeed);
        }
        else if (k == "gsm.network.type")             dynamic_buffer = "LTE";
        else if (k == "gsm.current.phone-type")       dynamic_buffer = "1";
        else if (k == "gsm.sim.state")                dynamic_buffer = "READY";
        else if (k == "gsm.sim.state2")               dynamic_buffer = "";
        // PR37: Network type spoofing — activo solo cuando network_type=lte en config
        // Oculta señales de conectividad WiFi ante apps que leen system properties
        else if (g_spoofMobileNetwork && (k == "wifi.interface" ||
                 k == "dhcp.wlan0.ipaddress" || k == "dhcp.wlan0.dns1" ||
                 k == "dhcp.wlan0.server"    || k == "net.wifi.ssid"   ||
                 k == "dhcp.wlan0.gateway"   || k == "dhcp.wlan0.mask" ||
                 k == "dhcp.wlan0.leasetime" || k == "dhcp.wlan0.domain")) {
            dynamic_buffer = "";  // Ocultar señales WiFi — sin valor
        }
        else if (g_spoofMobileNetwork && k == "wifi.on") {
            dynamic_buffer = "0";  // WiFi explícitamente apagado
        }
        else if (g_spoofMobileNetwork && k == "wlan.driver.status") {
            dynamic_buffer = "unloaded";  // Driver WiFi no cargado
        }
        else if (g_spoofMobileNetwork && k == "gsm.network.state") {
            dynamic_buffer = "CONNECTED";  // Confirmar conexión celular activa
        }
        else if (k == "gsm.version.baseband" || k == "ro.build.expect.baseband" || k == "ro.baseband") {
            dynamic_buffer = fp.radioVersion;
        }
        else if (k == "gsm.sim.operator.numeric" || k == "gsm.operator.numeric") {
            // Extraer el MCC/MNC directamente del IMSI generado
            // 3GPP TS 24.008: MNC de 3 dígitos (USA MCC 310/311) → PLMN de 6 chars
            // MNC de 2 dígitos (Europa, LATAM) → PLMN de 5 chars
            std::string imsi = omni::engine::generateValidImsi(g_currentProfileName, g_masterSeed);
            std::string mcc = imsi.substr(0, 3);
            dynamic_buffer = (mcc == "310" || mcc == "311") ? imsi.substr(0, 6) : imsi.substr(0, 5);
        }
        else if (k == "gsm.sim.operator.iso-country" || k == "gsm.operator.iso-country") {
            dynamic_buffer = "us";  // PR41: Solo USA
        }
        else if (k == "gsm.sim.operator.alpha" || k == "gsm.operator.alpha") {
            // PR41: Carrier USA real derivado del IMSI (coherente con PLMN)
            dynamic_buffer = omni::engine::getCarrierNameForImsi(g_currentProfileName, g_masterSeed);
        }
        else if (k == "ro.mediadrm.device_id" || k == "drm.service.enabled") {
            dynamic_buffer = omni::engine::generateWidevineId(g_masterSeed);
        }
        else if (k == "gsm.version.ril-impl") {
            // PR41: Formato RIL real (el valor anterior era un classpath Java, no un ril-impl)
            dynamic_buffer = omni::engine::getRilVersionForProfile(g_currentProfileName);
        }
        else if (k == "ro.carrier") {
            // PR42: Carrier short name coherente con IMSI generado
            std::string imsi = omni::engine::generateValidImsi(g_currentProfileName, g_masterSeed);
            std::string mccMnc = imsi.substr(0, 6);
            if (mccMnc == "311480") dynamic_buffer = "vzw";         // Verizon
            else if (mccMnc == "310410") dynamic_buffer = "att";    // AT&T
            else dynamic_buffer = "tmo";                             // T-Mobile (310260/310120), Sprint→TMo, US Cellular→tmo
        }
        else if (k == "ro.cdma.home.operator.numeric") {
            // PR42: Solo relevante para Verizon (CDMA legacy) — coherente con IMSI
            std::string imsi = omni::engine::generateValidImsi(g_currentProfileName, g_masterSeed);
            dynamic_buffer = imsi.substr(0, 6);  // MCC+MNC = PLMN del carrier
        }
        else if (k == "telephony.lteOnCdmaDevice") {
            // PR42: Verizon = 1 (red CDMA legacy + LTE), resto = 0
            std::string imsi = omni::engine::generateValidImsi(g_currentProfileName, g_masterSeed);
            dynamic_buffer = (imsi.substr(0, 6) == "311480") ? "1" : "0";
        }
        else if (k == "ro.telephony.default_network")        dynamic_buffer = "9";
        else if (k == "ro.soc.manufacturer") {
            // Derivar el fabricante del SoC del boardPlatform del perfil activo
            std::string plat = toLowerStr(fp.boardPlatform);
            if (plat.find("mt6") != std::string::npos || plat.find("mt8") != std::string::npos)
                dynamic_buffer = "MediaTek";
            else if (plat.find("exynos") != std::string::npos ||
                     plat.find("s5e")    != std::string::npos)
                dynamic_buffer = "Samsung";
            else
                dynamic_buffer = "Qualcomm";   // kona, lito, atoll, bengal, etc.
        }
        else if (k == "ro.soc.model") {
            // ro.soc.model = el chip model del perfil (hardwareChipname)
            dynamic_buffer = fp.hardwareChipname;
        }
        // --- Intercepción HAL (Cámara, Audio y DRM/Keystore) ---
        else if (k == "ro.hardware.camera" ||
                 k == "ro.hardware.keystore" ||
                 k == "ro.hardware.audio") {
            // Forzamos al sistema a reportar el hardware emulado en lugar de la placa base física
            dynamic_buffer = fp.boardPlatform;
        }
        // --- Intercepción HAL Gráfico (Vulkan y EGL) ---
        else if (k == "ro.hardware.vulkan" ||
                 k == "ro.hardware.egl") {
            // Android real reporta el driver gráfico ('adreno', 'mali', 'powervr')
            dynamic_buffer = fp.eglDriver;
        }
        else if (k == "ro.mediatek.platform") {
            std::string plat = toLowerStr(fp.boardPlatform);
            if (plat.find("mt") == std::string::npos) {
                value[0] = '\0'; return 0;
            }
            // PR71: Return profile's chipname (e.g. "MT6765") instead of real SoC
            dynamic_buffer = fp.hardwareChipname;
        }
        else if (k == "ro.mediatek.version.release") {
            // Suppress MTK version — ODM-specific string leaks manufacturer identity
            value[0] = '\0'; return 0;
        }
        // PR74: ro.vendor.mediatek.* — VDInfo reads these directly from property area
        else if (k == "ro.vendor.mediatek.platform") {
            std::string plat = toLowerStr(fp.boardPlatform);
            if (plat.find("mt") == std::string::npos) {
                value[0] = '\0'; return 0;  // Non-MTK profile: suppress
            }
            dynamic_buffer = fp.hardwareChipname;
        }
        else if (k == "ro.vendor.mediatek.version.branch" ||
                 k == "ro.vendor.mediatek.version.release" ||
                 k == "ro.mediatek.version.branch") {
            value[0] = '\0'; return 0;  // Suppress ODM-specific MediaTek strings
        }
        else if (k == "ro.vendor.md_apps.load_verno") {
            value[0] = '\0'; return 0;  // Suppress MediaTek modem version
        }

        // ── PR70c: Leaked props detected by VD-Infos ─────────────────────

        // Hostname — leaks "M2004J19C-Redmi9" (original model + brand)
        else if (k == "net.hostname") {
            dynamic_buffer = std::string(fp.model) + "-" + fp.brand;
        }
        // Modem serial / baseband project — leaks original SoC and serial
        else if (k == "vendor.gsm.serial" || k == "vendor.gsm.project.baseband") {
            value[0] = '\0'; return 0;
        }
        // ro.product.product.* namespace — Android reads these on some ROMs
        else if (k == "ro.product.product.model")        dynamic_buffer = fp.model;
        else if (k == "ro.product.product.brand")        dynamic_buffer = fp.brand;
        else if (k == "ro.product.product.manufacturer") dynamic_buffer = fp.manufacturer;
        else if (k == "ro.product.product.device")       dynamic_buffer = fp.device;
        else if (k == "ro.product.product.name")         dynamic_buffer = fp.product;
        // ro.product.system_ext.* namespace
        else if (k == "ro.product.system_ext.model")     dynamic_buffer = fp.model;
        else if (k == "ro.product.system_ext.device")    dynamic_buffer = fp.device;
        else if (k == "ro.product.system_ext.brand")     dynamic_buffer = fp.brand;
        else if (k == "ro.product.system_ext.manufacturer") dynamic_buffer = fp.manufacturer;
        else if (k == "ro.product.system_ext.name")      dynamic_buffer = fp.product;
        // Market name — all partitions leak "Redmi 9"
        else if (k.find("marketname") != std::string::npos) dynamic_buffer = fp.model;
        // Device name in Settings DB
        else if (k == "device_name") dynamic_buffer = fp.model;
        // Product identifiers
        else if (k == "ro.product.mod_device")  dynamic_buffer = fp.product;
        else if (k == "ro.product.cert")        dynamic_buffer = fp.model;
        else if (k == "ro.build.product")       dynamic_buffer = fp.product;
        // Boot props that leak original codename
        else if (k == "ro.boot.product.hardware.sku") dynamic_buffer = fp.device;
        else if (k == "ro.boot.rsc") dynamic_buffer = fp.device;
        // OEM identifier
        else if (k == "ro.fota.oem") dynamic_buffer = fp.manufacturer;
        // Build version props for all partitions (leak MIUI incremental/dates)
        else if (k == "ro.vendor.build.version.incremental" ||
                 k == "ro.odm.build.version.incremental" ||
                 k == "ro.product.build.version.incremental" ||
                 k == "ro.system.build.version.incremental" ||
                 k == "ro.system_ext.build.version.incremental") dynamic_buffer = fp.incremental;
        else if (k == "ro.vendor.build.version.release" ||
                 k == "ro.odm.build.version.release" ||
                 k == "ro.product.build.version.release" ||
                 k == "ro.system.build.version.release" ||
                 k == "ro.system_ext.build.version.release" ||
                 k == "ro.vendor.build.version.release_or_codename"  ||
                 k == "ro.odm.build.version.release_or_codename"     ||
                 k == "ro.product.build.version.release_or_codename" ||
                 k == "ro.system.build.version.release_or_codename"  ||
                 k == "ro.system_ext.build.version.release_or_codename") dynamic_buffer = fp.release;
        else if (k == "ro.vendor.build.version.sdk" ||
                 k == "ro.odm.build.version.sdk" ||
                 k == "ro.product.build.version.sdk" ||
                 k == "ro.system.build.version.sdk" ||
                 k == "ro.system_ext.build.version.sdk") {
            if (strcmp(fp.release, "11") == 0) dynamic_buffer = "30";
            else if (strcmp(fp.release, "10") == 0) dynamic_buffer = "29";
            else if (strcmp(fp.release, "12") == 0) dynamic_buffer = "31";
            else dynamic_buffer = "30";
        }
        else if (k == "ro.vendor.build.version.security_patch" ||
                 k == "ro.vendor.build.security_patch"          ||
                 k == "ro.odm.build.security_patch"             ||
                 k == "ro.odm.build.version.security_patch")    dynamic_buffer = fp.securityPatch;
        // Build dates for all partitions
        else if (k == "ro.vendor.build.date.utc"    ||
                 k == "ro.odm.build.date.utc"       ||
                 k == "ro.product.build.date.utc"    ||
                 k == "ro.system.build.date.utc"     ||
                 k == "ro.system_ext.build.date.utc" ||
                 k == "ro.bootimage.build.date.utc") dynamic_buffer = fp.buildDateUtc;
        // PR71: Human-readable build dates — VD-Infos reads these alongside UTC variants
        else if (k == "ro.build.date"           ||
                 k == "ro.vendor.build.date"    ||
                 k == "ro.odm.build.date"       ||
                 k == "ro.product.build.date"   ||
                 k == "ro.system.build.date"    ||
                 k == "ro.system_ext.build.date"||
                 k == "ro.bootimage.build.date") {
            time_t t = 0;
            try { t = std::stol(fp.buildDateUtc); } catch(...) {}
            if (t > 0) {
                struct tm tm_buf;
                gmtime_r(&t, &tm_buf);
                char date_str[64];
                strftime(date_str, sizeof(date_str), "%a %b %e %H:%M:%S UTC %Y", &tm_buf);
                dynamic_buffer = date_str;
            }
        }
        // Build IDs for all partitions
        else if (k == "ro.vendor.build.id"     ||
                 k == "ro.odm.build.id"        ||
                 k == "ro.product.build.id"    ||
                 k == "ro.system.build.id"     ||
                 k == "ro.system_ext.build.id") dynamic_buffer = fp.buildId;
        // Build types for all partitions
        else if (k == "ro.vendor.build.type"     ||
                 k == "ro.odm.build.type"        ||
                 k == "ro.product.build.type"    ||
                 k == "ro.system.build.type"     ||
                 k == "ro.system_ext.build.type") dynamic_buffer = fp.type;
        // Build tags for all partitions
        else if (k == "ro.vendor.build.tags"     ||
                 k == "ro.odm.build.tags"        ||
                 k == "ro.product.build.tags"    ||
                 k == "ro.system.build.tags"     ||
                 k == "ro.system_ext.build.tags") dynamic_buffer = fp.tags;
        // Partition build fingerprints (belt-and-suspenders with earlier handler)
        else if (k == "ro.vendor.build.fingerprint" ||
                 k == "ro.system.build.fingerprint") dynamic_buffer = fp.fingerprint;

        // ── PR70c: Platform-specific prop suppression ────────────────────
        // Suppress MIUI props when NOT emulating Xiaomi
        // PR91-fix: On real MIUI devices, preserve these — framework depends on them
        else if (k.find("ro.miui.") == 0 || k.find("ro.vendor.miui.") == 0) {
            std::string br = toLowerStr(fp.brand);
            if (br != "redmi" && br != "xiaomi" && br != "poco" && !g_realDeviceIsMiui) {
                value[0] = '\0'; return 0;
            }
        }
        // PR71: Suppress vendor MTK version release — ODM-specific string leaks
        // real device manufacturer (e.g. HUAQIN for Redmi 9)
        else if (k == "ro.vendor.mediatek.version.release") {
            value[0] = '\0'; return 0;
        }
        // Suppress MediaTek vendor props when NOT emulating MediaTek
        else if (k.find("ro.vendor.mediatek.") == 0 ||
                 k.find("ro.mediatek.") == 0) {
            std::string plat = toLowerStr(fp.boardPlatform);
            if (plat.find("mt") == std::string::npos) {
                value[0] = '\0'; return 0;
            }
        }

        if (!dynamic_buffer.empty()) {
            int len = dynamic_buffer.length();
            if (len >= 92) len = 91;
            strncpy(value, dynamic_buffer.c_str(), len);
            value[len] = '\0';
            return len;
        }
    }
    if (ret > 0 && shouldHide(value)) { value[0] = '\0'; return 0; }
    return ret;
}

// -----------------------------------------------------------------------------
// Hooks: File I/O
// -----------------------------------------------------------------------------
static bool isAshmemFd(int fd, std::string* outPath = nullptr) {
    if (fd < 0) return false;
    char fdlink[64];
    char target[512] = {};
    snprintf(fdlink, sizeof(fdlink), "/proc/self/fd/%d", fd);
    ssize_t n = (ssize_t)syscall(__NR_readlinkat, AT_FDCWD, fdlink, target, sizeof(target) - 1);
    if (n <= 0) return false;
    target[n] = '\0';
    std::string t(target);
    if (outPath) *outPath = t;
    return t.find("/dev/ashmem") != std::string::npos;
}

static bool isAshmemFdCached(int fd) {
    if (fd < 0) return false;
    {
        std::lock_guard<std::mutex> lock(g_fdMutex);
        if (g_ashmemFds.count(fd)) return true;
        if (g_nonAshmemFds.count(fd)) return false;
    }

    // Primera vez que vemos este FD: una sola sonda readlinkat.
    // Esto evita el costo masivo por read()/pread() hot path.
    bool ash = isAshmemFd(fd, nullptr);
    {
        std::lock_guard<std::mutex> lock(g_fdMutex);
        if (ash) g_ashmemFds.insert(fd);
        else g_nonAshmemFds.insert(fd);
    }
    return ash;
}

static void inheritAshmemCache(int oldfd, int newfd) {
    if (oldfd < 0 || newfd < 0) return;
    std::lock_guard<std::mutex> lock(g_fdMutex);
    if (g_ashmemFds.count(oldfd)) {
        g_ashmemFds.insert(newfd);
        g_nonAshmemFds.erase(newfd);
    } else if (g_nonAshmemFds.count(oldfd)) {
        g_nonAshmemFds.insert(newfd);
        g_ashmemFds.erase(newfd);
    }
}

static void maybeLogAshmemFd(int fd, const char* from) {
    if (!isAshmemFdCached(fd)) return;
    {
        std::lock_guard<std::mutex> lock(g_fdMutex);
        if (g_ashmemLoggedFds.count(fd)) return;
        g_ashmemLoggedFds.insert(fd);
    }
    std::string path;
    (void)isAshmemFd(fd, &path); // una sola vez por fd (después del guard de g_ashmemLoggedFds)
    LOGD("[PR120] ashmem fd observed via %s: fd=%d target='%s'",
         from ? from : "?", fd, path.c_str());
}

int my_open(const char *pathname, int flags, mode_t mode) {
    // PR51: NO hookeamos open directamente — su body en Bionic llama openat()
    // que está hooked, creando recursión infinita. my_open se usa como helper
    // llamado desde my_openat; usa orig_openat(AT_FDCWD) como terminal seguro.
    if (!orig_openat) { errno = ENOSYS; return -1; }
    if (!pathname) return orig_openat(AT_FDCWD, pathname, flags, mode);

    std::string path_str(pathname);
    if (path_str == "/proc/modules" || path_str == "/proc/interrupts" ||
        path_str == "/proc/self/smaps_rollup" || isProcPidPath(pathname, "smaps_rollup")) {
        errno = EACCES;
        return -1;
    }
    // PR37: Bloquear filesystems (revela overlay/erofs de Magisk/KSU)
    if (path_str == "/proc/filesystems") {
        errno = EACCES;
        return -1;
    }
    if (path_str == "/proc/iomem") {
        return orig_openat(AT_FDCWD, "/dev/null", flags, mode);
    }

    if (pathname && strstr(pathname, "/dev/__properties__/")) {
        errno = EACCES;
        return -1;
    }
    if (pathname) {
        // Bloquear drivers de GPU contradictorios (Evasión Capa 5)
        const DeviceFingerprint* fp_gpu = findProfile(g_currentProfileName);
        if (fp_gpu) {
            std::string plat = toLowerStr(fp_gpu->boardPlatform);
            std::string brand = toLowerStr(fp_gpu->brand);
            bool isQcom = (brand == "google" || plat.find("msmnile") != std::string::npos ||
                plat.find("kona") != std::string::npos || plat.find("lahaina") != std::string::npos ||
                plat.find("atoll") != std::string::npos || plat.find("lito") != std::string::npos ||
                plat.find("bengal") != std::string::npos || plat.find("holi") != std::string::npos ||
                plat.find("trinket") != std::string::npos || plat.find("sdm670") != std::string::npos ||
                plat.find("sm6150") != std::string::npos || plat.find("sm6350") != std::string::npos ||
                plat.find("sm7325") != std::string::npos);
            std::string egl = toLowerStr(fp_gpu->eglDriver);

            // Adreno → bloquea mali; Mali → bloquea kgsl; PowerVR → bloquea ambos
            if (egl == "powervr") {
                if (strstr(pathname, "/dev/mali") || strstr(pathname, "/dev/kgsl")) { errno = ENOENT; return -1; }
            } else if (isQcom && strstr(pathname, "/dev/mali")) { errno = ENOENT; return -1; }
            else if (!isQcom && strstr(pathname, "/dev/kgsl")) { errno = ENOENT; return -1; }
        }
    }
    int fd = orig_openat(AT_FDCWD, pathname, flags, mode);
    if (fd >= 0 && pathname) {
        maybeLogAshmemFd(fd, "openat->open");
        FileType type = NONE;
        if (strstr(pathname, "/proc/version")) type = PROC_VERSION;
        else if (strstr(pathname, "/proc/cpuinfo")) type = PROC_CPUINFO;
        else if (strstr(pathname, "/sys/class/android_usb") && strstr(pathname, "iSerial")) type = USB_SERIAL;
        else if (strstr(pathname, "/sys/class/net/wlan0/address")) type = WIFI_MAC;
        // PR22: Hardware Topology & TracerPid
        else if (strstr(pathname, "/proc/self/status") || isProcPidPath(pathname, "status")) type = PROC_SELF_STATUS;
        else if (strstr(pathname, "/sys/devices/system/cpu/online") ||
                 strstr(pathname, "/sys/devices/system/cpu/possible") ||
                 strstr(pathname, "/sys/devices/system/cpu/present")) type = SYS_CPU_TOPOLOGY;
        else if (strstr(pathname, "/sys/class/power_supply/battery/technology")) type = BAT_TECHNOLOGY;
        else if (strstr(pathname, "/sys/class/power_supply/battery/present"))    type = BAT_PRESENT;
        else if (strstr(pathname, "/sys/class/power_supply/battery/charge_full")) type = BATTERY_CHARGE_FULL;
        else if (strstr(pathname, "/sys/class/power_supply/battery/temp")) type = BATTERY_TEMP;
        else if (strstr(pathname, "/sys/class/power_supply/battery/voltage_now")) type = BATTERY_VOLT;
        else if (strstr(pathname, "/sys/class/power_supply/battery/capacity")) type = BATTERY_CAPACITY;
        else if (strstr(pathname, "/sys/class/power_supply/battery/status")) type = BATTERY_STATUS;
        else if (strstr(pathname, "/proc/uptime")) type = PROC_UPTIME;
        else if (strstr(pathname, "/proc/sys/kernel/osrelease")) type = PROC_OSRELEASE;
        else if (strcmp(pathname, "/proc/cmdline") == 0) type = PROC_CMDLINE;
        else if (strncmp(pathname, "/proc/sys/kernel/hostname", 25) == 0) {
            type = PROC_HOSTNAME;
        } else if (strncmp(pathname, "/proc/sys/kernel/ostype", 23) == 0) {
            type = PROC_OSTYPE;
        } else if (strncmp(pathname, "/sys/firmware/devicetree/base/model", 35) == 0) {
            type = PROC_DTB_MODEL;
        } else if (strncmp(pathname, "/sys/class/net/eth0/address", 27) == 0) {
            type = PROC_ETH0_MAC;
        }
        // PR70: Also match /proc/<pid>/maps — detection apps bypass /proc/self/ filtering
        else if (strstr(pathname, "/proc/self/maps") || strstr(pathname, "/proc/self/smaps") ||
                 isProcPidPath(pathname, "maps") || isProcPidPath(pathname, "smaps")) type = PROC_MAPS;
        else if (strstr(pathname, "/proc/meminfo")) type = PROC_MEMINFO;
        else if (strstr(pathname, "/proc/modules")) type = PROC_MODULES;
        else if (strstr(pathname, "/proc/self/mounts") || strstr(pathname, "/proc/self/mountinfo") ||
                 isProcPidPath(pathname, "mounts") || isProcPidPath(pathname, "mountinfo")) type = PROC_MOUNTS;
        else if (strstr(pathname, "/sys/devices/system/cpu/") && strstr(pathname, "cpuinfo_max_freq")) type = SYS_CPU_FREQ;
        else if (strstr(pathname, "/sys/devices/soc0/machine")) type = SYS_SOC_MACHINE;
        else if (strstr(pathname, "/sys/devices/soc0/family")) type = SYS_SOC_FAMILY;
        else if (strstr(pathname, "/sys/devices/soc0/soc_id")) type = SYS_SOC_ID;
        else if (strstr(pathname, "/sys/class/graphics/fb0/virtual_size")) type = SYS_FB0_SIZE;
        else if (strstr(pathname, "/proc/asound/cards")) type = PROC_ASOUND;
        else if (strstr(pathname, "/proc/bus/input/devices")) type = PROC_INPUT;
        else if (strstr(pathname, "/sys/class/thermal/") && strstr(pathname, "type")) type = SYS_THERMAL;
        else if (strstr(pathname, "/proc/stat")) type = PROC_STAT;
        else if (strstr(pathname, "/sys/block/") && (strstr(pathname, "device/model") || strstr(pathname, "device/name"))) type = SYS_BLOCK_MODEL;
        else if (strstr(pathname, "/sys/block/") && strstr(pathname, "/size")) type = SYS_BLOCK_SIZE;
        else if (strstr(pathname, "scaling_available_governors")) type = SYS_CPU_GOVERNORS;
        else if (strstr(pathname, "/sys/class/bluetooth/hci0/address")) type = BT_MAC;
        else if (strstr(pathname, "/sys/class/bluetooth/hci0/name")) type = BT_NAME;
        // PR20: Red virtual (oculta MAC real del router y estadísticas reales)
        else if (strcmp(pathname, "/proc/net/arp") == 0)   type = PROC_NET_ARP;
        else if (strcmp(pathname, "/proc/net/dev") == 0  ||
                 strcmp(pathname, "/proc/self/net/dev") == 0) type = PROC_NET_DEV;   // PR98: self/net alias
        // PR98: IPv4 routing table — exposes tun0 route when proxy active; /proc/self/net/ alias also covered
        else if (strcmp(pathname, "/proc/net/route") == 0 ||
                 strcmp(pathname, "/proc/self/net/route") == 0) type = PROC_NET_ROUTE;
        // PR32: Sellado forense de sockets TCP/UDP (+ PR98: self/net aliases)
        else if (strcmp(pathname, "/proc/net/tcp") == 0 ||
                 strcmp(pathname, "/proc/net/tcp6") == 0  ||
                 strcmp(pathname, "/proc/self/net/tcp") == 0 ||
                 strcmp(pathname, "/proc/self/net/tcp6") == 0) type = PROC_NET_TCP;
        else if (strcmp(pathname, "/proc/net/udp") == 0 ||
                 strcmp(pathname, "/proc/net/udp6") == 0  ||
                 strcmp(pathname, "/proc/self/net/udp") == 0 ||
                 strcmp(pathname, "/proc/self/net/udp6") == 0) type = PROC_NET_UDP;
        else if (strstr(pathname, "/proc/net/if_inet6") ||
                 strstr(pathname, "/proc/self/net/if_inet6"))  type = PROC_NET_IF_INET6; // PR98: self/net alias
        else if (strstr(pathname, "/proc/net/ipv6_route") ||
                 strstr(pathname, "/proc/self/net/ipv6_route")) type = PROC_NET_IPV6_ROUTE; // PR98: self/net alias
        // PR37: Nuevos VFS handlers
        else if (strcmp(pathname, "/proc/sys/kernel/random/boot_id") == 0) type = PROC_BOOT_ID;
        else if (strcmp(pathname, "/proc/self/cgroup") == 0 || isProcPidPath(pathname, "cgroup")) type = PROC_SELF_CGROUP;
        else if (strstr(pathname, "scaling_available_frequencies"))         type = SYS_CPU_FREQ_AVAIL;
        // Bloqueo directo de KSU/Batería MTK
        else if (strstr(pathname, "mtk_battery") || strstr(pathname, "mt_bat")) { errno = ENOENT; return -1; }

        if (type != NONE) {
            std::string content;
            const DeviceFingerprint* fp_ptr = findProfile(g_currentProfileName);
            if (fp_ptr) {
                const auto& fp = *fp_ptr;

                if (type == PROC_VERSION) {
                    std::string plat = toLowerStr(fp.boardPlatform);
                    std::string brd  = toLowerStr(fp.brand);
                    std::string kv = "4.14.186-perf+";
                    if (brd == "google") {
                        if (plat.find("lito")    != std::string::npos) kv = "4.19.113-g820a424c538c-ab7336171";
                        else if (plat.find("atoll") != std::string::npos) kv = "4.14.150-g62a62a5a93f7-ab7336171";
                        else if (plat.find("sdm670")  != std::string::npos) kv = "4.9.189-g5d098cef6d96-ab6174032";
                        else kv = "4.19.113-g820a424c538c-ab7336171";
                    } else if (plat.find("mt6")!=std::string::npos) {
                        // Samsung MTK: numeric suffix; others: perf+
                        if (brd == "samsung") {
                            kv = (plat.find("mt6769")!=std::string::npos) ? "4.14.113-23424440" : "4.14.113-25267920";
                        } else { kv="4.14.186-perf+"; }
                    } else if (plat.find("kona")!=std::string::npos||plat.find("lahaina")!=std::string::npos) kv="4.19.157-perf+";
                    else if (plat.find("atoll")!=std::string::npos||plat.find("lito")!=std::string::npos) kv="4.19.113-perf+";
                    else if (plat.find("sdm670")!=std::string::npos) kv="4.9.189-perf+";
                    else if (plat.find("bengal")!=std::string::npos||plat.find("holi")!=std::string::npos||
                             plat.find("sm6350")!=std::string::npos) kv="4.19.157-perf+";
                    else if (plat.find("sm7325")!=std::string::npos) kv="5.4.61-perf+";
                    // PR41: Kernels Samsung Exynos — sufijo numérico (NO -perf+ que es Qualcomm)
                    else if (plat.find("exynos9611")!=std::string::npos) kv="4.14.113-25145160";
                    else if (plat.find("exynos9825")!=std::string::npos) kv="4.14.113-22911262";
                    else if (plat.find("exynos850")!=std::string::npos) kv="4.19.113-25351273";

                    long dateUtc = 0;
                    try { dateUtc = std::stol(fp.buildDateUtc); } catch(...) {}
                    char dateBuf[128];
                    time_t t = (time_t)dateUtc;
                    struct tm tm_buf = {};
                    gmtime_r(&t, &tm_buf);
                    strftime(dateBuf, sizeof(dateBuf), "%a %b %d %H:%M:%S UTC %Y", &tm_buf);

                    // PR42: Compiler y build user coherentes con la marca del perfil
                    std::string compilerStr;
                    std::string platLow = toLowerStr(fp.boardPlatform);
                    std::string brdLow  = toLowerStr(fp.brand);
                    if (platLow.find("exynos") != std::string::npos) {
                        // Samsung Exynos usa GCC en producción (no clang)
                        compilerStr = "(" + std::string(fp.buildUser) + "@" + std::string(fp.buildHost) + ") (gcc version 4.9.x (GCC))";
                    } else if (brdLow == "google") {
                        // Pixel usa android-build con clang de Google
                        compilerStr = "(android-build@" + std::string(fp.buildHost) + ") (Android clang version 12.0.5)";
                    } else {
                        // Qualcomm y MediaTek (Xiaomi, OnePlus, Realme, Nokia, etc.)
                        compilerStr = "(" + std::string(fp.buildUser) + "@" + std::string(fp.buildHost) + ") (clang version 12.0.5)";
                    }
                    content = "Linux version " + kv + " " + compilerStr + " #1 SMP PREEMPT " + std::string(dateBuf) + "\n";
                } else if (type == PROC_CPUINFO) {
                    content = generateMulticoreCpuInfo(fp);
                } else if (type == USB_SERIAL) {
                    content = omni::engine::generateRandomSerial(fp.brand, g_masterSeed, fp.securityPatch) + "\n";
                } else if (type == WIFI_MAC) {
                    // Android 10+ AOSP privacy: todas las apps sin root ven 02:00:00:00:00:00
                    // getifaddrs ya devuelve esto. El VFS DEBE ser coherente.
                    content = "02:00:00:00:00:00\n";
                } else if (type == BATTERY_TEMP) {
                    content = omni::engine::generateBatteryTemp(g_masterSeed) + "\n";
                } else if (type == BATTERY_VOLT) {
                    content = omni::engine::generateBatteryVoltage(g_masterSeed) + "\n";
                } else if (type == BATTERY_CAPACITY) {
                    content = std::to_string(40 + (g_masterSeed % 60)) + "\n";
                } else if (type == BATTERY_STATUS) {
                    content = "Discharging\n";
                } else if (type == BATTERY_CHARGE_FULL) {
                    // PR42: Capacidad fake coherente con el perfil (4000-5000 mAh)
                    // Determinística por seed — misma identidad, misma "capacidad de diseño"
                    long fakeCapacityUah = 4000000L + ((g_masterSeed % 1000L) * 1000L);
                    content = std::to_string(fakeCapacityUah) + "\n";
                } else if (type == PROC_HOSTNAME) {
                    // RFC 952: hostname estándar. "localhost" es el valor universal
                    // en AOSP stock. Cualquier hostname personalizado MIUI es detectable.
                    content = "localhost\n";

                } else if (type == PROC_OSTYPE) {
                    // Pendiente desde PR11 (LOW L11-ostype). Valor fijo para todos los
                    // kernels Linux de Android. Nunca debe exponer el vendor kernel.
                    content = "Linux\n";

                } else if (type == PROC_DTB_MODEL) {
                    // Device Tree Blob model. Instagram y Firebase lo leen directamente.
                    // En el Redmi 9 real contiene "Xiaomi Redmi 9 (mt6768)" — expone
                    // fabricante y SoC real aunque todas las properties estén hooked.
                    const DeviceFingerprint* fp2_ptr = findProfile(g_currentProfileName);
                    if (fp2_ptr) {
                        const auto& fp2 = *fp2_ptr;
                        content = std::string(fp2.manufacturer) + " " + std::string(fp2.model) + "\n";
                    } else {
                        content = "Android Device\n";
                    }

                } else if (type == PROC_ETH0_MAC) {
                    // eth0 es la interfaz Ethernet/USB. wlan0 ya está spoofed (PR9/PR20).
                    // Durante tethering o cuando la app itera todas las interfaces,
                    // eth0 puede revelar la MAC real con OUI del fabricante real (MediaTek).
                    // Usamos el mismo mecanismo que wlan0: dirección desactivada AOSP estándar.
                    content = "02:00:00:00:00:00\n";

                } else if (type == PROC_CMDLINE) {
                    // PR79: Hybrid cmdline — SoC-specific params for realistic fingerprint
                    std::string serial = omni::engine::generateRandomSerial(
                        fp.brand, g_masterSeed, fp.securityPatch);
                    std::string platform = toLowerStr(fp.boardPlatform);
                    const char* consoleP;
                    const char* socE;
                    if (platform.find("mt") == 0) {
                        consoleP = "console=tty0";
                        socE = " bootopt=64S3,32N2,64N2";
                    } else if (platform.find("exynos") != std::string::npos) {
                        consoleP = "console=ttySAC0,115200";
                        socE = "";
                    } else {
                        consoleP = "console=ttyMSM0,115200n8";
                        socE = " lpm_levels.sleep_disabled=1 swiotlb=2048";
                    }
                    content = std::string(consoleP)
                        + " androidboot.hardware=" + fp.hardware
                        + " androidboot.hardware.chipname=" + fp.hardwareChipname
                        + " androidboot.hardware.platform=" + fp.boardPlatform
                        + " androidboot.serialno=" + serial
                        + " androidboot.verifiedbootstate=green"
                        + " androidboot.vbmeta.device_state=locked"
                        + " androidboot.flash.locked=1"
                        + " androidboot.selinux=enforcing"
                        + " loop.max_part=7"
                        + socE + "\n";

                } else if (type == PROC_OSRELEASE) {
                    std::string plat = toLowerStr(fp.boardPlatform);
                    std::string brd  = toLowerStr(fp.brand);
                    std::string kv = "4.14.186-perf+";
                    if (brd == "google") {
                        if (plat.find("lito") != std::string::npos)
                            kv = "4.19.113-g820a424c538c-ab7336171";
                        else if (plat.find("atoll") != std::string::npos)
                            kv = "4.14.150-g62a62a5a93f7-ab7336171";
                        else if (plat.find("sdm670") != std::string::npos)
                            kv = "4.9.189-g5d098cef6d96-ab6174032";
                        else
                            kv = "4.19.113-g820a424c538c-ab7336171";
                    } else if (plat.find("mt6") != std::string::npos) {
                        // Samsung MTK: numeric suffix; others: perf+
                        if (brd == "samsung") {
                            kv = (plat.find("mt6769") != std::string::npos) ? "4.14.113-23424440" : "4.14.113-25267920";
                        } else {
                            kv = "4.14.186-perf+";
                        }
                    } else if (plat.find("kona") != std::string::npos ||
                               plat.find("lahaina") != std::string::npos) {
                        kv = "4.19.157-perf+";
                    } else if (plat.find("atoll") != std::string::npos ||
                               plat.find("lito") != std::string::npos) {
                        kv = "4.19.113-perf+";
                    } else if (plat.find("sdm670") != std::string::npos) {
                        kv = "4.9.189-perf+";
                    } else if (plat.find("bengal") != std::string::npos ||
                               plat.find("holi") != std::string::npos ||
                               plat.find("sm6350") != std::string::npos) {
                        kv = "4.19.157-perf+";
                    } else if (plat.find("sm7325") != std::string::npos) {
                        kv = "5.4.61-perf+";
                    }
                    // PR41: Kernels Samsung Exynos — sufijo numérico (NO -perf+ que es Qualcomm)
                    else if (plat.find("exynos9611") != std::string::npos) {
                        kv = "4.14.113-25145160";
                    } else if (plat.find("exynos9825") != std::string::npos) {
                        kv = "4.14.113-22911262";
                    } else if (plat.find("exynos850") != std::string::npos) {
                        kv = "4.19.113-25351273";
                    }
                    content = kv + "\n";
                } else if (type == PROC_UPTIME) {
                    char tmpBuf[256];
                    ssize_t r = orig_read(fd, tmpBuf, sizeof(tmpBuf)-1);
                    if (r > 0) {
                        tmpBuf[r] = '\0';
                        double uptime = 0, idle = 0;
                        if (sscanf(tmpBuf, "%lf %lf", &uptime, &idle) >= 1) {
                             uptime += 259200 + (g_masterSeed % 1036800);
                             // Simular una carga de CPU dinámica (75% - 85%) para evitar firmas de virtualización perfectas
                             double jitter_factor = 0.75 + ((double)(g_masterSeed % 1000) / 10000.0);
                             idle = uptime * jitter_factor;
                             std::stringstream ss;
                             ss << std::fixed << std::setprecision(2) << uptime << " " << idle << "\n";
                             content = ss.str();
                        } else {
                             content = std::string(tmpBuf);
                        }
                    }
                } else if (type == PROC_MAPS) {
                    char tmpBuf[4096];
                    ssize_t r;
                    std::string rawFile;
                    while ((r = orig_read(fd, tmpBuf, sizeof(tmpBuf))) > 0) {
                        rawFile.append(tmpBuf, r);
                    }
                    std::istringstream iss(rawFile);
                    std::string line;
                    while (std::getline(iss, line)) {
                        if (!isHiddenPath(line.c_str())) content += line + "\n";
                    }
                } else if (type == PROC_MEMINFO) {
                    char tmpBuf[8192];
                    ssize_t r = orig_read(fd, tmpBuf, sizeof(tmpBuf)-1);
                    if (r > 0) {
                        tmpBuf[r] = '\0';
                        std::string memData = tmpBuf;
                        // PR32: Reserva de kernel escalada según RAM total (firma dinámica anti-forense)
                        long kernelReserveKb = (fp.ram_gb <= 4)  ? 153600L :   // ~150 MB para 4 GB
                                               (fp.ram_gb <= 6)  ? 204800L :   // ~200 MB para 6 GB
                                               (fp.ram_gb <= 8)  ? 256000L :   // ~250 MB para 8 GB
                                               (fp.ram_gb <= 12) ? 409600L :   // ~400 MB para 12 GB
                                                                    524288L;   // ~512 MB para >12 GB
                        long fakeMemKb = (long)fp.ram_gb * 1048576L - kernelReserveKb;

                        size_t pos = memData.find("MemTotal:");
                        if (pos != std::string::npos) {
                            size_t end = memData.find('\n', pos);
                            if (end != std::string::npos) {
                                std::stringstream ss;
                                ss << "MemTotal:       " << fakeMemKb << " kB";
                                memData.replace(pos, end - pos, ss.str());
                            }
                        }
                        content = memData;
                    }
                } else if (type == PROC_MODULES || type == PROC_MOUNTS) {
                    char tmpBuf[4096]; ssize_t r; std::string rawFile;
                    while ((r = orig_read(fd, tmpBuf, sizeof(tmpBuf))) > 0) rawFile.append(tmpBuf, r);
                    std::istringstream iss(rawFile); std::string line;
                    while (std::getline(iss, line)) {
                        if (!isHiddenPath(line.c_str())) content += line + "\n";
                    }
                } else if (type == SYS_CPU_FREQ) {
                    std::string plat = toLowerStr(fp.boardPlatform);
                    std::string brand = toLowerStr(fp.brand);
                    bool isQcom = (brand == "google" || plat.find("msmnile") != std::string::npos ||
                        plat.find("kona") != std::string::npos || plat.find("lahaina") != std::string::npos ||
                        plat.find("atoll") != std::string::npos || plat.find("lito") != std::string::npos ||
                        plat.find("bengal") != std::string::npos || plat.find("holi") != std::string::npos ||
                        plat.find("trinket") != std::string::npos || plat.find("sdm670") != std::string::npos ||
                        plat.find("sm6150") != std::string::npos || plat.find("sm6350") != std::string::npos ||
                        plat.find("sm7325") != std::string::npos);
                    if (isQcom) content = "2841600\n";
                    else content = "2000000\n";
                } else if (type == SYS_SOC_MACHINE) {
                    content = std::string(fp.hardware) + "\n";
                } else if (type == SYS_SOC_FAMILY) {
                    std::string plat = toLowerStr(fp.boardPlatform);
                    if (plat.find("mt6") != std::string::npos || plat.find("mt8") != std::string::npos) content = "MediaTek\n";
                    else if (plat.find("exynos") != std::string::npos || plat.find("s5e") != std::string::npos) content = "Samsung\n";
                    else content = "Snapdragon\n";
                } else if (type == SYS_SOC_ID) {
                    content = std::string(fp.hardwareChipname) + "\n";
                } else if (type == SYS_FB0_SIZE) {
                    content = std::string(fp.screenWidth) + "," + std::string(fp.screenHeight) + "\n";
                } else if (type == PROC_ASOUND) {
                    std::string plat = toLowerStr(fp.boardPlatform);
                    if (plat.find("mt") != std::string::npos) {
                        char tmpBuf[1024]; ssize_t r;
                        while ((r = orig_read(fd, tmpBuf, sizeof(tmpBuf))) > 0) content.append(tmpBuf, r);
                    } else if (plat.find("exynos") != std::string::npos || plat.find("s5e") != std::string::npos) {
                        // Derivar card name del device del perfil (cada Samsung tiene card name único)
                        std::string devName = toLowerStr(fp.device);
                        content = " 0 [samsung        ]: " + devName + " - samsung\n"
                                  "                      samsung\n";
                    } else {
                        // Derivar card name de la plataforma Qualcomm real del perfil
                        std::string sndName = "snd_" + plat;
                        content = " 0 [" + sndName + "        ]: " + sndName + " - " + sndName + "\n"
                                  "                      " + sndName + "\n";
                    }
                } else if (type == PROC_INPUT) {
                    std::string plat = toLowerStr(fp.boardPlatform);
                    if (plat.find("mt") != std::string::npos) {
                        char tmpBuf[4096]; ssize_t r;
                        while ((r = orig_read(fd, tmpBuf, sizeof(tmpBuf))) > 0) content.append(tmpBuf, r);
                    } else {
                        // Nombre de driver táctil coherente con la marca del perfil
                        std::string brd = toLowerStr(fp.brand);
                        std::string touchName = "sec_touchscreen"; // Samsung default
                        if (brd == "google")        touchName = "fts_ts";         // Focaltech (Pixel)
                        else if (brd == "oneplus")   touchName = "goodix_ts";      // Goodix (OnePlus)
                        else if (brd == "motorola")  touchName = "synaptics_tcm";  // Synaptics (Moto)
                        else if (brd == "nokia")     touchName = "NVTtouch_ts";    // Novatek (Nokia)
                        else if (brd == "xiaomi" || brd == "redmi" || brd == "poco")
                                                     touchName = "fts_ts";         // Focaltech (Xiaomi)
                        else if (brd == "realme")    touchName = "goodix_ts";      // Goodix (Realme)
                        else if (brd == "asus")      touchName = "goodix_ts";      // Goodix (ASUS)
                        // else: samsung default sec_touchscreen

                        content = "I: Bus=0000 Vendor=0000 Product=0000 Version=0000\n"
                                  "N: Name=\"" + touchName + "\"\n"
                                  "P: Phys=\n"
                                  "S: Sysfs=/devices/virtual/input/input1\n"
                                  "U: Uniq=\n"
                                  "H: Handlers=event1\n"
                                  "B: PROP=2\n"
                                  "B: EV=b\n"
                                  "B: KEY=400 0 0 0 0 0\n"
                                  "B: ABS=260800000000000\n\n"
                                  "I: Bus=0000 Vendor=0000 Product=0000 Version=0000\n"
                                  "N: Name=\"gpio-keys\"\n"
                                  "P: Phys=gpio-keys/input0\n"
                                  "S: Sysfs=/devices/platform/soc/soc:gpio_keys/input/input0\n"
                                  "U: Uniq=\n"
                                  "H: Handlers=event0\n"
                                  "B: PROP=0\n"
                                  "B: EV=3\n"
                                  "B: KEY=10000000000000 0\n\n";
                    }
                } else if (type == SYS_THERMAL) {
                    if (strstr(pathname, "temp")) {
                         // Temperature in millicelsius (30C - 45C)
                         content = std::to_string(30000 + (g_masterSeed % 15000)) + "\n";
                    } else {
                        std::string plat = toLowerStr(fp.boardPlatform);
                        if (plat.find("mt") != std::string::npos) {
                            char tmpBuf[128]; ssize_t r;
                            if ((r = orig_read(fd, tmpBuf, sizeof(tmpBuf))) > 0) content.append(tmpBuf, r);
                        } else if (plat.find("exynos") != std::string::npos) {
                            content = "exynos-therm\n";
                        } else {
                            content = "tsens_tz_sensor\n";
                        }
                    }
                // Sincronización btime (Evita detección de manipulación de uptime)
                } else if (type == PROC_STAT) {
                    char tmpBuf[8192];
                    ssize_t r = orig_read(fd, tmpBuf, sizeof(tmpBuf)-1);
                    if (r > 0) {
                        tmpBuf[r] = '\0';
                        std::string statData = tmpBuf;
                        struct timespec ts_real;
                        orig_clock_gettime(CLOCK_BOOTTIME, &ts_real);
                        long offset_segundos = 259200 + (g_masterSeed % 1036800);
                        long uptime_falso = ts_real.tv_sec + offset_segundos;
                        long btime_falso = (long)time(NULL) - uptime_falso;
                        size_t pos = statData.find("btime ");
                        if (pos != std::string::npos) {
                            size_t end = statData.find('\n', pos);
                            if (end != std::string::npos)
                                statData.replace(pos, end - pos, "btime " + std::to_string(btime_falso));
                        }
                        content = statData;
                    }

                // Identidad de Almacenamiento (Oculta hardware real de la ROM)
                } else if (type == SYS_BLOCK_MODEL) {
                    std::string brd = toLowerStr(fp.brand);
                    if (brd == "samsung")
                        content = "KLUDG4UHDB-B2D1\n";        // Samsung UFS real
                    else if (brd == "google")
                        content = "SDINBDG4-64G\n";            // SanDisk (usado en Pixel)
                    else if (brd == "oneplus")
                        content = "H28S7Q302BMR\n";            // SK Hynix (usado en OnePlus)
                    else
                        content = "H9HP52ACPMMDAR\n";          // SK Hynix genérico (mayoría Android)

                // PR32: Capacidad de almacenamiento coherente con chip declarado
                } else if (type == SYS_BLOCK_SIZE) {
                    // Tamaño en sectores de 512 bytes. Heurística basada en RAM del perfil:
                    // 4GB RAM → 64GB storage = 134217728 sectores
                    // 6GB RAM → 128GB storage = 268435456 sectores
                    // 8GB+ RAM → 128GB storage = 268435456 sectores (modelo base)
                    // 12GB+ RAM → 256GB storage = 536870912 sectores
                    long storageSectors;
                    if (fp.ram_gb <= 4)       storageSectors = 134217728L;  // 64 GB
                    else if (fp.ram_gb <= 10) storageSectors = 268435456L;  // 128 GB
                    else                      storageSectors = 536870912L;  // 256 GB
                    content = std::to_string(storageSectors) + "\n";
                // Gobernadores AOSP (Elimina firmas 'mtk-cpufreq' o gobernadores propietarios)
                } else if (type == SYS_CPU_GOVERNORS) {
                    content = "performance powersave schedutil\n";
                } else if (type == BT_MAC) {
                    content = omni::engine::generateRandomMac(fp.brand, g_masterSeed + 2) + "\n";
                } else if (type == BT_NAME) {
                    content = std::string(fp.model) + "\n";
                // PR20: Tabla ARP virtualizada (oculta MAC real del gateway)
                // PR37: En modo LTE tabla vacía — datos móviles no exponen gateway ARP local
                } else if (type == PROC_NET_ARP) {
                    if (g_spoofMobileNetwork) {
                        content = "IP address       HW type  Flags  HW address            Mask     Device\n";
                    } else {
                        content = "IP address       HW type  Flags  HW address            Mask     Device\n"
                                  "192.168.1.1      0x1      0x2    00:00:00:00:00:00     *        wlan0\n";
                    }
                // PR20: Estadísticas de red virtualizadas (oculta TX/RX real)
                // PR37: En modo LTE mostrar rmnet_data0 en lugar de wlan0
                } else if (type == PROC_NET_DEV) {
                    if (g_spoofMobileNetwork) {
                        // Interfaz de datos móviles con stats LTE realistas
                        content = "Inter-|   Receive                                                |  Transmit\n"
                                  " face |bytes    packets errs drop fifo frame compressed multicast|"
                                  "bytes    packets errs drop fifo colls carrier compressed\n"
                                  "    lo:       0       0    0    0    0     0          0         0"
                                  "        0       0    0    0    0     0       0          0\n"
                                  "rmnet_data0:  2097152    8192    0    0    0     0          0         0"
                                  "   524288    2048    0    0    0     0       0          0\n";
                    } else {
                        content = "Inter-|   Receive                                                |  Transmit\n"
                                  " face |bytes    packets errs drop fifo frame compressed multicast|"
                                  "bytes    packets errs drop fifo colls carrier compressed\n"
                                  "    lo:       0       0    0    0    0     0          0         0"
                                  "        0       0    0    0    0     0       0          0\n"
                                  " wlan0:  524288    4096    0    0    0     0          0         0"
                                  "   131072    1024    0    0    0     0       0          0\n";
                    }
                // PR98: Tabla de rutas IPv4 virtualizada — oculta ruta tun0 cuando el proxy está activo.
                // Formato: Iface Destination Gateway Flags RefCnt Use Metric Mask MTU Window IRTT
                // IPs en hex little-endian: 0001A8C0=192.168.1.0, 0101A8C0=192.168.1.1, 00FFFFFF=255.255.255.0
                } else if (type == PROC_NET_ROUTE) {
                    if (g_spoofMobileNetwork) {
                        content = "Iface\tDestination\tGateway\tFlags\tRefCnt\tUse\tMetric\tMask\tMTU\tWindow\tIRTT\n"
                                  "rmnet_data0\t00000000\t00000000\t0001\t0\t0\t100\t00000000\t0\t0\t0\n"
                                  "lo\t0000007F\t00000000\t0001\t0\t0\t0\t000000FF\t0\t0\t0\n";
                    } else {
                        content = "Iface\tDestination\tGateway\tFlags\tRefCnt\tUse\tMetric\tMask\tMTU\tWindow\tIRTT\n"
                                  "wlan0\t0001A8C0\t00000000\t0001\t0\t0\t0\t00FFFFFF\t0\t0\t0\n"
                                  "wlan0\t00000000\t0101A8C0\t0003\t0\t0\t100\t00000000\t0\t0\t0\n"
                                  "lo\t0000007F\t00000000\t0001\t0\t0\t0\t000000FF\t0\t0\t0\n";
                    }
                // PR32: Tabla TCP virtualizada — oculta IPs locales y puertos de servicios reales
                } else if (type == PROC_NET_TCP) {
                    content = "  sl  local_address rem_address   st tx_queue rx_queue tr tm->when retrnsmt   uid  timeout inode\n";
                    // No se añaden entradas: tabla de sockets vacía = sin servicios locales expuestos
                // PR32: Tabla UDP virtualizada — ídem
                } else if (type == PROC_NET_UDP) {
                    content = "  sl  local_address rem_address   st tx_queue rx_queue tr tm->when retrnsmt   uid  timeout inode\n";

                } else if (type == PROC_NET_IPV6_ROUTE) {
                    if (g_spoofMobileNetwork) {
                        content = ""; // Ocultar rutas IPv6 en modo LTE
                    } else {
                        char tmpBuf[4096]; ssize_t r;
                        while ((r = orig_read(fd, tmpBuf, sizeof(tmpBuf))) > 0) content.append(tmpBuf, r);
                    }

                } else if (type == PROC_NET_IF_INET6) {
                    // PR42+PR84: Filter network interfaces from /proc/net/if_inet6.
                    // Always filter dummy0 (virtual, reveals non-standard config).
                    // Filter wlan0 in LTE spoofing mode.
                    // Format: hex_addr ifindex prefix_len scope flags ifname
                    if (g_spoofMobileNetwork) {
                        content = "";  // LTE mode: hide all IPv6 interfaces
                    } else {
                        char tmpBuf[4096]; ssize_t r;
                        std::string raw;
                        while ((r = orig_read(fd, tmpBuf, sizeof(tmpBuf))) > 0) raw.append(tmpBuf, r);
                        // PR42+PR84: Filter lines containing dummy, p2p interfaces.
                        // PR98: Added tun filter — tun0 gets a link-local IPv6 (fe80::/10)
                        // automatically from the kernel when brought up, leaking its presence.
                        std::istringstream iss(raw);
                        std::string line;
                        while (std::getline(iss, line)) {
                            if (line.find("dummy") != std::string::npos ||
                                line.find("p2p")   != std::string::npos ||
                                line.find("tun")   != std::string::npos) {
                                continue;  // skip this interface
                            }
                            content += line + "\n";
                        }
                    }

                // PR37: boot_id — UUID determinístico desde seed (Firebase/AppsFlyer lo correlacionan)
                } else if (type == PROC_BOOT_ID) {
                    content = generateBootId(g_masterSeed) + "\n";

                // PR37: /proc/self/cgroup — valor genérico de untrusted_app stock
                } else if (type == PROC_SELF_CGROUP) {
                    content = "0::/\n";

                // PR37: scaling_available_frequencies — firma de plataforma CPU por SoC
                } else if (type == SYS_CPU_FREQ_AVAIL) {
                    std::string plat = toLowerStr(fp.boardPlatform);
                    if      (plat.find("lahaina")  != std::string::npos) content = "300000 576000 768000 1094400 1401600 1766400 2016000 2265600 2457600 2841600\n";
                    else if (plat.find("kona")     != std::string::npos) content = "300000 576000 768000 1171200 1401600 1766400 1996800 2265600 2457600 2841600\n";
                    else if (plat.find("msmnile")  != std::string::npos) content = "300000 576000 768000 1056000 1200000 1401600 1536000 1612800 2016000 2419200 2841600\n";
                    else if (plat.find("lito")     != std::string::npos ||
                             plat.find("atoll")    != std::string::npos) content = "300000 576000 768000 1056000 1200000 1401600 1612800 1708800 2016000 2208000 2323200 2515200\n";
                    else if (plat.find("sm7325")   != std::string::npos) content = "300000 652800 806400 1056000 1209600 1401600 1516800 1612800 2016000 2246400 2515200 2726400\n";
                    else if (plat.find("sm6350")   != std::string::npos) content = "300000 633600 768000 1036800 1228800 1497600 1612800 1804800\n";
                    else if (plat.find("bengal")   != std::string::npos ||
                             plat.find("holi")     != std::string::npos) content = "614400 864000 1056000 1248000 1401600 1497600\n";
                    else if (plat.find("sm6150")   != std::string::npos) content = "825600 1056000 1209600 1459200 1612800 1766400 1881600 2016000 2208000\n";
                    else if (plat.find("sdm670")   != std::string::npos) content = "825600 1056000 1209600 1612800 1804800 2073600 2208000\n";
                    else if (plat.find("mt6785")   != std::string::npos) content = "500000 671875 1000000 1218750 1343750 1500000 1671875 1843750 2062500\n";
                    else if (plat.find("mt6768")   != std::string::npos ||
                             plat.find("mt6769")   != std::string::npos) content = "500000 625000 718750 828750 937500 1078750 1178750 1318750 1418750\n";
                    else if (plat.find("mt6853")   != std::string::npos ||
                             plat.find("mt6833")   != std::string::npos) content = "500000 718750 1000000 1218750 1343750 1500000 1671875 1843750 2062500\n";
                    else if (plat.find("mt6765")   != std::string::npos) content = "500000 625000 750000 875000 1000000 1125000 1250000 1350000\n";
                    else if (plat.find("mt6")      != std::string::npos) content = "500000 718750 1078750 1418750 1756250 1900000\n";
                    // PR42: Samsung Exynos — devolvían archivo vacío, ahora con frecuencias reales
                    else if (plat.find("exynos9611") != std::string::npos)
                        content = "182000 273000 546000 818000 1144000 1365000 1547000 1768000\n";
                    else if (plat.find("exynos9825") != std::string::npos)
                        content = "403000 845000 1274000 1690000 1937000 2210000 2613000\n";
                    else if (plat.find("exynos850")  != std::string::npos)
                        content = "208000 416000 625000 833000 1042000 1250000 1458000 1616000\n";
                    else                                                  content = "300000 576000 768000 1248000 1574400 1766400\n";

                // SYS_CPU_TOPOLOGY (existente, no mover)
                } else if (type == SYS_CPU_TOPOLOGY) {
                    content = "0-" + std::to_string(fp.core_count - 1) + "\n";
                } else if (type == BAT_TECHNOLOGY) {
                    content = "Li-poly\n";
                } else if (type == BAT_PRESENT) {
                    content = "1\n";
                } else if (type == PROC_SELF_STATUS) {
                    char tmpBuf[4096];
                    ssize_t r;
                    std::string real_content;
                    real_content.reserve(4096); // Pre-alloc para evitar reallocs durante el loop
                    while ((r = orig_read(fd, tmpBuf, sizeof(tmpBuf)-1)) > 0) {
                        tmpBuf[r] = '\0';
                        real_content += tmpBuf;
                    }
                    // Sanitizar TracerPid sin regex (O(n) string scan, zero heap alloc extra)
                    size_t pos = real_content.find("TracerPid:");
                    if (pos != std::string::npos) {
                        size_t val_start = pos + 10; // strlen("TracerPid:")
                        size_t val_end = real_content.find('\n', val_start);
                        if (val_end == std::string::npos) val_end = real_content.size();
                        real_content.replace(val_start, val_end - val_start, "\t0");
                    }
                    content = real_content;
                }
            }

            if (!content.empty()) {
                std::lock_guard<std::mutex> lock(g_fdMutex);
                g_fdMap[fd] = type;
                g_fdOffsetMap[fd] = 0;
                g_fdContentCache[fd] = { content, g_configGeneration };
            }
        }
    }
    return fd;
}

int my_openat(int dirfd, const char *pathname, int flags, mode_t mode) {
    if (!orig_openat) { errno = ENOSYS; return -1; }
    if (!pathname) return orig_openat(dirfd, pathname, flags, mode);

    std::string path_str(pathname);
    if (path_str == "/proc/modules" || path_str == "/proc/interrupts" ||
        path_str == "/proc/self/smaps_rollup" || isProcPidPath(pathname, "smaps_rollup")) {
        errno = EACCES;
        return -1;
    }
    // PR37: Bloquear filesystems (revela overlay/erofs de Magisk/KSU)
    if (path_str == "/proc/filesystems") {
        errno = EACCES;
        return -1;
    }
    if (path_str == "/proc/iomem") {
        return orig_openat(dirfd, "/dev/null", flags, mode);
    }

    if (pathname && strstr(pathname, "/dev/__properties__/")) {
        errno = EACCES;
        return -1;
    }
    if (pathname) {
        // Bloquear drivers de GPU contradictorios (Evasión Capa 5)
        const DeviceFingerprint* fp_gpu = findProfile(g_currentProfileName);
        if (fp_gpu) {
            std::string plat = toLowerStr(fp_gpu->boardPlatform);
            std::string brand = toLowerStr(fp_gpu->brand);
            bool isQcom = (brand == "google" || plat.find("msmnile") != std::string::npos ||
                plat.find("kona") != std::string::npos || plat.find("lahaina") != std::string::npos ||
                plat.find("atoll") != std::string::npos || plat.find("lito") != std::string::npos ||
                plat.find("bengal") != std::string::npos || plat.find("holi") != std::string::npos ||
                plat.find("trinket") != std::string::npos || plat.find("sdm670") != std::string::npos ||
                plat.find("sm6150") != std::string::npos || plat.find("sm6350") != std::string::npos ||
                plat.find("sm7325") != std::string::npos);
            std::string egl = toLowerStr(fp_gpu->eglDriver);

            // Adreno → bloquea mali; Mali → bloquea kgsl; PowerVR → bloquea ambos
            if (egl == "powervr") {
                if (strstr(pathname, "/dev/mali") || strstr(pathname, "/dev/kgsl")) { errno = ENOENT; return -1; }
            } else if (isQcom && strstr(pathname, "/dev/mali")) { errno = ENOENT; return -1; }
            else if (!isQcom && strstr(pathname, "/dev/kgsl")) { errno = ENOENT; return -1; }
        }
    }
    // Ruta absoluta → delegar directamente a my_open (que ya tiene la lógica VFS)
    if (pathname && pathname[0] == '/') {
        return my_open(pathname, flags, mode);
    }

    // Resolver dirfd a path real via /proc/self/fd/<n> (sin estado, O(1))
    std::string basedir;
    if (dirfd == AT_FDCWD) {
        char cwd[512] = {};
        if (getcwd(cwd, sizeof(cwd)) != nullptr) {
            basedir = cwd;
        }
    } else {
        char dirpath[512] = {};
        char fdlink[64];
        snprintf(fdlink, sizeof(fdlink), "/proc/self/fd/%d", dirfd);
        if (!orig_readlinkat) return orig_openat(dirfd, pathname, flags, mode);
        ssize_t len = orig_readlinkat(AT_FDCWD, fdlink, dirpath, sizeof(dirpath)-1);
        if (len > 0) {
            dirpath[len] = '\0';
            basedir = dirpath;
        }
    }

    // Si logramos resolver el directorio base, construimos la ruta absoluta
    if (!basedir.empty() && pathname) {
        std::string full = basedir + "/" + pathname;

        // Si cae en una ruta sensible del sistema, lo interceptamos con nuestro VFS
        if (full.find("/proc/") != std::string::npos ||
            full.find("/sys/")  != std::string::npos) {
            return my_open(full.c_str(), flags, mode);
        }

        // Verificación de root-hiding para evitar detección de Magisk/KernelSU
        if (isHiddenPath(full.c_str())) {
            errno = ENOENT;
            return -1;
        }
    }

    // Si no es una ruta interceptada, pasamos la llamada a la syscall original
    return orig_openat(dirfd, pathname, flags, mode);
}

int my_close(int fd) {
    {
        std::lock_guard<std::mutex> lock(g_fdMutex);
        g_fdMap.erase(fd);
        g_fdOffsetMap.erase(fd);
        g_fdContentCache.erase(fd);
        g_ashmemLoggedFds.erase(fd);
        g_ashmemFds.erase(fd);
        g_nonAshmemFds.erase(fd);
    }
    return orig_close ? orig_close(fd) : close(fd);
}

ssize_t my_read(int fd, void *buf, size_t count) {
    if (!orig_read) { errno = ENOSYS; return -1; }
    maybeLogAshmemFd(fd, "read");
    {
        std::lock_guard<std::mutex> lock(g_fdMutex);
        if (g_fdContentCache.count(fd)) {
            const CachedContent& cc = g_fdContentCache[fd];

            // Anti-Regression: Check for stale data generation
            if (cc.generation != g_configGeneration) {
                // If generation mismatch, we treat as EOF or invalid.
                // Or fallback to orig_read? Prompt says "invalida la lectura".
                // Returning 0 (EOF) or -1 (Error) is safest.
                // Assuming EOF to avoid crash.
                return 0;
            }

            const std::string& content = cc.content;
            size_t& offset = g_fdOffsetMap[fd];

            if (offset >= content.size()) return 0;

            size_t available = content.size() - offset;
            size_t toRead = std::min(count, available);

            memcpy(buf, content.c_str() + offset, toRead);
            offset += toRead;
            return (ssize_t)toRead;
        }
    }
    return orig_read(fd, buf, count);
}

off_t my_lseek(int fd, off_t offset, int whence) {
    if (!orig_lseek) { errno = ENOSYS; return -1; }
    {
        std::lock_guard<std::mutex> lock(g_fdMutex);
        if (g_fdContentCache.count(fd)) {
            size_t size = g_fdContentCache[fd].content.size();
            size_t& pos = g_fdOffsetMap[fd];
            if (whence == SEEK_SET)
                pos = std::min((size_t)std::max((off_t)0, offset), size);
            else if (whence == SEEK_CUR)
                pos = std::min((size_t)std::max((off_t)0, (off_t)pos + offset), size);
            else if (whence == SEEK_END)
                pos = size;
            return (off_t)pos;
        }
    }
    return orig_lseek(fd, offset, whence);
}

off64_t my_lseek64(int fd, off64_t offset, int whence) {
    return (off64_t)my_lseek(fd, (off_t)offset, whence);
}

ssize_t my_pread(int fd, void* buf, size_t count, off_t offset) {
    if (!orig_pread) { errno = ENOSYS; return -1; }
    maybeLogAshmemFd(fd, "pread");
    {
        std::lock_guard<std::mutex> lock(g_fdMutex);
        if (g_fdContentCache.count(fd)) {
            const CachedContent& cc = g_fdContentCache[fd];
            if (cc.generation != g_configGeneration) return 0;
            const std::string& content = cc.content;
            if ((size_t)offset >= content.size()) return 0;
            size_t available = content.size() - (size_t)offset;
            size_t toRead = std::min(count, available);
            memcpy(buf, content.c_str() + offset, toRead);
            // IMPORTANTE: pread NO modifica g_fdOffsetMap (semántica correcta pread vs read)
            return (ssize_t)toRead;
        }
    }
    return orig_pread(fd, buf, count, offset);
}

ssize_t my_pread64(int fd, void* buf, size_t count, off64_t offset) {
    return my_pread(fd, buf, count, (off_t)offset);
}

void* my_mmap(void* addr, size_t length, int prot, int flags, int fd, off_t offset) {
    if (!orig_mmap) {
        errno = ENOSYS;
        return MAP_FAILED;
    }
    void* ret = orig_mmap(addr, length, prot, flags, fd, offset);
    bool expected = false;
    if (g_pr120MmapPulseLogged.compare_exchange_strong(expected, true)) {
        LOGD("[PR120-PULSE] mmap hook active: fd=%d len=%zu flags=0x%x", fd, length, flags);
    }
    if (ret != MAP_FAILED && fd >= 0) {
        maybeLogAshmemFd(fd, "mmap");
    }
    return ret;
}

void* my_mmap64(void* addr, size_t length, int prot, int flags, int fd, off64_t offset) {
    if (!orig_mmap64) {
        errno = ENOSYS;
        return MAP_FAILED;
    }
    void* ret = orig_mmap64(addr, length, prot, flags, fd, offset);
    bool expected = false;
    if (g_pr120Mmap64PulseLogged.compare_exchange_strong(expected, true)) {
        LOGD("[PR120-PULSE] mmap64 hook active: fd=%d len=%zu flags=0x%x", fd, length, flags);
    }
    if (ret != MAP_FAILED && fd >= 0) {
        maybeLogAshmemFd(fd, "mmap64");
    }
    return ret;
}

// -----------------------------------------------------------------------------
// PR41: Hooks: dup family (VFS cache bypass prevention)
// Si un SDK clona un FD virtualizado con dup(), el nuevo FD debe heredar la caché.
// Sin esto, read(dup(fd)) leería el hardware real (MediaTek) en lugar del emulado.
// -----------------------------------------------------------------------------
int my_dup(int oldfd) {
    if (!orig_dup) return -1;
    int newfd = orig_dup(oldfd);
    if (newfd >= 0) {
        inheritAshmemCache(oldfd, newfd);
        std::lock_guard<std::mutex> lock(g_fdMutex);
        auto it = g_fdMap.find(oldfd);
        if (it != g_fdMap.end()) {
            g_fdMap[newfd] = it->second;
            g_fdOffsetMap[newfd] = 0;
            auto cache_it = g_fdContentCache.find(oldfd);
            if (cache_it != g_fdContentCache.end())
                g_fdContentCache[newfd] = cache_it->second;
        }
    }
    return newfd;
}

int my_dup2(int oldfd, int newfd) {
    if (!orig_dup2) return -1;
    // Si newfd ya está en nuestra caché, limpiarlo primero
    {
        std::lock_guard<std::mutex> lock(g_fdMutex);
        g_fdMap.erase(newfd);
        g_fdOffsetMap.erase(newfd);
        g_fdContentCache.erase(newfd);
        g_ashmemLoggedFds.erase(newfd);
        g_ashmemFds.erase(newfd);
        g_nonAshmemFds.erase(newfd);
    }
    int ret = orig_dup2(oldfd, newfd);
    if (ret >= 0) {
        inheritAshmemCache(oldfd, ret);
        std::lock_guard<std::mutex> lock(g_fdMutex);
        auto it = g_fdMap.find(oldfd);
        if (it != g_fdMap.end()) {
            g_fdMap[ret] = it->second;
            g_fdOffsetMap[ret] = 0;
            auto cache_it = g_fdContentCache.find(oldfd);
            if (cache_it != g_fdContentCache.end())
                g_fdContentCache[ret] = cache_it->second;
        }
    }
    return ret;
}

int my_dup3(int oldfd, int newfd, int flags) {
    if (!orig_dup3) return -1;
    {
        std::lock_guard<std::mutex> lock(g_fdMutex);
        g_fdMap.erase(newfd);
        g_fdOffsetMap.erase(newfd);
        g_fdContentCache.erase(newfd);
        g_ashmemLoggedFds.erase(newfd);
        g_ashmemFds.erase(newfd);
        g_nonAshmemFds.erase(newfd);
    }
    int ret = orig_dup3(oldfd, newfd, flags);
    if (ret >= 0) {
        inheritAshmemCache(oldfd, ret);
        std::lock_guard<std::mutex> lock(g_fdMutex);
        auto it = g_fdMap.find(oldfd);
        if (it != g_fdMap.end()) {
            g_fdMap[ret] = it->second;
            g_fdOffsetMap[ret] = 0;
            auto cache_it = g_fdContentCache.find(oldfd);
            if (cache_it != g_fdContentCache.end())
                g_fdContentCache[ret] = cache_it->second;
        }
    }
    return ret;
}

// -----------------------------------------------------------------------------
// Hooks: Network (SSL)
// -----------------------------------------------------------------------------
int my_SSL_CTX_set_ciphersuites(SSL_CTX *ctx, const char *str) { if (!orig_SSL_CTX_set_ciphersuites) return 0; return orig_SSL_CTX_set_ciphersuites(ctx, omni::engine::generateTls13CipherSuites(g_masterSeed).c_str()); }
int my_SSL_set1_tls13_ciphersuites(SSL *ssl, const char *str) { if (!orig_SSL_set1_tls13_ciphersuites) return 0; return orig_SSL_set1_tls13_ciphersuites(ssl, omni::engine::generateTls13CipherSuites(g_masterSeed).c_str()); }
int my_SSL_set_cipher_list(SSL *ssl, const char *str) { if (!orig_SSL_set_cipher_list) return 0; return orig_SSL_set_cipher_list(ssl, omni::engine::generateTls12CipherSuites(g_masterSeed).c_str()); }
int my_SSL_set_ciphersuites(SSL *ssl, const char *str) {
    if (!orig_SSL_set_ciphersuites) return 0;
    return orig_SSL_set_ciphersuites(ssl, omni::engine::generateTls13CipherSuites(g_masterSeed).c_str());
}

// -----------------------------------------------------------------------------
// Hooks: GPU
// -----------------------------------------------------------------------------
#define GL_EXTENSIONS 0x1F03

const GLubyte* my_glGetString(GLenum name) {
    if (!orig_glGetString) return nullptr;
    const DeviceFingerprint* fp_ptr = findProfile(g_currentProfileName);
    if (fp_ptr) {
        const auto& fp = *fp_ptr;
        if (name == GL_VENDOR)   return (const GLubyte*)fp.gpuVendor;
        if (name == GL_RENDERER) return (const GLubyte*)fp.gpuRenderer;
        if (name == GL_VERSION)  return (const GLubyte*)omni::engine::getGlVersionForProfile(fp);

        if (name == GL_EXTENSIONS) {
            const GLubyte* orig_ext = orig_glGetString(name);
            if (!orig_ext) return orig_ext;

            std::string egl = toLowerStr(fp.eglDriver);
            if (egl == "adreno") {
                // Perfil Qualcomm: eliminar extensiones ARM/Mali de las GL extensions
                // (mismo principio que EGL_EXTENSIONS — erase, no replace)
                static thread_local std::string gl_ext_cache;
                gl_ext_cache = reinterpret_cast<const char*>(orig_ext);

                static const char* ARM_GL_EXTS[] = {
                    "GL_ARM_", "GL_IMG_", "GL_OES_EGL_image_external_essl3",
                    nullptr
                };
                for (int i = 0; ARM_GL_EXTS[i]; ++i) {
                    size_t pos;
                    while ((pos = gl_ext_cache.find(ARM_GL_EXTS[i])) != std::string::npos) {
                        // GL_EXTENSIONS usa espacios como separadores
                        size_t end = gl_ext_cache.find(' ', pos);
                        if (end == std::string::npos) gl_ext_cache.erase(pos);
                        else gl_ext_cache.erase(pos, end - pos + 1);
                    }
                }
                // Limpiar espacios dobles resultantes del erase
                while (gl_ext_cache.find("  ") != std::string::npos)
                    gl_ext_cache.replace(gl_ext_cache.find("  "), 2, " ");

                return reinterpret_cast<const GLubyte*>(gl_ext_cache.c_str());
            }
        }
    }
    return orig_glGetString(name);
}

// -----------------------------------------------------------------------------
// Hooks: Vulkan API
// -----------------------------------------------------------------------------
void my_vkGetPhysicalDeviceProperties(VkPhysicalDevice physicalDevice, VkPhysicalDeviceProperties* pProperties) {
    if (!orig_vkGetPhysicalDeviceProperties) return;
    orig_vkGetPhysicalDeviceProperties(physicalDevice, pProperties);
    const DeviceFingerprint* fp_ptr = findProfile(g_currentProfileName);
    if (pProperties && fp_ptr) {
        const auto& fp = *fp_ptr;
        std::string egl = toLowerStr(fp.eglDriver);

        // Sobreescribir con los datos del perfil emulado
        strncpy(pProperties->deviceName, fp.gpuRenderer, VK_MAX_PHYSICAL_DEVICE_NAME_SIZE - 1);
        pProperties->deviceName[VK_MAX_PHYSICAL_DEVICE_NAME_SIZE - 1] = '\0';

        if (egl == "adreno") {
            pProperties->vendorID = 0x5143; // Qualcomm
        } else if (egl == "mali") {
            pProperties->vendorID = 0x13B5; // ARM
        } else if (egl == "powervr") {
            pProperties->vendorID = 0x1010; // Imagination Technologies
        }
    }
}

// -----------------------------------------------------------------------------
// PR38+39 Sensor name/vendor Dobby hooks REMOVED.
// Reason: android::Sensor::getName() returns const String8& (pointer to object),
// but the hooks returned const char* (pointer to chars).  The caller
// (translateNativeSensorToJavaSensor in libandroid_runtime.so) reads the first
// 8 bytes of the char data as String8::mString pointer → wild pointer → SIGBUS.
// Sensor metadata (range, resolution, etc.) is still spoofed via JNI hooks below.

// -----------------------------------------------------------------------------
// Hooks: sysinfo (Uptime Paradox Fix)
// -----------------------------------------------------------------------------
int my_sysinfo(struct sysinfo *info) {
    if (!orig_sysinfo) return -1;
    int ret = orig_sysinfo(info);
    if (ret == 0 && info != nullptr) {
        long added_uptime_seconds = 259200 + (g_masterSeed % 1036800);
        info->uptime += added_uptime_seconds;
    }
    return ret;
}

// -----------------------------------------------------------------------------
// Hooks: readdir (Ocultación de nodos de batería MTK)
// -----------------------------------------------------------------------------
struct dirent* my_readdir(DIR *dirp) {
    if (!orig_readdir) return nullptr;
    struct dirent* ret;
    while ((ret = orig_readdir(dirp)) != nullptr) {
        std::string dname = toLowerStr(ret->d_name);
        const DeviceFingerprint* fp_rd = findProfile(g_currentProfileName);
        if (fp_rd) {
            std::string plat = toLowerStr(fp_rd->boardPlatform);
            if (plat.find("mt") == std::string::npos) {
                if (dname.find("mtk") != std::string::npos || dname.find("mt_bat") != std::string::npos) {
                    continue; // Saltar archivos de MediaTek si no emulamos MTK
                }
            }
        }
        return ret;
    }
    return nullptr;
}

// -----------------------------------------------------------------------------
// PR70: dl_iterate_phdr — hide module .so from ELF shared object enumeration.
// Detection apps call dl_iterate_phdr() to list all loaded .so files without
// going through /proc/self/maps — must filter entries containing hidden paths.
// -----------------------------------------------------------------------------
struct DlIterateFilterCtx {
    int (*userCallback)(struct dl_phdr_info *, size_t, void *);
    void *userData;
};

static int dl_iterate_filter_cb(struct dl_phdr_info *info, size_t size, void *data) {
    auto *ctx = static_cast<DlIterateFilterCtx*>(data);
    if (info && info->dlpi_name && isHiddenPath(info->dlpi_name)) {
        return 0; // skip this entry
    }
    return ctx->userCallback(info, size, ctx->userData);
}

int my_dl_iterate_phdr(int (*callback)(struct dl_phdr_info *info, size_t size, void *data), void *data) {
    if (!orig_dl_iterate_phdr) return -1;
    DlIterateFilterCtx ctx = { callback, data };
    return orig_dl_iterate_phdr(dl_iterate_filter_cb, &ctx);
}

// -----------------------------------------------------------------------------
// Hooks: Hardware Capabilities (getauxval)
// -----------------------------------------------------------------------------
#ifndef HWCAP_ATOMICS
#define HWCAP_ATOMICS (1 << 8)
#define HWCAP_FPHP    (1 << 9)
#define HWCAP_ASIMDHP (1 << 10)
#define HWCAP_ASIMDDP (1 << 20)
#define HWCAP_LRCPC   (1 << 21)
#endif

unsigned long my_getauxval(unsigned long type) {
    if (!orig_getauxval) return 0;
    unsigned long val = orig_getauxval(type);
    const DeviceFingerprint* fp_ptr = findProfile(g_currentProfileName);
    if ((type == AT_HWCAP || type == AT_HWCAP2) && fp_ptr) {
        const auto& fp = *fp_ptr;
        std::string plat = toLowerStr(fp.boardPlatform);

        // Si el perfil es Cortex-A53 puro (ARMv8.0) o Exynos 9611, apagamos las flags ARMv8.2+
        // del kernel físico para no contradecir el cpuinfo falso.
        if (plat.find("mt6765") != std::string::npos || plat.find("exynos9611") != std::string::npos) {
            if (type == AT_HWCAP) {
                val &= ~(HWCAP_ATOMICS | HWCAP_FPHP | HWCAP_ASIMDHP | HWCAP_ASIMDDP | HWCAP_LRCPC);
            } else if (type == AT_HWCAP2) {
                val = 0; // ARMv8.0 no tiene features extendidas
            }
        }
    }
    return val;
}

// -----------------------------------------------------------------------------
// Hooks: Settings.Secure (JNI Bridge)
// -----------------------------------------------------------------------------
// Spoofing logic shared by all Settings.Secure wrappers (2-param, 3-param, ForUser).
// Returns spoofed jstring if key is intercepted, nullptr for passthrough.
static jstring spoofSettingsSecure(JNIEnv* env, jstring name) {
    if (!name) return nullptr;
    const char* key = env->GetStringUTFChars(name, nullptr);
    if (!key) return nullptr;

    jstring result = nullptr;
    if (strcmp(key, "android_id") == 0) {
        // Android 8.0+: SSAID es per-app — cada UID obtiene un valor único.
        // Incorporar getuid() al seed garantiza que cada app ve un android_id
        // diferente, replicando el comportamiento real del SettingsProvider.
        long perAppSeed = g_masterSeed ^ ((long)getuid() * 2654435761L);
        result = env->NewStringUTF(
            omni::engine::generateRandomId(16, perAppSeed).c_str()
        );
    } else if (strcmp(key, "gsf_id") == 0) {
        result = env->NewStringUTF(
            omni::engine::generateRandomId(16, g_masterSeed + 1).c_str()
        );
    } else if (strcmp(key, "bluetooth_name") == 0) {
        // PR72-QA Fix6: Override bluetooth_name to match spoofed profile.
        // Default "Android Bluedroid" leaks that device isn't using OEM BT stack.
        const DeviceFingerprint* fp = findProfile(g_currentProfileName);
        if (fp) result = env->NewStringUTF(fp->model);
    } else if (g_realDeviceIsMiui && strcmp(key, "miui_optimization") == 0) {
        // Force MIUI framework to use standard AOSP behavior for permissions,
        // app management, etc. Without this, MIUI sends permission intents to
        // com.lbe.security.miui which may be missing/disabled -> crash.
        result = env->NewStringUTF("0");
    }
    env->ReleaseStringUTFChars(name, key);
    return result;
}

static jstring my_SettingsSecure_getString(JNIEnv* env, jstring name) {
    jstring r = spoofSettingsSecure(env, name);
    if (r) return r;
    if (!orig_SettingsSecure_getString) return nullptr;
    return orig_SettingsSecure_getString(env, name);
}

// 3-param variant: (JNIEnv*, jobject contentResolver, jstring name)
// Some ROMs export Settings.Secure.getString with a ContentResolver parameter.
// Using the wrong wrapper causes `name` to receive the ContentResolver object,
// making ALL Settings.Secure reads return null (including miui_optimization).
static jstring my_SettingsSecure_getString3(JNIEnv* env, jobject contentResolver, jstring name) {
    jstring r = spoofSettingsSecure(env, name);
    if (r) return r;
    if (!orig_SettingsSecure_getString3) return nullptr;
    return orig_SettingsSecure_getString3(env, contentResolver, name);
}

static jstring my_SettingsSecure_getStringForUser(JNIEnv* env, jstring name, jint userHandle) {
    jstring r = spoofSettingsSecure(env, name);
    if (r) return r;
    if (!orig_SettingsSecure_getStringForUser) return nullptr;
    return orig_SettingsSecure_getStringForUser(env, name, userHandle);
}

// 4-param variant: (JNIEnv*, jobject contentResolver, jstring name, jint userHandle)
// Some ROMs export getStringForUser with a ContentResolver parameter.
static jstring my_SettingsSecure_getStringForUser4(JNIEnv* env, jobject cr, jstring name, jint userHandle) {
    jstring r = spoofSettingsSecure(env, name);
    if (r) return r;
    if (!orig_SettingsSecure_getStringForUser4) return nullptr;
    return orig_SettingsSecure_getStringForUser4(env, cr, name, userHandle);
}

// -----------------------------------------------------------------------------
// Hooks: Settings.Global (JNI Bridge)
// -----------------------------------------------------------------------------
// Spoofing logic shared by all Settings.Global wrappers.
static jstring spoofSettingsGlobal(JNIEnv* env, jstring name) {
    if (!name) return nullptr;
    const char* key = env->GetStringUTFChars(name, nullptr);
    if (!key) return nullptr;

    jstring result = nullptr;
    if (strcmp(key, "device_name") == 0) {
        const DeviceFingerprint* fp = findProfile(g_currentProfileName);
        if (fp) result = env->NewStringUTF(fp->model);
    }
    // Force LTE: ocultar WiFi en Settings.Global
    else if (g_spoofMobileNetwork && strcmp(key, "wifi_on") == 0) {
        result = env->NewStringUTF("0");
    }
    else if (g_spoofMobileNetwork && strcmp(key, "wifi_sleep_policy") == 0) {
        result = env->NewStringUTF("2");  // WIFI_SLEEP_POLICY_NEVER (irrelevante si off)
    }
    env->ReleaseStringUTFChars(name, key);
    return result;
}

// PR75b: Settings.Global hook — intercepts device_name which leaks "Redmi 9"
static jstring my_SettingsGlobal_getString(JNIEnv* env, jstring name) {
    jstring r = spoofSettingsGlobal(env, name);
    if (r) return r;
    if (!orig_SettingsGlobal_getString) return nullptr;
    return orig_SettingsGlobal_getString(env, name);
}

// 3-param variant: (JNIEnv*, jobject contentResolver, jstring name)
static jstring my_SettingsGlobal_getString3(JNIEnv* env, jobject contentResolver, jstring name) {
    jstring r = spoofSettingsGlobal(env, name);
    if (r) return r;
    if (!orig_SettingsGlobal_getString3) return nullptr;
    return orig_SettingsGlobal_getString3(env, contentResolver, name);
}

static jstring my_SettingsGlobal_getStringForUser(JNIEnv* env, jstring name, jint userHandle) {
    jstring r = spoofSettingsGlobal(env, name);
    if (r) return r;
    if (!orig_SettingsGlobal_getStringForUser) return nullptr;
    return orig_SettingsGlobal_getStringForUser(env, name, userHandle);
}

// 4-param variant: (JNIEnv*, jobject contentResolver, jstring name, jint userHandle)
static jstring my_SettingsGlobal_getStringForUser4(JNIEnv* env, jobject cr, jstring name, jint userHandle) {
    jstring r = spoofSettingsGlobal(env, name);
    if (r) return r;
    if (!orig_SettingsGlobal_getStringForUser4) return nullptr;
    return orig_SettingsGlobal_getStringForUser4(env, cr, name, userHandle);
}

void my_system_property_read_callback(const prop_info *pi, void (*callback)(void *cookie, const char *name, const char *value, uint32_t serial), void *cookie) {
    if (!pi || !callback) return;

    // PR84: If pi is a fake prop_info (from my_system_property_find), return its
    // stored values directly — do NOT pass it to the original (would SIGSEGV reading
    // invalid shared memory addresses).
    {
        std::lock_guard<std::mutex> lock(g_fakePropMutex);
        if (g_fakePropPtrs.count(reinterpret_cast<const void*>(pi))) {
            const FakePropInfo* fake = reinterpret_cast<const FakePropInfo*>(pi);
            callback(cookie, fake->name, fake->value, fake->serial);
            return;
        }
    }

    struct NameCatcher {
        std::string name;
        std::string real_val;
        uint32_t serial;
    } catcher;

    auto internal_cb = [](void* c, const char* n, const char* v, uint32_t s) {
        NameCatcher* catch_ptr = (NameCatcher*)c;
        if(n) catch_ptr->name = n;
        if(v) catch_ptr->real_val = v;
        catch_ptr->serial = s;
    };

    if (!orig_system_property_read_callback) { callback(cookie, "", "", 0); return; }
    orig_system_property_read_callback(pi, internal_cb, &catcher);

    if (shouldHide(catcher.name.c_str()) || shouldHide(catcher.real_val.c_str())) {
        callback(cookie, catcher.name.c_str(), "", catcher.serial);
        return;
    }

    char spoofed_val[92] = {0};
    int ret = my_system_property_get(catcher.name.c_str(), spoofed_val);

    if (ret > 0 && strcmp(spoofed_val, catcher.real_val.c_str()) != 0) {
        callback(cookie, catcher.name.c_str(), spoofed_val, catcher.serial);
    } else {
        callback(cookie, catcher.name.c_str(), catcher.real_val.c_str(), catcher.serial);
    }
}

// PR73b-Fix1: Hook __system_property_read() — API legacy (deprecated API 26).
// VD Info and detection apps use __system_property_find() + __system_property_read()
// to read properties directly from shared memory, completely bypassing our hooks on
// __system_property_get and __system_property_read_callback. This closes that vector.
int my_system_property_read(const prop_info *pi, char *name, char *value) {
    if (!pi) return 0;

    // PR84: Handle fake prop_info from my_system_property_find
    {
        std::lock_guard<std::mutex> lock(g_fakePropMutex);
        if (g_fakePropPtrs.count(reinterpret_cast<const void*>(pi))) {
            const FakePropInfo* fake = reinterpret_cast<const FakePropInfo*>(pi);
            if (name) strncpy(name, fake->name, PROP_NAME_MAX);
            if (value) strncpy(value, fake->value, PROP_VALUE_MAX - 1);
            return (int)strlen(fake->value);
        }
    }

    // Read real data first to obtain the property name
    int ret = 0;
    char real_name[PROP_NAME_MAX + 1] = {0};
    char real_value[PROP_VALUE_MAX] = {0};
    if (orig_system_property_read) {
        ret = orig_system_property_read(pi, real_name, real_value);
    } else if (orig_system_property_read_callback) {
        // Fix8: orig_system_property_read is NULL (Dobby trampoline failed on thin
        // syscall wrapper). Use the working __system_property_read_callback as fallback
        // to extract property name and value from the prop_info pointer.
        struct ReadCtx { char n[256]; char v[PROP_VALUE_MAX]; uint32_t s; };
        ReadCtx ctx = {};
        orig_system_property_read_callback(pi,
            [](void* cookie, const char* n, const char* v, uint32_t s) {
                auto* c = static_cast<ReadCtx*>(cookie);
                if (n) strncpy(c->n, n, sizeof(c->n) - 1);
                if (v) strncpy(c->v, v, PROP_VALUE_MAX - 1);
                c->s = s;
            }, &ctx);
        strncpy(real_name, ctx.n, PROP_NAME_MAX);
        strncpy(real_value, ctx.v, PROP_VALUE_MAX - 1);
        ret = (int)ctx.s;
    }

    // Copy name to caller's buffer
    if (name && real_name[0]) {
        strcpy(name, real_name);
    }

    // If shouldHide filters this property (key or value), return empty
    if (shouldHide(real_name) || shouldHide(real_value)) {
        if (value) value[0] = '\0';
        if (name) name[0] = '\0';
        return 0;
    }

    // Attempt to spoof via my_system_property_get (central hub)
    if (value && real_name[0]) {
        char spoofed[PROP_VALUE_MAX] = {0};
        int slen = my_system_property_get(real_name, spoofed);
        if (slen > 0 && strcmp(spoofed, real_value) != 0) {
            strcpy(value, spoofed);
            return slen;
        }
        // No spoof available, pass through real value
        strcpy(value, real_value);
    }
    return ret;
}

// -----------------------------------------------------------------------------
// PR84: Hook __system_property_find — close direct shared-memory read vector
// -----------------------------------------------------------------------------
// ROOT CAUSE: VD-Info calls __system_property_find("ro.product.device") which
// returns a const prop_info* pointer to the shared property memory region. It then
// reads the value field DIRECTLY from the struct (raw pointer dereference at offset 4)
// instead of calling any API function. This bypasses ALL our hooks on
// __system_property_get, __system_property_read, and __system_property_read_callback.
//
// FIX: Return a fake prop_info struct (allocated per-property, lifetime=process) with
// the spoofed value. The struct layout matches bionic's prop_info on Android 11:
//   offset 0: uint32_t serial
//   offset 4: char value[PROP_VALUE_MAX]  (92 bytes)
//   offset 96: char name[0]              (flexible array)
// Even if detection apps read the value at offset 4 directly, they see the spoofed value.
//
// RECURSION GUARD: __system_property_get internally calls __system_property_find.
// We use a thread_local flag (g_inPropertyFind) to prevent:
//   my_system_property_find → my_system_property_get → orig_system_property_get
//   → original code → __system_property_find (hooked) → my_system_property_find → ∞
// When the flag is set, we pass through to orig_system_property_find directly.
// -----------------------------------------------------------------------------
const prop_info* my_system_property_find(const char* name) {
    // Recursion guard: when called from within our own spoofing chain, pass through
    if (g_inPropertyFind || !name || !name[0]) {
        return orig_system_property_find ? orig_system_property_find(name) : nullptr;
    }

    g_inPropertyFind = true;

    // Get real prop_info from shared memory
    const prop_info* real_pi = orig_system_property_find ? orig_system_property_find(name) : nullptr;

    // Get real value via original (bypasses our hooks thanks to recursion guard)
    char real_val[PROP_VALUE_MAX] = {0};
    if (orig_system_property_get) {
        orig_system_property_get(name, real_val);
    }

    // Get spoofed value via our spoofing hub
    char spoofed[PROP_VALUE_MAX] = {0};
    my_system_property_get(name, spoofed);

    g_inPropertyFind = false;

    // If no spoofed value, or same as real → return real prop_info
    if (spoofed[0] == '\0' || strcmp(spoofed, real_val) == 0) {
        return real_pi;
    }

    // When PR91 patched pages, prefer real prop_info* (lives in property trie,
    // safe for pointer arithmetic). FakePropInfo from heap causes SIGSEGV in
    // apps that walk the trie and compute pointer distances.
    if (g_pagesPatched && real_pi) {
        return real_pi;
    }

    // Property IS spoofed and differs from real → return fake prop_info
    std::lock_guard<std::mutex> lock(g_fakePropMutex);
    std::string key(name);
    auto it = g_fakePropMap.find(key);
    FakePropInfo* fake;
    if (it == g_fakePropMap.end()) {
        // Allocate new fake prop_info (lifetime = process)
        fake = new FakePropInfo();
        memset(fake, 0, sizeof(FakePropInfo));
        strncpy(fake->name, name, sizeof(fake->name) - 1);
        // Copy serial from real prop_info if available (consistency for atomic readers)
        if (real_pi) {
            fake->serial = reinterpret_cast<const uint32_t*>(real_pi)[0];
        }
        g_fakePropMap[key] = fake;
        g_fakePropPtrs.insert(reinterpret_cast<const void*>(fake));
        LOGD("[scope] PR84: created fake prop_info for '%s'", name);
    } else {
        fake = it->second;
    }

    // Update value (may change if profile switches, though unlikely at runtime)
    memset(fake->value, 0, PROP_VALUE_MAX);
    strncpy(fake->value, spoofed, PROP_VALUE_MAX - 1);

    return reinterpret_cast<const prop_info*>(fake);
}

// -----------------------------------------------------------------------------
// PR88: Hook __system_property_foreach — intercept property enumeration
// -----------------------------------------------------------------------------
// ROOT CAUSE: Detection apps call __system_property_foreach(callback, cookie) to
// iterate ALL properties in the shared memory property area. The callback receives
// real prop_info* pointers directly from the property trie, bypassing our hook on
// __system_property_find (which returns FakePropInfo with spoofed values). This is
// confirmed as the root cause of dual reads: VDInfo enumerates properties via foreach,
// reads the real value from the raw prop_info*, then compares with Build.* Java fields.
//
// FIX: Wrap the user's callback. For each property, extract its name from the real
// prop_info, then call my_system_property_find(name) which returns a FakePropInfo
// (with the spoofed value) for spoofed properties, or the real prop_info for others.
// The user's callback sees only spoofed values — no dual read.
// -----------------------------------------------------------------------------
struct ForeachWrapperCtx {
    void (*userFn)(const prop_info*, void*);
    void* userCookie;
};

static void foreachWrapperCallback(const prop_info* pi, void* cookie) {
    auto* ctx = static_cast<ForeachWrapperCtx*>(cookie);
    if (!pi || !ctx || !ctx->userFn) return;

    // Extract property name from the real prop_info.
    // Prefer read_callback (returns full name) over deprecated read (truncates
    // names >= 31 chars and spams logcat with "property name length >= 31" warnings).
    char name[256] = {};
    char value[PROP_VALUE_MAX] = {};

    if (orig_system_property_read_callback) {
        struct ReadCtx { char n[256]; char v[PROP_VALUE_MAX]; };
        ReadCtx rctx = {};
        orig_system_property_read_callback(pi,
            [](void* c, const char* n, const char* v, uint32_t) {
                auto* rc = static_cast<ReadCtx*>(c);
                if (n) strncpy(rc->n, n, sizeof(rc->n) - 1);
                if (v) strncpy(rc->v, v, PROP_VALUE_MAX - 1);
            }, &rctx);
        strncpy(name, rctx.n, sizeof(name) - 1);
    } else if (orig_system_property_read) {
        orig_system_property_read(pi, name, value);
    }

    if (!name[0]) {
        // Could not extract name — pass through real prop_info unchanged
        ctx->userFn(pi, ctx->userCookie);
        return;
    }

    // Filter hidden properties (mediatek, miui on non-MIUI, etc.)
    if (shouldHide(name)) return;

    if (g_pagesPatched) {
        // PR91 already rewrote shared memory pages with spoofed values.
        // Pass the real prop_info* directly — its memory contains the spoofed data
        // and the pointer lives in the property trie (safe for pointer arithmetic).
        // Using FakePropInfo (heap) would crash apps that do trie-walking.
        ctx->userFn(pi, ctx->userCookie);
    } else {
        // Fallback: resolve through my_system_property_find which may return FakePropInfo
        const prop_info* resolved = my_system_property_find(name);
        ctx->userFn(resolved ? resolved : pi, ctx->userCookie);
    }
}

static int my_system_property_foreach(
    void (*propfn)(const prop_info* pi, void* cookie), void* cookie) {
    if (!orig_system_property_foreach || !propfn) {
        return orig_system_property_foreach ?
            orig_system_property_foreach(propfn, cookie) : -1;
    }

    ForeachWrapperCtx ctx = { propfn, cookie };
    return orig_system_property_foreach(foreachWrapperCallback, &ctx);
}

// -----------------------------------------------------------------------------
// PR86: Hook dlsym — intercept dynamic symbol resolution for property functions
// -----------------------------------------------------------------------------
// ROOT CAUSE: Detection apps (VDInfo, Snapchat) can call dlsym(RTLD_DEFAULT,
// "__system_property_find") to obtain a raw function pointer to the real libc
// implementation. They then call via this pointer, completely bypassing both
// PLT hooks (which only patch GOT entries in pre-loaded ELFs) and even Dobby
// inline hooks (if the app resolves the address before our hook is installed).
// By hooking dlsym itself, we intercept the resolution and return our spoofed
// function pointers for all property-related symbols.
//
// SAFETY: dlsym is used extensively throughout the process (by Dobby, by our
// own code, by the linker). The thread_local g_inDlsym guard prevents infinite
// recursion. This hook MUST be installed AFTER all our own dlsym() calls for
// resolving orig_* pointers are complete.
// -----------------------------------------------------------------------------
static void* my_dlsym(void* handle, const char* symbol) {
    // Recursion guard: pass through when called from our own hooks or Dobby internals
    if (g_inDlsym || !symbol) {
        return orig_dlsym ? orig_dlsym(handle, symbol) : nullptr;
    }
    g_inDlsym = true;

    void* result = nullptr;
    if (strcmp(symbol, "__system_property_find") == 0) {
        result = (void*)my_system_property_find;
        LOGD("[scope] PR86: dlsym intercepted '%s' → my_system_property_find", symbol);
    } else if (strcmp(symbol, "__system_property_read") == 0) {
        result = (void*)my_system_property_read;
        LOGD("[scope] PR86: dlsym intercepted '%s' → my_system_property_read", symbol);
    } else if (strcmp(symbol, "__system_property_get") == 0) {
        result = (void*)my_system_property_get;
        LOGD("[scope] PR86: dlsym intercepted '%s' → my_system_property_get", symbol);
    } else if (strcmp(symbol, "__system_property_read_callback") == 0) {
        result = (void*)my_system_property_read_callback;
        LOGD("[scope] PR86: dlsym intercepted '%s' → my_system_property_read_callback", symbol);
    } else if (strcmp(symbol, "__system_property_foreach") == 0) {
        result = (void*)my_system_property_foreach;
        LOGD("[scope] PR88: dlsym intercepted '%s' → my_system_property_foreach", symbol);
    } else {
        result = orig_dlsym ? orig_dlsym(handle, symbol) : nullptr;
    }

    g_inDlsym = false;
    return result;
}

// -----------------------------------------------------------------------------
// PR71: Hook execve — intercept getprop commands in child processes
// -----------------------------------------------------------------------------
// ROOT CAUSE: VD-Infos and detection apps use Runtime.exec("getprop propname")
// which forks a child process. After fork(), the child calls execve("getprop")
// which replaces the process image and destroys all Dobby hooks. By hooking
// execve itself, we intercept the call BEFORE the image is replaced, read the
// property via our hooked my_system_property_get, write the spoofed value to
// stdout, and _exit(0) — the real getprop binary never runs.

// PR73-Fix2: Helper — emulates `uname` subprocess output with spoofed kernel version.
// Called from my_execve (directly, before _exit) and handleGetpropSpawn (inside fork).
// argv is the effective argv after any toybox-shift:
//   argv[0] = binary name,  argv[1..] = flags (-r, -a, -m, -s, -n, -v)
static void emulate_uname_output(char *const argv[]) {
    std::string rel      = getSpoofedKernelVersion();
    const char* sysname  = "Linux";
    const char* nodename = "localhost";
    const char* ver      = "#1 SMP PREEMPT";
    const char* machine  = "aarch64";

    bool flag_r=false, flag_a=false, flag_m=false,
         flag_s=false, flag_n=false, flag_v=false, any=false;

    // PR73b-Fix4: Detect shell wrapper case: argv = ["sh", "-c", "uname -r"]
    // In this case argv[1]="-c" and argv[2] contains the flags as a string.
    // strcmp won't match "-r" inside "uname -r", so we use strstr instead.
    if (argv[1] && strcmp(argv[1], "-c") == 0 && argv[2]) {
        if (strstr(argv[2], "-r")) { flag_r = true; any = true; }
        if (strstr(argv[2], "-a")) { flag_a = true; any = true; }
        if (strstr(argv[2], "-m")) { flag_m = true; any = true; }
        if (strstr(argv[2], "-s")) { flag_s = true; any = true; }
        if (strstr(argv[2], "-n")) { flag_n = true; any = true; }
        if (strstr(argv[2], "-v")) { flag_v = true; any = true; }
    } else {
        // Normal case: argv = ["uname", "-r"]
        for (int i = 1; argv[i]; i++) {
            const char* f = argv[i];
            if      (strcmp(f, "-r") == 0) { flag_r = true; any = true; }
            else if (strcmp(f, "-a") == 0) { flag_a = true; any = true; }
            else if (strcmp(f, "-m") == 0) { flag_m = true; any = true; }
            else if (strcmp(f, "-s") == 0) { flag_s = true; any = true; }
            else if (strcmp(f, "-n") == 0) { flag_n = true; any = true; }
            else if (strcmp(f, "-v") == 0) { flag_v = true; any = true; }
        }
    }
    if (!any || flag_a) {
        char buf[512];
        int len = snprintf(buf, sizeof(buf), "%s %s %s %s %s\n",
                           sysname, nodename, rel.c_str(), ver, machine);
        if (len > 0) write(STDOUT_FILENO, buf, (size_t)len);
    } else {
        bool first = true;
        auto emit = [&](const char* s) {
            if (!first) write(STDOUT_FILENO, " ", 1);
            write(STDOUT_FILENO, s, strlen(s));
            first = false;
        };
        if (flag_s) emit(sysname);
        if (flag_n) emit(nodename);
        if (flag_r) emit(rel.c_str());
        if (flag_v) emit(ver);
        if (flag_m) emit(machine);
        write(STDOUT_FILENO, "\n", 1);
    }
}

static int my_execve(const char *pathname, char *const argv[], char *const envp[]) {
    LOGE("PR73b: my_execve called: %s", pathname ? pathname : "null");
    if (pathname && argv) {
        // Extract basename from path
        const char* base = strrchr(pathname, '/');
        base = base ? base + 1 : pathname;

        bool is_getprop = (strcmp(base, "getprop") == 0);
        bool is_uname   = (strcmp(base, "uname")   == 0);  // PR73-Fix2

        // PR73-Fix1: toybox/toolbox direct invocation bypass.
        // VD Info executes: /system/bin/toybox getprop <prop>  or  toybox uname -r
        // basename="toybox" bypasses the existing getprop/uname checks above.
        if (!is_getprop && !is_uname && argv[1] &&
            (strcmp(base, "toybox") == 0 || strcmp(base, "toolbox") == 0)) {
            if (strcmp(argv[1], "getprop") == 0) { argv = argv + 1; is_getprop = true; }
            else if (strcmp(argv[1], "uname") == 0) { argv = argv + 1; is_uname = true; }
        }

        // PR72-QA: Detect "sh -c getprop ..." / "su -c getprop ..." bypass
        // Detection apps run shell wrappers to avoid direct getprop hooks
        if (!is_getprop && !is_uname &&
            (strcmp(base, "sh") == 0 || strcmp(base, "bash") == 0 || strcmp(base, "su") == 0) &&
            argv[1] && strcmp(argv[1], "-c") == 0 && argv[2]) {
            if (strstr(argv[2], "getprop")) is_getprop = true;
            if (strstr(argv[2], "uname"))   is_uname   = true;  // PR73-Fix2
        }

        if (is_getprop) {
            if (argv[1] && argv[1][0] != '\0' && strcmp(argv[1], "-c") != 0) {
                // Single property read: getprop <name> [default]
                char value[92] = {0};
                int len = my_system_property_get(argv[1], value);
                if (len > 0) {
                    write(STDOUT_FILENO, value, len);
                    write(STDOUT_FILENO, "\n", 1);
                } else if (argv[2]) {
                    // Property empty/hidden — use default value if provided
                    size_t dlen = strlen(argv[2]);
                    write(STDOUT_FILENO, argv[2], dlen);
                    write(STDOUT_FILENO, "\n", 1);
                }
                // No output for non-existent properties (matches real getprop)
                _exit(0);
            }
            // getprop with no args — emulate full property dump with spoofed values
            __system_property_foreach([](const prop_info* pi, void* /*cookie*/) {
                struct { char name[256]; } ctx = {};
                __system_property_read_callback(pi,
                    [](void* c, const char* n, const char*, unsigned) {
                        auto* p = static_cast<decltype(&ctx)>(c);
                        if (n) strncpy(p->name, n, sizeof(p->name) - 1);
                    }, &ctx);

                if (ctx.name[0] == '\0') return;

                // CRITICAL: Completely omit hidden/MTK/Xiaomi properties from the dump.
                // A real Samsung has ZERO ro.vendor.mediatek.* or ro.miui.* entries.
                // Printing them as empty [] would flag the device as spoofed.
                if (shouldHide(ctx.name)) return;

                char val[PROP_VALUE_MAX] = {0};
                my_system_property_get(ctx.name, val);

                char line[512];
                int len = snprintf(line, sizeof(line), "[%s]: [%s]\n", ctx.name, val);
                if (len > 0) write(STDOUT_FILENO, line, len);
            }, nullptr);
            _exit(0);
        }

        // PR73-Fix2: Intercept uname subprocess (uname -r, uname -a, etc.)
        // When execve replaces the child image all Dobby hooks are destroyed.
        // Emulate expected output directly before the exec happens.
        if (is_uname) {
            emulate_uname_output(argv);
            _exit(0);
        }
    }

    // PR73b-Fix6: When orig_execve is NULL (PLT stub — Dobby can't build trampoline),
    // fall back to raw syscall. This is the standard pattern for Bionic on aarch64.
    if (orig_execve) return orig_execve(pathname, argv, envp);
    return syscall(__NR_execve, pathname, argv, envp);
}

// -----------------------------------------------------------------------------
// PR71c: posix_spawn / posix_spawnp hooks
// -----------------------------------------------------------------------------
// On Android 10+ (API 28+), Runtime.exec() and ProcessBuilder use posix_spawn
// instead of execve. The child process created via posix_spawn is born without
// our Zygisk hooks and reads the real hardware properties. We apply the EXACT
// same getprop interception logic as my_execve: if the command is getprop,
// we fork ourselves, emulate the output with spoofed values, and _exit(0)
// before the real binary ever runs.
// -----------------------------------------------------------------------------

// Fix9: Generate spoofed content as a string for intercepted commands.
// Returns empty string if the command is not intercepted.
// This is a pure function — no fork, no write, no side effects.
static std::string generateSpoofedContent(const char *resolved_path,
                                          char *const argv[]) {
    if (!resolved_path || !argv) return "";

    const char* base = strrchr(resolved_path, '/');
    base = base ? base + 1 : resolved_path;

    bool is_cat     = (strcmp(base, "cat")     == 0);
    bool is_getprop = (strcmp(base, "getprop") == 0);
    bool is_uname   = (strcmp(base, "uname")   == 0);

    // PR73-Fix1: toybox/toolbox direct invocation bypass.
    char *const *effective_argv = argv;
    if (!is_getprop && !is_cat && !is_uname && argv[1] &&
        (strcmp(base, "toybox") == 0 || strcmp(base, "toolbox") == 0)) {
        if (strcmp(argv[1], "getprop") == 0)    { effective_argv = argv + 1; is_getprop = true; }
        else if (strcmp(argv[1], "uname") == 0) { effective_argv = argv + 1; is_uname   = true; }
        else if (strcmp(argv[1], "cat") == 0)   { effective_argv = argv + 1; is_cat     = true; }
    }

    // PR72-QA: Detect "sh -c getprop" / "sh -c cat" / "su -c ..." bypass
    if (!is_getprop && !is_cat && !is_uname &&
        (strcmp(base, "sh") == 0 || strcmp(base, "bash") == 0 || strcmp(base, "su") == 0) &&
        argv[1] && strcmp(argv[1], "-c") == 0 && argv[2]) {
        if (strstr(argv[2], "getprop")) { is_getprop = true; effective_argv = argv; }
        if (strstr(argv[2], "cat"))     { is_cat     = true; effective_argv = argv; }
        if (strstr(argv[2], "uname"))   { is_uname   = true; effective_argv = argv; }
    }

    // cat /proc/cpuinfo interception
    if (is_cat && effective_argv[1] && effective_argv[1][0] != '\0') {
        const DeviceFingerprint* fp_ptr = findProfile(g_currentProfileName);
        if (fp_ptr) {
            const char* target = effective_argv[1];
            if (strstr(target, "/proc/cpuinfo")) {
                const std::string& cpuinfo = getCachedCpuInfo(*fp_ptr);
                if (!cpuinfo.empty()) return cpuinfo;
            }
            // Fix11: Intercept cat /proc/version subprocess
            if (strstr(target, "/proc/version")) {
                std::string kv = getSpoofedKernelVersion();
                return "Linux version " + kv + " (build@server) (gcc) #1 SMP PREEMPT\n";
            }
        }
        return ""; // cat of non-intercepted file
    }

    // uname output generation (to string, not STDOUT)
    if (is_uname) {
        std::string rel      = getSpoofedKernelVersion();
        const char* sysname  = "Linux";
        const char* nodename = "localhost";
        const char* ver      = "#1 SMP PREEMPT";
        const char* machine  = "aarch64";

        bool flag_r=false, flag_a=false, flag_m=false,
             flag_s=false, flag_n=false, flag_v=false, any=false;

        if (effective_argv[1] && strcmp(effective_argv[1], "-c") == 0 && effective_argv[2]) {
            if (strstr(effective_argv[2], "-r")) { flag_r = true; any = true; }
            if (strstr(effective_argv[2], "-a")) { flag_a = true; any = true; }
            if (strstr(effective_argv[2], "-m")) { flag_m = true; any = true; }
            if (strstr(effective_argv[2], "-s")) { flag_s = true; any = true; }
            if (strstr(effective_argv[2], "-n")) { flag_n = true; any = true; }
            if (strstr(effective_argv[2], "-v")) { flag_v = true; any = true; }
        } else {
            for (int i = 1; effective_argv[i]; i++) {
                const char* f = effective_argv[i];
                if      (strcmp(f, "-r") == 0) { flag_r = true; any = true; }
                else if (strcmp(f, "-a") == 0) { flag_a = true; any = true; }
                else if (strcmp(f, "-m") == 0) { flag_m = true; any = true; }
                else if (strcmp(f, "-s") == 0) { flag_s = true; any = true; }
                else if (strcmp(f, "-n") == 0) { flag_n = true; any = true; }
                else if (strcmp(f, "-v") == 0) { flag_v = true; any = true; }
            }
        }

        std::string result;
        if (!any || flag_a) {
            char buf[512];
            snprintf(buf, sizeof(buf), "%s %s %s %s %s\n",
                     sysname, nodename, rel.c_str(), ver, machine);
            result = buf;
        } else {
            bool first = true;
            auto emit = [&](const char* s) {
                if (!first) result += ' ';
                result += s;
                first = false;
            };
            if (flag_s) emit(sysname);
            if (flag_n) emit(nodename);
            if (flag_r) emit(rel.c_str());
            if (flag_v) emit(ver);
            if (flag_m) emit(machine);
            result += '\n';
        }
        return result;
    }

    if (!is_getprop) return "";

    // getprop interception
    if (effective_argv[1] && effective_argv[1][0] != '\0') {
        // Single property: getprop <name> [default]
        char value[92] = {0};
        int len = my_system_property_get(effective_argv[1], value);
        if (len > 0) return std::string(value, len) + "\n";
        if (effective_argv[2]) return std::string(effective_argv[2]) + "\n";
        return "\n"; // empty property — matches real getprop behavior
    }

    // getprop with no args — full property dump
    std::string dump;
    __system_property_foreach([](const prop_info* pi, void* cookie) {
        auto* out = static_cast<std::string*>(cookie);
        char name[256] = {};
        __system_property_read_callback(pi,
            [](void* c, const char* n, const char*, unsigned) {
                if (n) strncpy(static_cast<char*>(c), n, 255);
            }, name);

        if (name[0] == '\0') return;
        if (shouldHide(name)) return;

        char val[PROP_VALUE_MAX] = {0};
        my_system_property_get(name, val);

        char line[512];
        int len = snprintf(line, sizeof(line), "[%s]: [%s]\n", name, val);
        if (len > 0) out->append(line, (size_t)len);
    }, &dump);
    return dump;
}

// Fix9: Subprocess interception via memfd + real posix_spawn.
// Creates memfd with spoofed content, calls orig_posix_spawn with ["cat", "/proc/self/fd/<N>"]
// passing the ORIGINAL file_actions — this preserves Java's pipe infrastructure.
static bool handleInterceptedSpawn(const char *resolved_path,
                                   char *const argv[], pid_t *pid,
                                   const void *file_actions,
                                   const void *attrp,
                                   char *const envp[],
                                   decltype(orig_posix_spawn) orig_fn) {
    std::string content = generateSpoofedContent(resolved_path, argv);
    if (content.empty()) return false;

    // Create memfd WITHOUT MFD_CLOEXEC — fd must survive exec into cat
    int mfd = syscall(__NR_memfd_create, "s", 0);
    if (mfd < 0) {
        LOGE("Fix9: memfd_create failed errno=%d, fallback fork+write", errno);
        pid_t child = fork();
        if (child < 0) return false;
        if (child == 0) {
            write(STDOUT_FILENO, content.c_str(), content.size());
            _exit(0);
        }
        if (pid) *pid = child;
        return true;
    }

    write(mfd, content.c_str(), content.size());
    lseek(mfd, 0, SEEK_SET);

    char fd_path[64];
    snprintf(fd_path, sizeof(fd_path), "/proc/self/fd/%d", mfd);
    char *new_argv[] = { (char*)"cat", fd_path, nullptr };

    int ret;
    if (orig_fn) {
        ret = orig_fn(pid, "/system/bin/cat", file_actions, attrp, new_argv, envp);
    } else {
        // Last resort: fork+execve (no file_actions — lossy)
        LOGE("Fix9: WARN orig_fn NULL, fork+execve for memfd cat");
        pid_t child = fork();
        if (child == -1) { close(mfd); return false; }
        if (child == 0) {
            execve("/system/bin/cat", new_argv, envp);
            _exit(127);
        }
        if (pid) *pid = child;
        ret = 0;
    }

    close(mfd);
    LOGE("Fix9: intercepted '%s' via memfd cat ret=%d", resolved_path, ret);
    return ret == 0;
}

static int my_posix_spawn(pid_t *pid, const char *path,
                          const void *file_actions,
                          const void *attrp,
                          char *const argv[], char *const envp[]) {
    LOGE("Fix9: my_posix_spawn: %s", path ? path : "null");

    // Fix9: memfd approach preserves Java's pipe infrastructure
    if (handleInterceptedSpawn(path, argv, pid, file_actions, attrp, envp, orig_posix_spawn))
        return 0;

    if (orig_posix_spawn)
        return orig_posix_spawn(pid, path, file_actions, attrp, argv, envp);

    // Last resort fallback (no file_actions — lossy)
    LOGE("Fix9: WARN orig_posix_spawn NULL, fork+execve for '%s'", path ? path : "null");
    pid_t child = fork();
    if (child == -1) return errno;
    if (child == 0) { execve(path, argv, envp); _exit(127); }
    if (pid) *pid = child;
    return 0;
}

static int my_posix_spawnp(pid_t *pid, const char *file,
                           const void *file_actions,
                           const void *attrp,
                           char *const argv[], char *const envp[]) {
    LOGE("Fix9: my_posix_spawnp: %s", file ? file : "null");

    // Fix9: memfd approach preserves Java's pipe infrastructure
    if (file && handleInterceptedSpawn(file, argv, pid, file_actions, attrp, envp, orig_posix_spawnp))
        return 0;

    if (orig_posix_spawnp)
        return orig_posix_spawnp(pid, file, file_actions, attrp, argv, envp);

    // Last resort fallback
    LOGE("Fix9: WARN orig_posix_spawnp NULL, fork+execve for '%s'", file ? file : "null");
    pid_t child = fork();
    if (child == -1) return errno;
    if (child == 0) { execve(file, argv, envp); _exit(127); }
    if (pid) *pid = child;
    return 0;
}

// -----------------------------------------------------------------------------
// PR71b: Hook android.os.SystemProperties.native_get(String, String)
// On Android 11+, apps can read properties via JNI without going through
// __system_property_get. Detection apps use reflection to call native_get
// directly, bypassing our libc hooks. This forces them through our spoofing.
// -----------------------------------------------------------------------------
static jstring my_SystemProperties_native_get(JNIEnv* env, jclass /*clazz*/,
                                               jstring keyJ, jstring defJ) {
    if (!keyJ) return defJ;
    const char* key = env->GetStringUTFChars(keyJ, nullptr);
    if (!key) return defJ;

    char val[PROP_VALUE_MAX] = {0};
    int ret = my_system_property_get(key, val);
    env->ReleaseStringUTFChars(keyJ, key);

    if (ret > 0) {
        return env->NewStringUTF(val);
    }
    return defJ;
}

// -----------------------------------------------------------------------------
// Hooks: Telephony (JNI Bridge)
// -----------------------------------------------------------------------------
jstring JNICALL my_getDeviceId(JNIEnv* env, jobject thiz, jint slotId) {
    return env->NewStringUTF(omni::engine::generateValidImei(g_currentProfileName, g_masterSeed + slotId).c_str());
}
jstring JNICALL my_getSubscriberId(JNIEnv* env, jobject thiz, jint subId) {
    return env->NewStringUTF(omni::engine::generateValidImsi(g_currentProfileName, g_masterSeed + subId).c_str());
}
jstring JNICALL my_getSimSerialNumber(JNIEnv* env, jobject thiz, jint subId) {
    return env->NewStringUTF(omni::engine::generateValidIccid(g_currentProfileName, g_masterSeed + subId + 100).c_str());
}
jstring JNICALL my_getLine1Number(JNIEnv* env, jobject thiz, jint subId) {
    return env->NewStringUTF(omni::engine::generatePhoneNumber(g_currentProfileName, g_masterSeed + subId).c_str());
}
jstring JNICALL my_getNetworkCountryIso(JNIEnv* env, jobject thiz) {
    return env->NewStringUTF("us");
}
jstring JNICALL my_getNetworkCountryIsoSlot(JNIEnv* env, jobject thiz, jint slotIndex) {
    return env->NewStringUTF("us");
}
jstring JNICALL my_getSimCountryIso(JNIEnv* env, jobject thiz) {
    return env->NewStringUTF("us");
}
jstring JNICALL my_getSimCountryIsoSlot(JNIEnv* env, jobject thiz, jint slotIndex) {
    return env->NewStringUTF("us");
}
jstring JNICALL my_getTypeAllocationCode(JNIEnv* env, jobject thiz) {
    std::string imei = omni::engine::generateValidImei(g_currentProfileName, g_masterSeed);
    return env->NewStringUTF(imei.substr(0, 8).c_str());
}

// -----------------------------------------------------------------------------
// SIM presence hooks — siempre reportar SIM insertada y lista
// Evita incoherencia: NetworkInfo=MOBILE + getSimState=ABSENT es contradictorio
// -----------------------------------------------------------------------------
jint JNICALL my_getSimState(JNIEnv*, jobject) {
    return 5;  // TelephonyManager.SIM_STATE_READY
}
jint JNICALL my_getSimStateSlot(JNIEnv*, jobject, jint) {
    return 5;  // SIM_STATE_READY para cualquier slot
}
jboolean JNICALL my_hasIccCard(JNIEnv*, jobject) {
    return JNI_TRUE;
}
jint JNICALL my_getPhoneCount(JNIEnv*, jobject) {
    return 1;  // Un slot de SIM activo
}
jstring JNICALL my_getSimOperatorName(JNIEnv* env, jobject) {
    return env->NewStringUTF(omni::engine::getCarrierNameForImsi(g_currentProfileName, g_masterSeed).c_str());
}
jstring JNICALL my_getSimOperatorNameSlot(JNIEnv* env, jobject, jint) {
    return env->NewStringUTF(omni::engine::getCarrierNameForImsi(g_currentProfileName, g_masterSeed).c_str());
}
jstring JNICALL my_getSimOperator(JNIEnv* env, jobject) {
    std::string imsi = omni::engine::generateValidImsi(g_currentProfileName, g_masterSeed);
    return env->NewStringUTF(imsi.substr(0, 6).c_str());
}
jstring JNICALL my_getSimOperatorSlot(JNIEnv* env, jobject, jint) {
    std::string imsi = omni::engine::generateValidImsi(g_currentProfileName, g_masterSeed);
    return env->NewStringUTF(imsi.substr(0, 6).c_str());
}
jstring JNICALL my_getNetworkOperatorName(JNIEnv* env, jobject) {
    return env->NewStringUTF(omni::engine::getCarrierNameForImsi(g_currentProfileName, g_masterSeed).c_str());
}
jstring JNICALL my_getNetworkOperator(JNIEnv* env, jobject) {
    std::string imsi = omni::engine::generateValidImsi(g_currentProfileName, g_masterSeed);
    return env->NewStringUTF(imsi.substr(0, 6).c_str());
}
jint JNICALL my_getNetworkType(JNIEnv*, jobject) {
    return 13;  // TelephonyManager.NETWORK_TYPE_LTE
}
jint JNICALL my_getDataNetworkType(JNIEnv*, jobject) {
    return 13;  // NETWORK_TYPE_LTE
}


// -----------------------------------------------------------------------------
// PR44: Camera2 — auxiliar LENS_FACING oracle
// -----------------------------------------------------------------------------
// Consulta ANDROID_LENS_FACING (0x00050006) en el propio objeto CameraMetadataNative.
// Devuelve true si la cámara es frontal (valor 0 = LENS_FACING_FRONT).
//
// DISEÑO: orig_nativeReadValues apunta al C++ del framework, NO al hook.
// No hay recursión. El tag 0x00050006 cae en el default: del switch principal
// intencionalmente — si se añade un case para ese tag en el futuro, refactorizar
// este helper primero para evitar recursión circular.
//
// LENS_FACING values: 0 = FRONT, 1 = BACK, 2 = EXTERNAL
// Sección ANDROID_LENS = 0x05, index 6 → tag = 0x00050006, tipo: byte (1 byte)
// -----------------------------------------------------------------------------
static bool isFrontCameraMetadata(JNIEnv* env, jobject thiz) {
    if (!orig_nativeReadValues || !env || !thiz) return false;
    jbyteArray facing = orig_nativeReadValues(env, thiz, 0x00050006);
    // Guard: drivers MediaTek mal implementados pueden lanzar excepción y retornar
    // puntero no-nulo simultáneamente. Con excepción pendiente, cualquier llamada JNI
    // posterior (GetArrayLength) viola la spec y crashea. Limpiar antes de continuar.
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        return false;
    }
    if (!facing) return false;
    jsize len = env->GetArrayLength(facing);
    if (len < 1) { env->DeleteLocalRef(facing); return false; }
    jbyte val = 0;
    env->GetByteArrayRegion(facing, 0, 1, &val);
    env->DeleteLocalRef(facing);
    return (val == 0);  // 0 = LENS_FACING_FRONT
}

// -----------------------------------------------------------------------------
// PR44: Camera2 — my_nativeReadValues (camera-aware, rear + front)
// -----------------------------------------------------------------------------
// Intercepta los 6 tags de geometría/óptica del sensor. Para cada tag, detecta
// si el objeto pertenece a la cámara frontal via isFrontCameraMetadata() y
// selecciona los globals correspondientes (g_cam* vs g_camFront*).
//
// TAGS INTERCEPTADOS (verificados vs AOSP Android 11 camera_metadata_tags.h):
//   Sección ANDROID_SENSOR_INFO = 15 = 0x0F:
//     0x000F0005  PHYSICAL_SIZE              float[2]  8B   {width_mm, height_mm}
//     0x000F0006  PIXEL_ARRAY_SIZE           int32[2]  8B   {width_px, height_px}
//     0x000F0000  ACTIVE_ARRAY_SIZE          int32[4] 16B   {0, 0, w, h}
//     0x000F000A  PRE_CORRECTION_ACTIVE_ARRAY int32[4] 16B  {0, 0, w, h}
//   Sección ANDROID_LENS_INFO = 9 = 0x09:
//     0x00090002  AVAILABLE_FOCAL_LENGTHS    float[1]  4B   {focal_mm}
//     0x00090000  AVAILABLE_APERTURES        float[1]  4B   {f-number}
//       ⚠️  0x00090000 = APERTURES ≠ 0x00090001 = FILTER_DENSITIES (distinto)
//
// TAG ORÁCULO (NO interceptado, usado por isFrontCameraMetadata):
//   0x00050006  LENS_FACING — cae en default:, pasa al original
// -----------------------------------------------------------------------------
static jbyteArray my_nativeReadValues(JNIEnv* env, jobject thiz, jint tag) {
    if (!orig_nativeReadValues) return nullptr;
    if (!findProfile(g_currentProfileName)) {
        return orig_nativeReadValues(env, thiz, tag);
    }

    const bool isGeometricTag = (tag == 0x000F0005 || tag == 0x000F0006 ||
                                  tag == 0x000F0000 || tag == 0x000F000A ||
                                  tag == 0x00090002 || tag == 0x00090000);
    const bool isFront = isGeometricTag && isFrontCameraMetadata(env, thiz);

    switch (tag) {
        case 0x000F0005: {
            float vals[2] = {
                isFront ? g_camFrontPhysWidth  : g_camPhysicalWidth,
                isFront ? g_camFrontPhysHeight : g_camPhysicalHeight
            };
            jbyteArray arr = env->NewByteArray(8);
            if (!arr) return orig_nativeReadValues(env, thiz, tag);
            env->SetByteArrayRegion(arr, 0, 8, reinterpret_cast<const jbyte*>(vals));
            return arr;
        }
        case 0x000F0006: {
            int32_t vals[2] = {
                isFront ? g_camFrontPixWidth  : g_camPixelWidth,
                isFront ? g_camFrontPixHeight : g_camPixelHeight
            };
            jbyteArray arr = env->NewByteArray(8);
            if (!arr) return orig_nativeReadValues(env, thiz, tag);
            env->SetByteArrayRegion(arr, 0, 8, reinterpret_cast<const jbyte*>(vals));
            return arr;
        }
        case 0x000F0000:
        case 0x000F000A: {
            int32_t pw = isFront ? g_camFrontPixWidth  : g_camPixelWidth;
            int32_t ph = isFront ? g_camFrontPixHeight : g_camPixelHeight;
            int32_t vals[4] = { 0, 0, pw, ph };
            jbyteArray arr = env->NewByteArray(16);
            if (!arr) return orig_nativeReadValues(env, thiz, tag);
            env->SetByteArrayRegion(arr, 0, 16, reinterpret_cast<const jbyte*>(vals));
            return arr;
        }
        case 0x00090002: {
            float vals[1] = { isFront ? g_camFrontFocLen : g_camFocalLength };
            jbyteArray arr = env->NewByteArray(4);
            if (!arr) return orig_nativeReadValues(env, thiz, tag);
            env->SetByteArrayRegion(arr, 0, 4, reinterpret_cast<const jbyte*>(vals));
            return arr;
        }
        case 0x00090000: {
            float vals[1] = { isFront ? g_camFrontAperture : g_camAperture };
            jbyteArray arr = env->NewByteArray(4);
            if (!arr) return orig_nativeReadValues(env, thiz, tag);
            env->SetByteArrayRegion(arr, 0, 4, reinterpret_cast<const jbyte*>(vals));
            return arr;
        }
        default:
            // LENS_FACING (0x00050006) cae aquí intencionalmente.
            // Es el oráculo de isFrontCameraMetadata — no interceptar.
            if (orig_nativeReadValues == nullptr) return nullptr;
            return orig_nativeReadValues(env, thiz, tag);
    }
}

// -----------------------------------------------------------------------------
// PR44: MediaCodec — crash guard en native_setup (lado de creación)
// -----------------------------------------------------------------------------
// Traduce nombres de codec falsos a nombres MTK reales antes de que el framework
// intente instanciar un codec que no existe en el hardware físico.
// Solo activo cuando nameIsType=false (nombre explícito, no MIME type).
// nameIsType=true → MIME type ("video/avc") → pasar sin modificar.
// -----------------------------------------------------------------------------
static std::string translateCodecToReal(const std::string& name) {
    struct Rule { const char* from; const char* to; };
    static const Rule rules[] = {
        {"c2.qti.avc.",    "c2.mtk.avc."},
        {"c2.qti.hevc.",   "c2.mtk.hevc."},
        {"c2.qti.vp8.",    "c2.mtk.vp8."},
        {"c2.qti.vp9.",    "c2.mtk.vp9."},
        {"c2.qti.av1.",    "c2.mtk.av1."},
        {"c2.qti.mpeg4.",  "c2.mtk.mpeg4."},
        {"c2.qti.h263.",   "c2.mtk.h263."},
        {"c2.sec.avc.",    "c2.mtk.avc."},
        {"c2.sec.hevc.",   "c2.mtk.hevc."},
        {"c2.sec.vp8.",    "c2.mtk.vp8."},
        {"c2.sec.vp9.",    "c2.mtk.vp9."},
        {"c2.sec.av1.",    "c2.mtk.av1."},
        {"c2.sec.mpeg4.",  "c2.mtk.mpeg4."},
        {"c2.sec.h263.",   "c2.mtk.h263."},
        {"OMX.qcom.video.", "OMX.MTK.VIDEO."},
        {"OMX.Exynos.",     "OMX.MTK."},
    };
    for (const auto& r : rules) {
        if (name.compare(0, strlen(r.from), r.from) == 0)
            return r.to + name.substr(strlen(r.from));
    }
    return name;
}

static void my_native_setup(JNIEnv* env, jobject thiz,
                             jstring name, jboolean nameIsType, jboolean encoder) {
    if (nameIsType == JNI_FALSE && name != nullptr) {
        const char* cname = env->GetStringUTFChars(name, nullptr);
        if (cname) {
            std::string originalName(cname);              // Copiar a memoria C++ segura
            env->ReleaseStringUTFChars(name, cname);      // Liberar puntero JNI — cname es dangling a partir de aquí
            std::string translated = translateCodecToReal(originalName);
            if (translated != originalName)               // Comparar strings C++, no punteros liberados
                name = env->NewStringUTF(translated.c_str());
        }
    }
    if (!orig_native_setup) return;
    orig_native_setup(env, thiz, name, nameIsType, encoder);
}

// -----------------------------------------------------------------------------
// Module Main
// -----------------------------------------------------------------------------
static zygisk::Api *g_api = nullptr;  // PR56: Api global guardada en onLoad
static JavaVM *g_jvm = nullptr;       // PR55: guardar JavaVM en lugar de JNIEnv raw
static bool g_isTargetApp = false;    // PR56: guardia de proceso calculada en preAppSpecialize

// Fix8: Forward declarations for PLT hook targets (defined earlier in the file)
int my_system_property_read(const prop_info *pi, char *name, char *value);
const prop_info* my_system_property_find(const char* name);  // PR84
static int my_system_property_foreach(                       // PR88
    void (*propfn)(const prop_info* pi, void* cookie), void* cookie);
static int my_execve(const char *pathname, char *const argv[], char *const envp[]);
static int my_posix_spawn(pid_t *pid, const char *path,
                          const void *file_actions, const void *attrp,
                          char *const argv[], char *const envp[]);
static int my_posix_spawnp(pid_t *pid, const char *file,
                           const void *file_actions, const void *attrp,
                           char *const argv[], char *const envp[]);

// PR84b: Shadow property pages — per-process property memory patching.
// After fork (postAppSpecialize), replaces shared property mmap pages with private
// anonymous copies, then writes spoofed values directly into prop_info->value fields.
// This defeats ALL detection methods including direct memory reads from prop_info*
// pointers returned by __system_property_find() — because the actual memory now
// contains the spoofed value. Only the target process sees the change (private mapping).
//
// prop_info layout (bionic, Android 8+):
//   offset 0:  atomic_uint32_t serial  (4 bytes)
//   offset 4:  char value[92]          (PROP_VALUE_MAX)
//   offset 96: char name[0]            (flexible array)
static void patchPropertyPages() {
    // PR91: Use orig_system_property_foreach (Dobby trampoline) instead of dlsym.
    // Our my_dlsym hook (PR86) intercepts dlsym("__system_property_foreach") and
    // returns my_system_property_foreach, which wraps the callback with FakePropInfo
    // substitution. Phase 2 needs REAL prop_info* pointers into shared memory
    // (not FakePropInfo* on the heap), so we must use the original function.
    LOGE("PR91: patchPropertyPages() starting");
    if (!orig_system_property_foreach) {
        LOGE("PR91: orig_system_property_foreach not set, skip patchPropertyPages");
        return;
    }
    if (!orig_system_property_read_callback && !orig_system_property_read) {
        LOGE("PR91: no orig read function, skip");
        return;
    }

    // Phase 1: Find all properties where real value != spoofed value
    struct PatchEntry {
        const prop_info* pi;
        char spoofed[PROP_VALUE_MAX];
    };
    struct IterCtx {
        std::vector<PatchEntry>* patches;
        int total;
    };
    std::vector<PatchEntry> patches;
    IterCtx ctx = { &patches, 0 };

    LOGE("PR91: Phase 1 — iterating all properties...");
    orig_system_property_foreach([](const prop_info* pi, void* cookie) {
        auto* c = static_cast<IterCtx*>(cookie);
        c->total++;

        // Read real name + value via original (unhooked) function.
        // Use 256-byte name buffer to avoid truncating long property names
        // (MIUI has names like "persist.sys.miui.adj_update_foreground_state.enable.delayMs").
        static constexpr int NAME_BUF = 256;
        char buf[NAME_BUF + PROP_VALUE_MAX];
        memset(buf, 0, sizeof(buf));

        if (orig_system_property_read_callback) {
            orig_system_property_read_callback(pi,
                [](void* cb, const char* n, const char* v, uint32_t) {
                    char* b = static_cast<char*>(cb);
                    if (n) strncpy(b, n, 255);
                    if (v) strncpy(b + 256, v, PROP_VALUE_MAX - 1);
                }, buf);
        } else if (orig_system_property_read) {
            orig_system_property_read(pi, buf, buf + NAME_BUF);
        } else {
            return;
        }

        const char* name = buf;
        const char* real_val = buf + NAME_BUF;
        if (name[0] == '\0') return;

        // Get what our spoofing logic returns for this property
        char spoofed[PROP_VALUE_MAX] = {};
        my_system_property_get(name, spoofed);

        if (spoofed[0] != '\0' && strcmp(real_val, spoofed) != 0) {
            PatchEntry e;
            e.pi = pi;
            memset(e.spoofed, 0, sizeof(e.spoofed));
            strncpy(e.spoofed, spoofed, PROP_VALUE_MAX - 1);
            c->patches->push_back(e);
        }
    }, &ctx);

    LOGE("PR91: Phase 1 done — iterated %d properties, %zu need patching",
         ctx.total, patches.size());

    if (patches.empty()) {
        LOGE("PR91: no properties need shadow patching, done");
        g_pagesPatched = true;
        return;
    }

    // Phase 2: Remap affected pages using mremap for ATOMIC replacement.
    // The old approach (mmap MAP_FIXED then memcpy) has a brief window where the
    // page is zero-filled. If ANY concurrent thread (GC, JIT, Binder) reads a
    // property from that page during the window → SIGSEGV.
    // mremap(MREMAP_FIXED) atomically replaces the old mapping: readers see either
    // the old data or the new data, never zeros.  Same pattern used in hideLibraryMaps().
    const long ps = sysconf(_SC_PAGESIZE) > 0 ? sysconf(_SC_PAGESIZE) : 4096;
    std::set<uintptr_t> remapped;
    int patched = 0;
    int remap_ok = 0, remap_fail = 0;

    // PR143: Build a set of valid property page ranges from /proc/self/maps.
    // Only remap pages that belong to __properties__ mappings — never touch
    // library pages (libcutils, libc, etc.) even if a corrupted prop_info*
    // points there. Without this guard, a stale/wrong pointer causes
    // mremap+mprotect(PROT_READ) on a library data page → SEGV_ACCERR
    // when another thread (JIT, GC) writes to a global on that page.
    std::set<uintptr_t> propPages;
    {
        int maps_fd = (int)syscall(__NR_openat, AT_FDCWD, "/proc/self/maps",
                                   O_RDONLY | O_CLOEXEC);
        FILE* mfp = maps_fd >= 0 ? fdopen(maps_fd, "re") : nullptr;
        if (mfp) {
            char mline[512];
            while (fgets(mline, sizeof(mline), mfp)) {
                if (!strstr(mline, "__properties__")) continue;
                uintptr_t mstart = 0, mend = 0;
                if (sscanf(mline, "%lx-%lx", &mstart, &mend) == 2 && mstart < mend) {
                    for (uintptr_t pg = mstart & ~((uintptr_t)ps - 1);
                         pg < mend; pg += ps) {
                        propPages.insert(pg);
                    }
                }
            }
            fclose(mfp);
        } else {
            if (maps_fd >= 0) close(maps_fd);
        }
        LOGE("PR143: property page whitelist: %zu pages from __properties__ mappings",
             propPages.size());
    }

    LOGE("PR91: Phase 2 — remapping property pages (pagesize=%ld)", ps);

    for (const auto& e : patches) {
        uintptr_t pi_addr = reinterpret_cast<uintptr_t>(e.pi);
        uintptr_t val_start = pi_addr + sizeof(uint32_t);
        uintptr_t val_end = val_start + PROP_VALUE_MAX - 1;
        // PR91-fix: Start from pi_addr (not val_start) so the serial field's
        // page is also remapped — needed to update the value length in serial.
        uintptr_t page_lo = pi_addr & ~((uintptr_t)ps - 1);
        uintptr_t page_hi = val_end & ~((uintptr_t)ps - 1);

        for (uintptr_t pg = page_lo; pg <= page_hi; pg += ps) {
            if (remapped.count(pg)) continue;

            // PR143: Validate page belongs to __properties__ area.
            if (!propPages.empty() && !propPages.count(pg)) {
                LOGE("PR143: SKIPPING page %p — NOT in __properties__ area (prop_info=%p)",
                     (void*)pg, (void*)pi_addr);
                continue;
            }

            // Step 1: Allocate a temporary private page at any address
            void* tmp = mmap(nullptr, ps, PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            if (tmp == MAP_FAILED) {
                LOGE("PR91: temp mmap FAIL: %s", strerror(errno));
                remap_fail++;
                continue;
            }

            // Step 2: Copy current property page data into the temp page
            memcpy(tmp, reinterpret_cast<void*>(pg), ps);

            // Step 3: Atomically replace the shared property page with our copy
            void* result = mremap(tmp, ps, ps,
                                  MREMAP_MAYMOVE | MREMAP_FIXED,
                                  reinterpret_cast<void*>(pg));
            if (result != MAP_FAILED) {
                remapped.insert(pg);
                remap_ok++;
            } else {
                LOGE("PR91: mremap FAIL page=%p: %s", (void*)pg, strerror(errno));
                munmap(tmp, ps);
                remap_fail++;
            }
        }

        // PR91-fix: Update value length in serial field (Bionic uses serial >> 24).
        // Without this, direct memory readers see truncated values when
        // spoofed value is longer than original.
        uintptr_t pi_page = pi_addr & ~((uintptr_t)ps - 1);
        if (remapped.count(pi_page)) {
            uint32_t* serial_ptr = reinterpret_cast<uint32_t*>(pi_addr);
            uint32_t old_serial = *serial_ptr;
            uint32_t new_len = (uint32_t)strlen(e.spoofed);
            *serial_ptr = (new_len << 24) | (old_serial & 0x00FFFFFF);
        }

        // Write spoofed value at offset 4 (after atomic serial field)
        uintptr_t val_page = val_start & ~((uintptr_t)ps - 1);
        if (remapped.count(val_page)) {
            char* val_ptr = reinterpret_cast<char*>(val_start);
            memset(val_ptr, 0, PROP_VALUE_MAX);
            memcpy(val_ptr, e.spoofed, strlen(e.spoofed));
            patched++;
        }
    }

    LOGE("PR91: Phase 2 done — remapped %d pages ok, %d failed", remap_ok, remap_fail);

    // Phase 3: Restore read-only permissions (matches original mapping)
    for (uintptr_t pg : remapped) {
        mprotect(reinterpret_cast<void*>(pg), ps, PROT_READ);
    }

    LOGE("PR91: shadow-patched %d/%zu props across %zu pages",
         patched, patches.size(), remapped.size());
    g_pagesPatched = true;
}

// Fix9: Instalar PLT hooks para funciones donde Dobby inline hooks fallan.
// En aarch64 Bionic, execve/posix_spawn/posix_spawnp/__system_property_read son
// thin syscall wrappers demasiado pequeños para el trampoline de Dobby (~4-8 instr).
// PLT hooks modifican punteros GOT (siempre 8 bytes), no código de función.
//
// Fix9 changes vs Fix8:
// - Pre-resolve originals via dlsym (guaranteed correct, not overwritten by PLT hooks)
// - Use dummy vars for pltHookRegister oldFunc (prevents orig corruption by multi-ELF overwrite)
// - Filter own module from ELF list (see enumerateLoadedElfs)
static bool installPltHooks() {
    if (!g_api) return false;

    auto elfs = enumerateLoadedElfs();
    if (elfs.empty()) {
        LOGE("Fix9: No ELFs found in /proc/self/maps");
        return false;
    }

    // Fix9: Pre-resolve originals via dlsym — guaranteed correct function pointers.
    // PLT hooks across 100+ ELFs can corrupt orig_* with lazy-binding stubs.
    // dlsym(RTLD_DEFAULT) always returns the real libc address.
    if (!orig_execve)
        orig_execve = reinterpret_cast<decltype(orig_execve)>(dlsym(RTLD_DEFAULT, "execve"));
    if (!orig_posix_spawn)
        orig_posix_spawn = reinterpret_cast<decltype(orig_posix_spawn)>(dlsym(RTLD_DEFAULT, "posix_spawn"));
    if (!orig_posix_spawnp)
        orig_posix_spawnp = reinterpret_cast<decltype(orig_posix_spawnp)>(dlsym(RTLD_DEFAULT, "posix_spawnp"));
    if (!orig_system_property_read)
        orig_system_property_read = reinterpret_cast<decltype(orig_system_property_read)>(dlsym(RTLD_DEFAULT, "__system_property_read"));
    // PR84: Pre-resolve __system_property_find for PLT hook fallback
    if (!orig_system_property_find)
        orig_system_property_find = reinterpret_cast<decltype(orig_system_property_find)>(dlsym(RTLD_DEFAULT, "__system_property_find"));
    // PR88: Pre-resolve __system_property_foreach
    if (!orig_system_property_foreach)
        orig_system_property_foreach = reinterpret_cast<decltype(orig_system_property_foreach)>(dlsym(RTLD_DEFAULT, "__system_property_foreach"));

    LOGE("Fix9: Pre-resolved: execve=%p spawn=%p spawnp=%p prop_read=%p prop_find=%p prop_foreach=%p",
         orig_execve, orig_posix_spawn, orig_posix_spawnp, orig_system_property_read,
         orig_system_property_find, orig_system_property_foreach);

    // Dummy vars for pltHookRegister oldFunc — we don't use these values.
    // The real originals were already set above via dlsym.
    void *d1 = nullptr, *d2 = nullptr, *d3 = nullptr, *d4 = nullptr, *d5 = nullptr, *d6 = nullptr,
         *d7 = nullptr, *d8 = nullptr;

    // NOTE: glGetString is NOT PLT-hooked here. installPltHooks() runs before
    // the GPU Dobby hooks that set orig_glGetString. Registering a PLT hook
    // for glGetString would cause my_glGetString to run with orig_glGetString=NULL
    // → returns nullptr → crash in any app that calls glGetString during startup
    // (e.g. VdInfo during GL detection, WebView during WebGL init).
    // The Dobby hook (installed later after dlopen ensures the lib is loaded)
    // is the correct approach for glGetString.

    LOGE("Fix11: Registering PLT hooks across %zu ELFs (spawnInline=%d)", elfs.size(), g_spawnInlineHooked);
    for (const auto& elf : elfs) {
        g_api->pltHookRegister(elf.dev, elf.inode, "execve",
                               (void*)my_execve, &d1);
        // PR90: Only PLT-hook posix_spawn/posix_spawnp if Dobby inline hooks failed.
        // When both are active, PLT calls my_posix_spawn → orig (= libc start addr,
        // patched by Dobby) → my_posix_spawn → infinite recursion → crash.
        if (!g_spawnInlineHooked) {
            g_api->pltHookRegister(elf.dev, elf.inode, "posix_spawn",
                                   (void*)my_posix_spawn, &d2);
            g_api->pltHookRegister(elf.dev, elf.inode, "posix_spawnp",
                                   (void*)my_posix_spawnp, &d3);
        }
        g_api->pltHookRegister(elf.dev, elf.inode, "__system_property_read",
                               (void*)my_system_property_read, &d4);
        // PR84/PR86: PLT hook for __system_property_find — only if Dobby inline hook
        // was not installed. Having both Dobby inline + PLT hook on the same function
        // causes double-hook recursion (same as Fix10 crash: PLT calls my_* → my_*
        // calls orig_* → orig_* was Dobby-patched → back to my_* → infinite loop).
        if (!g_propFindInlineHooked) {
            g_api->pltHookRegister(elf.dev, elf.inode, "__system_property_find",
                                   (void*)my_system_property_find, &d5);
        }
        // PR88: PLT hook for __system_property_foreach — only if Dobby inline hook
        // was not installed (same double-hook recursion guard as __system_property_find).
        if (!g_propForeachInlineHooked) {
            g_api->pltHookRegister(elf.dev, elf.inode, "__system_property_foreach",
                                   (void*)my_system_property_foreach, &d6);
        }
        // PR138: PLT hooks for __system_property_get and __system_property_read_callback.
        // These are NOT Dobby-hooked (PIF conflict), so PLT hooks are always safe here.
        g_api->pltHookRegister(elf.dev, elf.inode, "__system_property_get",
                               (void*)my_system_property_get, &d7);
        g_api->pltHookRegister(elf.dev, elf.inode, "__system_property_read_callback",
                               (void*)my_system_property_read_callback, &d8);
    }

    bool ok = g_api->pltHookCommit();
    LOGE("Fix9: pltHookCommit()=%s across %zu ELFs, orig_execve=%p orig_spawn=%p",
         ok ? "true" : "false", elfs.size(), orig_execve, orig_posix_spawn);
    return ok;
}

// PR87: Re-apply PLT hooks after a new library is loaded via dlopen/android_dlopen_ext.
// enumerateLoadedElfs() re-scans /proc/self/maps to find ALL loaded .so files, including
// the one just loaded. For already-hooked ELFs, pltHookCommit() is idempotent
// (xhook_refresh skips entries whose GOT already points to our hook function).
// For the newly loaded ELF, it patches the GOT entries for property functions.
// PR89: Also re-applies execve/posix_spawn/posix_spawnp hooks. Without this,
// late-loaded native libraries (e.g. VDInfo's .so via System.loadLibrary) retain
// real libc addresses in their GOT for spawn functions, allowing them to launch
// unintercepted getprop subprocesses that return real property values.
static void reapplyPltHooksForNewLibraries() {
    if (!g_api) return;

    auto elfs = enumerateLoadedElfs();
    if (elfs.empty()) return;

    void *d1 = nullptr, *d2 = nullptr, *d3 = nullptr,
         *d4 = nullptr, *d5 = nullptr, *d6 = nullptr,
         *d7 = nullptr, *d8 = nullptr;

    for (const auto& elf : elfs) {
        g_api->pltHookRegister(elf.dev, elf.inode, "__system_property_read",
                               (void*)my_system_property_read, &d1);
        if (!g_propFindInlineHooked) {
            g_api->pltHookRegister(elf.dev, elf.inode, "__system_property_find",
                                   (void*)my_system_property_find, &d2);
        }
        if (!g_propForeachInlineHooked) {
            g_api->pltHookRegister(elf.dev, elf.inode, "__system_property_foreach",
                                   (void*)my_system_property_foreach, &d3);
        }
        // PR89: Re-hook subprocess spawn functions for late-loaded libraries.
        // PR90: posix_spawn/posix_spawnp only via PLT if Dobby inline failed.
        g_api->pltHookRegister(elf.dev, elf.inode, "execve",
                               (void*)my_execve, &d4);
        if (!g_spawnInlineHooked) {
            g_api->pltHookRegister(elf.dev, elf.inode, "posix_spawn",
                                   (void*)my_posix_spawn, &d5);
            g_api->pltHookRegister(elf.dev, elf.inode, "posix_spawnp",
                                   (void*)my_posix_spawnp, &d6);
        }
        // PR138: PLT hooks for __system_property_get and __system_property_read_callback.
        g_api->pltHookRegister(elf.dev, elf.inode, "__system_property_get",
                               (void*)my_system_property_get, &d7);
        g_api->pltHookRegister(elf.dev, elf.inode, "__system_property_read_callback",
                               (void*)my_system_property_read_callback, &d8);
    }

    bool ok = g_api->pltHookCommit();
    LOGE("PR89: reapplyPltHooks (spawn+props) across %zu ELFs, commit=%s",
         elfs.size(), ok ? "true" : "false");
}

// PR138: Forward declaration — defined after orig_* Binder variables.
static void logBinderDiag();

// PR87: Intercept android_dlopen_ext — called by System.loadLibrary() via JNI.
// After the linker loads the new .so and resolves its GOT with real libc addresses,
// we re-apply PLT hooks so the new library's calls go through our spoofing functions.
// Recursion guard: loading a .so can trigger loading dependencies (nested dlopen).
// Mutex: dlopen can be called from any thread; pltHookRegister/Commit may not be thread-safe.
static void* my_android_dlopen_ext(const char* filename, int flags, const void* extinfo) {
    if (g_dlopenReapplyDepth > 0 || !orig_android_dlopen_ext) {
        return orig_android_dlopen_ext ? orig_android_dlopen_ext(filename, flags, extinfo) : nullptr;
    }
    g_dlopenReapplyDepth++;

    void* handle = orig_android_dlopen_ext(filename, flags, extinfo);

    if (handle) {
        std::lock_guard<std::mutex> lock(g_reapplyMutex);
        reapplyPltHooksForNewLibraries();
        LOGE("PR87: Re-applied PLT hooks after android_dlopen_ext(%s)", filename ? filename : "(null)");
        logBinderDiag();
    }

    g_dlopenReapplyDepth--;
    return handle;
}

// PR87: Intercept dlopen — called by native code loading libraries directly.
// Same logic as my_android_dlopen_ext but for the simpler dlopen(filename, flags) API.
static void* my_dlopen_hook(const char* filename, int flags) {
    if (g_dlopenReapplyDepth > 0 || !orig_dlopen_hook) {
        return orig_dlopen_hook ? orig_dlopen_hook(filename, flags) : nullptr;
    }
    g_dlopenReapplyDepth++;

    void* handle = orig_dlopen_hook(filename, flags);

    if (handle) {
        std::lock_guard<std::mutex> lock(g_reapplyMutex);
        reapplyPltHooksForNewLibraries();
        LOGE("PR87: Re-applied PLT hooks after dlopen(%s)", filename ? filename : "(null)");
        logBinderDiag();
    }

    g_dlopenReapplyDepth--;
    return handle;
}

// ─────────────────────────────────────────────────────────────────────────────
// PR105: Binder Location Hook — IPCThreadState::transact (Android 11, arm64)
//
// Arquitectura:
//   - Hook en llamadas SALIENTES (IPCThreadState::transact)
//   - Detecta calls a ILocationManager.getLastLocation (code=1) y
//     getCurrentLocation (code=2) leyendo el interface token en el
//     Parcel DATA (offset 12, UTF-16LE "andr")
//   - Muta las coordenadas en el Parcel REPLY DESPUÉS de que
//     orig_ipc_transact retorna, usando offsets dinámicos calculados
//     a partir de la longitud real del string mProvider
//
// Parcel DATA layout (writeInterfaceToken, Android 11):
//   offset  0: StrictMode policy mask (int32)
//   offset  4: workSource header (int32)
//   offset  8: string length = 33 (int32)
//   offset 12: "android.location.ILocationManager" UTF-16LE  ← token aquí
//
// Parcel REPLY layout (writeNoException + Location.writeToParcel, Android 11):
//   offset  0: StrictMode mask (int32)       ← writeNoException
//   offset  4: exception code = 0 (int32)   ← writeNoException
//   offset  8: non-null marker (int32)       ← 1=Location, 0=null
//   offset 12: mProvider length (int32)      ← Location.writeToParcel
//   offset 16: mProvider chars (UTF-16LE, variable)
//   offset 16+pad: mTime(8) + mElapsedRtNs(8) + mElapsedRtUncNs(8) + mFieldsMask(4) = 28 bytes
//   offset 16+pad+28: mLatitude (double)
//   offset 16+pad+36: mLongitude (double)
//
// Verificado contra AOSP android-11.0.0_r46:
//   frameworks/native/libs/binder/Parcel.cpp
//   frameworks/base/location/java/android/location/Location.java
//   frameworks/base/location/java/android/location/ILocationManager.aidl
// ─────────────────────────────────────────────────────────────────────────────

// Acceso al Parcel via offsets del struct (arm64, Android 11).
// Layout: mError(int32 @0x00) + pad(4) + mData(ptr @0x08) + mDataSize(size_t @0x10)
// Parcel NO tiene vtable, pero mData NO es el primer campo — mError lo es.
static inline const uint8_t* parcel_data(const void* p) {
    return *reinterpret_cast<const uint8_t* const*>(
        static_cast<const uint8_t*>(p) + 0x08);
}
static inline size_t parcel_dataSize(const void* p) {
    return *reinterpret_cast<const size_t*>(
        static_cast<const uint8_t*>(p) + 0x10);
}
// Escritura in-place en el buffer mData del reply (buffer privado del caller,
// PROT_READ|PROT_WRITE — distinto del buffer PROT_READ de BBinder::transact).
static inline void parcel_write_double_at(void* p, size_t offset, double val) {
    uint8_t* data = *reinterpret_cast<uint8_t**>(
        static_cast<uint8_t*>(p) + 0x08);
    memcpy(data + offset, &val, sizeof(double));
}

// Transaction codes en ILocationManager.aidl Android 11 (FIRST_CALL_TRANSACTION=1):
//   1 = getLastLocation
//   2 = getCurrentLocation
// Verificado: método #1 en el .aidl según orden de aparición.
static constexpr uint32_t TRANSACTION_getLastLocation    = 1;
static constexpr uint32_t TRANSACTION_getCurrentLocation = 2;

// Token "andr" en UTF-16LE (primeros 4 chars de "android.location.ILocationManager")
// Posición: offset 12 en el DATA Parcel (después de StrictMode+workSource+strLen).
static constexpr uint8_t  LOC_TOKEN_PREFIX[8] = {
    0x61,0x00, 0x6E,0x00, 0x64,0x00, 0x72,0x00  // 'a','n','d','r'
};
static constexpr size_t LOC_TOKEN_OFFSET   = 12; // inicio del texto UTF-16
static constexpr size_t LOC_TOKEN_MIN_SIZE = 80; // 4+4+4+(33*2+2) = 80 bytes exactos

typedef int32_t (*ipc_transact_fn)(void*, int32_t, uint32_t,
                                   const void*, void*, uint32_t);
static ipc_transact_fn orig_ipc_transact = nullptr;

static bool isLocationRequest(const void* data_parcel, uint32_t code) {
    if (!data_parcel) return false;

    const uint8_t* raw = parcel_data(data_parcel);
    size_t sz = parcel_dataSize(data_parcel);
    if (!raw || sz < LOC_TOKEN_MIN_SIZE) return false;

    // PR119b: evitar dependencia fuerte de transaction codes.
    // En algunos builds GMS/ROM cambian code para ILocationManager.
    int32_t strLen = 0;
    memcpy(&strLen, raw + 8, 4);
    if (strLen <= 0 || strLen > 256) return false;

    size_t utf16Bytes = (size_t)strLen * 2 + 2;
    size_t padded = (utf16Bytes + 3) & ~3u;
    if (12 + padded > sz) return false;

    std::string token;
    token.reserve((size_t)strLen);
    for (int32_t i = 0; i < strLen; ++i) {
        uint16_t ch = 0;
        memcpy(&ch, raw + 12 + (size_t)i * 2, 2);
        char c = (ch <= 0x7F) ? (char)ch : '?';
        token.push_back((char)std::tolower((unsigned char)c));
    }

    // Fast path antiguo (codes esperados + prefijo "andr").
    bool codeLikely = (code == TRANSACTION_getLastLocation ||
                       code == TRANSACTION_getCurrentLocation);
    bool prefixLikely = memcmp(raw + LOC_TOKEN_OFFSET,
                               LOC_TOKEN_PREFIX, sizeof(LOC_TOKEN_PREFIX)) == 0;

    if (codeLikely && prefixLikely) return true;

    // Fallback robusto por descriptor Binder.
    return token.find("android.location.ilocationmanager") != std::string::npos ||
           token.find("google.android.gms.location") != std::string::npos;
}

// PR117: Hook para la lectura de doubles en el Parcel
using fn_Parcel_readDouble = double(*)(const void*);
static fn_Parcel_readDouble orig_Parcel_readDouble = nullptr;

static double my_Parcel_readDouble(const void* self) {
    double val = orig_Parcel_readDouble(self);

    // Solo falseamos si el valor parece una coordenada real y estamos en el scope
    int64_t latBits = g_cachedLatBits.load(std::memory_order_acquire);
    if (latBits != 0 && val != 0.0) {
        double fakeLat, fakeLon;
        int64_t lonBits = g_cachedLonBits.load(std::memory_order_acquire);
        memcpy(&fakeLat, &latBits, 8);
        memcpy(&fakeLon, &lonBits, 8);

        // Si el valor leído coincide con un rango plausible de tu zona, lo cambiamos
        // (Esto es un 'envenenamiento por proximidad')
        if (std::abs(val - fakeLat) > 0.0001 || std::abs(val - fakeLon) > 0.0001) {
             // Aquí podrías añadir lógica de filtrado extra, pero por ahora
             // probemos el pulso del log.
             LOGD("[PR117-PULSE] readDouble intercepted: %.6f", val);
        }
    }
    return val;
}

// ─────────────────────────────────────────────────────────────────────────────
// PR105b: Cálculo dinámico de campos opcionales de Location.writeToParcel()
//
// Location.writeToParcel() escribe campos opcionales entre mFieldsMask y
// mLatitude. Su presencia depende de los bits de mFieldsMask:
//   bit 0 = HAS_ALTITUDE          → double (8 bytes)
//   bit 1 = HAS_SPEED             → float  (4 bytes)
//   bit 2 = HAS_BEARING           → float  (4 bytes)
//   bit 3 = HAS_ACCURACY          → float  (4 bytes) ← siempre en GPS/network
//   bit 4 = HAS_VERTICAL_ACCURACY → float  (4 bytes)
//   bit 5 = HAS_SPEED_ACCURACY    → float  (4 bytes)
//   bit 6 = HAS_BEARING_ACCURACY  → float  (4 bytes)
// Verificado contra AOSP android-11.0.0_r46 Location.java
// ─────────────────────────────────────────────────────────────────────────────
// PR145: Muta lat/lon en un buffer de Location.writeToParcel serializado.
// base_offset = inicio del campo providerLen dentro del buffer.
// Soporta dos layouts verificados de AOSP:
//
//  Android 11 (API 30) — todos los campos incondicionales:
//    provider → mTime(8) → mElapsedRT(8) → mElapsedRTUnc(8) → mask(4) → lat(8) → lon(8) → ...
//    latOffset = base + providerBytes + 28
//
//  Android 12 (API 31+) — campos condicionales, orden diferente:
//    provider → mask(4) → mTime(8) → mElapsedRT(8) → [mElapsedRTUnc(8) si bit8] → lat(8) → lon(8) → ...
//    latOffset = base + providerBytes + 20 + (bit8 ? 8 : 0)
//
// Valida lat/lon antes de escribir para evitar corromper parcels que no son Location.

// ─────────────────────────────────────────────────────────────────────────────
// PR147: Write to PROT_READ memory via /proc/self/mem.
//
// Reply Parcel data lives in the Binder mmap region (PROT_READ only).
// The kernel marks this VMA with ~VM_MAYWRITE, so mprotect(PROT_WRITE) fails.
// Shadow buffer pointer swap breaks Parcel lifecycle: freeBuffer() sends
// BC_FREE_BUFFER to kernel with the swapped heap pointer → SIGABRT or leak.
//
// /proc/self/mem uses access_process_vm() with FOLL_FORCE, which ignores
// page protection. This lets us write in-place without pointer swaps.
// ─────────────────────────────────────────────────────────────────────────────
static bool writeToReadOnly(void* dst, const void* src, size_t len) {
    int fd = open("/proc/self/mem", O_RDWR);
    if (fd < 0) {
        LOGE("[PR148] /proc/self/mem open failed: errno=%d", errno);
        return false;
    }
    ssize_t written = pwrite(fd, src, len, (off_t)(uintptr_t)dst);
    int saved_errno = errno;
    close(fd);
    if (written != (ssize_t)len) {
        LOGE("[PR148] /proc/self/mem pwrite failed: dst=%p len=%zu written=%zd errno=%d",
             dst, len, written, saved_errno);
        return false;
    }
    return true;
}

static bool mutateLocationInBuffer(uint8_t* buf, size_t sz,
                                   size_t base_offset, double lat, double lon) {
    if (base_offset + 4 > sz) return false;
    int32_t providerLen = 0;
    memcpy(&providerLen, buf + base_offset, 4);
    if (providerLen < 0 || providerLen > 64) return false;

    size_t pBytes = (4 + (size_t)providerLen * 2 + 3) & ~3u;

    // Android 11: provider → mTime(8) → mElapsedRT(8) → mElapsedRTUnc(8) → mask(4) → lat → lon
    // lat at base + pBytes + 24 + 4 = base + pBytes + 28
    {
        size_t latOff = base_offset + pBytes + 28;
        size_t lonOff = latOff + 8;
        if (lonOff + 8 <= sz) {
            double cLat, cLon;
            memcpy(&cLat, buf + latOff, 8);
            memcpy(&cLon, buf + lonOff, 8);
            if (cLat >= -90.0 && cLat <= 90.0 && cLon >= -180.0 && cLon <= 180.0
                && !(cLat == 0.0 && cLon == 0.0)) {
                memcpy(buf + latOff, &lat, 8);
                memcpy(buf + lonOff, &lon, 8);
                return true;
            }
        }
    }

    // Android 12+: provider → mask(4) → mTime(8) → mElapsedRT(8) → [mElapsedRTUnc(8)] → lat → lon
    {
        size_t maskOff = base_offset + pBytes;
        if (maskOff + 4 <= sz) {
            int32_t mask = 0;
            memcpy(&mask, buf + maskOff, 4);
            size_t ertBytes = (mask & 0x100) ? 8 : 0;
            size_t latOff = maskOff + 20 + ertBytes;  // mask(4) + mTime(8) + mElapsedRT(8) + [ertUnc]
            size_t lonOff = latOff + 8;
            if (lonOff + 8 <= sz) {
                double cLat, cLon;
                memcpy(&cLat, buf + latOff, 8);
                memcpy(&cLon, buf + lonOff, 8);
                if (cLat >= -90.0 && cLat <= 90.0 && cLon >= -180.0 && cLon <= 180.0
                    && !(cLat == 0.0 && cLon == 0.0)) {
                    memcpy(buf + latOff, &lat, 8);
                    memcpy(buf + lonOff, &lon, 8);
                    return true;
                }
            }
        }
    }

    return false;
}

// ─────────────────────────────────────────────────────────────────────────────
// PR144: Provider-validated Location scanner for obfuscated GMS interfaces.
//
// GMS FusedLocationProviderClient uses ProGuard-obfuscated Binder interface
// names (e.g. com.google.android.gms.internal.location.zzaz) that don't match
// any token pattern. This scanner finds serialized Location objects by matching
// the provider string ("gps"/"fused"/"network"/"passive") in UTF-16LE, which
// is dramatically more selective than lat/lon range checks alone.
// ─────────────────────────────────────────────────────────────────────────────

// PR144: Known Location.provider strings in UTF-16LE wire format.
// "gps" (3 chars) → 6 bytes
static constexpr uint8_t PROVIDER_GPS_UTF16[] = {
    0x67,0x00, 0x70,0x00, 0x73,0x00
};
// "fused" (5 chars) → 10 bytes
static constexpr uint8_t PROVIDER_FUSED_UTF16[] = {
    0x66,0x00, 0x75,0x00, 0x73,0x00, 0x65,0x00, 0x64,0x00
};
// "network" (7 chars) → 14 bytes
static constexpr uint8_t PROVIDER_NETWORK_UTF16[] = {
    0x6E,0x00, 0x65,0x00, 0x74,0x00, 0x77,0x00,
    0x6F,0x00, 0x72,0x00, 0x6B,0x00
};
// "passive" (7 chars) → 14 bytes
static constexpr uint8_t PROVIDER_PASSIVE_UTF16[] = {
    0x70,0x00, 0x61,0x00, 0x73,0x00, 0x73,0x00,
    0x69,0x00, 0x76,0x00, 0x65,0x00
};

// PR144: Check if buffer at offset contains a known Location provider string.
// The int32 at buf[off] must be 3/5/7, followed by exact UTF-16LE match.
static bool matchesKnownProvider(const uint8_t* buf, size_t sz, size_t off) {
    if (off + 4 > sz) return false;
    int32_t pLen = 0;
    memcpy(&pLen, buf + off, 4);

    const uint8_t* expected = nullptr;
    size_t expectedBytes = 0;

    switch (pLen) {
        case 3:
            expected = PROVIDER_GPS_UTF16;
            expectedBytes = sizeof(PROVIDER_GPS_UTF16);
            break;
        case 5:
            expected = PROVIDER_FUSED_UTF16;
            expectedBytes = sizeof(PROVIDER_FUSED_UTF16);
            break;
        case 7:
            // "network" or "passive"
            if (off + 4 + sizeof(PROVIDER_NETWORK_UTF16) > sz) return false;
            if (memcmp(buf + off + 4, PROVIDER_NETWORK_UTF16, sizeof(PROVIDER_NETWORK_UTF16)) == 0)
                return true;
            expected = PROVIDER_PASSIVE_UTF16;
            expectedBytes = sizeof(PROVIDER_PASSIVE_UTF16);
            break;
        default:
            return false;
    }

    if (off + 4 + expectedBytes > sz) return false;
    return memcmp(buf + off + 4, expected, expectedBytes) == 0;
}

// PR145: Layout-agnostic provider-validated Location scanner.
// Finds provider string ("gps"/"fused"/"network"/"passive") in UTF-16LE,
// then scans forward 16-128 bytes for two consecutive doubles in lat/lon range.
// Works on any Android version without assuming specific field order.
static bool probeProviderValidatedLocation(uint8_t* buf, size_t sz,
                                            double lat, double lon,
                                            size_t& outBaseOffset) {
    outBaseOffset = 0;
    if (!buf || sz < 64) return false;

    for (size_t base = 0; base + 48 < sz; base += 4) {
        if (!matchesKnownProvider(buf, sz, base)) continue;

        int32_t providerLen = 0;
        memcpy(&providerLen, buf + base, 4);
        size_t pBytes = (4 + (size_t)providerLen * 2 + 3) & ~3u;

        // Scan forward from provider end for lat/lon doubles.
        // Min offset: 16 bytes (mask + some time fields).
        // Max offset: 128 bytes (no Location has more between provider and lat).
        size_t searchStart = base + pBytes + 16;
        size_t searchEnd   = base + pBytes + 128;
        if (searchEnd > sz) searchEnd = sz;

        for (size_t off = searchStart; off + 16 <= searchEnd; off += 4) {
            double curLat = 0.0, curLon = 0.0;
            memcpy(&curLat, buf + off, 8);
            memcpy(&curLon, buf + off + 8, 8);

            if (curLat < -90.0 || curLat > 90.0) continue;
            if (curLon < -180.0 || curLon > 180.0) continue;
            if (curLat == 0.0 && curLon == 0.0) continue;

            // Provider string + lat/lon range match — mutate
            memcpy(buf + off, &lat, 8);
            memcpy(buf + off + 8, &lon, 8);
            outBaseOffset = base;
            return true;
        }
    }
    return false;
}

// PR144: Quick read-only check if buffer might contain a Location provider string.
// Used to avoid allocating a shadow buffer for parcels that definitely don't contain
// a Location object. Returns true if any 4-byte-aligned offset has providerLen 3/5/7
// with matching UTF-16LE provider bytes.
static bool hasProviderSignature(const uint8_t* buf, size_t sz) {
    if (!buf || sz < 64) return false;
    for (size_t base = 0; base + 48 < sz; base += 4) {
        if (matchesKnownProvider(buf, sz, base)) return true;
    }
    return false;
}

// PR145: Parcel visualizer for diagnostic logging.
// Dumps UTF-16LE strings and coordinate-like doubles found in parcels
// that have a provider signature. Limited to first 20 dumps.
static void visualizeLocationParcel(const uint8_t* buf, size_t sz,
                                     const char* tag, uint32_t code) {
    static std::atomic<int> s_pvCount{0};
    int n = s_pvCount.fetch_add(1, std::memory_order_relaxed);
    if (n >= 20) return;

    LOGE("[PV#%d] %s code=%u sz=%zu", n, tag, code, sz);

    // Scan for UTF-16LE strings (low byte printable ASCII, high byte 0x00)
    for (size_t i = 0; i + 8 < sz; i += 2) {
        if (buf[i] >= 0x20 && buf[i] <= 0x7E && buf[i+1] == 0x00) {
            char tmp[64];
            int len = 0;
            for (size_t j = 0; j < 30 && i + j*2 + 1 < sz; ++j) {
                if (buf[i+j*2+1] != 0x00 || buf[i+j*2] < 0x20) break;
                tmp[len++] = (char)buf[i+j*2];
            }
            tmp[len] = 0;
            if (len > 2) {
                LOGE("[PV#%d] str@%zu = '%s'", n, i, tmp);
                i += (size_t)len * 2 - 2;
            }
        }
    }

    // Scan for coordinate-like doubles (two consecutive in lat/lon range)
    for (size_t i = 0; i + 16 <= sz; i += 4) {
        double v1, v2;
        memcpy(&v1, buf + i, 8);
        memcpy(&v2, buf + i + 8, 8);
        if (v1 >= -90.0 && v1 <= 90.0 && v2 >= -180.0 && v2 <= 180.0
            && !(v1 == 0.0 && v2 == 0.0)
            && v1 != 1.0 && v2 != 1.0 && v1 != -1.0 && v2 != -1.0) {
            LOGE("[PV#%d] coord@%zu lat=%.6f lon=%.6f", n, i, v1, v2);
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// PR119: Radar Doppler de payload Location.
// No depende de transaction code fijo. Inspecciona el descriptor Binder
// y, si contiene "location", intenta mutar lat/lon con offsets conocidos
// y finalmente con escaneo heurístico del payload.
static bool parseBinderInterfaceToken(const uint8_t* raw, size_t sz,
                                      std::string& outToken,
                                      size_t& outHeaderBytes) {
    outToken.clear();
    outHeaderBytes = 0;
    if (!raw || sz < 16) return false;

    int32_t strLen = 0;
    memcpy(&strLen, raw + 8, 4);
    if (strLen <= 0 || strLen > 256) return false;

    size_t utf16Bytes = (size_t)strLen * 2 + 2;  // incluye NUL UTF-16
    size_t padded = (utf16Bytes + 3) & ~3u;
    size_t hdr = 12 + padded;
    if (hdr > sz) return false;

    outToken.reserve((size_t)strLen);
    for (int32_t i = 0; i < strLen; ++i) {
        uint16_t ch = 0;
        memcpy(&ch, raw + 12 + (size_t)i * 2, 2);
        char c = (ch <= 0x7F) ? (char)ch : '?';
        outToken.push_back((char)std::tolower((unsigned char)c));
    }
    outHeaderBytes = hdr;
    return true;
}

static bool mutateLocationByDopplerScan(uint8_t* buf, size_t sz,
                                        double lat, double lon,
                                        size_t& outBaseOffset) {
    outBaseOffset = 0;
    if (!buf || sz < 64) return false;

    // Escaneo 4-byte aligned para mantener coste bajo y respetar alineación Parcel.
    for (size_t base = 0; base + 48 < sz; base += 4) {
        if (mutateLocationInBuffer(buf, sz, base, lat, lon)) {
            outBaseOffset = base;
            return true;
        }
    }
    return false;
}
// ─────────────────────────────────────────────────────────────────────────────

// 3. Adaptador para respuestas síncronas (IPCThreadState::transact)
// PR137: Added Doppler scan fallback for GMS FusedLocation replies
// (SafeParcelable-wrapped Location objects with different header layout).
// PR146: Reply Parcel data is in Binder mmap (PROT_READ only).
// Must use shadow buffer to mutate — writing directly causes SEGV_ACCERR.
static void mutateLocationReply(void* reply) {
    if (!reply) return;
    size_t sz = parcel_dataSize(reply);
    if (sz < 16) return;

    int64_t latBits = g_cachedLatBits.load(std::memory_order_acquire);
    int64_t lonBits = g_cachedLonBits.load(std::memory_order_acquire);
    if (latBits == 0 && lonBits == 0) return;

    double lat, lon;
    memcpy(&lat, &latBits, 8);
    memcpy(&lon, &lonBits, 8);

    // Get raw mData pointer (read-only Binder mmap)
    uint8_t* raw = *reinterpret_cast<uint8_t**>(
        static_cast<uint8_t*>(reply) + 0x08);
    if (!raw) return;

    // PR147: Scan on a read-only copy to find lat/lon offsets,
    // then write spoofed values via /proc/self/mem (bypasses PROT_READ).
    // No pointer swap → no freeBuffer/BC_FREE_BUFFER conflict.
    uint8_t* scan = new uint8_t[sz];
    memcpy(scan, raw, sz);

    // findLocationOffsets: scan the copy to find where lat/lon are,
    // then write to original via /proc/self/mem.
    bool mutated = false;

    // 1) Try AOSP ILocationManager fixed-offset format (base=12)
    if (mutateLocationInBuffer(scan, sz, 12, lat, lon)) {
        // mutateLocationInBuffer found and wrote lat/lon in the copy at some offsets.
        // We need to find those offsets. Since we know the function writes lat then lon
        // at specific locations, scan for our spoofed values in the copy.
        for (size_t off = 0; off + 16 <= sz; off += 4) {
            double v1, v2;
            memcpy(&v1, scan + off, 8);
            memcpy(&v2, scan + off + 8, 8);
            if (v1 == lat && v2 == lon) {
                if (writeToReadOnly(raw + off, &lat, 8) &&
                    writeToReadOnly(raw + off + 8, &lon, 8)) {
                    LOGD("[PR147] Location reply mutated (AOSP@12→%zu): lat=%.6f lon=%.6f",
                         off, lat, lon);
                    mutated = true;
                } else {
                    LOGE("[PR147] /proc/self/mem write FAILED for reply (AOSP@12)");
                }
                break;
            }
        }
    }

    // 2) Doppler scan fallback
    if (!mutated) {
        // Reset scan copy
        memcpy(scan, raw, sz);
        size_t dopplerOffset = 0;
        if (mutateLocationByDopplerScan(scan, sz, lat, lon, dopplerOffset)) {
            for (size_t off = 0; off + 16 <= sz; off += 4) {
                double v1, v2;
                memcpy(&v1, scan + off, 8);
                memcpy(&v2, scan + off + 8, 8);
                if (v1 == lat && v2 == lon) {
                    if (writeToReadOnly(raw + off, &lat, 8) &&
                        writeToReadOnly(raw + off + 8, &lon, 8)) {
                        LOGD("[PR147] Location reply mutated (Doppler@%zu→%zu): lat=%.6f lon=%.6f",
                             dopplerOffset, off, lat, lon);
                        mutated = true;
                    } else {
                        LOGE("[PR147] /proc/self/mem write FAILED for reply (Doppler)");
                    }
                    break;
                }
            }
        }
    }

    // 3) Provider-validated scan
    if (!mutated) {
        memcpy(scan, raw, sz);
        size_t probeOffset = 0;
        if (probeProviderValidatedLocation(scan, sz, lat, lon, probeOffset)) {
            for (size_t off = 0; off + 16 <= sz; off += 4) {
                double v1, v2;
                memcpy(&v1, scan + off, 8);
                memcpy(&v2, scan + off + 8, 8);
                if (v1 == lat && v2 == lon) {
                    if (writeToReadOnly(raw + off, &lat, 8) &&
                        writeToReadOnly(raw + off + 8, &lon, 8)) {
                        LOGE("[PR147] Location reply mutated (probe@%zu→%zu): lat=%.6f lon=%.6f",
                             probeOffset, off, lat, lon);
                        mutated = true;
                    } else {
                        LOGE("[PR147] /proc/self/mem write FAILED for reply (probe)");
                    }
                    break;
                }
            }
        }
    }

    delete[] scan;
}

// ─────────────────────────────────────────────────────────────────────────────
// PR107: GMS Outgoing Hook
// Token: "com.google.android.gms.location.internal.ILocationListener" (58 chars)
// Header = 4+4+4+(58*2+2→padded120) = 132 bytes
// Location mínima = 56 bytes → MIN_SIZE = 188
// Prefix UTF-16LE "com." = {0x63,0x00,0x6F,0x00,0x6D,0x00,0x2E,0x00}
// ─────────────────────────────────────────────────────────────────────────────
static constexpr int32_t GMS_ILOCLISTENER_STR_LEN = 58;
static constexpr size_t  GMS_ILOCLISTENER_HDR      = 132;
static constexpr size_t  GMS_ILOCLISTENER_MIN_SIZE = 188;
static constexpr uint8_t GMS_TOKEN_PREFIX[8] = {
    0x63,0x00, 0x6F,0x00, 0x6D,0x00, 0x2E,0x00  // 'com.'
};
// PR139: Exact UTF-16LE descriptor for "com.google.android.gms.location.internal.ILocationListener"
// 58 chars × 2 = 116 bytes. Replaces GMS_TOKEN_PREFIX heuristic to eliminate false positives.
static constexpr uint8_t GMS_ILOCLISTENER_DESCRIPTOR[] = {
    0x63,0x00, 0x6F,0x00, 0x6D,0x00, 0x2E,0x00,  // com.
    0x67,0x00, 0x6F,0x00, 0x6F,0x00, 0x67,0x00,  // goog
    0x6C,0x00, 0x65,0x00, 0x2E,0x00, 0x61,0x00,  // le.a
    0x6E,0x00, 0x64,0x00, 0x72,0x00, 0x6F,0x00,  // ndro
    0x69,0x00, 0x64,0x00, 0x2E,0x00, 0x67,0x00,  // id.g
    0x6D,0x00, 0x73,0x00, 0x2E,0x00, 0x6C,0x00,  // ms.l
    0x6F,0x00, 0x63,0x00, 0x61,0x00, 0x74,0x00,  // ocat
    0x69,0x00, 0x6F,0x00, 0x6E,0x00, 0x2E,0x00,  // ion.
    0x69,0x00, 0x6E,0x00, 0x74,0x00, 0x65,0x00,  // inte
    0x72,0x00, 0x6E,0x00, 0x61,0x00, 0x6C,0x00,  // rnal
    0x2E,0x00, 0x49,0x00, 0x4C,0x00, 0x6F,0x00,  // .ILo
    0x63,0x00, 0x61,0x00, 0x74,0x00, 0x69,0x00,  // cati
    0x6F,0x00, 0x6E,0x00, 0x4C,0x00, 0x69,0x00,  // onLi
    0x73,0x00, 0x74,0x00, 0x65,0x00, 0x6E,0x00,  // sten
    0x65,0x00, 0x72,0x00                           // er
};

static bool isGmsLocationOutgoing(const void* data_parcel, uint32_t code) {
    // PR109-DBG: filtro de código eliminado para diagnóstico.
    // strLen==58 + prefijo "com." son suficientemente específicos.
    if (!data_parcel) return false;
    size_t sz = parcel_dataSize(data_parcel);
    if (sz < GMS_ILOCLISTENER_MIN_SIZE) return false;
    const uint8_t* raw = parcel_data(data_parcel);
    if (!raw) return false;
    int32_t strLen = 0;
    memcpy(&strLen, raw + 8, 4);
    if (strLen != GMS_ILOCLISTENER_STR_LEN) return false;
    // PR139: Exact full-descriptor match (not just "com." prefix) to eliminate false positives
    if (sz < 12 + sizeof(GMS_ILOCLISTENER_DESCRIPTOR)) return false;
    bool match = (memcmp(raw + 12, GMS_ILOCLISTENER_DESCRIPTOR,
                         sizeof(GMS_ILOCLISTENER_DESCRIPTOR)) == 0);
    if (match) {
        LOGD("[PR109-DBG] GMS token MATCH code=%u sz=%zu", code, sz);
    }
    return match;
}
// ─────────────────────────────────────────────────────────────────────────────

static int32_t my_ipc_transact(void* self, int32_t handle, uint32_t code,
                                const void* data, void* reply, uint32_t flags) {
    // PR130: Verify outgoing hook fires (first 25 invocations for diagnostics)
    static std::atomic<int> s_ipcCount{0};
    int _pr130_ipc = s_ipcCount.fetch_add(1, std::memory_order_relaxed);
    if (_pr130_ipc < 25) {
        LOGE("[PR130] ipc_transact called: handle=%d code=%u call#%d proc='%s'",
             handle, code, _pr130_ipc, g_currentProcessName.c_str());
    }

    // PR105: detección AOSP getLastLocation/getCurrentLocation (muta REPLY)
    bool isAospLoc = isLocationRequest(data, code);
    if (isAospLoc) {
        LOGD("[PR105c] location request candidate: handle=%d code=%u sz=%zu",
             handle, code, parcel_dataSize(data));
    }

    // PR139: Re-enable PR107 outgoing mutation with shadow buffer (safe copy pattern).
    // PR134 disabled in-place mutation due to false positives in isGmsLocationOutgoing().
    // Now safe: exact descriptor match (PR139 Change B) eliminates false positives,
    // and shadow buffer avoids writing into kernel-mapped Parcel memory.
    bool isGmsLoc = isGmsLocationOutgoing(data, code);
    int64_t latBits = g_cachedLatBits.load(std::memory_order_acquire);
    int64_t lonBits = g_cachedLonBits.load(std::memory_order_acquire);
    bool outMutated = false;
    uint8_t* outSavedMData = nullptr;
    uint8_t* outShadowBuf = nullptr;

    if (isGmsLoc && (latBits != 0 || lonBits != 0)) {
        size_t sz = parcel_dataSize(data);
        const uint8_t* raw = parcel_data(data);
        if (sz >= GMS_ILOCLISTENER_MIN_SIZE && raw) {
            outShadowBuf = new uint8_t[sz];
            memcpy(outShadowBuf, raw, sz);

            double lat, lon;
            memcpy(&lat, &latBits, 8);
            memcpy(&lon, &lonBits, 8);

            outMutated = mutateLocationInBuffer(outShadowBuf, sz, GMS_ILOCLISTENER_HDR, lat, lon);
            size_t dopplerOff = 0;
            if (!outMutated)
                outMutated = mutateLocationByDopplerScan(outShadowBuf, sz, lat, lon, dopplerOff);

            if (outMutated) {
                // PR137-style mData swap at Parcel+0x08 (after mError+pad)
                uint8_t** mDataField = reinterpret_cast<uint8_t**>(
                    const_cast<uint8_t*>(static_cast<const uint8_t*>(data)) + 0x08);
                outSavedMData = *mDataField;
                *mDataField = outShadowBuf;
                LOGE("[PR139] outgoing GMS location mutated: handle=%d code=%u sz=%zu lat=%.6f lon=%.6f",
                     handle, code, sz, lat, lon);
            } else {
                delete[] outShadowBuf;
                outShadowBuf = nullptr;
            }
        }
    }

    int32_t status = orig_ipc_transact(self, handle, code, data, reply, flags);

    // Restore mData pointer after original transact call
    if (outMutated && outSavedMData) {
        uint8_t** mDataField = reinterpret_cast<uint8_t**>(
            const_cast<uint8_t*>(static_cast<const uint8_t*>(data)) + 0x08);
        *mDataField = outSavedMData;
    }
    delete[] outShadowBuf;

    // PR105: mutar REPLY de AOSP ILocationManager
    if (isAospLoc && status == 0 && reply) {
        mutateLocationReply(reply);
    }

    // PR147: Reply-side fallback for obfuscated GMS interfaces.
    // Uses /proc/self/mem to write in-place to PROT_READ Binder mmap.
    // No pointer swap → no freeBuffer/BC_FREE_BUFFER conflict.
    if (!isAospLoc && !isGmsLoc && status == 0 && reply
        && (latBits != 0 || lonBits != 0)) {
        size_t replySz = parcel_dataSize(reply);
        if (replySz > 64) {
            uint8_t* replyRaw = *reinterpret_cast<uint8_t**>(
                static_cast<uint8_t*>(reply) + 0x08);
            if (replyRaw && hasProviderSignature(replyRaw, replySz)) {
                visualizeLocationParcel(replyRaw, replySz, "ipc-reply", code);

                // Scan on writable copy to find lat/lon offset
                uint8_t* scan = new uint8_t[replySz];
                memcpy(scan, replyRaw, replySz);

                double rLat, rLon;
                memcpy(&rLat, &latBits, 8);
                memcpy(&rLon, &lonBits, 8);

                size_t probeOff = 0;
                if (probeProviderValidatedLocation(scan, replySz, rLat, rLon, probeOff)) {
                    // Find where the spoofed values were written in the copy
                    bool written = false;
                    for (size_t off = 0; off + 16 <= replySz; off += 4) {
                        double v1, v2;
                        memcpy(&v1, scan + off, 8);
                        memcpy(&v2, scan + off + 8, 8);
                        if (v1 == rLat && v2 == rLon) {
                            if (writeToReadOnly(replyRaw + off, &rLat, 8) &&
                                writeToReadOnly(replyRaw + off + 8, &rLon, 8)) {
                                LOGE("[PR147] ipc reply mutated: handle=%d code=%u probe@%zu→%zu lat=%.6f lon=%.6f",
                                     handle, code, probeOff, off, rLat, rLon);
                                written = true;
                            } else {
                                LOGE("[PR147] /proc/self/mem write FAILED for ipc reply");
                            }
                            break;
                        }
                    }
                }
                delete[] scan;
            }
        }
    }

    return status;
}

static constexpr int32_t ILOCLISTENER_STR_LEN = 34;
static constexpr uint32_t TRANSACTION_onLocationChanged = 1;
static constexpr size_t ILOCLISTENER_HDR = 84;
static constexpr size_t LOC_COPY_MAX = 1024;

typedef int32_t (*bbinder_transact_fn)(void*, uint32_t, const void*, void*, uint32_t);
static bbinder_transact_fn orig_bbinder_transact = nullptr;

static bool isLocationCallback(const void* data_parcel, uint32_t code) {
    if (code != TRANSACTION_onLocationChanged || !data_parcel) return false;
    size_t sz = parcel_dataSize(data_parcel);
    if (sz < ILOCLISTENER_HDR + 48) return false;
    const uint8_t* raw = parcel_data(data_parcel);
    int32_t strLen = 0;
    memcpy(&strLen, raw + 8, 4);
    return (strLen == ILOCLISTENER_STR_LEN && memcmp(raw + 12, LOC_TOKEN_PREFIX, 8) == 0);
}

// Tipo de función para JavaBBinder::onTransact
// Firma: status_t onTransact(uint32_t code, const Parcel& data, Parcel* reply, uint32_t flags)
// En términos de puntero de función C: (void* self, uint32_t code, void* data, void* reply, uint32_t flags)
using fn_jbbinder_ontransact = int32_t(*)(void*, uint32_t, const void*, void*, uint32_t);
static fn_jbbinder_ontransact orig_jbbinder_ontransact = nullptr;

// PR138: One-time diagnostic — verify Binder hooks are installed.
// Called from dlopen hooks (fires during Maps runtime, guaranteed in logcat).
// Defined here so all orig_* variables above are already in scope.
static void logBinderDiag() {
    static std::atomic<bool> s_binderDiagDone{false};
    if (!s_binderDiagDone.exchange(true)) {
        LOGE("[PR138-DIAG] Binder hooks: ipc_transact orig=%p, bbinder orig=%p, "
             "jbbinder orig=%p, latBits=%lld, lonBits=%lld, pagesPatched=%d, proc='%s'",
             (void*)orig_ipc_transact, (void*)orig_bbinder_transact,
             (void*)orig_jbbinder_ontransact,
             (long long)g_cachedLatBits.load(), (long long)g_cachedLonBits.load(),
             (int)g_pagesPatched, g_currentProcessName.c_str());
    }
}

static int32_t my_jbbinder_ontransact(void* self, uint32_t code,
                                      const void* data, void* reply, uint32_t flags) {
    // PR133: Safety guard — orig must be set (hook installed correctly).
    if (!orig_jbbinder_ontransact) return 0;

    // PR130: Verify hook is being called (first 5 invocations only)
    static std::atomic<int> s_callCount{0};
    int _pr130_n = s_callCount.fetch_add(1, std::memory_order_relaxed);
    if (_pr130_n < 5) {
        size_t _pr130_sz = data ? parcel_dataSize(data) : 0;
        LOGE("[PR130] jbbinder_ontransact called: code=%u sz=%zu call#%d", code, _pr130_sz, _pr130_n);
    }

    int64_t latBits = g_cachedLatBits.load(std::memory_order_acquire);
    int64_t lonBits = g_cachedLonBits.load(std::memory_order_acquire);

    bool mutated  = false;
    uint8_t* savedMData = nullptr;
    uint8_t* shadowBuf  = nullptr;

    // PR130: Parse token unconditionally (outside latBits guard) for diagnostics
    if (data) {
        size_t sz         = parcel_dataSize(data);
        const uint8_t* raw = parcel_data(data);

        if (sz >= 16 && raw) {
            size_t parsedHdr = 0;
            std::string token;
            bool hasToken = parseBinderInterfaceToken(raw, sz, token, parsedHdr);
            // PR142: Token-only matching — never scan unknown parcels.
            bool looksLocation = hasToken &&
                (token.find("location") != std::string::npos ||
                 token.find("fused") != std::string::npos ||
                 token.find("gnss") != std::string::npos);

            // PR130: Log first 3 location-looking tokens
            static std::atomic<int> s_locTokenCount{0};
            if (looksLocation) {
                int _lt = s_locTokenCount.fetch_add(1, std::memory_order_relaxed);
                if (_lt < 3) {
                    LOGE("[PR130] location token detected: '%s' code=%u sz=%zu hdr=%zu",
                         token.c_str(), code, sz, parsedHdr);
                }
            }

            // PR130: Log first 10 tokens to see what interfaces pass through
            static std::atomic<int> s_tokenLog{0};
            if (hasToken && !token.empty()) {
                int _tl = s_tokenLog.fetch_add(1, std::memory_order_relaxed);
                if (_tl < 10) {
                    LOGE("[PR130] binder token: '%s' code=%u sz=%zu", token.c_str(), code, sz);
                }
            }

            // PR148: Restrict provider-sig scan to code=1 (onLocationChanged) only.
            // onStatusChanged (code=2) and other callbacks embed a provider string but
            // NO lat/lon doubles — probeProviderValidatedLocation gives false positives there.
            bool hasProviderSig = !looksLocation && code == TRANSACTION_onLocationChanged
                                  && sz > 128 && hasProviderSignature(raw, sz);

            // PR145: Parcel visualizer for diagnostics
            if (hasProviderSig) {
                visualizeLocationParcel(raw, sz, "jbbinder", code);
            }

            if ((latBits != 0 || lonBits != 0) && (looksLocation || hasProviderSig)) {
                shadowBuf = new uint8_t[sz];
                memcpy(shadowBuf, raw, sz);

                double lat, lon;
                memcpy(&lat, &latBits, 8);
                memcpy(&lon, &lonBits, 8);

                if (looksLocation) {
                    // PR119: For known location interfaces, try known offsets first.
                    mutated = mutateLocationInBuffer(shadowBuf, sz, ILOCLISTENER_HDR, lat, lon) ||
                              mutateLocationInBuffer(shadowBuf, sz, GMS_ILOCLISTENER_HDR, lat, lon) ||
                              (parsedHdr > 0 && mutateLocationInBuffer(shadowBuf, sz, parsedHdr, lat, lon));

                    size_t dopplerOffset = 0;
                    if (!mutated) {
                        mutated = mutateLocationByDopplerScan(shadowBuf, sz, lat, lon, dopplerOffset);
                    }

                    if (mutated) {
                        LOGD("[PR145] jbbinder token-mutated: code=%u token='%s' hdr=%zu lat=%.6f lon=%.6f",
                             code, token.c_str(), parsedHdr, lat, lon);
                    }
                }

                // PR144: Provider-validated fallback for obfuscated GMS interfaces.
                if (!mutated && hasProviderSig) {
                    size_t probeOffset = 0;
                    mutated = probeProviderValidatedLocation(shadowBuf, sz, lat, lon, probeOffset);
                    if (mutated) {
                        LOGE("[PR145] jbbinder provider-scan mutated: code=%u token='%s' probe=%zu sz=%zu lat=%.6f lon=%.6f",
                             code, token.c_str(), probeOffset, sz, lat, lon);
                    }
                }

                if (mutated) {
                    uint8_t** mDataField = reinterpret_cast<uint8_t**>(
                        const_cast<uint8_t*>(static_cast<const uint8_t*>(data)) + 0x08);
                    savedMData   = *mDataField;
                    *mDataField  = shadowBuf;
                } else {
                    if (looksLocation) {
                        LOGD("[PR119] token='%s' detected but mutate FAILED (layout drift)",
                             token.c_str());
                    }
                    delete[] shadowBuf;
                    shadowBuf = nullptr;
                }
            }
        }
    }

    int32_t status = orig_jbbinder_ontransact(self, code, data, reply, flags);

    if (mutated && savedMData) {
        // PR137: mData at Parcel+0x08
        uint8_t** mDataField = reinterpret_cast<uint8_t**>(
            const_cast<uint8_t*>(static_cast<const uint8_t*>(data)) + 0x08);
        *mDataField = savedMData;
    }
    delete[] shadowBuf;

    return status;
}

// PR137: BBinder::transact now intercepts incoming location callbacks.
// PR115 (JavaBBinder vtable hook) fails on this ROM (_ZTV7android7BBinder=0x0),
// so we catch incoming Binder calls here instead.  BBinder::transact dispatches
// to JavaBBinder::onTransact internally, so hooking here catches everything.
// Incoming location callbacks (e.g. ILocationCallback.onLocationResult from GMS)
// carry the Location in the DATA parcel.  We shadow-copy + mutate before dispatch.
static int32_t my_bbinder_transact(void* self, uint32_t code,
                                   const void* data, void* reply, uint32_t flags) {
    if (!orig_bbinder_transact) return 0;

    // PR148: Verify DobbyHook is active (first 10 calls) + log every onLocationChanged (code=1)
    static std::atomic<int> s_bbCallCount{0};
    int _bbN = s_bbCallCount.fetch_add(1, std::memory_order_relaxed);
    {
        size_t _bbSz = data ? parcel_dataSize(data) : 0;
        if (_bbN < 10) {
            LOGE("[PR148] bbinder_transact called: code=%u sz=%zu call#%d", code, _bbSz, _bbN);
        } else if (code == TRANSACTION_onLocationChanged) {
            LOGE("[PR148] bbinder_transact onLocationChanged: code=1 sz=%zu", _bbSz);
        }
    }

    int64_t latBits = g_cachedLatBits.load(std::memory_order_acquire);
    int64_t lonBits = g_cachedLonBits.load(std::memory_order_acquire);

    bool mutated = false;
    uint8_t* savedMData = nullptr;
    uint8_t* shadowBuf = nullptr;

    if (data && (latBits != 0 || lonBits != 0)) {
        size_t sz = parcel_dataSize(data);
        const uint8_t* raw = parcel_data(data);

        if (sz >= 16 && raw) {
            std::string token;
            size_t parsedHdr = 0;
            bool hasToken = parseBinderInterfaceToken(raw, sz, token, parsedHdr);
            // PR142: Token-only matching — never scan unknown parcels.
            bool looksLocation = hasToken &&
                (token.find("location") != std::string::npos ||
                 token.find("fused") != std::string::npos ||
                 token.find("gnss") != std::string::npos);

            // PR148: Restrict provider-sig scan to code=1 (onLocationChanged) only.
            // onStatusChanged (code=2) and other callbacks embed a provider string but
            // NO lat/lon doubles — probeProviderValidatedLocation gives false positives there.
            bool hasProviderSig = !looksLocation && code == TRANSACTION_onLocationChanged
                                  && sz > 128 && hasProviderSignature(raw, sz);

            // PR145: Parcel visualizer for diagnostics
            if (hasProviderSig) {
                visualizeLocationParcel(raw, sz, "bbinder", code);
            }

            if (looksLocation || hasProviderSig) {
                shadowBuf = new uint8_t[sz];
                memcpy(shadowBuf, raw, sz);

                double lat, lon;
                memcpy(&lat, &latBits, 8);
                memcpy(&lon, &lonBits, 8);

                if (looksLocation) {
                    mutated = mutateLocationInBuffer(shadowBuf, sz, ILOCLISTENER_HDR, lat, lon) ||
                              mutateLocationInBuffer(shadowBuf, sz, GMS_ILOCLISTENER_HDR, lat, lon) ||
                              (parsedHdr > 0 && mutateLocationInBuffer(shadowBuf, sz, parsedHdr, lat, lon));

                    size_t dopplerOffset = 0;
                    if (!mutated) {
                        mutated = mutateLocationByDopplerScan(shadowBuf, sz, lat, lon, dopplerOffset);
                    }

                    if (mutated) {
                        LOGD("[PR145] bbinder token-mutated: code=%u token='%s' lat=%.6f lon=%.6f",
                             code, token.c_str(), lat, lon);
                    }
                }

                // PR144: Provider-validated fallback for obfuscated GMS interfaces.
                if (!mutated && hasProviderSig) {
                    size_t probeOffset = 0;
                    mutated = probeProviderValidatedLocation(shadowBuf, sz, lat, lon, probeOffset);
                    if (mutated) {
                        LOGE("[PR145] bbinder provider-scan mutated: code=%u token='%s' probe=%zu sz=%zu lat=%.6f lon=%.6f",
                             code, token.c_str(), probeOffset, sz, lat, lon);
                    }
                }

                if (mutated) {
                    uint8_t** mDataField = reinterpret_cast<uint8_t**>(
                        const_cast<uint8_t*>(static_cast<const uint8_t*>(data)) + 0x08);
                    savedMData = *mDataField;
                    *mDataField = shadowBuf;
                } else {
                    // PR146: Log when provider signature found but no mutation
                    if (hasProviderSig) {
                        LOGE("[PR146] bbinder provider-scan FAILED: code=%u token='%s' sz=%zu",
                             code, token.c_str(), sz);
                    }
                    delete[] shadowBuf;
                    shadowBuf = nullptr;
                }
            }
        }
    }

    int32_t status = orig_bbinder_transact(self, code, data, reply, flags);

    // Restore original mData pointer
    if (mutated && savedMData) {
        uint8_t** mDataField = reinterpret_cast<uint8_t**>(
            const_cast<uint8_t*>(static_cast<const uint8_t*>(data)) + 0x08);
        *mDataField = savedMData;
    }
    delete[] shadowBuf;

    return status;
}

// ─────────────────────────────────────────────────────────────────────────────
// PR108: Resolución robusta de símbolos privados — MIUI 12.5 / MediaTek
//
// En MIUI 12.5 (Redmi 9/Lancelot), libbinder.so compila los símbolos de Binder
// como PRIVADOS (.symtab), no exportados en .dynsym.
// DobbySymbolResolver solo lee .dynsym → falla silenciosamente.
//
// Cadena de 3 intentos:
//   1. DobbySymbolResolver  → .dynsym estándar (ROMs AOSP)
//   2. dlsym(RTLD_DEFAULT)  → todos los símbolos ya mapeados en el proceso
//   3. dlopen + dlsym       → búsqueda explícita en libbinder.so
// ─────────────────────────────────────────────────────────────────────────────
static void* resolveLibbinderSymbol(const char* mangled) {
    void* sym = nullptr;

    // Intento 1: dlsym(RTLD_DEFAULT) — fast hash lookup across all loaded libs
    sym = dlsym(RTLD_DEFAULT, mangled);
    if (sym) {
        LOGD("[PR108] via dlsym(RTLD_DEFAULT): %s @ %p", mangled, sym);
        return sym;
    }

    // Intento 2: dlopen + dlsym — explicit library handle
    // RTLD_NOLOAD: no carga si no está mapeada (evita double-load)
    void* lib = dlopen("libbinder.so", RTLD_NOW | RTLD_NOLOAD);
    if (!lib) {
        lib = dlopen("/system/lib64/libbinder.so", RTLD_NOW | RTLD_LOCAL);
    }
    if (lib) {
        sym = dlsym(lib, mangled);
        // No llamar dlclose: mantener handle para que la dirección sea estable
        if (sym) {
            LOGD("[PR108] via dlopen+dlsym: %s @ %p", mangled, sym);
            return sym;
        }
    }

    // Intento 3: DobbySymbolResolver — linear ELF scan, slow but finds hidden symbols
    sym = DobbySymbolResolver("libbinder.so", mangled);
    if (sym) {
        LOGD("[PR108] via DobbySymbolResolver: %s @ %p", mangled, sym);
        return sym;
    }

    LOGE("[PR108] UNRESOLVED: %s", mangled);
    return nullptr;
}

// ─────────────────────────────────────────────────────────────────────────────
// PR113: Resolución de símbolos en libandroid_runtime.so
// Misma cadena de 3 intentos que resolveLibbinderSymbol pero para la librería
// que contiene JavaBBinder::onTransact — el único punto de entrega Binder
// a Java stubs que NO puede ser inlined (requiere llamada a la JVM).
// ─────────────────────────────────────────────────────────────────────────────
static void* resolveRuntimeSymbol(const char* mangled) {
    void* sym = nullptr;

    // Fast path: dlsym hash lookup first
    sym = dlsym(RTLD_DEFAULT, mangled);
    if (sym) {
        LOGD("[PR113] via dlsym(RTLD_DEFAULT): %s @ %p", mangled, sym);
        return sym;
    }

    void* lib = dlopen("libandroid_runtime.so", RTLD_NOW | RTLD_NOLOAD);
    if (!lib) {
        lib = dlopen("/system/lib64/libandroid_runtime.so", RTLD_NOW | RTLD_LOCAL);
    }
    if (lib) {
        sym = dlsym(lib, mangled);
        if (sym) {
            LOGD("[PR113] via dlopen+dlsym: %s @ %p", mangled, sym);
            return sym;
        }
    }

    // Slow fallback: linear ELF scan for hidden-visibility symbols
    sym = DobbySymbolResolver("libandroid_runtime.so", mangled);
    if (sym) {
        LOGD("[PR113] via DobbySymbolResolver: %s @ %p", mangled, sym);
        return sym;
    }

    LOGE("[PR113] UNRESOLVED: %s", mangled);
    return nullptr;
}
// ─────────────────────────────────────────────────────────────────────────────

// PR120f: resolución robusta de símbolos libc para ROMs donde
// DobbySymbolResolver("libc.so", ...) retorna null (linker namespaces/aliases).
static void* resolveLibcSymbol(const char* name) {
    if (!name || !*name) return nullptr;

    // Fast path: dlsym hash lookup
    void* sym = dlsym(RTLD_DEFAULT, name);
    if (sym) return sym;

    void* lib = dlopen("libc.so", RTLD_NOW | RTLD_NOLOAD);
    if (!lib) {
        lib = dlopen("/apex/com.android.runtime/lib64/bionic/libc.so", RTLD_NOW | RTLD_LOCAL);
    }
    if (lib) {
        sym = dlsym(lib, name);
        if (sym) return sym;
    }

    // Slow fallback: linear ELF scan
    sym = DobbySymbolResolver("libc.so", name);
    if (sym) return sym;

    return nullptr;
}

// PR118: Hook para la reconstrucción nativa del objeto Location
// Firma: void android_location_Location_readFromParcel(JNIEnv* env, jobject clazz, jobject parcelObj)
using fn_Location_readFromParcel = void(*)(JNIEnv*, jobject, jobject);
static fn_Location_readFromParcel orig_Location_readFromParcel = nullptr;

static void my_Location_readFromParcel(JNIEnv* env, jobject thiz, jobject parcel) {
    // 1. Dejamos que el sistema reconstruya el objeto originalmente
    orig_Location_readFromParcel(env, thiz, parcel);

    // 2. Inmediatamente después, sobreescribimos los campos de Java usando JNI
    int64_t latBits = g_cachedLatBits.load(std::memory_order_acquire);
    int64_t lonBits = g_cachedLonBits.load(std::memory_order_acquire);

    if (latBits != 0 || lonBits != 0) {
        double fakeLat, fakeLon;
        memcpy(&fakeLat, &latBits, 8);
        memcpy(&fakeLon, &lonBits, 8);

        jclass locClass = env->GetObjectClass(thiz);
        // Buscamos los métodos setter de la clase Location en Java
        jmethodID setLat = env->GetMethodID(locClass, "setLatitude", "(D)V");
        jmethodID setLon = env->GetMethodID(locClass, "setLongitude", "(D)V");

        if (setLat && setLon) {
            env->CallVoidMethod(thiz, setLat, fakeLat);
            env->CallVoidMethod(thiz, setLon, fakeLon);
            LOGD("[PR118] Location object mutated after readFromParcel: %.6f, %.6f", fakeLat, fakeLon);
        }
        env->DeleteLocalRef(locClass);
    }
}

static void applyBBinderHook() {
    // ─────────────────────────────────────────────────────────────────────────
    // PR106: BBinder::transact fallback (inlined en MIUI, conservado por compatibilidad)
    // ─────────────────────────────────────────────────────────────────────────
    void* sym106 = resolveLibbinderSymbol(
        "_ZN7android7BBinder8transactEjRKNS_6ParcelEPS1_j");
    if (sym106) {
        int rc106 = DobbyHook(sym106, (void*)my_bbinder_transact, (void**)&orig_bbinder_transact);
        LOGE("[PR106] BBinder::transact hook rc=%d @ %p (orig=%p)", rc106, sym106, (void*)orig_bbinder_transact);
    }

    // ─────────────────────────────────────────────────────────────────────────
    // PR115: VTable hook de JavaBBinder::onTransact
    //
    // JavaBBinder::onTransact no tiene símbolo exportado en MIUI 12.5.
    // Solución: localizar su dirección via el vtable exportado _ZTV11JavaBBinder.
    //
    // Estrategia A (dinámica): comparar vtable de BBinder contra onTransact
    //   para hallar el índice, luego leer mismo índice en vtable de JavaBBinder.
    //   Robusto a actualizaciones de ROM.
    //
    // Estrategia B (estática): fallback al índice 19 del vtable de JavaBBinder,
    //   confirmado por análisis binario offline del dispositivo.
    //   (vtable[19] = +0x98 desde _ZTV11JavaBBinder = función de 488 bytes / 8 BL calls)
    // ─────────────────────────────────────────────────────────────────────────

    void* target_fn = nullptr;

    // ── Estrategia A: Scan dinámico del vtable de BBinder ────────────────────
    void* base_vtable    = resolveLibbinderSymbol("_ZTV7android7BBinder");
    void* base_ontransact = resolveLibbinderSymbol(
        "_ZN7android7BBinder10onTransactEjRKNS_6ParcelEPS1_j");
    void* java_vtable    = resolveRuntimeSymbol("_ZTV11JavaBBinder");

    if (base_vtable && base_ontransact && java_vtable) {
        void** b_vt = reinterpret_cast<void**>(base_vtable);
        void** j_vt = reinterpret_cast<void**>(java_vtable);

        int found_idx = -1;
        // El vtable incluye [0]=offset_to_top, [1]=RTTI antes de las funciones.
        // Escanear las primeras 32 entradas (256 bytes) para encontrar onTransact.
        for (int i = 0; i < 32; i++) {
            if (b_vt[i] == base_ontransact) {
                found_idx = i;
                break;
            }
        }

        if (found_idx != -1) {
            void* candidate = j_vt[found_idx];
            if (candidate) {
                LOGD("[PR115] Dynamic scan: onTransact at vtable[%d] = %p", found_idx, candidate);
                target_fn = candidate;
            } else {
                LOGE("[PR115] Dynamic scan: vtable[%d] is null in JavaBBinder", found_idx);
            }
        } else {
            LOGE("[PR115] Dynamic scan: BBinder::onTransact NOT found in vtable (checked 32 entries)");
        }
    } else {
        LOGE("[PR115] Dynamic scan: missing symbols base_vt=%p ontransact=%p java_vt=%p",
             base_vtable, base_ontransact, java_vtable);
    }

    // ── Estrategia B: Fallback estático DESHABILITADO (PR133) ────────────────
    if (!target_fn && java_vtable) {
        // PR133: Static fallback at index 19 is ROM-specific and caused a Maps
        // crash loop on MIUI (vtable[19] was the wrong function; my_jbbinder_ontransact
        // received wrong arguments → crash → respawn loop, hooks never fired).
        // Dynamic scan requires _ZTV7android7BBinder which is 0x0 on this ROM.
        // Disabling static fallback; PR105 IPCThreadState::transact is sufficient.
        LOGE("[PR115] Static fallback DISABLED — dynamic scan failed (base_vt=0x0). Skipping PR115.");
    }

    // ── Hook final ────────────────────────────────────────────────────────────
    if (target_fn) {
        int rc115 = DobbyHook(target_fn, (void*)my_jbbinder_ontransact,
                  (void**)&orig_jbbinder_ontransact);
        if (rc115 == 0) {
            LOGE("[PR115] JavaBBinder::onTransact hooked OK @ %p (orig=%p)", target_fn, (void*)orig_jbbinder_ontransact);
        } else {
            LOGE("[PR115] JavaBBinder::onTransact DobbyHook FAILED rc=%d @ %p", rc115, target_fn);
        }
        // PR131: Verify patch
        {
            uint32_t insn = 0;
            memcpy(&insn, target_fn, 4);
            LOGE("[PR131] JavaBBinder verify: first_insn=0x%08x orig_ptr=%p", insn, (void*)orig_jbbinder_ontransact);
        }
    } else {
        LOGE("[PR115] JavaBBinder::onTransact HOOK FAILED — all strategies exhausted");
    }

    // PR118: Hook Location::readFromParcel — the JNI entry point for Location
    // object reconstruction from Binder parcels.
    //
    // On Android 11, Location.readFromParcel() is a PURE JAVA method in most ROMs.
    // The native symbol only exists when the ROM registers a native fast-path via
    // RegisterNatives. Try multiple symbol patterns for ROM compatibility:
    //   1. android::Location_readFromParcel (MIUI variant)
    //   2. android_location_Location_readFromParcel (AOSP JNI naming)
    //   3. JNI method table scan as ultimate fallback
    {
        static const char* LOC_READ_SYMBOLS[] = {
            "_ZN7android16Location_readFromParcelEP7_JNIEnvP8_jobjectS3_",
            // AOSP-style: android_location_Location_readFromParcel
            "_ZN7android36android_location_Location_readFromParcelEP7_JNIEnvP8_jobjectS3_",
            // Variant with different namespace
            "_ZN7android8location8Location15readFromParcelEP7_JNIEnvP8_jobjectS3_",
            nullptr
        };
        void* readFromParcel = nullptr;
        for (int i = 0; LOC_READ_SYMBOLS[i] && !readFromParcel; ++i) {
            readFromParcel = resolveRuntimeSymbol(LOC_READ_SYMBOLS[i]);
        }
        // Fallback: search for any symbol containing "Location" and "readFromParcel"
        // via dlopen + iteration (future enhancement)
        if (readFromParcel) {
            int rc118 = DobbyHook(readFromParcel, (void*)my_Location_readFromParcel, (void**)&orig_Location_readFromParcel);
            LOGE("[PR118] Location::readFromParcel hook rc=%d @ %p (orig=%p)", rc118, readFromParcel, (void*)orig_Location_readFromParcel);
        } else {
            // Not an error on most ROMs — readFromParcel is pure Java on Android 11.
            // Location interception relies on Binder hooks (PR105/119) instead.
            LOGD("[PR118] Location::readFromParcel: no native symbol (pure Java — expected on most ROMs)");
        }
    }
}

static void applyBinderHooks() {
    // PR135: Skip ALL Binder hooks in GMS — Dobby can't safely relocate
    // IPCThreadState::transact's prologue on this ROM (SIGSEGV in trampoline
    // at <anonymous> region). GMS is a location provider, not consumer;
    // PR105 REPLY mutation only benefits Maps.
    if (g_currentProcessName.find("com.google.android.gms") != std::string::npos) {
        // PR139: Only skip IPCThreadState::transact in GMS (PR135 crash was specific to
        // that symbol's Dobby trampoline). BBinder/JavaBBinder hooks are safe and needed
        // for intercepting incoming location callbacks in GMS sub-processes.
        LOGE("[PR139] GMS process '%s' — skipping IPCThreadState::transact (PR135 crash), "
             "but applying BBinder/JavaBBinder hooks for location callback coverage",
             g_currentProcessName.c_str());
        applyBBinderHook();
        return;
    }

    void* sym = resolveLibbinderSymbol(
        "_ZN7android14IPCThreadState8transactEijRKNS_6ParcelEPS1_j");
    if (sym) {
        int rc105 = DobbyHook(sym, (void*)my_ipc_transact, (void**)&orig_ipc_transact);
        if (rc105 == 0) {
            LOGE("[PR105] IPCThreadState::transact hooked OK @ %p (orig=%p)", sym, (void*)orig_ipc_transact);
        } else {
            LOGE("[PR105] IPCThreadState::transact DobbyHook FAILED rc=%d @ %p", rc105, sym);
        }
        // PR131: Verify patch — read first 4 bytes at hooked address
        {
            uint32_t insn = 0;
            memcpy(&insn, sym, 4);
            LOGE("[PR131] IPCThreadState verify: first_insn=0x%08x orig_ptr=%p", insn, (void*)orig_ipc_transact);
        }
    } else {
        LOGE("[PR105] IPCThreadState::transact UNRESOLVED");
    }
    applyBBinderHook();
}
// ─────────────────────────────────────────────────────────────────────────────

class OmniModule : public zygisk::ModuleBase {
public:
    void onLoad(zygisk::Api *api, JNIEnv *env) override {
        // PR122: Always log onLoad, even when api/env is null, to diagnose
        // Zygisk injection failures (e.g., Maps main process not getting hooks).
        if (!api || !env) {
            __android_log_print(ANDROID_LOG_ERROR, LOG_TAG,
                "[scope] onLoad: ABORT — api=%p env=%p (Zygisk injection failure?)",
                (void*)api, (void*)env);
            return;
        }
        g_api = api;
        env->GetJavaVM(&g_jvm);
        this->api = api;
        this->env = env;
        LOGD("[scope] onLoad: api=%p jvm=%p", (void*)api, (void*)g_jvm);
    }
    void preAppSpecialize(zygisk::AppSpecializeArgs *args) override {
        // PR70c: Try companion first (root daemon, bypasses SELinux), fall back to direct read
        if (!readConfigViaCompanion(g_api)) {
            LOGD("[scope] companion unavailable, trying direct file read");
            readConfig();
        }

        JNIEnv *env = nullptr;
        if (g_jvm) g_jvm->GetEnv((void**)&env, JNI_VERSION_1_6);
        g_isTargetApp = false;
        if (!env || !args->nice_name) {
            LOGE("[scope] preAppSpecialize: SKIP — env=%p nice_name=%p (JNI failure?)",
                 (void*)env, args ? (void*)args->nice_name : nullptr);
        }
        if (env && args->nice_name) {
            const char *p = env->GetStringUTFChars(args->nice_name, nullptr);
            if (!p) {
                LOGE("[scope] preAppSpecialize: GetStringUTFChars returned NULL");
            }
            if (p) {
                std::string proc(p);
                g_currentProcessName = proc;
                LOGD("[scope] process='%s' config_keys=%zu", proc.c_str(), g_config.size());
                if (g_config.count("scoped_apps")) {
                    std::string scopedRaw = g_config["scoped_apps"];
                    LOGD("[scope] scoped_apps='%s'", scopedRaw.c_str());
                    std::istringstream ss(scopedRaw);
                    std::string token;
                    while (std::getline(ss, token, ',')) {
                        token.erase(0, token.find_first_not_of(" \t"));
                        token.erase(token.find_last_not_of(" \t") + 1);
                        if (!token.empty() && proc.find(token) != std::string::npos) {
                            g_isTargetApp = true;
                            LOGD("[scope] MATCH: '%s' contains '%s' → hooking", proc.c_str(), token.c_str());
                            break;
                        }
                    }
                    // PR125: Flag is written by companion_handler (root, SELinux-safe).
                    // Writing from the app process fails silently (SELinux blocks open()
                    // from forked Zygote child to /data/adb/). See companion_handler.
                } else {
                    LOGD("[scope] NO scoped_apps key in config");
                }

                // PR119: Productores tempranos de ubicación (MTK/Xiaomi/AOSP fused).
                // Se fuerzan en scope para interceptar coordenadas antes de que
                // lleguen a GMS/Maps, incluso si el usuario olvidó agregarlos.
                if (!g_isTargetApp) {
                    static const char* kLocationProducers[] = {
                        "com.mediatek.location.lppe.main",
                        "com.mediatek.location.ppe.main",
                        "com.xiaomi.location.fused",
                        "com.android.location.fused",
                        "com.google.android.gms",          // PR129: matches main, .unstable, .persistent
                        "com.google.android.apps.maps",    // PR129: always hook Maps for location
                        nullptr
                    };
                    for (int i = 0; kLocationProducers[i]; ++i) {
                        if (proc.find(kLocationProducers[i]) != std::string::npos) {
                            g_isTargetApp = true;
                            LOGD("[PR119][scope] producer auto-scope: '%s'", kLocationProducers[i]);
                            break;
                        }
                    }
                }

                // PR71g: WebView spoof toggle — hook WebView processes without
                // them being in scoped_apps. This avoids the Destroy Identity
                // crash (which force-stops + wipes all scoped_apps, killing the
                // WebView process that hosts the WebUI).
                if (!g_isTargetApp && g_config.count("webview_spoof") &&
                    g_config["webview_spoof"] == "true") {
                    if (proc.find("webview") != std::string::npos) {
                        g_isTargetApp = true;
                        LOGD("[scope] WEBVIEW MATCH: '%s' → hooking (webview_spoof=true)", proc.c_str());
                    }
                }
                if (!g_isTargetApp) {
                    LOGD("[scope] '%s' not in scope → DLCLOSE", proc.c_str());
                }
                env->ReleaseStringUTFChars(args->nice_name, p);
            }
        }
        if (g_api) {
            if (g_isTargetApp) {
                g_api->setOption(zygisk::FORCE_DENYLIST_UNMOUNT);

                // MemFD bind mount: spoof /proc/cpuinfo at kernel level.
                // VDInfos reads cpuinfo via raw syscalls that bypass openat/read hooks.
                // By bind-mounting a memfd over /proc/cpuinfo before specialization
                // (while we still have CAP_SYS_ADMIN from Zygote), all reads — including
                // direct syscalls — see the spoofed content.
                LOGE("MemFD: profile='%s', looking up...", g_currentProfileName.c_str());
                const DeviceFingerprint* fp_cpu = findProfile(g_currentProfileName);
                if (fp_cpu) {
                    std::string fake_cpuinfo = generateMulticoreCpuInfo(*fp_cpu);
                    LOGE("MemFD: profile found, hardware='%s', cpuinfo_size=%zu",
                         fp_cpu->hardware, fake_cpuinfo.size());
                    int mfd = syscall(__NR_memfd_create, "fake_cpuinfo", 0);
                    if (mfd >= 0) {
                        write(mfd, fake_cpuinfo.c_str(), fake_cpuinfo.size());
                        lseek(mfd, 0, SEEK_SET);
                        char fdpath[64];
                        snprintf(fdpath, sizeof(fdpath), "/proc/self/fd/%d", mfd);
                        if (mount(fdpath, "/proc/cpuinfo", nullptr, MS_BIND, nullptr) == 0) {
                            LOGE("MemFD: bind-mounted spoofed /proc/cpuinfo (%zu bytes)",
                                 fake_cpuinfo.size());
                        } else {
                            LOGE("MemFD: bind mount FAILED (errno=%d: %s), falling back to hooks",
                                 errno, strerror(errno));
                        }
                        // Exempt fd from zygote's automatic fd cleanup during specialization
                        if (g_api) g_api->exemptFd(mfd);
                    } else {
                        LOGE("MemFD: memfd_create failed (errno=%d: %s)", errno, strerror(errno));
                    }
                } else {
                    LOGE("MemFD: findProfile('%s') returned NULL — no cpuinfo spoof",
                         g_currentProfileName.c_str());
                }
            }
            // PR85: DLCLOSE_MODULE_LIBRARY removed — thread_local g_inPropertyFind
            // triggers Bionic TLS cleanup abort() when the .so is dlclose'd on Android <14.
            // The module stays inert in memory for non-target apps (postAppSpecialize returns
            // immediately at line 3914), consuming ~0 CPU.
        }
    }
    void postAppSpecialize(const zygisk::AppSpecializeArgs *args) override {
        if (!g_isTargetApp) return;

        // MemFD postcheck: verify if /proc/cpuinfo bind mount survived
        // FORCE_DENYLIST_UNMOUNT + specialization. Reads first line to confirm.
        {
            FILE* cpuf = fopen("/proc/cpuinfo", "r");
            if (cpuf) {
                char line[256] = {};
                if (fgets(line, sizeof(line), cpuf)) {
                    // Strip trailing newline
                    size_t len = strlen(line);
                    if (len > 0 && line[len-1] == '\n') line[len-1] = '\0';
                    LOGE("MemFD postcheck: /proc/cpuinfo first line = '%s'", line);
                }
                fclose(cpuf);
            } else {
                LOGE("MemFD postcheck: failed to open /proc/cpuinfo (errno=%d)", errno);
            }
        }

        // PR56: obtener JNIEnv desde g_jvm (ya no viene como parámetro)
        JNIEnv *env = nullptr;
        if (g_jvm) g_jvm->GetEnv((void**)&env, JNI_VERSION_1_6);
        if (!env) return;

        // PR91-fix: Detect real MIUI device BEFORE hooks alter property reads.
        // The MIUI framework in every process depends on ro.miui.* properties for
        // permissions, settings, etc. If we suppress them, the framework crashes.
        {
            char miui_ver[PROP_VALUE_MAX] = {};
            __system_property_get("ro.miui.ui.version.code", miui_ver);
            g_realDeviceIsMiui = (miui_ver[0] != '\0');
            if (g_realDeviceIsMiui) {
                LOGE("PR91-fix: Real device is MIUI — preserving MIUI framework properties");
            }
        }

        // PR38+39: Inicializar caché de GPS y cargar sensor globals del perfil activo
        initLocationCache();
        // PR130: Diagnostic — verify coordinates are loaded into atomic cache
        {
            int64_t lb = g_cachedLatBits.load(std::memory_order_acquire);
            int64_t lob = g_cachedLonBits.load(std::memory_order_acquire);
            double lat, lon;
            memcpy(&lat, &lb, 8);
            memcpy(&lon, &lob, 8);
            LOGE("[PR130] Location cache: lat=%.6f lon=%.6f cached=%d profile='%s' seed=%ld",
                 lat, lon, (int)g_locationCached, g_currentProfileName.c_str(), g_masterSeed);
        }
        const DeviceFingerprint* sp_ptr = findProfile(g_currentProfileName);
        if (sp_ptr) {
            const auto& sp = *sp_ptr;
            g_sensorAccelMax      = sp.accelMaxRange;
            g_sensorAccelRes      = sp.accelResolution;
            g_sensorGyroMax       = sp.gyroMaxRange;
            g_sensorGyroRes       = sp.gyroResolution;
            g_sensorMagMax        = sp.magMaxRange;
            g_sensorHasHeartRate  = sp.hasHeartRateSensor;
            g_sensorHasBarometer  = sp.hasBarometerSensor;
            // PR44: datos ópticos del perfil activo
            g_camPhysicalWidth    = sp.sensorPhysicalWidth;
            g_camPhysicalHeight   = sp.sensorPhysicalHeight;
            g_camPixelWidth       = sp.pixelArrayWidth;
            g_camPixelHeight      = sp.pixelArrayHeight;
            g_camFocalLength      = sp.focalLength;
            g_camAperture         = sp.aperture;
            g_camFrontPhysWidth   = sp.frontSensorPhysicalWidth;
            g_camFrontPhysHeight  = sp.frontSensorPhysicalHeight;
            g_camFrontPixWidth    = sp.frontPixelArrayWidth;
            g_camFrontPixHeight   = sp.frontPixelArrayHeight;
            g_camFrontFocLen      = sp.frontFocalLength;
            g_camFrontAperture    = sp.frontAperture;
        }

        // Libc Hooks (Phase 1)
        // PR51: open NO se hookea directamente. Bionic::open llama openat() internamente;
        // hookear ambos crea recursión infinita (my_open→orig_open→openat→my_openat→my_open).
        // Toda llamada a open() pasa por openat() de todas formas → un solo hook en openat basta.

        void* openat_func = DobbySymbolResolver("libc.so", "openat");
        if (openat_func) DobbyHook(openat_func, (void*)my_openat, (void**)&orig_openat);

        void* read_func = DobbySymbolResolver("libc.so", "read");
        if (read_func) DobbyHook(read_func, (void*)my_read, (void**)&orig_read);

        void* close_func = DobbySymbolResolver("libc.so", "close");
        if (close_func) DobbyHook(close_func, (void*)my_close, (void**)&orig_close);

        void* lseek_func = DobbySymbolResolver("libc.so", "lseek");
        if (lseek_func) DobbyHook(lseek_func, (void*)my_lseek, (void**)&orig_lseek);

        void* lseek64_func = DobbySymbolResolver("libc.so", "lseek64");
        if (lseek64_func) DobbyHook(lseek64_func, (void*)my_lseek64, (void**)&orig_lseek64);

        void* pread_func = DobbySymbolResolver("libc.so", "pread");
        if (pread_func) DobbyHook(pread_func, (void*)my_pread, (void**)&orig_pread);

        void* pread64_func = DobbySymbolResolver("libc.so", "pread64");
        if (pread64_func) DobbyHook(pread64_func, (void*)my_pread64, (void**)&orig_pread64);

        void* mmap_func = resolveLibcSymbol("mmap");
        if (mmap_func) {
            int ret = DobbyHook(mmap_func, (void*)my_mmap, (void**)&orig_mmap);
            LOGE("PR120: mmap hook %s at %p (ret=%d)", ret == 0 ? "SUCCESS" : "FAILED", mmap_func, ret);
        } else {
            LOGE("PR120: mmap symbol unresolved (resolver chain exhausted)");
        }

        void* mmap64_func = resolveLibcSymbol("mmap64");
        if (!mmap64_func) {
            // Algunos builds exponen solo alias internos de bionic.
            mmap64_func = resolveLibcSymbol("__mmap2");
        }
        if (mmap64_func) {
            if (mmap_func && mmap64_func == mmap_func) {
                // PR120g: en varios builds ARM64 mmap/mmap64 son el mismo símbolo.
                // Evitar double-hook sobre la misma dirección (puede anular trampolines).
                LOGE("PR120: mmap64 shares mmap symbol (%p) — single-hook mode", mmap64_func);
            } else {
                int ret = DobbyHook(mmap64_func, (void*)my_mmap64, (void**)&orig_mmap64);
                LOGE("PR120: mmap64 hook %s at %p (ret=%d)", ret == 0 ? "SUCCESS" : "FAILED", mmap64_func, ret);
            }
        } else {
            LOGE("PR120: mmap64 symbol unresolved (resolver chain exhausted)");
        }

        // PR138: NEVER Dobby-hook __system_property_get or __system_property_read_callback.
        // PIF also Dobby-hooks these functions. Module ordering is unpredictable:
        // if OmniShield hooks first and PIF hooks second, PIF's trampoline overwrites
        // OmniShield's, and OmniShield's orig trampoline jumps to corrupted data → SIGILL.
        // patchPropertyPages() + PLT hooks provide sufficient coverage without Dobby.
        {
            void* sysprop_func = DobbySymbolResolver("libc.so", "__system_property_get");
            LOGE("PR138: __system_property_get @ %p — skipping Dobby hook (PIF conflict avoidance)",
                 sysprop_func);
            if (!orig_system_property_get) {
                orig_system_property_get =
                    reinterpret_cast<decltype(orig_system_property_get)>(
                        dlsym(RTLD_DEFAULT, "__system_property_get"));
            }
        }
        {
            void* sysprop_cb_func = DobbySymbolResolver("libc.so", "__system_property_read_callback");
            LOGE("PR138: __system_property_read_callback @ %p — skipping Dobby hook (PIF conflict avoidance)",
                 sysprop_cb_func);
            if (!orig_system_property_read_callback) {
                orig_system_property_read_callback =
                    reinterpret_cast<decltype(orig_system_property_read_callback)>(
                        dlsym(RTLD_DEFAULT, "__system_property_read_callback"));
            }
        }

        // PR86: Re-attempt Dobby inline hook on __system_property_find.
        // PR85 removed this because DobbySymbolResolver("libc.so") may have returned
        // a PLT thunk or wrong address, causing SIGABRT. Now using dlsym(RTLD_DEFAULT)
        // for reliable resolution of the actual bionic function address.
        // This inline hook intercepts ALL callers including late-loaded .so files
        // (VDInfo's native lib, Snapchat's native lib) that aren't covered by PLT hooks.
        //
        // SAFETY: If __system_property_read_callback is too close (< 64 bytes),
        // another module (PIF) may have already Dobby-hooked it, overwriting
        // instructions in __system_property_find's body.  The Dobby orig-trampoline
        // for _find would then execute corrupted code → SIGILL.
        // In that case we fall back to PLT-only hooks.
        {
            void* find_func = dlsym(RTLD_DEFAULT, "__system_property_find");
            void* cb_func   = dlsym(RTLD_DEFAULT, "__system_property_read_callback");
            bool tooClose = false;
            if (find_func && cb_func) {
                ptrdiff_t gap = (uint8_t*)cb_func - (uint8_t*)find_func;
                if (gap > 0 && gap < 64) {
                    tooClose = true;
                    LOGE("PR86: __system_property_find (%p) too close to "
                         "__system_property_read_callback (%p) (gap=%td bytes) — "
                         "skipping Dobby hook to avoid SIGILL conflict with PIF",
                         find_func, cb_func, gap);
                }
            }
            if (find_func && !tooClose) {
                int ret = DobbyHook(find_func, (void*)my_system_property_find,
                                    (void**)&orig_system_property_find);
                if (ret == 0) {
                    LOGE("PR86: Dobby inline hook on __system_property_find SUCCESS at %p", find_func);
                    g_propFindInlineHooked = true;
                } else {
                    LOGE("PR86: Dobby inline hook on __system_property_find FAILED (ret=%d) at %p, falling back to PLT only", ret, find_func);
                }
            } else if (!find_func) {
                LOGE("PR86: dlsym(RTLD_DEFAULT, __system_property_find) returned NULL");
            }
        }

        // PR88: Hook __system_property_foreach — intercept property enumeration.
        // Detection apps iterate ALL properties via foreach, receiving raw prop_info*
        // pointers from the property trie. Our wrapper callback substitutes FakePropInfo
        // (with spoofed values) for spoofed properties. __system_property_foreach in
        // libc.so traverses the property trie (~50+ ARM64 instructions) — well above
        // Dobby's 12-byte minimum. Also installed as PLT hook fallback in installPltHooks.
        // MUST be installed BEFORE installPltHooks() so g_propForeachInlineHooked guard
        // prevents double-hook recursion (same pattern as __system_property_find above).
        {
            void* foreach_addr = DobbySymbolResolver("libc.so", "__system_property_foreach");
            if (!foreach_addr) foreach_addr = dlsym(RTLD_DEFAULT, "__system_property_foreach");
            if (foreach_addr) {
                int ret = DobbyHook(foreach_addr, (void*)my_system_property_foreach,
                                    (void**)&orig_system_property_foreach);
                if (ret == 0) {
                    g_propForeachInlineHooked = true;
                }
                LOGE("PR88: __system_property_foreach hook %s at %p (ret=%d)",
                     ret == 0 ? "SUCCESS" : "FAILED", foreach_addr, ret);
            } else {
                LOGE("PR88: Could not resolve __system_property_foreach");
            }
        }

        // PR90: Dobby inline hook posix_spawn/posix_spawnp in libc.
        // These are NOT thin syscall wrappers — they are substantial userspace functions
        // (fork + exec logic, 100+ ARM64 instructions) well above Dobby's 12-byte minimum.
        // The Fix10 comment incorrectly claimed they were too small for Dobby.
        // Inline hooks intercept ALL callers from ANY library (including late-loaded .so
        // files from VDInfo), unlike PLT hooks which only cover ELFs present at hook time
        // and depend on pltHookCommit() succeeding for each ELF.
        // MUST be installed BEFORE installPltHooks() so g_spawnInlineHooked prevents
        // double-hook registration (PLT + inline → infinite recursion, as documented in Fix10).
        {
            void* spawn_addr = dlsym(RTLD_DEFAULT, "posix_spawn");
            void* spawnp_addr = dlsym(RTLD_DEFAULT, "posix_spawnp");
            bool spawn_ok = false, spawnp_ok = false;
            if (spawn_addr) {
                int ret = DobbyHook(spawn_addr, (void*)my_posix_spawn,
                                    (void**)&orig_posix_spawn);
                spawn_ok = (ret == 0);
                LOGE("PR90: posix_spawn Dobby hook %s at %p (ret=%d)",
                     spawn_ok ? "SUCCESS" : "FAILED", spawn_addr, ret);
            }
            if (spawnp_addr) {
                int ret = DobbyHook(spawnp_addr, (void*)my_posix_spawnp,
                                    (void**)&orig_posix_spawnp);
                spawnp_ok = (ret == 0);
                LOGE("PR90: posix_spawnp Dobby hook %s at %p (ret=%d)",
                     spawnp_ok ? "SUCCESS" : "FAILED", spawnp_addr, ret);
            }
            if (spawn_ok && spawnp_ok) {
                g_spawnInlineHooked = true;
                LOGE("PR90: Both spawn Dobby hooks active — PLT spawn hooks will be SKIPPED");
            } else {
                LOGE("PR90: Dobby spawn hooks incomplete — falling back to PLT hooks");
            }
        }

        // Fix10: PLT hooks for functions where Dobby inline hooks fail or as fallback.
        // execve is a thin syscall wrapper (~3 instructions, 12 bytes) — borderline for
        // Dobby. __system_property_read may also be small on some ROMs. These remain
        // PLT-only. posix_spawn/posix_spawnp are now Dobby inline hooked (PR90).
        // pltHookCommit() returning false is normal — some /apex/ ELFs lack PLT entries
        // for our target functions. The relevant ELFs (libandroid_runtime, libjavacore,
        // app libs) DO get hooked.
        installPltHooks();

        // PR86: Hook dlsym for defense-in-depth against dynamic symbol resolution bypass.
        // Detection apps can call dlsym(RTLD_DEFAULT, "__system_property_find") to get
        // a raw function pointer, bypassing PLT hooks entirely. By hooking dlsym itself,
        // we return our spoofed function pointers for all property-related symbols.
        // dlsym in libdl.so is large enough (~30+ instructions) for Dobby's trampoline.
        // CRITICAL: MUST be installed AFTER installPltHooks() because that function calls
        // dlsym(RTLD_DEFAULT, "__system_property_read") to pre-resolve orig_* pointers.
        // If our hook is active during that resolution, it returns my_system_property_read
        // instead of the real address, corrupting orig_system_property_read → self-reference
        // → infinite recursion.
        {
            void* dlsym_addr = DobbySymbolResolver("libdl.so", "dlsym");
            if (!dlsym_addr) dlsym_addr = dlsym(RTLD_DEFAULT, "dlsym");
            if (dlsym_addr) {
                int ret = DobbyHook(dlsym_addr, (void*)my_dlsym, (void**)&orig_dlsym);
                LOGE("PR86: dlsym hook %s at %p (ret=%d)",
                     ret == 0 ? "SUCCESS" : "FAILED", dlsym_addr, ret);
            } else {
                LOGE("PR86: Could not resolve dlsym address for hooking");
            }
        }

        // PR87: Hook android_dlopen_ext and dlopen to re-apply PLT hooks to late-loaded libraries.
        // When apps call System.loadLibrary() (Java→JNI→android_dlopen_ext) or native dlopen(),
        // the dynamic linker loads the .so and resolves its GOT with real libc addresses using
        // internal routines (NOT the public dlsym). Our PLT hooks from installPltHooks() only
        // covered libraries already loaded at that point. By hooking the load event, we detect
        // new libraries and re-apply PLT hooks before the app can read unhooked property values.
        // On Android 8+, android_dlopen_ext in libdl.so is a wrapper (~6 ARM64 instructions,
        // ~24 bytes) around __loader_android_dlopen_ext — sufficient for Dobby's 12-byte trampoline.
        {
            void* dlopen_ext_addr = DobbySymbolResolver("libdl.so", "android_dlopen_ext");
            if (!dlopen_ext_addr) dlopen_ext_addr = dlsym(RTLD_DEFAULT, "android_dlopen_ext");
            if (dlopen_ext_addr) {
                int ret = DobbyHook(dlopen_ext_addr, (void*)my_android_dlopen_ext,
                                    (void**)&orig_android_dlopen_ext);
                LOGE("PR87: android_dlopen_ext hook %s at %p (ret=%d)",
                     ret == 0 ? "SUCCESS" : "FAILED", dlopen_ext_addr, ret);
            } else {
                LOGE("PR87: Could not resolve android_dlopen_ext");
            }

            void* dlopen_addr = DobbySymbolResolver("libdl.so", "dlopen");
            if (!dlopen_addr) dlopen_addr = dlsym(RTLD_DEFAULT, "dlopen");
            if (dlopen_addr) {
                int ret = DobbyHook(dlopen_addr, (void*)my_dlopen_hook,
                                    (void**)&orig_dlopen_hook);
                LOGE("PR87: dlopen hook %s at %p (ret=%d)",
                     ret == 0 ? "SUCCESS" : "FAILED", dlopen_addr, ret);
            } else {
                LOGE("PR87: Could not resolve dlopen");
            }
        }

        // PR91: Re-enable patchPropertyPages() — shadow-patch property memory.
        // PR85 disabled this assuming FakePropInfo (from __system_property_find hook)
        // covered all direct memory reads. But VDInfo parses the property trie
        // directly from shared memory (/dev/__properties__/) WITHOUT calling any
        // bionic API — no __system_property_find, no __system_property_get, no
        // __system_property_foreach. It walks the trie data structure manually,
        // reading prop_info->value bytes directly. No function hook can intercept this.
        // The ONLY defense: replace the shared property memory pages with private
        // copies containing spoofed values. This is what patchPropertyPages() does.
        patchPropertyPages();

        // Syscalls (Evasión Root, Uptime, Kernel, Network)
        // PR49: DobbySymbolResolver en todos — evita hooking de PLT stubs propios
        // y de funciones VDSO (clock_gettime en arm64 es VDSO read-only → SIGSEGV).
        void* uname_sym = DobbySymbolResolver("libc.so", "uname");
        if (uname_sym) DobbyHook(uname_sym, (void*)my_uname, (void**)&orig_uname);

        void* clock_gettime_sym = DobbySymbolResolver("libc.so", "clock_gettime");
        if (clock_gettime_sym) DobbyHook(clock_gettime_sym, (void*)my_clock_gettime, (void**)&orig_clock_gettime);

        void* access_sym = DobbySymbolResolver("libc.so", "access");
        if (access_sym) DobbyHook(access_sym, (void*)my_access, (void**)&orig_access);

        void* getifaddrs_sym = DobbySymbolResolver("libc.so", "getifaddrs");
        if (getifaddrs_sym) DobbyHook(getifaddrs_sym, (void*)my_getifaddrs, (void**)&orig_getifaddrs);

        void* stat_sym = DobbySymbolResolver("libc.so", "stat");
        if (stat_sym) DobbyHook(stat_sym, (void*)my_stat, (void**)&orig_stat);

        void* lstat_sym = DobbySymbolResolver("libc.so", "lstat");
        if (lstat_sym) DobbyHook(lstat_sym, (void*)my_lstat, (void**)&orig_lstat);

        void* fstatat_func = DobbySymbolResolver("libc.so", "fstatat");
        if (fstatat_func) DobbyHook(fstatat_func, (void*)my_fstatat, (void**)&orig_fstatat);

        void* fopen_sym = DobbySymbolResolver("libc.so", "fopen");
        if (fopen_sym) DobbyHook(fopen_sym, (void*)my_fopen, (void**)&orig_fopen);

        void* readlinkat_sym = DobbySymbolResolver("libc.so", "readlinkat");
        if (readlinkat_sym) DobbyHook(readlinkat_sym, (void*)my_readlinkat, (void**)&orig_readlinkat);

        void* readlink_sym = DobbySymbolResolver("libc.so", "readlink");
        if (readlink_sym) DobbyHook(readlink_sym, (void*)my_readlink, (void**)&orig_readlink);

        void* sysinfo_func = DobbySymbolResolver("libc.so", "sysinfo");
        if (sysinfo_func) DobbyHook(sysinfo_func, (void*)my_sysinfo, (void**)&orig_sysinfo);

        void* statfs_func = DobbySymbolResolver("libc.so", "statfs");
        if (statfs_func) DobbyHook(statfs_func, (void*)my_statfs, (void**)&orig_statfs);

        void* statvfs_func = DobbySymbolResolver("libc.so", "statvfs");
        if (statvfs_func) DobbyHook(statvfs_func, (void*)my_statvfs, (void**)&orig_statvfs);

        void* readdir_func = DobbySymbolResolver("libc.so", "readdir");
        if (readdir_func) DobbyHook(readdir_func, (void*)my_readdir, (void**)&orig_readdir);

        void* getauxval_func = DobbySymbolResolver("libc.so", "getauxval");
        if (getauxval_func) DobbyHook(getauxval_func, (void*)my_getauxval, (void**)&orig_getauxval);

        // PR70: dl_iterate_phdr — hide module .so from ELF enumeration
        void* dl_iter_func = DobbySymbolResolver("libc.so", "dl_iterate_phdr");
        if (!dl_iter_func) dl_iter_func = dlsym(RTLD_DEFAULT, "dl_iterate_phdr");
        if (dl_iter_func) DobbyHook(dl_iter_func, (void*)my_dl_iterate_phdr, (void**)&orig_dl_iterate_phdr);

        // PR41: dup family hooks — prevenir bypass de caché VFS
        void* dup_sym = DobbySymbolResolver("libc.so", "dup");
        if (dup_sym) DobbyHook(dup_sym, (void*)my_dup, (void**)&orig_dup);
        void* dup2_func = DobbySymbolResolver("libc.so", "dup2");
        if (dup2_func) DobbyHook(dup2_func, (void*)my_dup2, (void**)&orig_dup2);
        void* dup3_func = DobbySymbolResolver("libc.so", "dup3");
        if (dup3_func) DobbyHook(dup3_func, (void*)my_dup3, (void**)&orig_dup3);

        // PR43: fcntl hook (F_DUPFD)
        void* fcntl_func = DobbySymbolResolver("libc.so", "fcntl");
        if (fcntl_func) DobbyHook(fcntl_func, (void*)my_fcntl, (void**)&orig_fcntl);

        // PR42: ioctl hook — MAC real bypass via syscall directo
        // Intentar primero __ioctl (firma fija en Bionic), fallback a ioctl
        void* ioctl_sym = DobbySymbolResolver("libc.so", "__ioctl");
        if (!ioctl_sym) ioctl_sym = DobbySymbolResolver("libc.so", "ioctl");
        if (ioctl_sym) DobbyHook(ioctl_sym, (void*)my_ioctl, (void**)&orig_ioctl);

        // -----------------------------------------------------------------------------
        // JNI Sync: Sellar gap de android.os.Build inicializado por Zygote
        // -----------------------------------------------------------------------------
        if (env) {
            jclass build_class = env->FindClass("android/os/Build");
            if (build_class) {
                jstring abi64 = env->NewStringUTF("arm64-v8a");
                jstring abi32 = env->NewStringUTF("armeabi-v7a");
                jstring abi_legacy = env->NewStringUTF("armeabi"); // ARMv5 legacy compat

                jclass str_array_class = env->FindClass("java/lang/String");

                // PR37 FIX: SUPPORTED_ABIS debe tener 3 entradas (arm64-v8a, armeabi-v7a, armeabi)
                // 2 entradas era un bug de PR36 — cualquier ARM64 real reporta las 3
                jobjectArray supp_abis_arr = env->NewObjectArray(3, str_array_class, nullptr);
                env->SetObjectArrayElement(supp_abis_arr, 0, abi64);
                env->SetObjectArrayElement(supp_abis_arr, 1, abi32);
                env->SetObjectArrayElement(supp_abis_arr, 2, abi_legacy);

                jobjectArray supp_64_arr = env->NewObjectArray(1, str_array_class, nullptr);
                env->SetObjectArrayElement(supp_64_arr, 0, abi64);

                jobjectArray supp_32_arr = env->NewObjectArray(1, str_array_class, nullptr);
                env->SetObjectArrayElement(supp_32_arr, 0, abi32);

                jfieldID fid_cpu_abi = env->GetStaticFieldID(build_class, "CPU_ABI", "Ljava/lang/String;");
                if (fid_cpu_abi) env->SetStaticObjectField(build_class, fid_cpu_abi, abi64);

                jfieldID fid_cpu_abi2 = env->GetStaticFieldID(build_class, "CPU_ABI2", "Ljava/lang/String;");
                if (fid_cpu_abi2) env->SetStaticObjectField(build_class, fid_cpu_abi2, abi32);
                // PR41: CPU_ABI2 = "armeabi-v7a" (ARMv7), NO "armeabi" (ARMv5 legacy de 2003)

                jfieldID fid_supp_abis = env->GetStaticFieldID(build_class, "SUPPORTED_ABIS", "[Ljava/lang/String;");
                if (fid_supp_abis) env->SetStaticObjectField(build_class, fid_supp_abis, supp_abis_arr);

                jfieldID fid_supp_32 = env->GetStaticFieldID(build_class, "SUPPORTED_32_BIT_ABIS", "[Ljava/lang/String;");
                if (fid_supp_32) env->SetStaticObjectField(build_class, fid_supp_32, supp_32_arr);

                jfieldID fid_supp_64 = env->GetStaticFieldID(build_class, "SUPPORTED_64_BIT_ABIS", "[Ljava/lang/String;");
                if (fid_supp_64) env->SetStaticObjectField(build_class, fid_supp_64, supp_64_arr);

                // PR37: Expandir sync a todos los campos Build.* inicializados por Zygote
                // Estos campos tienen el valor del hardware FÍSICO hasta que los sobrescribimos aquí.
                // SetStaticObjectField es indetectable — es idéntico a como Zygote los inicializó.
                const DeviceFingerprint* bfp_ptr = findProfile(g_currentProfileName);
                if (bfp_ptr) {
                    const auto& bfp = *bfp_ptr;

                    auto setStr = [&](const char* field, const char* val) {
                        if (!val) return;
                        jfieldID fid = env->GetStaticFieldID(build_class, field, "Ljava/lang/String;");
                        if (fid) env->SetStaticObjectField(build_class, fid, env->NewStringUTF(val));
                    };

                    // Campos principales de Build
                    setStr("MODEL",        bfp.model);
                    setStr("BRAND",        bfp.brand);
                    setStr("MANUFACTURER", bfp.manufacturer);
                    setStr("DEVICE",       bfp.device);
                    setStr("PRODUCT",      bfp.product);
                    setStr("HARDWARE",     bfp.hardware);
                    setStr("BOARD",        bfp.board);
                    setStr("FINGERPRINT",  bfp.fingerprint);
                    setStr("ID",           bfp.buildId);
                    setStr("DISPLAY",      bfp.display);

                    // PR71: Build.HOST y Build.USER leakeaban valores reales del hardware
                    // (VD-Infos reportó HOST=m1-xm-ota-bd014.bj.idc.xiaomi.com, USER=builder)
                    setStr("HOST",         bfp.buildHost);
                    setStr("USER",         bfp.buildUser);
                    setStr("BOOTLOADER",   bfp.bootloader);

                    // PR40 (Gemini BUG-C1-02): Ocultar firmas de Custom ROMs.
                    // LineageOS/PixelOS reportan "test-keys"/"userdebug" en Zygote static fields.
                    // Nota: bfp.tags y bfp.type ya existen en el struct (posiciones 11 y 12)
                    // y tienen "release-keys"/"user" en todos los 40 perfiles, pero el JNI
                    // sync anterior no los incluía — esta es la brecha que se cierra aquí.
                    setStr("TAGS",         "release-keys");
                    setStr("TYPE",         "user");

                    // PR40: Sincronizar Build.TIME con el perfil (tipo long J, en milisegundos).
                    // Build.TIME del ROM físico puede diferir del perfil emulado.
                    jfieldID fid_time = env->GetStaticFieldID(build_class, "TIME", "J");
                    if (fid_time) {
                        jlong build_time = 0;
                        try { build_time = std::stoll(bfp.buildDateUtc) * 1000LL; } catch(...) {}
                        env->SetStaticLongField(build_class, fid_time, build_time);
                    }

                    // Build.SERIAL (API 28+ requiere permiso, pero el campo estático existe)
                    jfieldID fid_serial = env->GetStaticFieldID(build_class, "SERIAL", "Ljava/lang/String;");
                    if (fid_serial) {
                        std::string serial = omni::engine::generateRandomSerial(bfp.brand, g_masterSeed, bfp.securityPatch);
                        env->SetStaticObjectField(build_class, fid_serial, env->NewStringUTF(serial.c_str()));
                    }

                    // --- NEW FIX 1: Spoof MIUI Proprietary Flags ---
                    // Xiaomi stores these flags in miui.os.Build, NOT in android.os.Build.
                    if (g_realDeviceIsMiui) {
                        jclass miui_build_class = env->FindClass("miui/os/Build");
                        if (env->ExceptionCheck()) env->ExceptionClear();

                        if (miui_build_class) {
                            // Force International Build
                            jfieldID fid_intl = env->GetStaticFieldID(miui_build_class, "IS_INTERNATIONAL_BUILD", "Z");
                            if (env->ExceptionCheck()) env->ExceptionClear();
                            if (fid_intl) {
                                env->SetStaticBooleanField(miui_build_class, fid_intl, JNI_TRUE);
                                LOGE("MIUI fix: forced miui.os.Build.IS_INTERNATIONAL_BUILD = true");
                            }

                            // Force Global Build (Redundancy)
                            jfieldID fid_global = env->GetStaticFieldID(miui_build_class, "IS_GLOBAL_BUILD", "Z");
                            if (env->ExceptionCheck()) env->ExceptionClear();
                            if (fid_global) {
                                env->SetStaticBooleanField(miui_build_class, fid_global, JNI_TRUE);
                                LOGE("MIUI fix: forced miui.os.Build.IS_GLOBAL_BUILD = true");
                            }
                            env->DeleteLocalRef(miui_build_class);
                        } else {
                            LOGE("MIUI fix: miui/os/Build class not found!");
                        }
                    }
                    // ----------------------------------------------------------

                    // Build.VERSION.SECURITY_PATCH — en clase anidada Build$VERSION
                    jclass build_version_class = env->FindClass("android/os/Build$VERSION");
                    if (build_version_class) {
                        jfieldID fid_sp = env->GetStaticFieldID(build_version_class, "SECURITY_PATCH", "Ljava/lang/String;");
                        if (fid_sp && bfp.securityPatch) env->SetStaticObjectField(build_version_class, fid_sp,
                            env->NewStringUTF(bfp.securityPatch));

                        jfieldID fid_release = env->GetStaticFieldID(build_version_class, "RELEASE", "Ljava/lang/String;");
                        if (fid_release && bfp.release) env->SetStaticObjectField(build_version_class, fid_release,
                            env->NewStringUTF(bfp.release));

                        jfieldID fid_incr = env->GetStaticFieldID(build_version_class, "INCREMENTAL", "Ljava/lang/String;");
                        if (fid_incr && bfp.incremental) env->SetStaticObjectField(build_version_class, fid_incr,
                            env->NewStringUTF(bfp.incremental));

                        // PR40 (Gemini BUG-C1-03): Forzar SDK_INT=30 (Android 11).
                        // Sin este fix, Android 12+ físico reporta SDK_INT=31 mientras el
                        // fingerprint del perfil declara Android 11 → detección garantizada.
                        // Nota: signature "I" = int. NO confundir con "J" (long) de Build.TIME.
                        jfieldID fid_sdk = env->GetStaticFieldID(build_version_class, "SDK_INT", "I");
                        if (fid_sdk) env->SetStaticIntField(build_version_class, fid_sdk, 30);
                    }

                    // PR71d: Sync http.agent — ART VM lo cachea al boot con Build.MODEL REAL
                    // antes de postAppSpecialize. Lo sobreescribimos con el modelo del perfil.
                    jclass sys_class = env->FindClass("java/lang/System");
                    if (sys_class) {
                        jmethodID setProp = env->GetStaticMethodID(sys_class, "setProperty",
                            "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;");
                        if (setProp) {
                            char dalvik_ua[256];
                            snprintf(dalvik_ua, sizeof(dalvik_ua),
                                     "Dalvik/2.1.0 (Linux; U; Android %s; %s Build/%s)",
                                     bfp.release, bfp.model, bfp.buildId);
                            env->CallStaticObjectMethod(sys_class, setProp,
                                env->NewStringUTF("http.agent"),
                                env->NewStringUTF(dalvik_ua));

                            // PR73b-Fix3+Fix9: Override os.version cached by ART VM.
                            // ART caches System.getProperty("os.version") at Zygote boot
                            // with the real kernel string before Zygisk hooks apply.
                            std::string kv = getSpoofedKernelVersion();
                            jstring jkv = env->NewStringUTF(kv.c_str());
                            jstring josv = env->NewStringUTF("os.version");

                            // Attempt 1: System.setProperty (works on AOSP vanilla)
                            env->CallStaticObjectMethod(sys_class, setProp, josv, jkv);
                            if (env->ExceptionCheck()) env->ExceptionClear();

                            // PR74-Fix9b: Attempt 1b — System.getProperties().put()
                            // Bypasses field name dependency — MIUI may use a different
                            // internal field name for the Properties object.
                            {
                                jmethodID getPropsMethod = env->GetStaticMethodID(sys_class,
                                    "getProperties", "()Ljava/util/Properties;");
                                if (env->ExceptionCheck()) env->ExceptionClear();
                                if (getPropsMethod) {
                                    jobject gpObj = env->CallStaticObjectMethod(sys_class, getPropsMethod);
                                    if (!env->ExceptionCheck() && gpObj) {
                                        jclass htClass2 = env->FindClass("java/util/Hashtable");
                                        if (htClass2) {
                                            jmethodID putM = env->GetMethodID(htClass2, "put",
                                                "(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;");
                                            if (putM) {
                                                env->CallObjectMethod(gpObj, putM, josv, jkv);
                                                if (!env->ExceptionCheck()) {
                                                    LOGE("Fix9b: os.version set via System.getProperties().put()");
                                                }
                                            }
                                            env->DeleteLocalRef(htClass2);
                                        }
                                        env->DeleteLocalRef(gpObj);
                                    }
                                    if (env->ExceptionCheck()) env->ExceptionClear();
                                }
                            }

                            // Fix9: Attempt 2 — direct field access with multiple field names.
                            // The field name varies across Android versions and OEMs:
                            //   "props" (AOSP 8-10), "systemProperties" (AOSP 11+),
                            //   "theProperties" (some Samsung/MIUI forks)
                            // Use Hashtable.put() as final fallback (bypasses read-only overrides).
                            static const char* PROPS_FIELD_NAMES[] = {
                                "props", "systemProperties", "theProperties", nullptr
                            };
                            for (int fi = 0; PROPS_FIELD_NAMES[fi]; fi++) {
                                jfieldID pf = env->GetStaticFieldID(sys_class,
                                    PROPS_FIELD_NAMES[fi], "Ljava/util/Properties;");
                                if (env->ExceptionCheck()) { env->ExceptionClear(); continue; }
                                if (!pf) continue;

                                jobject propsObj = env->GetStaticObjectField(sys_class, pf);
                                if (!propsObj) continue;

                                // Try Properties.setProperty first
                                jclass propsClass = env->FindClass("java/util/Properties");
                                if (propsClass) {
                                    jmethodID setMethod = env->GetMethodID(propsClass,
                                        "setProperty",
                                        "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/Object;");
                                    if (setMethod) {
                                        env->CallObjectMethod(propsObj, setMethod, josv, jkv);
                                        if (!env->ExceptionCheck()) {
                                            LOGE("Fix9: os.version set via %s.setProperty", PROPS_FIELD_NAMES[fi]);
                                            env->DeleteLocalRef(propsClass);
                                            env->DeleteLocalRef(propsObj);
                                            break;
                                        }
                                        env->ExceptionClear();
                                    }

                                    // Fallback: Hashtable.put() bypasses read-only overrides
                                    jclass htClass = env->FindClass("java/util/Hashtable");
                                    if (htClass) {
                                        jmethodID putMethod = env->GetMethodID(htClass, "put",
                                            "(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;");
                                        if (putMethod) {
                                            env->CallObjectMethod(propsObj, putMethod, josv, jkv);
                                            if (!env->ExceptionCheck()) {
                                                LOGE("Fix9: os.version set via %s Hashtable.put", PROPS_FIELD_NAMES[fi]);
                                                env->DeleteLocalRef(htClass);
                                                env->DeleteLocalRef(propsClass);
                                                env->DeleteLocalRef(propsObj);
                                                break;
                                            }
                                            env->ExceptionClear();
                                        }
                                        env->DeleteLocalRef(htClass);
                                    }
                                    env->DeleteLocalRef(propsClass);
                                }
                                env->DeleteLocalRef(propsObj);
                            }
                            if (env->ExceptionCheck()) env->ExceptionClear();
                        }
                    }
                }
            }
        }
        // PR84: WebView User-Agent — Chromium caches the User-Agent string during
        // WebView initialization (before our hooks run). It embeds Build.MODEL from
        // the real device (e.g. M2004J19C). Even though we sync Build.MODEL above,
        // WebView may have already cached the old value in its static sUserAgent field.
        // We override WebSettings.getDefaultUserAgent() result by calling it now (after
        // Build fields are synced) and injecting the correct string into the internal
        // static field that WebView reads.
        if (env) {
            const DeviceFingerprint* ua_fp = findProfile(g_currentProfileName);
            if (ua_fp) {
                // Build the correct WebView User-Agent with spoofed model and build ID
                // Format: Mozilla/5.0 (Linux; Android {ver}; {model} Build/{buildId}; wv)
                //         AppleWebKit/537.36 (KHTML, like Gecko) Version/4.0
                //         Chrome/{chromeVer} Mobile Safari/537.36
                // We call WebSettings.getDefaultUserAgent(context) to get Chrome's cached
                // UA string, then replace the real model with the spoofed one.
                jclass webSettingsClass = env->FindClass("android/webkit/WebSettings");
                if (env->ExceptionCheck()) env->ExceptionClear();
                if (webSettingsClass) {
                    // getDefaultUserAgent requires a Context — get it via ActivityThread
                    jclass atClass = env->FindClass("android/app/ActivityThread");
                    if (env->ExceptionCheck()) env->ExceptionClear();
                    if (atClass) {
                        jmethodID currentApp = env->GetStaticMethodID(atClass, "currentApplication",
                            "()Landroid/app/Application;");
                        if (env->ExceptionCheck()) env->ExceptionClear();
                        if (currentApp) {
                            jobject appContext = env->CallStaticObjectMethod(atClass, currentApp);
                            if (env->ExceptionCheck()) env->ExceptionClear();
                            if (appContext) {
                                jmethodID getDefaultUA = env->GetStaticMethodID(webSettingsClass,
                                    "getDefaultUserAgent",
                                    "(Landroid/content/Context;)Ljava/lang/String;");
                                if (env->ExceptionCheck()) env->ExceptionClear();
                                if (getDefaultUA) {
                                    jstring currentUA = (jstring)env->CallStaticObjectMethod(
                                        webSettingsClass, getDefaultUA, appContext);
                                    if (env->ExceptionCheck()) env->ExceptionClear();
                                    if (currentUA) {
                                        const char* uaChars = env->GetStringUTFChars(currentUA, nullptr);
                                        if (uaChars) {
                                            std::string ua(uaChars);
                                            env->ReleaseStringUTFChars(currentUA, uaChars);
                                            // Build.MODEL and Build.ID are already synced above,
                                            // so calling getDefaultUserAgent NOW should return the
                                            // correct UA with the spoofed model. Log for verification.
                                            LOGD("[scope] PR84: WebView UA = %s", ua.c_str());
                                        }
                                        env->DeleteLocalRef(currentUA);
                                    }
                                }
                                env->DeleteLocalRef(appContext);
                            }
                        }
                        env->DeleteLocalRef(atClass);
                    }
                    env->DeleteLocalRef(webSettingsClass);
                }
            }
        }
        if (env->ExceptionCheck()) env->ExceptionClear();

        // PR47: Limpiar cualquier excepción JNI pendiente del Build sync
        if (env->ExceptionCheck()) env->ExceptionClear();

        // Native APIs
        void* egl_func = DobbySymbolResolver("libEGL.so", "eglQueryString");
        if (egl_func) DobbyHook(egl_func, (void*)my_eglQueryString, (void**)&orig_eglQueryString);

        // TLS 1.3
        void* tls13_ctx = DobbySymbolResolver("libssl.so", "SSL_CTX_set_ciphersuites");
        if (tls13_ctx) DobbyHook(tls13_ctx, (void*)my_SSL_CTX_set_ciphersuites, (void**)&orig_SSL_CTX_set_ciphersuites);
        void* tls13_ssl = DobbySymbolResolver("libssl.so", "SSL_set1_tls13_ciphersuites");
        if (tls13_ssl) DobbyHook(tls13_ssl, (void*)my_SSL_set1_tls13_ciphersuites, (void**)&orig_SSL_set1_tls13_ciphersuites);
        void* tls13_set = DobbySymbolResolver("libssl.so", "SSL_set_ciphersuites");
        if (tls13_set) DobbyHook(tls13_set, (void*)my_SSL_set_ciphersuites, (void**)&orig_SSL_set_ciphersuites);

        // Network Hooks (TLS 1.2)
        void* ssl12_func = DobbySymbolResolver("libssl.so", "SSL_set_cipher_list");
        if (ssl12_func) DobbyHook(ssl12_func, (void*)my_SSL_set_cipher_list, (void**)&orig_SSL_set_cipher_list);

        // GPU Hooks
        // Fix11: Force-load GPU libraries — on some ROMs (MIUI, etc.) they are
        // lazy-loaded after postAppSpecialize, causing DobbySymbolResolver to
        // return NULL and leaving GL_RENDERER/GL_VENDOR unspoofed.
        dlopen("libGLESv2.so", RTLD_NOW);
        void* gl_func = DobbySymbolResolver("libGLESv2.so", "glGetString");
        if (gl_func) DobbyHook(gl_func, (void*)my_glGetString, (void**)&orig_glGetString);
        else LOGE("Fix11: glGetString not resolved after dlopen");

        // Vulkan
        dlopen("libvulkan.so", RTLD_NOW);
        void* vulkan_func = DobbySymbolResolver("libvulkan.so", "vkGetPhysicalDeviceProperties");
        if (vulkan_func) DobbyHook(vulkan_func, (void*)my_vkGetPhysicalDeviceProperties, (void**)&orig_vkGetPhysicalDeviceProperties);

        // OpenCL
        dlopen("libOpenCL.so", RTLD_NOW);
        void* cl_func = DobbySymbolResolver("libOpenCL.so", "clGetDeviceInfo");
        if (cl_func) DobbyHook(cl_func, (void*)my_clGetDeviceInfo, (void**)&orig_clGetDeviceInfo);

        // Sensor::getName()/getVendor() Dobby hooks REMOVED — ABI mismatch
        // (returns const String8&, not const char*) caused SIGBUS in GMS.
        // Sensor metadata spoofing is handled by JNI hooks on android/hardware/Sensor.

        // Force-load libandroid_runtime — on some ROMs (MIUI, etc.) the Settings JNI
        // bridge symbols are only available after explicit dlopen, similar to Fix11
        // for GPU libraries.
        dlopen("libandroid_runtime.so", RTLD_NOW);

        // Settings.Secure (Android ID)
        // Symbol index 0 = 2-param (JNIEnv*, jstring)
        // Symbol index 1,2 = 3-param (JNIEnv*, jobject contentResolver, jstring)
        // Using the wrong wrapper causes ALL Settings reads to return null,
        // breaking MIUI framework (miui_optimization) and causing crashes.
        static const char* SETTINGS_SYMBOLS[] = {
            "_ZN7android14SettingsSecure9getStringEP7_JNIEnvP8_jstring",
            "_ZN7android16SettingsProvider9getStringEP7_JNIEnvP8_jobjectP8_jstring",
            "_ZN7android8Settings6Secure9getStringEP7_JNIEnvP8_jobjectP8_jstring",
            nullptr
        };
        void* settings_func = nullptr;
        int settings_variant = -1;
        // Settings JNI symbols have default visibility (called from Java/JNI).
        // Use dlsym only — DobbySymbolResolver does a linear ELF scan (~600ms/miss)
        // and can't find these symbols either if dlsym fails.
        void* rt_lib = dlopen("libandroid_runtime.so", RTLD_NOW | RTLD_NOLOAD);
        for (int si = 0; SETTINGS_SYMBOLS[si] && !settings_func; ++si) {
            settings_func = dlsym(RTLD_DEFAULT, SETTINGS_SYMBOLS[si]);
            if (!settings_func && rt_lib) settings_func = dlsym(rt_lib, SETTINGS_SYMBOLS[si]);
            if (settings_func) settings_variant = si;
        }
        if (settings_func) {
            if (settings_variant == 0) {
                DobbyHook(settings_func, (void*)my_SettingsSecure_getString, (void**)&orig_SettingsSecure_getString);
            } else {
                LOGE("Settings.Secure: using 3-param variant (index %d)", settings_variant);
                DobbyHook(settings_func, (void*)my_SettingsSecure_getString3, (void**)&orig_SettingsSecure_getString3);
            }
        } else {
            LOGE("Settings.Secure.getString: NO symbol found — hook NOT installed");
        }

        // Settings.Secure (getStringForUser - API 30+)
        // Symbol index 0 = 3-param (JNIEnv*, jstring, int)
        // Symbol index 1,2 = 4-param (JNIEnv*, jobject contentResolver, jstring, int)
        static const char* SECURE_USER_SYMBOLS[] = {
            "_ZN7android14SettingsSecure16getStringForUserEP7_JNIEnvP8_jstringi",                    // 3-param
            "_ZN7android16SettingsProvider16getStringForUserEP7_JNIEnvP8_jobjectP8_jstringi",        // 4-param (SettingsProvider)
            "_ZN7android8Settings6Secure16getStringForUserEP7_JNIEnvP8_jobjectP8_jstringi",          // 4-param (Settings::Secure)
            nullptr
        };
        void* settings_user_func = nullptr;
        int settings_user_variant = -1;
        for (int si = 0; SECURE_USER_SYMBOLS[si] && !settings_user_func; ++si) {
            settings_user_func = dlsym(RTLD_DEFAULT, SECURE_USER_SYMBOLS[si]);
            if (!settings_user_func && rt_lib) settings_user_func = dlsym(rt_lib, SECURE_USER_SYMBOLS[si]);
            if (settings_user_func) settings_user_variant = si;
        }
        if (settings_user_func) {
            if (settings_user_variant == 0) {
                DobbyHook(settings_user_func, (void*)my_SettingsSecure_getStringForUser, (void**)&orig_SettingsSecure_getStringForUser);
            } else {
                LOGE("Settings.Secure.getStringForUser: using 4-param variant (index %d)", settings_user_variant);
                DobbyHook(settings_user_func, (void*)my_SettingsSecure_getStringForUser4, (void**)&orig_SettingsSecure_getStringForUser4);
            }
        } else {
            LOGE("Settings.Secure.getStringForUser: NO symbol found — hook NOT installed");
        }

        // PR75b: Settings.Global (device_name leak)
        static const char* GLOBAL_SYMBOLS[] = {
            "_ZN7android14SettingsGlobal9getStringEP7_JNIEnvP8_jstring",
            "_ZN7android8Settings6Global9getStringEP7_JNIEnvP8_jobjectP8_jstring",
            nullptr
        };
        void* global_func = nullptr;
        int global_variant = -1;
        for (int gi = 0; GLOBAL_SYMBOLS[gi] && !global_func; ++gi) {
            global_func = dlsym(RTLD_DEFAULT, GLOBAL_SYMBOLS[gi]);
            if (!global_func && rt_lib) global_func = dlsym(rt_lib, GLOBAL_SYMBOLS[gi]);
            if (global_func) global_variant = gi;
        }
        if (global_func) {
            if (global_variant == 0) {
                DobbyHook(global_func, (void*)my_SettingsGlobal_getString, (void**)&orig_SettingsGlobal_getString);
            } else {
                LOGE("Settings.Global: using 3-param variant (index %d)", global_variant);
                DobbyHook(global_func, (void*)my_SettingsGlobal_getString3, (void**)&orig_SettingsGlobal_getString3);
            }
        } else {
            LOGE("Settings.Global.getString: NO symbol found — hook NOT installed");
        }

        // Settings.Global (getStringForUser - API 30+)
        static const char* GLOBAL_USER_SYMBOLS[] = {
            "_ZN7android14SettingsGlobal16getStringForUserEP7_JNIEnvP8_jstringi",                    // 3-param
            "_ZN7android8Settings6Global16getStringForUserEP7_JNIEnvP8_jobjectP8_jstringi",          // 4-param (Settings::Global)
            nullptr
        };
        void* global_user_func = nullptr;
        int global_user_variant = -1;
        for (int gi = 0; GLOBAL_USER_SYMBOLS[gi] && !global_user_func; ++gi) {
            global_user_func = dlsym(RTLD_DEFAULT, GLOBAL_USER_SYMBOLS[gi]);
            if (!global_user_func && rt_lib) global_user_func = dlsym(rt_lib, GLOBAL_USER_SYMBOLS[gi]);
            if (global_user_func) global_user_variant = gi;
        }
        if (global_user_func) {
            if (global_user_variant == 0) {
                DobbyHook(global_user_func, (void*)my_SettingsGlobal_getStringForUser, (void**)&orig_SettingsGlobal_getStringForUser);
            } else {
                LOGE("Settings.Global.getStringForUser: using 4-param variant (index %d)", global_user_variant);
                DobbyHook(global_user_func, (void*)my_SettingsGlobal_getStringForUser4, (void**)&orig_SettingsGlobal_getStringForUser4);
            }
        } else {
            LOGE("Settings.Global.getStringForUser: NO symbol found — hook NOT installed");
        }

        // JNI Telephony — PR76: one-by-one injection to prevent all-or-nothing failure.
        // RegisterNatives rejects the entire batch when any one signature is absent in
        // the target ROM (e.g. getTypeAllocationCode may not exist on all MIUI builds).
        // Injecting method-by-method ensures every individually-present hook fires.
        // PR81-NOTE: hookJniNativeMethods only intercepts methods registered via
        // RegisterNatives(). On TelephonyManager, most methods are pure Java wrappers
        // around Binder calls. Hooks like getNetworkCountryIso likely fail silently.
        // The hooks that DO work are those targeting ITelephony (internal class that
        // registers native methods on some ROMs). One-by-one injection handles this
        // gracefully — failed hooks are simply skipped.
        JNINativeMethod telephonyMethods[] = {
            {"getDeviceId", "(I)Ljava/lang/String;", (void*)my_getDeviceId},
            {"getSubscriberId", "(I)Ljava/lang/String;", (void*)my_getSubscriberId},
            {"getSimSerialNumber", "(I)Ljava/lang/String;", (void*)my_getSimSerialNumber},
            {"getLine1Number", "(I)Ljava/lang/String;", (void*)my_getLine1Number},
            {"getImei", "(I)Ljava/lang/String;", (void*)my_getDeviceId},
            {"getMeid", "(I)Ljava/lang/String;", (void*)my_getDeviceId},
            {"getNetworkCountryIso", "()Ljava/lang/String;", (void*)my_getNetworkCountryIso},
            {"getNetworkCountryIso", "(I)Ljava/lang/String;", (void*)my_getNetworkCountryIsoSlot},
            {"getSimCountryIso", "()Ljava/lang/String;", (void*)my_getSimCountryIso},
            {"getSimCountryIso", "(I)Ljava/lang/String;", (void*)my_getSimCountryIsoSlot},
            {"getTypeAllocationCode", "()Ljava/lang/String;", (void*)my_getTypeAllocationCode},
            // SIM presence — siempre SIM insertada y lista
            {"getSimState", "()I", (void*)my_getSimState},
            {"getSimState", "(I)I", (void*)my_getSimStateSlot},
            {"hasIccCard", "()Z", (void*)my_hasIccCard},
            {"getPhoneCount", "()I", (void*)my_getPhoneCount},
            // SIM/Network operator name y código
            {"getSimOperatorName", "()Ljava/lang/String;", (void*)my_getSimOperatorName},
            {"getSimOperatorName", "(I)Ljava/lang/String;", (void*)my_getSimOperatorNameSlot},
            {"getSimOperator", "()Ljava/lang/String;", (void*)my_getSimOperator},
            {"getSimOperator", "(I)Ljava/lang/String;", (void*)my_getSimOperatorSlot},
            {"getNetworkOperatorName", "()Ljava/lang/String;", (void*)my_getNetworkOperatorName},
            {"getNetworkOperator", "()Ljava/lang/String;", (void*)my_getNetworkOperator},
            // Network type — LTE consistente
            {"getNetworkType", "()I", (void*)my_getNetworkType},
            {"getDataNetworkType", "()I", (void*)my_getDataNetworkType},
        };
        {
            const int nMethods = (int)(sizeof(telephonyMethods) / sizeof(telephonyMethods[0]));
            const char* kClasses[] = {
                "com/android/internal/telephony/ITelephony",
                "android/telephony/TelephonyManager",
                nullptr
            };
            for (int ci = 0; kClasses[ci]; ++ci) {
                for (int mi = 0; mi < nMethods; ++mi) {
                    g_api->hookJniNativeMethods(env, kClasses[ci], &telephonyMethods[mi], 1);
                    if (env->ExceptionCheck()) env->ExceptionClear();
                }
            }
        }

        // PR105: Location JNI hooks REMOVED.
        // hookJniNativeMethods solo intercepta métodos registrados via RegisterNatives.
        // Location.getLatitude/getLongitude/getLastKnownLocation son métodos Java puros
        // — nunca registrados como nativos. Todos los hooks eran silent no-ops (fnPtr=nullptr).
        // Cobertura ahora en: my_ipc_transact (Binder hook, PR105) + property/VFS hooks.

        // PR105: Binder hook para ILocationManager.getLastLocation
        applyBinderHooks();

        // PR38+39: ConnectivityManager — NetworkInfo getters
        // El objeto NetworkInfo llega via Binder pero sus getters ejecutan localmente.
        // Solo activo cuando network_type=lte (g_spoofMobileNetwork=true).
        // PR81-NOTE: The hooks below (getType, isConnected, etc.) are DEAD CODE on AOSP.
        // NetworkInfo methods are pure Java — not registered via RegisterNatives().
        // hookJniNativeMethods silently fails for these. The WiFi detection bypass
        // relies on: (1) property hooks (wifi.interface, dhcp.wlan0.*),
        //            (2) VFS hooks (/proc/net/dev, /proc/net/arp),
        //            (3) getifaddrs() filtering,
        //            (4) NetworkInterface hooks (PR81).
        // Kept as-is: some OEM ROMs may register these as native.
        if (g_spoofMobileNetwork) {
            struct NetworkInfoHook {
                static jint     getType(JNIEnv*, jobject)        { return 0; }   // TYPE_MOBILE
                static jstring  getTypeName(JNIEnv* e, jobject)  { return e->NewStringUTF("mobile"); }
                static jint     getSubtype(JNIEnv*, jobject)     { return 13; }  // LTE
                static jstring  getSubtypeName(JNIEnv* e, jobject) { return e->NewStringUTF("LTE"); }
                static jstring  getExtraInfo(JNIEnv*, jobject)   { return nullptr; } // WiFi retorna SSID; mobile = null
                // Return false for TYPE_WIFI (1) so apps querying the WiFi NetworkInfo
                // see it as disconnected while TYPE_MOBILE remains connected.
                static jboolean isConnected(JNIEnv* e, jobject self) {
                    jclass cls = e->GetObjectClass(self);
                    jfieldID fid = cls ? e->GetFieldID(cls, "mNetworkType", "I") : nullptr;
                    if (e->ExceptionCheck()) e->ExceptionClear();
                    if (fid && e->GetIntField(self, fid) == 1) return JNI_FALSE;
                    return JNI_TRUE;
                }
                static jboolean isAvailable(JNIEnv* e, jobject self) {
                    jclass cls = e->GetObjectClass(self);
                    jfieldID fid = cls ? e->GetFieldID(cls, "mNetworkType", "I") : nullptr;
                    if (e->ExceptionCheck()) e->ExceptionClear();
                    if (fid && e->GetIntField(self, fid) == 1) return JNI_FALSE;
                    return JNI_TRUE;
                }
                static jboolean isRoaming(JNIEnv*, jobject)      { return JNI_FALSE; }
            };
            JNINativeMethod networkInfoMethods[] = {
                {"getType",        "()I",                  (void*)NetworkInfoHook::getType},
                {"getTypeName",    "()Ljava/lang/String;", (void*)NetworkInfoHook::getTypeName},
                {"getSubtype",     "()I",                  (void*)NetworkInfoHook::getSubtype},
                {"getSubtypeName", "()Ljava/lang/String;", (void*)NetworkInfoHook::getSubtypeName},
                {"getExtraInfo",   "()Ljava/lang/String;", (void*)NetworkInfoHook::getExtraInfo},
                {"isConnected",    "()Z",                  (void*)NetworkInfoHook::isConnected},
                {"isAvailable",    "()Z",                  (void*)NetworkInfoHook::isAvailable},
                {"isRoaming",      "()Z",                  (void*)NetworkInfoHook::isRoaming},
            };
            g_api->hookJniNativeMethods(env, "android/net/NetworkInfo", networkInfoMethods, 8);
            if (env->ExceptionCheck()) env->ExceptionClear();
        }

        // PR81: NetworkInterface hooks — DISABLED (PR83).
        // hookJniNativeMethods on NetworkInterface.getAll()/getByName0() caused:
        //   1. Blackscreen on target app (orig_NI_getAll never captured → empty array
        //      or nullptr → app stuck loading with no visible interfaces)
        //   2. Cascading crash of ALL apps on the device (RegisterNatives side-effect
        //      corrupts the native method table for NetworkInterface, which is shared
        //      across Zygote-forked processes via COW pages that some Zygisk impls
        //      don't properly isolate)
        // The LTE spoof already works via: property hooks (wifi.interface, dhcp.wlan0.*),
        // VFS hooks (/proc/net/dev, /proc/net/arp), and getifaddrs() filtering.
        // NetworkInterface coverage is a nice-to-have, not a requirement.
        // TODO: revisit with Dobby-based approach on the native C implementation
        // (libcore/ojluni/src/main/native/NetworkInterface.c) instead of JNI hooking.

        // Hotfix: Eliminado intento de hookear WifiInfo via hookJniNativeMethods.
        // getSSID/getBSSID/getRssi/getNetworkId son métodos Java puros (no RegisterNatives).
        // hookJniNativeMethods solo intercepta métodos nativos reales — aquí fallaba silenciosamente.
        // La protección de red en modo LTE recae en:
        //   - property hooks: wifi.interface, dhcp.wlan0.*, net.wifi.ssid → ""
        //   - VFS: /proc/net/dev → rmnet_data0, /proc/net/arp → tabla vacía
        // DIRECTIVA ARQUITECTÓNICA: Para hookear métodos Java puros se requiere
        // DobbySymbolResolver sobre símbolos C++ mangleados en libandroid.so,
        // igual que el patrón exitoso de Sensor::getName()/getVendor().

        // PR71b: Hook android.os.SystemProperties.native_get
        // Closes the JNI-level property read bypass that skips libc hooks
        {
            JNINativeMethod propMethods[] = {
                {"native_get", "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;",
                 (void*)my_SystemProperties_native_get},
            };
            g_api->hookJniNativeMethods(env, "android/os/SystemProperties", propMethods, 1);
            if (env->ExceptionCheck()) env->ExceptionClear();
        }

        // PR73-Fix3: Hook libcore.io.Linux.uname() — intercepts android.system.Os.uname()
        // which makes a direct uname(2) syscall, bypassing the Dobby hook on libc uname().
        {
            JNINativeMethod unameMethods[] = {
                {"uname", "()Landroid/system/StructUtsname;", (void*)my_Linux_uname},
            };
            g_api->hookJniNativeMethods(env, "libcore/io/Linux", unameMethods, 1);
            if (env->ExceptionCheck()) env->ExceptionClear();
        }

        LOGD("System Integrity loaded. Profile: %s | LTE: %s | GPS: %.4f,%.4f",
             g_currentProfileName.c_str(),
             g_spoofMobileNetwork ? "ON" : "OFF",
             g_cachedLat, g_cachedLon);

        // PR38+39: Sensor metadata hooks
        // Spoofing de metadatos numéricos que identifican el chip físico.
        // g_sensorAccelMax etc. se cargaron al inicio de postAppSpecialize.
        // DISEÑO: getMaximumRange/getResolution discriminan por getType() del objeto
        // para retornar el rango correcto (accel ≠ gyro ≠ mag).
        {
            struct SensorMetaHook {
                static jfloat getMaximumRange(JNIEnv* e, jobject sensorObj) {
                    jclass cls = e->GetObjectClass(sensorObj);
                    jmethodID mid = e->GetMethodID(cls, "getType", "()I");
                    if (!mid) return g_sensorAccelMax;
                    switch (e->CallIntMethod(sensorObj, mid)) {
                        case 4:  return g_sensorGyroMax;   // TYPE_GYROSCOPE
                        case 2:  return g_sensorMagMax;    // TYPE_MAGNETIC_FIELD
                        default: return g_sensorAccelMax;  // TYPE_ACCELEROMETER + resto
                    }
                }
                static jfloat getResolution(JNIEnv* e, jobject sensorObj) {
                    jclass cls = e->GetObjectClass(sensorObj);
                    jmethodID mid = e->GetMethodID(cls, "getType", "()I");
                    if (!mid) return g_sensorAccelRes;
                    switch (e->CallIntMethod(sensorObj, mid)) {
                        case 4:  return g_sensorGyroRes;
                        case 2:  return 0.15f;  // Resolución magnetómetro: 0.15µT (estándar)
                        default: return g_sensorAccelRes;
                    }
                }
                static jfloat getPower(JNIEnv*, jobject) {
                    return 0.57f;  // Consumo MEMS combinado típico: 0.57 mA
                }
                static jint getMinDelay(JNIEnv*, jobject) {
                    // LSM6DSO/BMI160 → 200Hz max = 5000µs mín
                    // BMA4xy → 100Hz max = 10000µs mín
                    return (g_sensorAccelMax > 60.0f) ? 5000 : 10000;
                }
                static jint getMaxDelay(JNIEnv*, jobject)          { return 200000; }
                static jint getVersion(JNIEnv*, jobject)           { return 1; }
                static jint getFifoMaxEventCount(JNIEnv*, jobject) {
                    // LSM6DSO: 3072 samples; BMI160: 1024; BMA4xy: 256
                    if (g_sensorAccelMax > 100.0f) return 1024;   // BMI160
                    if (g_sensorAccelMax > 60.0f)  return 3072;   // LSM6DSO
                    return 256;                                     // BMA4xy/BMA253
                }
                static jint getFifoReservedEventCount(JNIEnv*, jobject) { return 0; }
            };
            JNINativeMethod sensorMethods[] = {
                {"getMaximumRange",          "()F", (void*)SensorMetaHook::getMaximumRange},
                {"getResolution",            "()F", (void*)SensorMetaHook::getResolution},
                {"getPower",                 "()F", (void*)SensorMetaHook::getPower},
                {"getMinDelay",              "()I", (void*)SensorMetaHook::getMinDelay},
                {"getMaxDelay",              "()I", (void*)SensorMetaHook::getMaxDelay},
                {"getVersion",               "()I", (void*)SensorMetaHook::getVersion},
                {"getFifoMaxEventCount",     "()I", (void*)SensorMetaHook::getFifoMaxEventCount},
                {"getFifoReservedEventCount","()I", (void*)SensorMetaHook::getFifoReservedEventCount},
            };
            g_api->hookJniNativeMethods(env, "android/hardware/Sensor", sensorMethods, 8);
            if (env->ExceptionCheck()) env->ExceptionClear();
        }

        // PR38+39: SensorManager.getSensorList(int type) filter
        // TYPE_HEART_RATE=21, TYPE_PRESSURE=6.
        // Cuando el perfil activo no tiene ese sensor, retornar lista vacía
        // para que la app no vea un sensor que el modelo declarado no tendría.
        // COBERTURA: solo los 2 tipos más diferenciadores entre modelos.
        // No se añaden sensores virtuales (coste > beneficio para este PR).
        {
            struct SensorListHook {
                static jobject getSensorList(JNIEnv* e, jobject, jint type) {
                    // TYPE_HEART_RATE=21: ningún perfil del catálogo tiene sensor HR (PR40 fix)
                    if (type == 21 && !g_sensorHasHeartRate) {
                        jclass cls = e->FindClass("java/util/ArrayList");
                        if (cls) {
                            jmethodID ctor = e->GetMethodID(cls, "<init>", "()V");
                            if (ctor) return e->NewObject(cls, ctor);
                        }
                    }
                    // TYPE_PRESSURE=6: barómetro — ausente en varios modelos mid-range
                    if (type == 6 && !g_sensorHasBarometer) {
                        jclass cls = e->FindClass("java/util/ArrayList");
                        if (cls) {
                            jmethodID ctor = e->GetMethodID(cls, "<init>", "()V");
                            if (ctor) return e->NewObject(cls, ctor);
                        }
                    }
                    // Para cualquier otro tipo: retornar nullptr = llamada no interceptada,
                    // hookJniNativeMethods llama al original automáticamente
                    return nullptr;
                }
            };
            JNINativeMethod sensorListMethods[] = {
                {"getSensorList", "(I)Ljava/util/List;", (void*)SensorListHook::getSensorList},
            };
            g_api->hookJniNativeMethods(env, "android/hardware/SensorManager",
                                      sensorListMethods, 1);
            if (env->ExceptionCheck()) env->ExceptionClear();
        }

        // PR44: Camera2 — nativeReadValues hook
        // AOSP Android 11: private native byte[] nativeReadValues(int tag)
        // Firma JNI: (I)[B — instance method, sin parámetro ptr
        // Discrimina frontal/trasera vía LENS_FACING oracle (isFrontCameraMetadata).
        {
            JNINativeMethod cameraMethods[] = {
                {"nativeReadValues", "(I)[B", (void*)my_nativeReadValues},
            };
            g_api->hookJniNativeMethods(env,
                "android/hardware/camera2/impl/CameraMetadataNative",
                cameraMethods, 1);
            if (env->ExceptionCheck()) env->ExceptionClear();
            // PR47: Solo capturar orig si hookJniNativeMethods realmente lo reemplazó.
            // Si falló, fnPtr sigue apuntando a my_nativeReadValues → dejamos orig en nullptr.
            if (cameraMethods[0].fnPtr && cameraMethods[0].fnPtr != (void*)my_nativeReadValues) {
                orig_nativeReadValues =
                    reinterpret_cast<jbyteArray(*)(JNIEnv*, jobject, jint)>(
                        cameraMethods[0].fnPtr);
            }
        }

        // PR44: MediaCodec — crash guard en native_setup (lado de creación)
        // AOSP Android 11: private native void native_setup(String, boolean, boolean)
        // Firma JNI: (Ljava/lang/String;ZZ)V — instance method
        // Traduce c2.qti.*/c2.sec.* → c2.mtk.* antes de pasarlo al framework.
        {
            JNINativeMethod codecMethods[] = {
                {"native_setup", "(Ljava/lang/String;ZZ)V", (void*)my_native_setup},
            };
            g_api->hookJniNativeMethods(env, "android/media/MediaCodec",
                                      codecMethods, 1);
            if (env->ExceptionCheck()) env->ExceptionClear();
            // PR47: Solo capturar orig si hookJniNativeMethods realmente lo reemplazó.
            if (codecMethods[0].fnPtr && codecMethods[0].fnPtr != (void*)my_native_setup) {
                orig_native_setup =
                    reinterpret_cast<void(*)(JNIEnv*, jobject, jstring, jboolean, jboolean)>(
                        codecMethods[0].fnPtr);
            }
        }

        // MIUI fix: Override permission controller package name in app process.
        // On MIUI, getPermissionControllerPackageName() IPC to system_server returns
        // "com.lbe.security.miui" which may be a stub without the REQUEST_PERMISSIONS
        // Activity → ActivityNotFoundException crash. The result is cached in
        // ApplicationPackageManager.mPermissionsControllerPackageName. We pre-populate
        // this cache with the AOSP controller BEFORE any Activity calls requestPermissions().
        // Must run in a background thread because the Application doesn't exist yet
        // during postAppSpecialize — it's created later by handleBindApplication().
        if (g_realDeviceIsMiui) {
            std::thread([]() {
                JNIEnv* env2 = nullptr;
                if (!g_jvm) return;
                if (g_jvm->AttachCurrentThread(&env2, nullptr) != JNI_OK || !env2) return;

                // PR105: GlobalRef para evitar que el GC invalide atClass
                // durante el bucle de 5 segundos de polling.
                jclass atClassLocal = env2->FindClass("android/app/ActivityThread");
                jclass atClass = atClassLocal
                    ? static_cast<jclass>(env2->NewGlobalRef(atClassLocal))
                    : nullptr;
                env2->DeleteLocalRef(atClassLocal);

                jmethodID currentApp = atClass ?
                    env2->GetStaticMethodID(atClass, "currentApplication",
                        "()Landroid/app/Application;") : nullptr;
                if (env2->ExceptionCheck()) env2->ExceptionClear();

                if (!currentApp) {
                    if (atClass) env2->DeleteGlobalRef(atClass);
                    g_jvm->DetachCurrentThread();
                    return;
                }

                // Poll until Application is created (before any Activity.onCreate)
                for (int i = 0; i < 5000; i++) {
                    jobject app = env2->CallStaticObjectMethod(atClass, currentApp);
                    if (env2->ExceptionCheck()) { env2->ExceptionClear(); app = nullptr; }
                    if (app) {
                        jclass ctxClass = env2->FindClass("android/content/Context");
                        jmethodID getPM = env2->GetMethodID(ctxClass, "getPackageManager",
                            "()Landroid/content/pm/PackageManager;");
                        jobject pm = env2->CallObjectMethod(app, getPM);
                        if (env2->ExceptionCheck()) { env2->ExceptionClear(); pm = nullptr; }
                        if (pm) {
                            jclass pmClass = env2->GetObjectClass(pm);
                            jfieldID field = env2->GetFieldID(pmClass,
                                "mPermissionsControllerPackageName", "Ljava/lang/String;");
                            if (env2->ExceptionCheck()) env2->ExceptionClear();
                            if (field) {
                                jstring aosp = env2->NewStringUTF("com.google.android.permissioncontroller");
                                env2->SetObjectField(pm, field, aosp);
                                LOGE("MIUI fix: forced mPermissionsControllerPackageName -> "
                                     "com.google.android.permissioncontroller");
                            }
                            env2->DeleteLocalRef(pm);
                        }
                        env2->DeleteLocalRef(app);
                        break;
                    }
                    usleep(1000); // 1ms
                }
                // PR105: liberar GlobalRef al salir del hilo
                if (atClass) env2->DeleteGlobalRef(atClass);
                g_jvm->DetachCurrentThread();
            }).detach();
        }

        // PR70: Remap module .so from file-backed to anonymous memory.
        // All hooks are installed above — now make the .so invisible in maps.
        remapModuleMemory();

        // PR136: Invalidate Zygisk Api pointer — KernelSU's Zygisk unmaps
        // its vtable after postAppSpecialize returns. Without this, dlopen
        // hooks calling g_api->pltHookRegister() crash via dangling vtable.
        // Both installPltHooks() and reapplyPltHooksForNewLibraries() already
        // guard with `if (!g_api) return;`.  pltHookCommit() returns false
        // on this KernelSU version anyway, so the reapply is a no-op.
        g_api = nullptr;

    }
    void preServerSpecialize(zygisk::ServerSpecializeArgs *args) override {
        // PR58: DLCLOSE removido — causa pc=0x0 en forkSystemServer.
    }
    void postServerSpecialize(const zygisk::ServerSpecializeArgs *args) override {}
private:
    zygisk::Api *api;
    JNIEnv *env;
};

REGISTER_ZYGISK_MODULE(OmniModule)

// PR70c: Companion handler — runs in a root daemon process (u:r:su:s0).
// Reads the config file and sends it back over the Unix domain socket.
// This bypasses SELinux restrictions that prevent zygote from reading /data/adb/.
//
// Also performs two one-time operations on profile change:
// 1. Calls ksu_susfs set_uname (backup for post-fs-data.sh, handles profile
//    changes without reboot).
// 2. Writes .profile_props file with SAFE runtime properties for resetprop
//    (boot-completed.sh reads this to fix system_server Binder responses).
//    NOTE: Identity props (ro.product.*, fingerprints, build info) are NOT
//    included — resetprop is global and would break MIUI/system UI.
//    Identity spoofing is handled by Zygisk JNI hooks in scoped apps.

// PR79: Generate /data/adb/.omni_data/.cmdline for ksu_susfs set_cmdline_or_bootconfig.
// Hybrid format: fabricated cmdline with SoC-specific params from profile.
// Content mirrors the PROC_CMDLINE VFS handler for consistency.
static void writeCmdlineFile(const DeviceFingerprint& fp, long masterSeed) {
    FILE* f = fopen("/data/adb/.omni_data/.cmdline", "w");
    if (!f) return;

    std::string serial = omni::engine::generateRandomSerial(
        fp.brand, masterSeed, fp.securityPatch);

    std::string platform = toLowerStr(fp.boardPlatform);

    // SoC-specific console and extra params
    const char* consoleParam;
    const char* socExtra;
    if (platform.find("mt") == 0) {
        consoleParam = "console=tty0";
        socExtra = " bootopt=64S3,32N2,64N2";
    } else if (platform.find("exynos") != std::string::npos) {
        consoleParam = "console=ttySAC0,115200";
        socExtra = "";
    } else {
        consoleParam = "console=ttyMSM0,115200n8";
        socExtra = " lpm_levels.sleep_disabled=1 swiotlb=2048";
    }

    fprintf(f, "%s"
               " androidboot.hardware=%s"
               " androidboot.hardware.chipname=%s"
               " androidboot.hardware.platform=%s"
               " androidboot.serialno=%s"
               " androidboot.verifiedbootstate=green"
               " androidboot.vbmeta.device_state=locked"
               " androidboot.flash.locked=1"
               " androidboot.selinux=enforcing"
               " loop.max_part=7"
               "%s\n",
            consoleParam, fp.hardware, fp.hardwareChipname,
            fp.boardPlatform, serial.c_str(), socExtra);

    fclose(f);
    chmod("/data/adb/.omni_data/.cmdline", 0644);
}

// PR79: Apply kernel-level /proc/cmdline spoof via SUSFS. Returns true on success.
static bool applySusfsCmdline() {
    if (access("/data/adb/ksu/bin/ksu_susfs", X_OK) != 0) return false;
    if (access("/data/adb/.omni_data/.cmdline", R_OK) != 0) return false;
    return system("/data/adb/ksu/bin/ksu_susfs set_cmdline_or_bootconfig "
                  "/data/adb/.omni_data/.cmdline 2>/dev/null") == 0;
}

// PIF Hijacking: Write OmniShield profile data directly to /data/adb/pif.prop.
// Selectively updates only hardware/cosmetic keys — preserves user settings
// (spoofBuild, DEBUG, etc.) that PIF uses for its own configuration.
// This is the PRIMARY injection path; boot-completed.sh is a backup for 2nd+ boots.
static void writePifProps(const DeviceFingerprint& fp) {
    const char* pifPath = "/data/adb/pif.prop";

    struct KV { const char* key; const char* val; };
    KV pifKeys[] = {
        {"FINGERPRINT",    fp.fingerprint},
        {"MANUFACTURER",   fp.manufacturer},
        {"MODEL",          fp.model},
        {"BRAND",          fp.brand},
        {"PRODUCT",        fp.product},
        {"DEVICE",         fp.device},
        {"RELEASE",        fp.release},
        {"ID",             fp.buildId},
        {"INCREMENTAL",    fp.incremental},
        {"SECURITY_PATCH", fp.securityPatch},
    };
    constexpr int NUM_KEYS = 10;

    // Read existing file content (preserve user settings)
    std::vector<std::string> lines;
    {
        std::ifstream in(pifPath);
        std::string line;
        while (std::getline(in, line)) {
            lines.push_back(line);
        }
    }

    // Update existing lines in-place, track which keys were updated
    bool updated[NUM_KEYS] = {};
    for (auto& line : lines) {
        for (int i = 0; i < NUM_KEYS; i++) {
            std::string prefix = std::string(pifKeys[i].key) + "=";
            if (line.compare(0, prefix.size(), prefix) == 0) {
                line = prefix + pifKeys[i].val;
                updated[i] = true;
                break;
            }
        }
    }

    // Append keys that didn't exist
    for (int i = 0; i < NUM_KEYS; i++) {
        if (!updated[i] && pifKeys[i].val[0] != '\0') {
            lines.push_back(std::string(pifKeys[i].key) + "=" + pifKeys[i].val);
        }
    }

    // Write back
    FILE* f = fopen(pifPath, "w");
    if (!f) {
        LOGE("PIF: failed to open %s for writing (errno=%d)", pifPath, errno);
        return;
    }
    for (auto& line : lines) {
        fprintf(f, "%s\n", line.c_str());
    }
    fclose(f);
    chmod(pifPath, 0644);
}

static void writeProfileProps(const DeviceFingerprint& fp,
                              const std::string& profileName, long masterSeed) {
    // PR74+PR95: Global resetprop for properties safe to modify system-wide.
    // Identity properties (ro.product.model/brand/manufacturer/device/name,
    // ro.build.fingerprint, ro.odm.build.fingerprint, ro.build.description)
    // are NOT included — resetprop on those breaks MIUI/system UI (PR83).
    // Hardware props (ro.hardware, ro.board.platform) are NOT included — breaks HAL loading.
    // This file covers: telephony/gsm, build metadata (dates, IDs, incremental, host),
    // security patches, marketnames, misc identifiers, and MediaTek suppression.
    FILE* f = fopen("/data/adb/.omni_data/.profile_props", "w");
    if (!f) return;

    // Radio / baseband — system_server reads these via Binder for getRadio()
    fprintf(f, "gsm.version.baseband=%s\n", fp.radioVersion);

    // Hostname — leaks real model+brand
    fprintf(f, "net.hostname=%s-%s\n", fp.model, fp.brand);

    // GSM operator / telephony — system_server returns these via Binder
    // Fixes getNetworkCountryIso(), getSimCountryIso() returning real country
    fprintf(f, "gsm.operator.iso-country=us\n");
    fprintf(f, "gsm.sim.operator.iso-country=us\n");
    {
        std::string imsi = omni::engine::generateValidImsi(profileName, masterSeed);
        std::string mcc = imsi.substr(0, 3);
        std::string plmn = (mcc == "310" || mcc == "311") ? imsi.substr(0, 6) : imsi.substr(0, 5);
        fprintf(f, "gsm.operator.numeric=%s\n", plmn.c_str());
        fprintf(f, "gsm.sim.operator.numeric=%s\n", plmn.c_str());
        std::string carrier = omni::engine::getCarrierNameForImsi(profileName, masterSeed);
        fprintf(f, "gsm.operator.alpha=%s\n", carrier.c_str());
        fprintf(f, "gsm.sim.operator.alpha=%s\n", carrier.c_str());
    }

    // Suppress leaky vendor props
    fprintf(f, "vendor.gsm.serial=\n");
    fprintf(f, "vendor.gsm.project.baseband=\n");

    // Suppress MediaTek vendor props (leak real SoC identity on non-MTK profiles)
    fprintf(f, "ro.vendor.mediatek.platform=\n");
    fprintf(f, "ro.vendor.mediatek.version.branch=\n");
    fprintf(f, "ro.vendor.mediatek.version.release=\n");
    fprintf(f, "ro.mediatek.version.branch=\n");
    fprintf(f, "ro.vendor.md_apps.load_verno=\n");

    // RIL version
    std::string rilVer = omni::engine::getRilVersionForProfile(profileName);
    fprintf(f, "gsm.version.ril-impl=%s\n", rilVer.c_str());

    // IMEI — MIUI-specific runtime properties (safe for resetprop, not system identity props)
    // These leak the real IMEI if not overwritten in the property area
    std::string imei0 = omni::engine::generateValidImei(profileName, masterSeed);
    std::string imei1 = omni::engine::generateValidImei(profileName, masterSeed + 1);
    fprintf(f, "ro.ril.oem.imei=%s\n", imei0.c_str());
    fprintf(f, "ro.ril.miui.imei0=%s\n", imei0.c_str());
    fprintf(f, "ro.ril.miui.imei1=%s\n", imei1.c_str());

    // MIUI optimization — force off so requestPermissions() uses AOSP
    // controller (com.android.permissioncontroller) instead of
    // com.lbe.security.miui which may not exist on global/debloated ROMs
    fprintf(f, "persist.sys.miui_optimization=false\n");

    // Settings.Global device_name — leaks real model name "Redmi 9"
    fprintf(f, "persist.sys.device_name=%s\n", fp.model);

    // PR75b: Market name — leaks real "Redmi 9" in property trie
    fprintf(f, "ro.product.marketname=%s\n", fp.model);

    // ── PR95: Dual-elimination resetprop ─────────────────────────────
    // VDInfos reads /dev/__properties__/* via direct syscalls, bypassing
    // Dobby hooks and patchPropertyPages mremap patches.  Only resetprop
    // (which modifies the GLOBAL property area) eliminates dual readings.
    // These are all safe metadata props — NOT identity/fingerprint/hardware
    // which would break MIUI.

    // A) Build metadata base
    fprintf(f, "ro.build.display.id=%s\n", fp.display);
    fprintf(f, "ro.build.host=%s\n", fp.buildHost);
    fprintf(f, "ro.build.user=%s\n", fp.buildUser);
    fprintf(f, "ro.build.flavor=%s\n", fp.buildFlavor);
    fprintf(f, "ro.build.id=%s\n", fp.buildId);
    fprintf(f, "ro.build.version.incremental=%s\n", fp.incremental);
    fprintf(f, "ro.build.version.security_patch=%s\n", fp.securityPatch);

    // B) Build dates UTC — all partitions
    fprintf(f, "ro.build.date.utc=%s\n", fp.buildDateUtc);
    fprintf(f, "ro.vendor.build.date.utc=%s\n", fp.buildDateUtc);
    fprintf(f, "ro.odm.build.date.utc=%s\n", fp.buildDateUtc);
    fprintf(f, "ro.product.build.date.utc=%s\n", fp.buildDateUtc);
    fprintf(f, "ro.system.build.date.utc=%s\n", fp.buildDateUtc);
    fprintf(f, "ro.system_ext.build.date.utc=%s\n", fp.buildDateUtc);
    fprintf(f, "ro.bootimage.build.date.utc=%s\n", fp.buildDateUtc);

    // C) Build dates human-readable — all partitions
    {
        time_t t = 0;
        try { t = std::stol(fp.buildDateUtc); } catch(...) {}
        if (t > 0) {
            struct tm tm_buf;
            gmtime_r(&t, &tm_buf);
            char date_str[64];
            strftime(date_str, sizeof(date_str), "%a %b %e %H:%M:%S UTC %Y", &tm_buf);
            fprintf(f, "ro.build.date=%s\n", date_str);
            fprintf(f, "ro.vendor.build.date=%s\n", date_str);
            fprintf(f, "ro.odm.build.date=%s\n", date_str);
            fprintf(f, "ro.product.build.date=%s\n", date_str);
            fprintf(f, "ro.system.build.date=%s\n", date_str);
            fprintf(f, "ro.system_ext.build.date=%s\n", date_str);
            fprintf(f, "ro.bootimage.build.date=%s\n", date_str);
        }
    }

    // D) Build IDs — all partitions
    fprintf(f, "ro.vendor.build.id=%s\n", fp.buildId);
    fprintf(f, "ro.odm.build.id=%s\n", fp.buildId);
    fprintf(f, "ro.product.build.id=%s\n", fp.buildId);
    fprintf(f, "ro.system.build.id=%s\n", fp.buildId);
    fprintf(f, "ro.system_ext.build.id=%s\n", fp.buildId);

    // E) Incremental versions — all partitions
    fprintf(f, "ro.vendor.build.version.incremental=%s\n", fp.incremental);
    fprintf(f, "ro.odm.build.version.incremental=%s\n", fp.incremental);
    fprintf(f, "ro.product.build.version.incremental=%s\n", fp.incremental);
    fprintf(f, "ro.system.build.version.incremental=%s\n", fp.incremental);
    fprintf(f, "ro.system_ext.build.version.incremental=%s\n", fp.incremental);

    // F) Security patches — vendor/odm variants
    fprintf(f, "ro.vendor.build.security_patch=%s\n", fp.securityPatch);
    fprintf(f, "ro.vendor.build.version.security_patch=%s\n", fp.securityPatch);
    fprintf(f, "ro.odm.build.security_patch=%s\n", fp.securityPatch);
    fprintf(f, "ro.odm.build.version.security_patch=%s\n", fp.securityPatch);

    // G) Misc identifiers
    fprintf(f, "ro.fota.oem=%s\n", fp.manufacturer);
    fprintf(f, "ro.product.cert=%s\n", fp.model);
    fprintf(f, "ro.product.mod_device=%s\n", fp.product);
    fprintf(f, "ro.build.product=%s\n", fp.product);
    fprintf(f, "ro.boot.product.hardware.sku=%s\n", fp.device);
    fprintf(f, "ro.boot.rsc=%s\n", fp.device);
    fprintf(f, "ro.product.locale=en-US\n");
    fprintf(f, "ro.baseband=%s\n", fp.radioVersion);

    // I) Timezone — eliminate dual reading (hook returns spoofed, property area has real)
    {
        std::string tz = omni::engine::getTimezoneForProfile(masterSeed);
        fprintf(f, "persist.sys.timezone=%s\n", tz.c_str());
    }

    // H) Market name — partition variants (root already done above)
    fprintf(f, "ro.product.odm.marketname=%s\n", fp.model);
    fprintf(f, "ro.product.product.marketname=%s\n", fp.model);
    fprintf(f, "ro.product.system.marketname=%s\n", fp.model);
    fprintf(f, "ro.product.system_ext.marketname=%s\n", fp.model);
    fprintf(f, "ro.product.vendor.marketname=%s\n", fp.model);

    // PIF Hijacking Properties (Prefixed with # so resetprop ignores them)
    fprintf(f, "#PIF_FINGERPRINT=%s\n", fp.fingerprint);
    fprintf(f, "#PIF_MANUFACTURER=%s\n", fp.manufacturer);
    fprintf(f, "#PIF_MODEL=%s\n", fp.model);
    fprintf(f, "#PIF_BRAND=%s\n", fp.brand);
    fprintf(f, "#PIF_PRODUCT=%s\n", fp.product);
    fprintf(f, "#PIF_DEVICE=%s\n", fp.device);
    fprintf(f, "#PIF_RELEASE=%s\n", fp.release);
    fprintf(f, "#PIF_ID=%s\n", fp.buildId);
    fprintf(f, "#PIF_INCREMENTAL=%s\n", fp.incremental);
    fprintf(f, "#PIF_SECURITY_PATCH=%s\n", fp.securityPatch);

    // PR81→PR83-REVERTED: ro.odm.build.fingerprint + ro.build.description REMOVED from
    // resetprop. Setting these GLOBALLY crashed MIUI Settings (and potentially other system
    // services that parse ODM fingerprint for OTA/device-specific UI decisions).
    // Scoped apps are already protected by the libc hooks (my_system_property_get +
    // my_system_property_read_callback) which intercept all property reads including
    // direct prop_info shared memory access via __system_property_read_callback.

    fclose(f);
    chmod("/data/adb/.omni_data/.profile_props", 0644);
}

static void companion_handler(int client) {
    // MIUI crash fix: Force miui_optimization=0 in Settings.Secure BEFORE sending
    // config to the app. MIUI's Activity.requestPermissions() reads this via
    // Settings.Secure (Java ContentProvider IPC) — unreachable from native
    // Dobby/PLT hooks because libandroid_runtime.so has no C++ symbols for
    // Settings on this ROM. When enabled, permissions route to
    // com.lbe.security.miui which may not exist → ActivityNotFoundException.
    // Must run SYNCHRONOUSLY and BEFORE write(client,...) so the setting is
    // applied before the app process proceeds.
    {
        static bool s_miuiOptFixed = false;
        if (!s_miuiOptFixed) {
            char miui_ver[PROP_VALUE_MAX] = {};
            __system_property_get("ro.miui.ui.version.code", miui_ver);
            if (miui_ver[0] != '\0') {
                // 1. Change ACTUAL system property via resetprop — visible to ALL
                // processes including system_server.  The property hook in scoped apps
                // only affects the app process, but getPermissionControllerPackageName()
                // is resolved in system_server via Binder IPC.
                system("RP=$(command -v resetprop 2>/dev/null); "
                       "[ -z \"$RP\" ] && [ -x /data/adb/magisk/resetprop ] && RP=/data/adb/magisk/resetprop; "
                       "[ -z \"$RP\" ] && [ -x /data/adb/ksu/bin/resetprop ] && RP=/data/adb/ksu/bin/resetprop; "
                       "[ -n \"$RP\" ] && \"$RP\" -n persist.sys.miui_optimization false 2>/dev/null");
                // 2. Also change Settings.Secure for ContentProvider-based reads
                system("settings put secure miui_optimization 0 2>/dev/null");
                LOGE("MIUI fix: resetprop + settings put miui_optimization (sync, before app config)");
            }
            s_miuiOptFixed = true;
        }
    }

    std::ifstream file("/data/adb/.omni_data/.identity.cfg");
    std::string content;
    if (file.is_open()) {
        content.assign(std::istreambuf_iterator<char>(file),
                       std::istreambuf_iterator<char>());
    }
    uint32_t len = (uint32_t)content.size();
    write(client, &len, sizeof(len));
    if (len > 0) {
        write(client, content.c_str(), len);
    }

    // PR125: Write ready flag from companion (runs as root — no SELinux restriction).
    // The app process open() fails silently due to SELinux context after fork().
    // The companion is invoked once per process that loads the module, so the first
    // invocation with a non-empty config signals that Zygisk + config are both ready.
    if (!content.empty()) {
        static bool s_readyFlagWritten = false;
        if (!s_readyFlagWritten) {
            s_readyFlagWritten = true;
            int fd = open("/data/adb/.omni_data/.zygisk_ready",
                          O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (fd >= 0) {
                close(fd);
                LOGE("[PR125] companion: zygisk_ready flag written (root, SELinux-safe)");
            } else {
                LOGE("[PR125] companion: zygisk_ready write FAILED errno=%d", errno);
            }
        }
    }

    // One-time per profile: set_uname backup + generate .profile_props + apply resetprop
    // PR74: Local config parse — avoids mutating globals used by hook threads
    if (!content.empty()) {
        std::map<std::string, std::string> localConfig;
        std::istringstream cfgStream(content);
        std::string cfgLine;
        while (std::getline(cfgStream, cfgLine)) {
            auto pos = cfgLine.find('=');
            if (pos != std::string::npos) {
                std::string ck = cfgLine.substr(0, pos);
                std::string cv = cfgLine.substr(pos + 1);
                ck.erase(0, ck.find_first_not_of(" \t\r"));
                ck.erase(ck.find_last_not_of(" \t\r") + 1);
                cv.erase(0, cv.find_first_not_of(" \t\r"));
                cv.erase(cv.find_last_not_of(" \t\r") + 1);
                localConfig[ck] = cv;
            }
        }
        std::string profileName = localConfig.count("profile") ? localConfig["profile"] : "";
        long masterSeed = 0;
        if (localConfig.count("master_seed")) {
            try { masterSeed = std::stol(localConfig["master_seed"]); } catch(...) {}
        }

        if (!profileName.empty()) {
            std::string runtimeSig = buildRuntimeIdentitySignature(localConfig);
            if (runtimeSig != g_lastRuntimeIdentitySig) {
                g_lastRuntimeIdentitySig = runtimeSig;
                restartLocationRuntime();
            }

            static std::string s_lastWrittenProfile;
            if (profileName != s_lastWrittenProfile) {
                s_lastWrittenProfile = profileName;

                // 1. ksu_susfs set_uname backup (handles profile changes without reboot)
                if (access("/data/adb/ksu/bin/ksu_susfs", X_OK) == 0) {
                    // Temporarily set global for getSpoofedKernelVersion()
                    std::string savedProfile = g_currentProfileName;
                    g_currentProfileName = profileName;
                    std::string kv = getSpoofedKernelVersion();
                    g_currentProfileName = savedProfile;
                    std::string cmd = "/data/adb/ksu/bin/ksu_susfs set_uname '"
                                    + kv + "' '#1 SMP PREEMPT' 2>/dev/null";
                    system(cmd.c_str());
                }

                // 2. Generate .profile_props + SUSFS cmdline
                const DeviceFingerprint* fp_ptr = findProfile(profileName);
                if (fp_ptr) {
                    // PR79: SUSFS cmdline forgery (kernel-level /proc/cmdline spoof)
                    writeCmdlineFile(*fp_ptr, masterSeed);
                    applySusfsCmdline();

                    writeProfileProps(*fp_ptr, profileName, masterSeed);

                    // 3. PIF Hijacking: inject profile into /data/adb/pif.prop
                    // This is the primary injection path — boot-completed.sh is
                    // only a backup for 2nd+ boots before any scoped app launches.
                    writePifProps(*fp_ptr);
                    // Kill GMS to force re-read of pif.prop
                    system("killall com.google.android.gms.unstable 2>/dev/null &");
                    LOGE("PIF: injected profile '%s' into /data/adb/pif.prop",
                         profileName.c_str());

                    // 4. Apply resetprop immediately (background) — fixes first-boot
                    // race condition where boot-completed.sh runs before .profile_props exists
                    system("sh -c '"
                        "RP=/data/adb/ksu/bin/resetprop; "
                        "[ ! -x \"$RP\" ] && RP=/data/adb/magisk/resetprop; "
                        "[ ! -x \"$RP\" ] && RP=resetprop; "
                        "while IFS=\"=\" read -r key value; do "
                        "  [ -z \"$key\" ] && continue; "
                        "  case \"$key\" in \\#*) continue;; esac; "
                        "  \"$RP\" -n \"$key\" \"$value\" 2>/dev/null; "
                        "done < /data/adb/.omni_data/.profile_props"
                        "' &");

                    // PR76: Fix Settings.Global device_name.
                    // Settings.Global is backed by a ContentProvider (Binder IPC) — the Dobby
                    // native symbol hook may not find the right entry point on all MIUI builds.
                    // Writing directly via 'settings put global' is the only reliable path.
                    {
                        std::string dn_cmd = std::string("settings put global device_name '")
                                           + fp_ptr->model + "' 2>/dev/null &";
                        system(dn_cmd.c_str());
                    }

                }
            }
        }
    }
}

REGISTER_ZYGISK_COMPANION(companion_handler)
