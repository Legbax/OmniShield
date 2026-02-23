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
static std::mutex g_fdMutex;

// Original Pointers
static int (*orig_system_property_get)(const char *key, char *value);
static int (*orig_open)(const char *pathname, int flags, mode_t mode);
static ssize_t (*orig_read)(int fd, void *buf, size_t count);
static int (*orig_close)(int fd);

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
typedef struct ssl_st SSL;
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
    if (!str) return false;
    std::string s = str;
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c){ return std::tolower(c); });
    return s.find("mediatek") != std::string::npos || s.find("mt67") != std::string::npos || s.find("lancelot") != std::string::npos;
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
        }
    }
    return fd;
}

int my_close(int fd) {
    { std::lock_guard<std::mutex> lock(g_fdMutex); g_fdMap.erase(fd); }
    if (orig_close) return orig_close(fd);
    return close(fd);
}

ssize_t my_read(int fd, void *buf, size_t count) {
    FileType type = NONE;
    { std::lock_guard<std::mutex> lock(g_fdMutex); if (g_fdMap.count(fd)) type = g_fdMap[fd]; }

    if (type != NONE && VORTEX_PROFILES.count(g_currentProfileName)) {
        const auto& fp = VORTEX_PROFILES.at(g_currentProfileName);
        std::string content;

        if (type == PROC_VERSION) {
            content = "Linux version 4.19.113-vortex (builder@vortex) (clang 12.0.5) #1 SMP PREEMPT " + std::string(fp.buildDateUtc) + "\n";
        } else if (type == PROC_CPUINFO) {
            content = "Processor\t: AArch64 (vortex)\nHardware\t: " + std::string(fp.hardware) + "\n";
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

        size_t len = content.size();
        if (count < len) len = count;
        memcpy(buf, content.c_str(), len);
        return len;
    }
    return orig_read(fd, buf, count);
}

// -----------------------------------------------------------------------------
// Hooks: Sensor Jitter
// -----------------------------------------------------------------------------
ssize_t my_ASensorEventQueue_getEvents(ASensorEventQueue* queue, ASensorEvent* events, size_t count) {
    ssize_t ret = orig_ASensorEventQueue_getEvents(queue, events, count);
    if (ret > 0 && g_enableJitter) {
        static std::mt19937 gen(g_masterSeed);
        static std::uniform_real_distribution<float> dist(-0.0001f, 0.0001f);
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
int my_SSL_set_cipher_list(SSL *ssl, const char *str) {
    std::string spoofed = vortex::engine::generateJA3CipherSuites(g_masterSeed);
    return orig_SSL_set_cipher_list(ssl, spoofed.c_str());
}

// -----------------------------------------------------------------------------
// Hooks: GPU
// -----------------------------------------------------------------------------
const GLubyte* my_glGetString(GLenum name) {
    if (VORTEX_PROFILES.count(g_currentProfileName)) {
        const auto& fp = VORTEX_PROFILES.at(g_currentProfileName);
        if (name == GL_VENDOR) return (const GLubyte*)fp.gpuVendor;
        if (name == GL_RENDERER) return (const GLubyte*)fp.gpuRenderer;
        if (name == GL_VERSION) return (const GLubyte*)fp.gpuVersion;
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
    // Aquí deberíamos llamar al original, pero en JNI nativo se suele buscar el método de la superclase
    return nullptr;
}

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

        // Libc Hooks
        DobbyHook((void*)__system_property_get, (void*)my_system_property_get, (void**)&orig_system_property_get);
        DobbyHook((void*)open, (void*)my_open, (void**)&orig_open);
        DobbyHook((void*)read, (void*)my_read, (void**)&orig_read);
        DobbyHook((void*)close, (void*)my_close, (void**)&orig_close);

        // Android/Sensor Hooks
        void* sensor_func = DobbySymbolResolver("libandroid.so", "ASensorEventQueue_getEvents");
        if (sensor_func) DobbyHook(sensor_func, (void*)my_ASensorEventQueue_getEvents, (void**)&orig_ASensorEventQueue_getEvents);

        // Network Hooks
        void* ssl_func = DobbySymbolResolver("libssl.so", "SSL_set_cipher_list");
        if (ssl_func) DobbyHook(ssl_func, (void*)my_SSL_set_cipher_list, (void**)&orig_SSL_set_cipher_list);

        // GPU Hooks
        void* gl_func = DobbySymbolResolver("libGLESv2.so", "glGetString");
        if (gl_func) DobbyHook(gl_func, (void*)my_glGetString, (void**)&orig_glGetString);

        // Sellar Settings.Secure (Android ID)
        void* settings_func = DobbySymbolResolver("libandroid_runtime.so", "_ZN7android14SettingsSecure9getStringEP7_JNIEnvP8_jstring");
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
