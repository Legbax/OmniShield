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

// FD Tracking
enum FileType { NONE = 0, PROC_VERSION, PROC_CPUINFO, USB_SERIAL, WIFI_MAC, BATTERY_TEMP, BATTERY_VOLT, PROC_MAPS, PROC_UPTIME, BATTERY_CAPACITY, BATTERY_STATUS };
static std::map<int, FileType> g_fdMap;
static std::map<int, size_t> g_fdOffsetMap; // Thread-safe offset tracking
static std::map<int, std::string> g_fdContentCache; // Cache content for stable reads
static std::mutex g_fdMutex;

// Original Pointers
static int (*orig_system_property_get)(const char *key, char *value);
static int (*orig_open)(const char *pathname, int flags, mode_t mode);
static ssize_t (*orig_read)(int fd, void *buf, size_t count);
static int (*orig_close)(int fd);
static int (*orig_stat)(const char*, struct stat*);
static int (*orig_lstat)(const char*, struct stat*);
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

// Settings.Secure
static jstring (*orig_SettingsSecure_getString)(JNIEnv*, jobject, jobject, jstring);
static jstring (*orig_SettingsSecure_getStringForUser)(JNIEnv*, jobject, jobject, jstring, jint);

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
// Phase 2 Hooks
// -----------------------------------------------------------------------------

static inline bool isHiddenPath(const char* path) {
    if (!path || path[0] == '\0') return false;
    // Obfuscated checks for "omnishield" and "vortex"
    bool h1 = false, h2 = false;
    // "om" + "ni" + "shi" + "eld"
    if (strstr(path, "om") && strstr(path, "ni") && strstr(path, "shi") && strstr(path, "eld")) h1 = true;
    // "vor" + "tex"
    if (strstr(path, "vor") && strstr(path, "tex")) h2 = true;

    return strcasestr(path, "magisk") || strcasestr(path, "kernelsu") ||
           strcasestr(path, "susfs") || strcasestr(path, "omni_data") ||
           strcasestr(path, "android_cache_data") || strcasestr(path, "tombstones") || h1 || h2;
}

int my_stat(const char* pathname, struct stat* statbuf) {
    if (isHiddenPath(pathname)) { errno = ENOENT; return -1; }
    return orig_stat(pathname, statbuf);
}
int my_lstat(const char* pathname, struct stat* statbuf) {
    if (isHiddenPath(pathname)) { errno = ENOENT; return -1; }
    return orig_lstat(pathname, statbuf);
}
FILE* my_fopen(const char* pathname, const char* mode) {
    if (isHiddenPath(pathname)) { errno = ENOENT; return nullptr; }
    return orig_fopen(pathname, mode);
}

// 1. EGL Spoofing
const char* my_eglQueryString(void* display, int name) {
    if (name == EGL_VENDOR && G_DEVICE_PROFILES.count(g_currentProfileName)) {
        return G_DEVICE_PROFILES.at(g_currentProfileName).gpuVendor;
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
            std::string plat = toLowerStr(G_DEVICE_PROFILES.at(g_currentProfileName).boardPlatform);
            if (plat.find("mt6") != std::string::npos) kv = "4.14.141-perf+";
            else if (plat.find("kona") != std::string::npos || plat.find("lahaina") != std::string::npos) kv = "4.19.157-perf+";
            else if (plat.find("atoll") != std::string::npos || plat.find("lito") != std::string::npos) kv = "4.19.113-perf+";
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
    if (platform.find("mt6768") != std::string::npos || platform.find("sdm670") != std::string::npos || platform.find("exynos850") != std::string::npos)
        return "fp asimd evtstrm aes pmull sha1 sha2 crc32 atomics fphp asimdhp cpuid asimdrdm";
    return "fp asimd evtstrm aes pmull sha1 sha2 crc32 atomics fphp asimdhp cpuid asimdrdm lrcpc dcpop asimddp";
}

std::string generateMulticoreCpuInfo(const DeviceFingerprint& fp) {
    std::string out;
    std::string platform = toLowerStr(fp.boardPlatform);
    std::string features = getArmFeatures(platform);

    if (platform.find("mt6768") != std::string::npos) {
        for(int i = 0; i < 8; ++i) {
            bool isBig = (i >= 6);
            out += "processor\t: " + std::to_string(i) + "\n";
            out += "BogoMIPS\t: " + std::string(isBig ? "52.00" : "26.00") + "\n";
            out += "Features\t: " + features + "\n";
            out += "CPU implementer\t: 0x41\n";
            out += "CPU architecture: 8\n";
            out += "CPU variant\t: 0x0\n";
            out += "CPU part\t: " + std::string(isBig ? "0xd0a" : "0xd03") + "\n";
            out += "CPU revision\t: 4\n\n";
        }
    } else {
        for(int i = 0; i < 8; ++i) {
            out += "processor\t: " + std::to_string(i) + "\n";
            out += "BogoMIPS\t: 26.00\n";
            out += "Features\t: " + features + "\n";
            out += "CPU implementer\t: 0x41\n";
            out += "CPU architecture: 8\n\n";
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
    int fd = orig_open(pathname, flags, mode);
    if (fd >= 0 && pathname) {
        FileType type = NONE;
        if (strstr(pathname, "/proc/version")) type = PROC_VERSION;
        else if (strstr(pathname, "/proc/cpuinfo")) type = PROC_CPUINFO;
        else if (strstr(pathname, "/sys/class/android_usb") && strstr(pathname, "iSerial")) type = USB_SERIAL;
        else if (strstr(pathname, "/sys/class/net/wlan0/address")) type = WIFI_MAC;
        else if (strstr(pathname, "/sys/class/power_supply/battery/temp")) type = BATTERY_TEMP;
        else if (strstr(pathname, "/sys/class/power_supply/battery/voltage_now")) type = BATTERY_VOLT;
        else if (strstr(pathname, "/sys/class/power_supply/battery/capacity")) type = BATTERY_CAPACITY;
        else if (strstr(pathname, "/sys/class/power_supply/battery/status")) type = BATTERY_STATUS;
        else if (strstr(pathname, "/proc/uptime")) type = PROC_UPTIME;
        else if (strstr(pathname, "/proc/self/maps") || strstr(pathname, "/proc/self/smaps")) type = PROC_MAPS;

        if (type != NONE) {
            std::string content;
            if (G_DEVICE_PROFILES.count(g_currentProfileName)) {
                const auto& fp = G_DEVICE_PROFILES.at(g_currentProfileName);

                if (type == PROC_VERSION) {
                    std::string plat = toLowerStr(fp.boardPlatform);
                    std::string kv = "4.14.186-perf+";
                    if (plat.find("mt6") != std::string::npos) kv = "4.14.141-perf+";
                    else if (plat.find("kona") != std::string::npos || plat.find("lahaina") != std::string::npos) kv = "4.19.157-perf+";
                    else if (plat.find("atoll") != std::string::npos || plat.find("lito") != std::string::npos) kv = "4.19.113-perf+";

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
                    content = omni::engine::generateRandomMac(fp.brand, g_masterSeed + 50) + "\n";
                } else if (type == BATTERY_TEMP) {
                    content = omni::engine::generateBatteryTemp(g_masterSeed) + "\n";
                } else if (type == BATTERY_VOLT) {
                    content = omni::engine::generateBatteryVoltage(g_masterSeed) + "\n";
                } else if (type == BATTERY_CAPACITY) {
                    content = std::to_string(40 + (g_masterSeed % 60)) + "\n";
                } else if (type == BATTERY_STATUS) {
                    content = "Not charging\n";
                } else if (type == PROC_UPTIME) {
                    char tmpBuf[256];
                    ssize_t r = orig_read(fd, tmpBuf, sizeof(tmpBuf)-1);
                    if (r > 0) {
                        tmpBuf[r] = '\0';
                        double uptime = 0, idle = 0;
                        if (sscanf(tmpBuf, "%lf %lf", &uptime, &idle) >= 1) {
                             uptime += 259200 + (g_masterSeed % 1036800);
                             // Idle time as a coherent fraction (e.g. 80%) of uptime to avoid math anomalies
                             idle = uptime * 0.80;
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
                }
            }

            if (!content.empty()) {
                std::lock_guard<std::mutex> lock(g_fdMutex);
                g_fdMap[fd] = type;
                g_fdOffsetMap[fd] = 0;
                g_fdContentCache[fd] = content;
            }
        }
    }
    return fd;
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
            const std::string& content = g_fdContentCache[fd];
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
const GLubyte* my_glGetString(GLenum name) {
    if (G_DEVICE_PROFILES.count(g_currentProfileName)) {
        const auto& fp = G_DEVICE_PROFILES.at(g_currentProfileName);
        if (name == GL_VENDOR) return (const GLubyte*)fp.gpuVendor;
        if (name == GL_RENDERER) return (const GLubyte*)fp.gpuRenderer;
        if (name == GL_VERSION) return (const GLubyte*)omni::engine::getGlVersionForProfile(fp);
    }
    return orig_glGetString(name);
}

// -----------------------------------------------------------------------------
// Hooks: Settings.Secure (JNI Bridge)
// -----------------------------------------------------------------------------
static jstring my_SettingsSecure_getString(JNIEnv* env, jobject thiz, jobject resolver, jstring name) {
    const char* key = env->GetStringUTFChars(name, nullptr);
    jstring result = nullptr;
    if (strcmp(key, "android_id") == 0) {
        result = env->NewStringUTF(omni::engine::generateRandomId(16, g_masterSeed).c_str());
    } else if (strcmp(key, "gsf_id") == 0) {
        result = env->NewStringUTF(omni::engine::generateRandomId(16, g_masterSeed + 1).c_str());
    }
    env->ReleaseStringUTFChars(name, key);
    if (result) return result;
    return orig_SettingsSecure_getString(env, thiz, resolver, name);
}

static jstring my_SettingsSecure_getStringForUser(JNIEnv* env, jobject thiz, jobject resolver, jstring name, jint userHandle) {
    return my_SettingsSecure_getString(env, thiz, resolver, name);
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

        void* read_func = DobbySymbolResolver(nullptr, "read");
        if (read_func) DobbyHook(read_func, (void*)my_read, (void**)&orig_read);

        void* close_func = DobbySymbolResolver(nullptr, "close");
        if (close_func) DobbyHook(close_func, (void*)my_close, (void**)&orig_close);

        void* sysprop_func = DobbySymbolResolver(nullptr, "__system_property_get");
        if (sysprop_func) DobbyHook(sysprop_func, (void*)my_system_property_get, (void**)&orig_system_property_get);

        // Syscalls (Evasión Root, Uptime, Kernel, Network)
        DobbyHook((void*)uname, (void*)my_uname, (void**)&orig_uname);
        DobbyHook((void*)clock_gettime, (void*)my_clock_gettime, (void**)&orig_clock_gettime);
        DobbyHook((void*)access, (void*)my_access, (void**)&orig_access);
        DobbyHook((void*)getifaddrs, (void*)my_getifaddrs, (void**)&orig_getifaddrs);
        DobbyHook((void*)stat, (void*)my_stat, (void**)&orig_stat);
        DobbyHook((void*)lstat, (void*)my_lstat, (void**)&orig_lstat);
        DobbyHook((void*)fopen, (void*)my_fopen, (void**)&orig_fopen);
        DobbyHook((void*)readlinkat, (void*)my_readlinkat, (void**)&orig_readlinkat);

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
