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
#include <chrono>
#include <random>
#include <time.h>
#include <iomanip>
#include <sys/utsname.h>
#include <ifaddrs.h>
#include <linux/if_packet.h>
#include <sys/socket.h>
#include <errno.h>
#include <vulkan/vulkan.h>
#include <sys/sysinfo.h>
#include <dirent.h>

#include "zygisk.hpp"
#include "dobby.h"
#include "omni_profiles.h"
#include "omni_engine.hpp"

#define LOG_TAG "AndroidSystem"
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// Globals
static std::map<std::string, std::string> g_config;
static std::string g_currentProfileName = "Redmi 9";
static long g_masterSeed = 0;
static bool g_enableJitter = true;
static uint64_t g_configGeneration = 0;

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
    PROC_HOSTNAME,
    PROC_OSTYPE,
    PROC_DTB_MODEL,
    PROC_ETH0_MAC,
    PROC_SELF_STATUS, SYS_CPU_TOPOLOGY, BAT_TECHNOLOGY, BAT_PRESENT,
    SYS_BLOCK_SIZE, PROC_NET_TCP, PROC_NET_UDP
};
static std::map<int, FileType> g_fdMap;
static std::map<int, size_t> g_fdOffsetMap; // Thread-safe offset tracking
static std::map<int, CachedContent> g_fdContentCache; // Cache content for stable reads
static std::mutex g_fdMutex;

// Original Pointers
static int (*orig_system_property_get)(const char *key, char *value);
static void (*orig_system_property_read_callback)(const prop_info *pi, void (*callback)(void *cookie, const char *name, const char *value, uint32_t serial), void *cookie);
static int (*orig_open)(const char *pathname, int flags, mode_t mode);
static int (*orig_openat)(int dirfd, const char *pathname, int flags, mode_t mode);
static ssize_t (*orig_read)(int fd, void *buf, size_t count);
static int (*orig_close)(int fd);
static off_t (*orig_lseek)(int fd, off_t offset, int whence);
static off64_t (*orig_lseek64)(int fd, off64_t offset, int whence);
static ssize_t (*orig_pread)(int fd, void* buf, size_t count, off_t offset);
static ssize_t (*orig_pread64)(int fd, void* buf, size_t count, off64_t offset);
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

// Phase 4 Originals (v11.9.9)
static int (*orig_sysinfo)(struct sysinfo *info);
static struct dirent* (*orig_readdir)(DIR *dirp);

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

// Vulkan & Sensors
static void (*orig_vkGetPhysicalDeviceProperties)(VkPhysicalDevice physicalDevice, VkPhysicalDeviceProperties* pProperties);
static const char* (*orig_Sensor_getName)(void* sensor);
static const char* (*orig_Sensor_getVendor)(void* sensor);

// Settings.Secure
static jstring (*orig_SettingsSecure_getString)(JNIEnv*, jstring);
static jstring (*orig_SettingsSecure_getStringForUser)(JNIEnv*, jstring, jint);

// Helper
inline std::string toLowerStr(const char* s) {
    if (!s) return "";
    std::string res = s;
    std::transform(res.begin(), res.end(), res.begin(), [](unsigned char c){ return std::tolower(c); });
    return res;
}

// Config
void readConfig() {
    std::ifstream file("/data/adb/.omni_data/.identity.cfg");
    if (!file.is_open()) return;
    std::string line;
    while (std::getline(file, line)) {
        size_t eq = line.find('=');
        if (eq != std::string::npos) {
            std::string key = line.substr(0, eq);
            std::string val = line.substr(eq + 1);
            key.erase(0, key.find_first_not_of(" \t"));
            key.erase(key.find_last_not_of(" \t") + 1);
            val.erase(0, val.find_first_not_of(" \t"));
            val.erase(val.find_last_not_of(" \t") + 1);
            g_config[key] = val;
        }
    }
    if (g_config.count("profile")) g_currentProfileName = g_config["profile"];
    if (g_config.count("master_seed")) try { g_masterSeed = std::stol(g_config["master_seed"]); } catch(...) {}
    if (g_config.count("jitter")) g_enableJitter = (g_config["jitter"] == "true");
}

bool shouldHide(const char* key) {
    if (!key || key[0] == '\0') return false;
    std::string s = toLowerStr(key);
    if (g_currentProfileName == "Redmi 9" && s.find("lancelot") != std::string::npos) return false;
    if (G_DEVICE_PROFILES.count(g_currentProfileName)) {
        const auto& fp = G_DEVICE_PROFILES.at(g_currentProfileName);
        if (toLowerStr(fp.brand).find("xiaomi") != std::string::npos ||
            toLowerStr(fp.hardware).find("mt") != std::string::npos) {
            if (s.find("mediatek") != std::string::npos) return false;
        }
    }
    return s.find("mediatek") != std::string::npos || s.find("lancelot") != std::string::npos;
}

// -----------------------------------------------------------------------------
// Hooks: OpenCL
// -----------------------------------------------------------------------------
cl_int my_clGetDeviceInfo(cl_device_id device, cl_device_info param_name, size_t param_value_size, void *param_value, size_t *param_value_size_ret) {
    cl_int ret = orig_clGetDeviceInfo(device, param_name, param_value_size, param_value, param_value_size_ret);
    if (ret == 0 && G_DEVICE_PROFILES.count(g_currentProfileName)) {
        const auto& fp = G_DEVICE_PROFILES.at(g_currentProfileName);
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
        "android_cache_data", "tombstones",
        nullptr
    };

    for (int i = 0; HIDDEN_TOKENS[i] != nullptr; ++i) {
        if (strcasestr(path, HIDDEN_TOKENS[i])) return true;
    }
    return false;
}

int my_stat(const char* pathname, struct stat* statbuf) {
    if (isHiddenPath(pathname)) { errno = ENOENT; return -1; }
    return orig_stat(pathname, statbuf);
}
int my_lstat(const char* pathname, struct stat* statbuf) {
    if (isHiddenPath(pathname)) { errno = ENOENT; return -1; }
    return orig_lstat(pathname, statbuf);
}

int my_fstatat(int dirfd, const char *pathname, struct stat *statbuf, int flags) {
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
FILE* my_fopen(const char* pathname, const char* mode) {
    if (isHiddenPath(pathname)) { errno = ENOENT; return nullptr; }
    return orig_fopen(pathname, mode);
}

// 1. EGL Spoofing
#define EGL_EXTENSIONS_ENUM 0x3055

const char* my_eglQueryString(void* display, int name) {
    if (G_DEVICE_PROFILES.count(g_currentProfileName)) {
        const auto& fp = G_DEVICE_PROFILES.at(g_currentProfileName);

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
int my_uname(struct utsname *buf) {
    int ret = orig_uname(buf);
    if (ret == 0 && buf != nullptr) {
        strcpy(buf->machine, "aarch64"); strcpy(buf->nodename, "localhost");

        std::string kv = "4.14.186-perf+";
        if (g_currentProfileName == "Redmi 9") {
             kv = "4.14.186-perf+";
        } else if (G_DEVICE_PROFILES.count(g_currentProfileName)) {
            const auto& kfp = G_DEVICE_PROFILES.at(g_currentProfileName);
            std::string plat = toLowerStr(kfp.boardPlatform);
            std::string brd  = toLowerStr(kfp.brand);

            if (brd == "google") {
                // Google compila sus propios kernels con hashes de commit específicos
                if (plat.find("lito") != std::string::npos)
                    kv = "4.19.113-g820a424c538c-ab7336171";        // Pixel 5, 4a 5G
                else if (plat.find("atoll") != std::string::npos)
                    kv = "4.14.150-g62a62a5a93f7-ab7336171";         // Pixel 4a (sunfish/SM7150)
                else if (plat.find("sdm670") != std::string::npos)
                    kv = "4.9.189-g5d098cef6d96-ab6174032";         // Pixel 3a XL
                else
                    kv = "4.19.113-g820a424c538c-ab7336171";        // fallback Google
            } else if (plat.find("mt6") != std::string::npos) {
                kv = "4.14.141-perf+";
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
        }

        strcpy(buf->release, kv.c_str());
        strcpy(buf->version, "#1 SMP PREEMPT");
    }
    return ret;
}

// 4. Deep VFS (Root Hiding)
int my_access(const char *pathname, int mode) {
    if (isHiddenPath(pathname)) { errno = ENOENT; return -1; }
    return orig_access(pathname, mode);
}

// 5. Network Interfaces (Layer 2)
int my_getifaddrs(struct ifaddrs **ifap) {
    int ret = orig_getifaddrs(ifap);
    if (ret == 0 && ifap != nullptr && *ifap != nullptr) {
        struct ifaddrs *ifa = *ifap;
        while (ifa) {
            if (ifa->ifa_addr && ifa->ifa_addr->sa_family == AF_PACKET) {
                struct sockaddr_ll *s = (struct sockaddr_ll*)ifa->ifa_addr;

                // Validación de seguridad contra punteros nulos
                if (ifa->ifa_name != nullptr && strcmp(ifa->ifa_name, "wlan0") == 0) {
                    // Static MAC 02:00:00:00:00:00 for AOSP privacy
                    unsigned char static_mac[] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x00};
                    memcpy(s->sll_addr, static_mac, 6);
                }
            }
            ifa = ifa->ifa_next;
        }
    }
    return ret;
}

// -----------------------------------------------------------------------------
// Phase 3 Hooks & Helpers
// -----------------------------------------------------------------------------

static std::string getArmFeatures(const std::string& platform) {
    if (platform.find("mt6768") != std::string::npos || platform.find("mt6765") != std::string::npos ||
        platform.find("sdm670") != std::string::npos || platform.find("exynos850") != std::string::npos)
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
    if (isHiddenPath(pathname)) { errno = ENOENT; return -1; }
    return orig_readlinkat(dirfd, pathname, buf, bufsiz);
}

// -----------------------------------------------------------------------------
// Hooks: System Properties
// -----------------------------------------------------------------------------
int my_system_property_get(const char *key, char *value) {
    if (shouldHide(key)) { if(value) value[0] = '\0'; return 0; }
    int ret = orig_system_property_get(key, value);

    if (G_DEVICE_PROFILES.count(g_currentProfileName)) {
        const auto& fp = G_DEVICE_PROFILES.at(g_currentProfileName);
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
        else if (k == "ro.build.description")               dynamic_buffer = fp.buildDescription;
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
            std::string region = omni::engine::getRegionForProfile(g_currentProfileName);
            if (region == "europe")      dynamic_buffer = "en-GB";
            else if (region == "latam")  dynamic_buffer = "es-US";
            else                         dynamic_buffer = "en-US"; // usa + default
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
                dynamic_buffer = "soc/11120000.ufs";          // Samsung Exynos UFS genérico
            } else {
                dynamic_buffer = "soc/1d84000.ufshc";         // Qualcomm UFS genérico (SM7150/SM8250/etc.)
            }
        }
        else if (k == "ro.boot.flash.locked")         dynamic_buffer = "1";
        else if (k == "ro.boot.vbmeta.device_state")  dynamic_buffer = "locked";
        else if (k == "ro.boot.veritymode")           dynamic_buffer = "enforcing";
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
        else if (k == "ro.odm.build.fingerprint"         ||
                 k == "ro.system_ext.build.fingerprint")      dynamic_buffer = fp.fingerprint;
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
            std::string region = omni::engine::getRegionForProfile(g_currentProfileName);
            dynamic_buffer = (region == "europe") ? "GB" : "US";
        }
        else if (k == "persist.sys.language") {
            std::string region = omni::engine::getRegionForProfile(g_currentProfileName);
            dynamic_buffer = (region == "latam") ? "es" : "en";
        }
        // --- v12.10: Chronos & Command Shield ---
        else if (k == "ro.opengles.version")          dynamic_buffer = fp.openGlEs;
        else if (k == "sys.usb.state" || k == "sys.usb.config") dynamic_buffer = "mtp";
        else if (k == "persist.sys.timezone") {
            std::string region = omni::engine::getRegionForProfile(g_currentProfileName);
            if (region == "europe") dynamic_buffer = "Europe/London";
            else if (region == "latam") dynamic_buffer = "America/Sao_Paulo";
            else dynamic_buffer = "America/New_York";
        }
        else if (k == "gsm.network.type")             dynamic_buffer = "LTE";
        else if (k == "gsm.current.phone-type")       dynamic_buffer = "1";
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
            // Sincronizar ISO con la región del motor
            std::string region = omni::engine::getRegionForProfile(g_currentProfileName);
            if (region == "usa") dynamic_buffer = "us";
            else if (region == "europe") dynamic_buffer = "gb";
            else if (region == "latam") dynamic_buffer = "br";
            else dynamic_buffer = "us";
        }
        else if (k == "gsm.sim.operator.alpha" || k == "gsm.operator.alpha") {
            dynamic_buffer = "Omni Network";
        }
        else if (k == "ro.mediadrm.device_id" || k == "drm.service.enabled") {
            dynamic_buffer = omni::engine::generateWidevineId(g_masterSeed);
        }
        else if (k == "gsm.version.ril-impl")
            dynamic_buffer = "com.android.internal.telephony.uicc.RILConstants";
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
        else if (k == "ro.mediatek.version.release" ||
                 k == "ro.mediatek.platform") {
            // Si una app (o el DRM) busca explícitamente firmas MTK en las properties, las vaciamos
            // a menos que estemos emulando un MediaTek
            std::string plat = toLowerStr(fp.boardPlatform);
            if (plat.find("mt") == std::string::npos) {
                dynamic_buffer = "";
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
int my_open(const char *pathname, int flags, mode_t mode) {
    if (pathname && strstr(pathname, "/dev/__properties__/")) {
        errno = EACCES;
        return -1;
    }
    if (pathname) {
        // Bloquear drivers de GPU contradictorios (Evasión Capa 5)
        if (G_DEVICE_PROFILES.count(g_currentProfileName)) {
            std::string plat = toLowerStr(G_DEVICE_PROFILES.at(g_currentProfileName).boardPlatform);
            std::string brand = toLowerStr(G_DEVICE_PROFILES.at(g_currentProfileName).brand);
            bool isQcom = (brand == "google" || plat.find("msmnile") != std::string::npos ||
                plat.find("kona") != std::string::npos || plat.find("lahaina") != std::string::npos ||
                plat.find("atoll") != std::string::npos || plat.find("lito") != std::string::npos ||
                plat.find("bengal") != std::string::npos || plat.find("holi") != std::string::npos ||
                plat.find("trinket") != std::string::npos || plat.find("sdm670") != std::string::npos ||
                plat.find("sm6150") != std::string::npos || plat.find("sm6350") != std::string::npos ||
                plat.find("sm7325") != std::string::npos);
            std::string egl = toLowerStr(G_DEVICE_PROFILES.at(g_currentProfileName).eglDriver);

            // Adreno → bloquea mali; Mali → bloquea kgsl; PowerVR → bloquea ambos
            if (egl == "powervr") {
                if (strstr(pathname, "/dev/mali") || strstr(pathname, "/dev/kgsl")) { errno = ENOENT; return -1; }
            } else if (isQcom && strstr(pathname, "/dev/mali")) { errno = ENOENT; return -1; }
            else if (!isQcom && strstr(pathname, "/dev/kgsl")) { errno = ENOENT; return -1; }
        }
    }
    int fd = orig_open(pathname, flags, mode);
    if (fd >= 0 && pathname) {
        FileType type = NONE;
        if (strstr(pathname, "/proc/version")) type = PROC_VERSION;
        else if (strstr(pathname, "/proc/cpuinfo")) type = PROC_CPUINFO;
        else if (strstr(pathname, "/sys/class/android_usb") && strstr(pathname, "iSerial")) type = USB_SERIAL;
        else if (strstr(pathname, "/sys/class/net/wlan0/address")) type = WIFI_MAC;
        // PR22: Hardware Topology & TracerPid
        else if (strstr(pathname, "/proc/self/status")) type = PROC_SELF_STATUS;
        else if (strstr(pathname, "/sys/devices/system/cpu/online") ||
                 strstr(pathname, "/sys/devices/system/cpu/possible") ||
                 strstr(pathname, "/sys/devices/system/cpu/present")) type = SYS_CPU_TOPOLOGY;
        else if (strstr(pathname, "/sys/class/power_supply/battery/technology")) type = BAT_TECHNOLOGY;
        else if (strstr(pathname, "/sys/class/power_supply/battery/present"))    type = BAT_PRESENT;
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
        else if (strstr(pathname, "/proc/self/maps") || strstr(pathname, "/proc/self/smaps")) type = PROC_MAPS;
        else if (strstr(pathname, "/proc/meminfo")) type = PROC_MEMINFO;
        else if (strstr(pathname, "/proc/modules")) type = PROC_MODULES;
        else if (strstr(pathname, "/proc/self/mounts") || strstr(pathname, "/proc/self/mountinfo")) type = PROC_MOUNTS;
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
        else if (strcmp(pathname, "/proc/net/dev") == 0)   type = PROC_NET_DEV;
        // PR32: Sellado forense de sockets TCP/UDP
        else if (strcmp(pathname, "/proc/net/tcp") == 0 ||
                 strcmp(pathname, "/proc/net/tcp6") == 0)  type = PROC_NET_TCP;
        else if (strcmp(pathname, "/proc/net/udp") == 0 ||
                 strcmp(pathname, "/proc/net/udp6") == 0)  type = PROC_NET_UDP;
        // Bloqueo directo de KSU/Batería MTK
        else if (strstr(pathname, "mtk_battery") || strstr(pathname, "mt_bat")) { errno = ENOENT; return -1; }

        if (type != NONE) {
            std::string content;
            if (G_DEVICE_PROFILES.count(g_currentProfileName)) {
                const auto& fp = G_DEVICE_PROFILES.at(g_currentProfileName);

                if (type == PROC_VERSION) {
                    std::string plat = toLowerStr(fp.boardPlatform);
                    std::string brd  = toLowerStr(fp.brand);
                    std::string kv = "4.14.186-perf+";
                    if (brd == "google") {
                        if (plat.find("lito")    != std::string::npos) kv = "4.19.113-g820a424c538c-ab7336171";
                        else if (plat.find("atoll") != std::string::npos) kv = "4.14.150-g62a62a5a93f7-ab7336171";
                        else if (plat.find("sdm670")  != std::string::npos) kv = "4.9.189-g5d098cef6d96-ab6174032";
                        else kv = "4.19.113-g820a424c538c-ab7336171";
                    } else if (plat.find("mt6")!=std::string::npos) kv="4.14.141-perf+";
                    else if (plat.find("kona")!=std::string::npos||plat.find("lahaina")!=std::string::npos) kv="4.19.157-perf+";
                    else if (plat.find("atoll")!=std::string::npos||plat.find("lito")!=std::string::npos) kv="4.19.113-perf+";
                    else if (plat.find("sdm670")!=std::string::npos) kv="4.9.189-perf+";
                    else if (plat.find("bengal")!=std::string::npos||plat.find("holi")!=std::string::npos||
                             plat.find("sm6350")!=std::string::npos) kv="4.19.157-perf+";
                    else if (plat.find("sm7325")!=std::string::npos) kv="5.4.61-perf+";

                    long dateUtc = 0;
                    try { dateUtc = std::stol(fp.buildDateUtc); } catch(...) {}
                    char dateBuf[128];
                    time_t t = (time_t)dateUtc;
                    struct tm* tm_info = gmtime(&t);
                    strftime(dateBuf, sizeof(dateBuf), "%a %b %d %H:%M:%S UTC %Y", tm_info);

                    content = "Linux version " + kv + " (builder@android) (clang 12.0.5) #1 SMP PREEMPT " + std::string(dateBuf) + "\n";
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
                    if (G_DEVICE_PROFILES.count(g_currentProfileName)) {
                        const auto& fp2 = G_DEVICE_PROFILES.at(g_currentProfileName);
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
                    content = "console=tty0 root=/dev/ram0 rw init=/init loglevel=4 androidboot.hardware=" + std::string(fp.hardware) + " androidboot.verifiedbootstate=green androidboot.vbmeta.device_state=locked androidboot.flash.locked=1\n";

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
                        kv = "4.14.141-perf+";
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
                    std::string plat = toLowerStr(fp.boardPlatform);
                    if (plat.find("mt") != std::string::npos) {
                        char tmpBuf[128]; ssize_t r;
                        if ((r = orig_read(fd, tmpBuf, sizeof(tmpBuf))) > 0) content.append(tmpBuf, r);
                    } else if (plat.find("exynos") != std::string::npos) {
                        content = "exynos-therm\n";
                    } else {
                        content = "tsens_tz_sensor\n";
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
                } else if (type == PROC_NET_ARP) {
                    content = "IP address       HW type  Flags  HW address            Mask     Device\n"
                              "192.168.1.1      0x1      0x2    00:00:00:00:00:00     *        wlan0\n";
                // PR20: Estadísticas de red virtualizadas (oculta TX/RX real)
                } else if (type == PROC_NET_DEV) {
                    content = "Inter-|   Receive                                                |  Transmit\n"
                              " face |bytes    packets errs drop fifo frame compressed multicast|"
                              "bytes    packets errs drop fifo colls carrier compressed\n"
                              "    lo:       0       0    0    0    0     0          0         0"
                              "        0       0    0    0    0     0       0          0\n"
                              " wlan0:  524288    4096    0    0    0     0          0         0"
                              "   131072    1024    0    0    0     0       0          0\n";
                // PR32: Tabla TCP virtualizada — oculta IPs locales y puertos de servicios reales
                } else if (type == PROC_NET_TCP) {
                    content = "  sl  local_address rem_address   st tx_queue rx_queue tr tm->when retrnsmt   uid  timeout inode\n";
                    // No se añaden entradas: tabla de sockets vacía = sin servicios locales expuestos
                // PR32: Tabla UDP virtualizada — ídem
                } else if (type == PROC_NET_UDP) {
                    content = "  sl  local_address rem_address   st tx_queue rx_queue tr tm->when retrnsmt   uid  timeout inode\n";
                // Añade esto al final de los else if del contenido VFS
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
    if (pathname && strstr(pathname, "/dev/__properties__/")) {
        errno = EACCES;
        return -1;
    }
    if (pathname) {
        // Bloquear drivers de GPU contradictorios (Evasión Capa 5)
        if (G_DEVICE_PROFILES.count(g_currentProfileName)) {
            std::string plat = toLowerStr(G_DEVICE_PROFILES.at(g_currentProfileName).boardPlatform);
            std::string brand = toLowerStr(G_DEVICE_PROFILES.at(g_currentProfileName).brand);
            bool isQcom = (brand == "google" || plat.find("msmnile") != std::string::npos ||
                plat.find("kona") != std::string::npos || plat.find("lahaina") != std::string::npos ||
                plat.find("atoll") != std::string::npos || plat.find("lito") != std::string::npos ||
                plat.find("bengal") != std::string::npos || plat.find("holi") != std::string::npos ||
                plat.find("trinket") != std::string::npos || plat.find("sdm670") != std::string::npos ||
                plat.find("sm6150") != std::string::npos || plat.find("sm6350") != std::string::npos ||
                plat.find("sm7325") != std::string::npos);
            std::string egl = toLowerStr(G_DEVICE_PROFILES.at(g_currentProfileName).eglDriver);

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
    }
    return orig_close ? orig_close(fd) : close(fd);
}

ssize_t my_read(int fd, void *buf, size_t count) {
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

// -----------------------------------------------------------------------------
// Hooks: Network (SSL)
// -----------------------------------------------------------------------------
int my_SSL_CTX_set_ciphersuites(SSL_CTX *ctx, const char *str) { return orig_SSL_CTX_set_ciphersuites(ctx, omni::engine::generateTls13CipherSuites(g_masterSeed).c_str()); }
int my_SSL_set1_tls13_ciphersuites(SSL *ssl, const char *str) { return orig_SSL_set1_tls13_ciphersuites(ssl, omni::engine::generateTls13CipherSuites(g_masterSeed).c_str()); }
int my_SSL_set_cipher_list(SSL *ssl, const char *str) { return orig_SSL_set_cipher_list(ssl, omni::engine::generateTls12CipherSuites(g_masterSeed).c_str()); }
int my_SSL_set_ciphersuites(SSL *ssl, const char *str) {
    return orig_SSL_set_ciphersuites(ssl, omni::engine::generateTls13CipherSuites(g_masterSeed).c_str());
}

// -----------------------------------------------------------------------------
// Hooks: GPU
// -----------------------------------------------------------------------------
#define GL_EXTENSIONS 0x1F03

const GLubyte* my_glGetString(GLenum name) {
    if (G_DEVICE_PROFILES.count(g_currentProfileName)) {
        const auto& fp = G_DEVICE_PROFILES.at(g_currentProfileName);
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
    orig_vkGetPhysicalDeviceProperties(physicalDevice, pProperties);
    if (pProperties && G_DEVICE_PROFILES.count(g_currentProfileName)) {
        const auto& fp = G_DEVICE_PROFILES.at(g_currentProfileName);
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
// Hooks: SensorManager (Limpieza de firmas MTK/Xiaomi)
// -----------------------------------------------------------------------------
const char* my_Sensor_getName(void* sensor) {
    const char* orig_name = orig_Sensor_getName(sensor);
    if (!orig_name) return nullptr;

    static thread_local std::string name_cache;
    name_cache = orig_name;

    std::string lower_name = toLowerStr(orig_name);
    if (lower_name.find("mtk") != std::string::npos ||
        lower_name.find("mediatek") != std::string::npos ||
        lower_name.find("xiaomi") != std::string::npos) {

        size_t pos;
        while ((pos = name_cache.find("MTK")) != std::string::npos) name_cache.replace(pos, 3, "AOSP");
        while ((pos = name_cache.find("mtk")) != std::string::npos) name_cache.replace(pos, 3, "AOSP");
        while ((pos = name_cache.find("MediaTek")) != std::string::npos) name_cache.replace(pos, 8, "AOSP");
        while ((pos = name_cache.find("Xiaomi")) != std::string::npos) name_cache.replace(pos, 6, "AOSP");
    }
    return name_cache.c_str();
}

const char* my_Sensor_getVendor(void* sensor) {
    const char* orig_vendor = orig_Sensor_getVendor(sensor);
    if (!orig_vendor) return nullptr;

    static thread_local std::string vendor_cache;
    vendor_cache = orig_vendor;

    std::string lower_vendor = toLowerStr(orig_vendor);
    if (lower_vendor.find("mtk") != std::string::npos ||
        lower_vendor.find("mediatek") != std::string::npos ||
        lower_vendor.find("xiaomi") != std::string::npos) {
        vendor_cache = "AOSP Framework"; // Vendor genérico y seguro
    }
    return vendor_cache.c_str();
}

// -----------------------------------------------------------------------------
// Hooks: sysinfo (Uptime Paradox Fix)
// -----------------------------------------------------------------------------
int my_sysinfo(struct sysinfo *info) {
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
    struct dirent* ret;
    while ((ret = orig_readdir(dirp)) != nullptr) {
        std::string dname = toLowerStr(ret->d_name);
        if (G_DEVICE_PROFILES.count(g_currentProfileName)) {
            std::string plat = toLowerStr(G_DEVICE_PROFILES.at(g_currentProfileName).boardPlatform);
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
// Hooks: Settings.Secure (JNI Bridge)
// -----------------------------------------------------------------------------
static jstring my_SettingsSecure_getString(JNIEnv* env, jstring name) {
    if (!name) return nullptr;
    const char* key = env->GetStringUTFChars(name, nullptr);
    if (!key) return nullptr;

    jstring result = nullptr;
    if (strcmp(key, "android_id") == 0) {
        result = env->NewStringUTF(
            omni::engine::generateRandomId(16, g_masterSeed).c_str()
        );
    } else if (strcmp(key, "gsf_id") == 0) {
        result = env->NewStringUTF(
            omni::engine::generateRandomId(16, g_masterSeed + 1).c_str()
        );
    }
    env->ReleaseStringUTFChars(name, key);

    if (result) return result;
    return orig_SettingsSecure_getString(env, name);
}

static jstring my_SettingsSecure_getStringForUser(JNIEnv* env, jstring name, jint userHandle) {
    return my_SettingsSecure_getString(env, name);
}

void my_system_property_read_callback(const prop_info *pi, void (*callback)(void *cookie, const char *name, const char *value, uint32_t serial), void *cookie) {
    if (!pi || !callback) return;

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


// -----------------------------------------------------------------------------
// Module Main
// -----------------------------------------------------------------------------
class OmniModule : public zygisk::Module {
public:
    void onLoad(zygisk::Api *api, JNIEnv *env) override {
        this->api = api;
        this->env = env;
    }
    void preAppSpecialize(zygisk::Api *api, JNIEnv *env) override { readConfig(); }
    void postAppSpecialize(zygisk::Api *api, JNIEnv *env) override {
        api->setOption(zygisk::FORCE_DENYLIST_UNMOUNT);

        // Libc Hooks (Phase 1)
        void* open_func = DobbySymbolResolver(nullptr, "open");
        if (open_func) DobbyHook(open_func, (void*)my_open, (void**)&orig_open);

        void* openat_func = DobbySymbolResolver(nullptr, "openat");
        if (openat_func) DobbyHook(openat_func, (void*)my_openat, (void**)&orig_openat);

        void* read_func = DobbySymbolResolver(nullptr, "read");
        if (read_func) DobbyHook(read_func, (void*)my_read, (void**)&orig_read);

        void* close_func = DobbySymbolResolver(nullptr, "close");
        if (close_func) DobbyHook(close_func, (void*)my_close, (void**)&orig_close);

        void* lseek_func = DobbySymbolResolver(nullptr, "lseek");
        if (lseek_func) DobbyHook(lseek_func, (void*)my_lseek, (void**)&orig_lseek);

        void* lseek64_func = DobbySymbolResolver(nullptr, "lseek64");
        if (lseek64_func) DobbyHook(lseek64_func, (void*)my_lseek64, (void**)&orig_lseek64);

        void* pread_func = DobbySymbolResolver(nullptr, "pread");
        if (pread_func) DobbyHook(pread_func, (void*)my_pread, (void**)&orig_pread);

        void* pread64_func = DobbySymbolResolver(nullptr, "pread64");
        if (pread64_func) DobbyHook(pread64_func, (void*)my_pread64, (void**)&orig_pread64);

        void* sysprop_func = DobbySymbolResolver(nullptr, "__system_property_get");
        if (sysprop_func) DobbyHook(sysprop_func, (void*)my_system_property_get, (void**)&orig_system_property_get);

        void* sysprop_cb_func = DobbySymbolResolver(nullptr, "__system_property_read_callback");
        if (sysprop_cb_func) DobbyHook(sysprop_cb_func, (void*)my_system_property_read_callback, (void**)&orig_system_property_read_callback);

        // Syscalls (Evasión Root, Uptime, Kernel, Network)
        DobbyHook((void*)uname, (void*)my_uname, (void**)&orig_uname);
        DobbyHook((void*)clock_gettime, (void*)my_clock_gettime, (void**)&orig_clock_gettime);
        DobbyHook((void*)access, (void*)my_access, (void**)&orig_access);
        DobbyHook((void*)getifaddrs, (void*)my_getifaddrs, (void**)&orig_getifaddrs);
        DobbyHook((void*)stat, (void*)my_stat, (void**)&orig_stat);
        DobbyHook((void*)lstat, (void*)my_lstat, (void**)&orig_lstat);
        void* fstatat_func = DobbySymbolResolver(nullptr, "fstatat");
        if (fstatat_func) DobbyHook(fstatat_func, (void*)my_fstatat, (void**)&orig_fstatat);
        DobbyHook((void*)fopen, (void*)my_fopen, (void**)&orig_fopen);
        DobbyHook((void*)readlinkat, (void*)my_readlinkat, (void**)&orig_readlinkat);

        void* sysinfo_func = DobbySymbolResolver(nullptr, "sysinfo");
        if (sysinfo_func) DobbyHook(sysinfo_func, (void*)my_sysinfo, (void**)&orig_sysinfo);

        void* readdir_func = DobbySymbolResolver(nullptr, "readdir");
        if (readdir_func) DobbyHook(readdir_func, (void*)my_readdir, (void**)&orig_readdir);

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
        void* gl_func = DobbySymbolResolver("libGLESv2.so", "glGetString");
        if (gl_func) DobbyHook(gl_func, (void*)my_glGetString, (void**)&orig_glGetString);

        // Vulkan
        void* vulkan_func = DobbySymbolResolver("libvulkan.so", "vkGetPhysicalDeviceProperties");
        if (vulkan_func) DobbyHook(vulkan_func, (void*)my_vkGetPhysicalDeviceProperties, (void**)&orig_vkGetPhysicalDeviceProperties);

        // OpenCL
        void* cl_func = DobbySymbolResolver("libOpenCL.so", "clGetDeviceInfo");
        if (cl_func) DobbyHook(cl_func, (void*)my_clGetDeviceInfo, (void**)&orig_clGetDeviceInfo);

        // Sensores (Mangled names en libandroid.so o libsensors.so)
        // _ZNK7android6Sensor7getNameEv -> android::Sensor::getName() const
        // _ZNK7android6Sensor9getVendorEv -> android::Sensor::getVendor() const
        void* sensor_name_func = DobbySymbolResolver("libandroid.so", "_ZNK7android6Sensor7getNameEv");
        if (!sensor_name_func) sensor_name_func = DobbySymbolResolver("libsensors.so", "_ZNK7android6Sensor7getNameEv");
        if (sensor_name_func) DobbyHook(sensor_name_func, (void*)my_Sensor_getName, (void**)&orig_Sensor_getName);

        void* sensor_vendor_func = DobbySymbolResolver("libandroid.so", "_ZNK7android6Sensor9getVendorEv");
        if (!sensor_vendor_func) sensor_vendor_func = DobbySymbolResolver("libsensors.so", "_ZNK7android6Sensor9getVendorEv");
        if (sensor_vendor_func) DobbyHook(sensor_vendor_func, (void*)my_Sensor_getVendor, (void**)&orig_Sensor_getVendor);

        // Settings.Secure (Android ID)
        static const char* SETTINGS_SYMBOLS[] = {
            "_ZN7android14SettingsSecure9getStringEP7_JNIEnvP8_jstring",
            "_ZN7android16SettingsProvider9getStringEP7_JNIEnvP8_jobjectP8_jstring",
            "_ZN7android8Settings6Secure9getStringEP7_JNIEnvP8_jobjectP8_jstring",
            nullptr
        };
        void* settings_func = nullptr;
        for (int si = 0; SETTINGS_SYMBOLS[si] && !settings_func; ++si) {
            settings_func = DobbySymbolResolver("libandroid_runtime.so", SETTINGS_SYMBOLS[si]);
        }
        if (settings_func) DobbyHook(settings_func, (void*)my_SettingsSecure_getString, (void**)&orig_SettingsSecure_getString);

        // Settings.Secure (getStringForUser - API 30+)
        void* settings_user_func = DobbySymbolResolver("libandroid_runtime.so", "_ZN7android14SettingsSecure16getStringForUserEP7_JNIEnvP8_jstringi");
        if (settings_user_func) DobbyHook(settings_user_func, (void*)my_SettingsSecure_getStringForUser, (void**)&orig_SettingsSecure_getStringForUser);

        // JNI Telephony
        JNINativeMethod telephonyMethods[] = {
            {"getDeviceId", "(I)Ljava/lang/String;", (void*)my_getDeviceId},
            {"getSubscriberId", "(I)Ljava/lang/String;", (void*)my_getSubscriberId},
            {"getSimSerialNumber", "(I)Ljava/lang/String;", (void*)my_getSimSerialNumber},
            {"getLine1Number", "(I)Ljava/lang/String;", (void*)my_getLine1Number},
            {"getImei", "(I)Ljava/lang/String;", (void*)my_getDeviceId},
            {"getMeid", "(I)Ljava/lang/String;", (void*)my_getDeviceId},
        };
        api->hookJniNativeMethods(env, "com/android/internal/telephony/ITelephony", telephonyMethods, 6);
        api->hookJniNativeMethods(env, "android/telephony/TelephonyManager", telephonyMethods, 6);

        LOGD("System Integrity loaded. Profile: %s", g_currentProfileName.c_str());
    }
    void preServerSpecialize(zygisk::Api *api, JNIEnv *env) override {}
    void postServerSpecialize(zygisk::Api *api, JNIEnv *env) override {}
private:
    zygisk::Api *api;
    JNIEnv *env;
};

static OmniModule module_instance;
extern "C" { void zygisk_module_entry(zygisk::Api *api, JNIEnv *env) { api->registerModule(&module_instance); } }
