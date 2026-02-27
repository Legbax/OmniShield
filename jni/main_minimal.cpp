// PR61: Módulo mínimo absoluto — test de diagnóstico de crash
// PROPÓSITO: Determinar si el crash viene de nuestro código o de Zygisk Next / KSU.
// - CERO hooks, CERO maps, CERO std::string en scope global, CERO Dobby
// - Si este módulo crashea: el problema es Zygisk Next / KSU / kernel, no nuestro código
// - Si no crashea: la causa está en main.cpp y debemos bisectar
#include "zygisk.hpp"
#include <android/log.h>

#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, "OmniMINIMAL", __VA_ARGS__)

class MinimalModule : public zygisk::ModuleBase {
public:
    void onLoad(zygisk::Api *api, JNIEnv *env) override {
        LOGD("MINIMAL: onLoad ejecutado — modulo cargado OK");
    }
    void preAppSpecialize(zygisk::AppSpecializeArgs *args) override {}
    void postAppSpecialize(const zygisk::AppSpecializeArgs *args) override {}
    void preServerSpecialize(zygisk::ServerSpecializeArgs *args) override {}
    void postServerSpecialize(const zygisk::ServerSpecializeArgs *args) override {}
};

REGISTER_ZYGISK_MODULE(MinimalModule)
