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
#include <sys/utsname.h>
#include <ifaddrs.h>
#include <linux/if_packet.h>
#include <sys/socket.h>
#include <errno.h>

#include "zygisk.hpp"
#include "dobby.h"
#include "vortex_profiles.h"
#include "vortex_engine.hpp"

#define LOG_TAG "VortexNative"
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// Globals
static std::map<std::string, std::string> g_config;
static std::string g_currentProfileName = "Redmi 9";
static long g_masterSeed = 0;
static bool g_enableJitter = true;

// FD Tracking
enum FileType { NONE = 0, PROC_VERSION, PROC_CPUINFO, USB_SERIAL, WIFI_MAC, BATTERY_TEMP, BATTERY_VOLT };
static std::map<int, FileType> g_fdMap;
static std::map<int, size_t> g_fdOffsetMap; // Thread-safe offset tracking
static std::mutex g_fdMutex;

// Original Pointers
static int (*orig_system_property_get)(const char *key, char *value);
static int (*orig_open)(const char *pathname, int flags, mode_t mode);
static ssize_t (*orig_read)(int fd, void *buf, size_t count);
static int (*orig_close)(int fd);

// Phase 2 Originals
#define EGL_VENDOR 0x3053
static const char* (*orig_eglQueryString)(void* display, int name);
static int (*orig_clock_gettime)(clockid_t clockid, struct timespec *tp);
static int (*orig_uname)(struct utsname *buf);
static int (*orig_access)(const char *pathname, int mode);
static int (*orig_getifaddrs)(struct ifaddrs **ifap);

// Phase 3 Originals
typedef int (*orig_DrmGetProperty_t)(void*, const char*, char*, size_t*);
static orig_DrmGetProperty_t orig_DrmGetProperty;
static ssize_t (*orig_readlinkat)(int dirfd, const char *pathname, char *buf, size_t bufsiz);

// Sensor Structures
typedef struct {
    int32_t version; int32_t sensor; int32_t type; int32_t reserved0;
    int64_t timestamp;
    union { float data[16]; uint64_t step_counter; };
    int32_t flags; int32_t reserved1[3];
} ASensorEvent;
typedef struct ASensorEventQueue ASensorEventQueue;
static ssize_t (*orig_ASensorEventQueue_getEvents)(ASensorEventQueue* queue, ASensorEvent* events, size_t count);

// SSL Pointers
typedef struct ssl_ctx_st SSL_CTX;
typedef struct ssl_st SSL;
static int (*orig_SSL_CTX_set_ciphersuites)(SSL_CTX *ctx, const char *str);
static int (*orig_SSL_set1_tls13_ciphersuites)(SSL *ssl, const char *str);
static int (*orig_SSL_set_cipher_list)(SSL *ssl, const char *str);

// GL Pointers
typedef unsigned char GLubyte;
typedef unsigned int GLenum;
#define GL_VENDOR 0x1F00
#define GL_RENDERER 0x1F01
#define GL_VERSION 0x1F02
static const GLubyte* (*orig_glGetString)(GLenum name);

// Settings.Secure
static jstring (*orig_SettingsSecure_getString)(JNIEnv*, jobject, jobject, jstring);

// Helper
inline std::string toLowerStr(const char* s) {
    if (!s) return "";
    std::string res = s;
    std::transform(res.begin(), res.end(), res.begin(), [](unsigned char c){ return std::tolower(c); });
    return res;
}

// Config
void readConfig() {
    std::ifstream file("/data/adb/vortex/vortex.prop");
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

bool shouldHide(const char* str) {
    if (!str || str[0] == '\0') return false;
    std::string s = toLowerStr(str);

    if (VORTEX_PROFILES.count(g_currentProfileName)) {
        const auto& fp = VORTEX_PROFILES.at(g_currentProfileName);
        std::vector<std::string> legitimate = {
            toLowerStr(fp.hardware), toLowerStr(fp.hardwareChipname), toLowerStr(fp.boardPlatform)
        };
        for (const auto& leg : legitimate) {
            if (!leg.empty() && s.find(leg) != std::string::npos) return false;
        }
    }
    return s.find("mediatek") != std::string::npos || s.find("mt67") != std::string::npos || s.find("lancelot") != std::string::npos;
}

// -----------------------------------------------------------------------------
// Phase 2 Hooks
// -----------------------------------------------------------------------------

// 1. EGL Spoofing
const char* my_eglQueryString(void* display, int name) {
    if (name == EGL_VENDOR && VORTEX_PROFILES.count(g_currentProfileName)) {
        return VORTEX_PROFILES.at(g_currentProfileName).gpuVendor;
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
        strcpy(buf->machine, "aarch64");
        strcpy(buf->nodename, "localhost");
        strcpy(buf->release, "4.19.113-vortex"); // Coherente con VFS
        strcpy(buf->version, "#1 SMP PREEMPT");
    }
    return ret;
}

// 4. Deep VFS (Root Hiding)
int my_access(const char *pathname, int mode) {
    if (pathname != nullptr) {
        // strcasestr no asigna memoria, garantizando rendimiento en hot-paths
        if (strcasestr(pathname, "magisk") ||
            strcasestr(pathname, "lsposed") ||
            strcasestr(pathname, "twres") ||
            strcasestr(pathname, "zygisk") ||
            strcasestr(pathname, "vortex")) {
            errno = ENOENT;
            return -1;
        }
    }
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
                    std::string brand = VORTEX_PROFILES.count(g_currentProfileName) ?
                                        VORTEX_PROFILES.at(g_currentProfileName).brand : "default";
                    std::string mac_str = vortex::engine::generateRandomMac(brand, g_masterSeed + 50);

                    // Sobrescribir bytes reales
                    unsigned int mac[6];
                    if (sscanf(mac_str.c_str(), "%x:%x:%x:%x:%x:%x",
                        &mac[0], &mac[1], &mac[2], &mac[3], &mac[4], &mac[5]) == 6) {
                        for(int i = 0; i < 6; ++i) s->sll_addr[i] = (unsigned char)mac[i];
                    }
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

std::string generateMulticoreCpuInfo(const DeviceFingerprint& fp) {
    std::string out;
    for(int i = 0; i < 8; ++i) {
        out += "processor\t: " + std::to_string(i) + "\n";
        out += "BogoMIPS\t: 26.00\n";
        out += "Features\t: fp asimd evtstrm aes pmull sha1 sha2 crc32 atomics fphp asimdhp cpuid asimdrdm lrcpc dcpop asimddp\n";
        out += "CPU implementer\t: 0x41\n";
        out += "CPU architecture: 8\n\n";
    }
    out += "Hardware\t: " + std::string(fp.hardware) + "\n";
    out += "Revision\t: 0000\n";
    out += "Serial\t\t: 0000000000000000\n";
    return out;
}

int my_DrmGetProperty(void* self, const char* name, char* value, size_t* size) {
    if (name && strcmp(name, "deviceUniqueId") == 0) {
        auto rawId = vortex::engine::generateWidevineBytes(g_masterSeed);
        if (*size >= 16) {
            memcpy(value, rawId.data(), 16);
            *size = 16;
            return 0;
        }
    }
    return orig_DrmGetProperty(self, name, value, size);
}

ssize_t my_readlinkat(int dirfd, const char *pathname, char *buf, size_t bufsiz) {
    if (pathname && (strcasestr(pathname, "magisk") || strcasestr(pathname, "vortex") || strcasestr(pathname, "zygisk"))) {
        errno = ENOENT;
        return -1;
    }
    return orig_readlinkat(dirfd, pathname, buf, bufsiz);
}

// -----------------------------------------------------------------------------
// Hooks: System Properties
// -----------------------------------------------------------------------------
int my_system_property_get(const char *key, char *value) {
    if (shouldHide(key)) { if(value) value[0] = '\0'; return 0; }
    int ret = orig_system_property_get(key, value);

    if (VORTEX_PROFILES.count(g_currentProfileName)) {
        const auto& fp = VORTEX_PROFILES.at(g_currentProfileName);
        std::string k = key;
        const char* replacement = nullptr;

        if (k == "ro.product.model") replacement = fp.model;
        else if (k == "ro.product.brand") replacement = fp.brand;
        else if (k == "ro.product.manufacturer") replacement = fp.manufacturer;
        else if (k == "ro.product.device") replacement = fp.device;
        else if (k == "ro.product.name") replacement = fp.product;
        else if (k == "ro.hardware") replacement = fp.hardware;
        else if (k == "ro.board.platform") replacement = fp.boardPlatform;
        else if (k == "ro.build.fingerprint") replacement = fp.fingerprint;
        else if (k == "ro.build.id") replacement = fp.buildId;
        else if (k == "ro.serialno" || k == "ro.boot.serialno") {
            static std::string serial = vortex::engine::generateRandomSerial(fp.brand, g_masterSeed, fp.securityPatch);
            replacement = serial.c_str();
        } else if (k == "ro.sf.lcd_density") {
            replacement = fp.screenDensity;
        } else if (k == "ro.product.display_resolution") {
            static std::string res = std::string(fp.screenWidth) + "x" + std::string(fp.screenHeight);
            replacement = res.c_str();
        } else if (k == "gsm.device.id" || k == "ro.ril.miui.imei0") {
            static std::string imei = vortex::engine::generateValidImei(g_currentProfileName, g_masterSeed);
            replacement = imei.c_str();
        }

        if (replacement) {
            int len = strlen(replacement);
            if (len >= 92) len = 91;
            strncpy(value, replacement, len);
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

        if (type != NONE) {
            std::lock_guard<std::mutex> lock(g_fdMutex);
            g_fdMap[fd] = type;
            g_fdOffsetMap[fd] = 0;
        }
    }
    return fd;
}

int my_close(int fd) {
    {
        std::lock_guard<std::mutex> lock(g_fdMutex);
        g_fdMap.erase(fd);
        g_fdOffsetMap.erase(fd);
    }
    return orig_close ? orig_close(fd) : close(fd);
}

ssize_t my_read(int fd, void *buf, size_t count) {
    FileType type = NONE;
    { std::lock_guard<std::mutex> lock(g_fdMutex); if(g_fdMap.count(fd)) type = g_fdMap[fd]; }

    if (type != NONE && VORTEX_PROFILES.count(g_currentProfileName)) {
        const auto& fp = VORTEX_PROFILES.at(g_currentProfileName);
        std::string content;

        if (type == PROC_VERSION) {
            content = "Linux version 4.19.113-vortex (builder@vortex) (clang 12.0.5) #1 SMP PREEMPT " + std::string(fp.buildDateUtc) + "\n";
        } else if (type == PROC_CPUINFO) {
            content = generateMulticoreCpuInfo(fp);
        } else if (type == USB_SERIAL) {
            static std::string serial = vortex::engine::generateRandomSerial(fp.brand, g_masterSeed, fp.securityPatch);
            content = serial + "\n";
        } else if (type == WIFI_MAC) {
            static std::string mac = vortex::engine::generateRandomMac(fp.brand, g_masterSeed + 50);
            content = mac + "\n";
        } else if (type == BATTERY_TEMP) {
            content = vortex::engine::generateBatteryTemp(g_masterSeed) + "\n";
        } else if (type == BATTERY_VOLT) {
            content = vortex::engine::generateBatteryVoltage(g_masterSeed) + "\n";
        }

        std::lock_guard<std::mutex> lock(g_fdMutex); // Evita Race Conditions
        size_t current_offset = g_fdOffsetMap[fd];
        if (current_offset >= content.size()) return 0; // EOF real

        size_t available = content.size() - current_offset;
        size_t toRead = std::min(count, available);
        memcpy(buf, content.c_str() + current_offset, toRead);
        g_fdOffsetMap[fd] += toRead;
        return toRead;
    }
    return orig_read(fd, buf, count);
}

// -----------------------------------------------------------------------------
// Hooks: Sensor Jitter
// -----------------------------------------------------------------------------
ssize_t my_ASensorEventQueue_getEvents(ASensorEventQueue* queue, ASensorEvent* events, size_t count) {
    ssize_t ret = orig_ASensorEventQueue_getEvents(queue, events, count);
    if (ret > 0 && g_enableJitter) {
        struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
        long dynSeed = g_masterSeed ^ (ts.tv_nsec >> 10);
        std::mt19937 gen(dynSeed);
        std::uniform_real_distribution<float> dist(-0.002f, 0.002f);
        for (ssize_t i = 0; i < ret; ++i) {
            if (events[i].type == 1 || events[i].type == 4 || events[i].type == 2) {
                events[i].data[0] += dist(gen);
                events[i].data[1] += dist(gen);
                events[i].data[2] += dist(gen);
            }
        }
    }
    return ret;
}

// -----------------------------------------------------------------------------
// Hooks: Network (SSL)
// -----------------------------------------------------------------------------
int my_SSL_CTX_set_ciphersuites(SSL_CTX *ctx, const char *str) { return orig_SSL_CTX_set_ciphersuites(ctx, vortex::engine::generateTls13CipherSuites(g_masterSeed).c_str()); }
int my_SSL_set1_tls13_ciphersuites(SSL *ssl, const char *str) { return orig_SSL_set1_tls13_ciphersuites(ssl, vortex::engine::generateTls13CipherSuites(g_masterSeed).c_str()); }
int my_SSL_set_cipher_list(SSL *ssl, const char *str) { return orig_SSL_set_cipher_list(ssl, vortex::engine::generateTls12CipherSuites(g_masterSeed).c_str()); }

// -----------------------------------------------------------------------------
// Hooks: GPU
// -----------------------------------------------------------------------------
const GLubyte* my_glGetString(GLenum name) {
    if (VORTEX_PROFILES.count(g_currentProfileName)) {
        const auto& fp = VORTEX_PROFILES.at(g_currentProfileName);
        if (name == GL_VENDOR) return (const GLubyte*)fp.gpuVendor;
        if (name == GL_RENDERER) return (const GLubyte*)fp.gpuRenderer;
        if (name == GL_VERSION) return (const GLubyte*)vortex::engine::getGlVersionForProfile(fp);
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
        result = env->NewStringUTF(vortex::engine::generateRandomId(16, g_masterSeed).c_str());
    } else if (strcmp(key, "gsf_id") == 0) {
        result = env->NewStringUTF(vortex::engine::generateRandomId(16, g_masterSeed + 1).c_str());
    }
    env->ReleaseStringUTFChars(name, key);
    if (result) return result;
    return orig_SettingsSecure_getString(env, thiz, resolver, name);
}

// -----------------------------------------------------------------------------
// Hooks: Widevine (JNI Bridge)
// -----------------------------------------------------------------------------
jbyteArray JNICALL my_getPropertyByteArray(JNIEnv* env, jobject thiz, jstring jprop) {
    const char* prop = env->GetStringUTFChars(jprop, nullptr);
    if (strcmp(prop, "deviceUniqueId") == 0) {
        auto rawId = vortex::engine::generateWidevineBytes(g_masterSeed);
        jbyteArray jarray = env->NewByteArray(16);
        env->SetByteArrayRegion(jarray, 0, 16, (jbyte*)rawId.data());
        env->ReleaseStringUTFChars(jprop, prop);
        return jarray;
    }
    env->ReleaseStringUTFChars(jprop, prop);
    return nullptr;
}

jstring JNICALL my_getDeviceId(JNIEnv* env, jobject thiz, jint slotId) { return env->NewStringUTF(vortex::engine::generateValidImei(g_currentProfileName, g_masterSeed + slotId).c_str()); }
jstring JNICALL my_getSubscriberId(JNIEnv* env, jobject thiz, jint subId) { return env->NewStringUTF(vortex::engine::generateValidImsi(g_currentProfileName, g_masterSeed + subId).c_str()); }
jstring JNICALL my_getSimSerialNumber(JNIEnv* env, jobject thiz, jint subId) { return env->NewStringUTF(vortex::engine::generateValidIccid(g_currentProfileName, g_masterSeed + subId + 100).c_str()); }
jstring JNICALL my_getLine1Number(JNIEnv* env, jobject thiz, jint subId) { return env->NewStringUTF(vortex::engine::generatePhoneNumber(g_currentProfileName, g_masterSeed + subId).c_str()); }


// -----------------------------------------------------------------------------
// Module Main
// -----------------------------------------------------------------------------
class VortexModule : public zygisk::Module {
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

        // Phase 2 Hooks: Deep Phantom
        void* egl_func = DobbySymbolResolver("libEGL.so", "eglQueryString");
        if (egl_func) DobbyHook(egl_func, (void*)my_eglQueryString, (void**)&orig_eglQueryString);

        void* clock_func = DobbySymbolResolver(nullptr, "clock_gettime");
        if (clock_func) DobbyHook(clock_func, (void*)my_clock_gettime, (void**)&orig_clock_gettime);

        void* uname_func = DobbySymbolResolver(nullptr, "uname");
        if (uname_func) DobbyHook(uname_func, (void*)my_uname, (void**)&orig_uname);

        void* access_func = DobbySymbolResolver(nullptr, "access");
        if (access_func) DobbyHook(access_func, (void*)my_access, (void**)&orig_access);

        void* getifaddrs_func = DobbySymbolResolver(nullptr, "getifaddrs");
        if (getifaddrs_func) DobbyHook(getifaddrs_func, (void*)my_getifaddrs, (void**)&orig_getifaddrs);

        // Phase 3 Hooks: Final Seal
        void* drm_func = DobbySymbolResolver("libmediadrm.so", "DrmGetProperty");
        if (drm_func) DobbyHook(drm_func, (void*)my_DrmGetProperty, (void**)&orig_DrmGetProperty);

        void* readlinkat_func = DobbySymbolResolver(nullptr, "readlinkat");
        if (readlinkat_func) DobbyHook(readlinkat_func, (void*)my_readlinkat, (void**)&orig_readlinkat);

        // Android/Sensor Hooks
        void* sensor_func = DobbySymbolResolver("libandroid.so", "ASensorEventQueue_getEvents");
        if (sensor_func) DobbyHook(sensor_func, (void*)my_ASensorEventQueue_getEvents, (void**)&orig_ASensorEventQueue_getEvents);

        // Network Hooks (TLS 1.2 & 1.3)
        void* ssl12_func = DobbySymbolResolver("libssl.so", "SSL_set_cipher_list");
        if (ssl12_func) DobbyHook(ssl12_func, (void*)my_SSL_set_cipher_list, (void**)&orig_SSL_set_cipher_list);

        void* ssl13_ctx_func = DobbySymbolResolver("libssl.so", "SSL_CTX_set_ciphersuites");
        if (ssl13_ctx_func) DobbyHook(ssl13_ctx_func, (void*)my_SSL_CTX_set_ciphersuites, (void**)&orig_SSL_CTX_set_ciphersuites);

        void* ssl13_ssl_func = DobbySymbolResolver("libssl.so", "SSL_set1_tls13_ciphersuites");
        if (ssl13_ssl_func) DobbyHook(ssl13_ssl_func, (void*)my_SSL_set1_tls13_ciphersuites, (void**)&orig_SSL_set1_tls13_ciphersuites);

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

        // JNI Bridge for MediaDrm
        JNINativeMethod drmMethods[] = {
            {"getPropertyByteArray", "(Ljava/lang/String;)[B", (void*)my_getPropertyByteArray}
        };
        api->hookJniNativeMethods(env, "android/media/MediaDrm", drmMethods, 1);

        LOGD("Vortex Gold Ghost loaded. Profile: %s", g_currentProfileName.c_str());
    }
    void preServerSpecialize(zygisk::Api *api, JNIEnv *env) override {}
    void postServerSpecialize(zygisk::Api *api, JNIEnv *env) override {}
private:
    zygisk::Api *api;
    JNIEnv *env;
};

static VortexModule module_instance;
extern "C" { void zygisk_module_entry(zygisk::Api *api, JNIEnv *env) { api->registerModule(&module_instance); } }
