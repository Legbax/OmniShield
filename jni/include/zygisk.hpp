#pragma once

#include <jni.h>

#define ZYGISK_API_VERSION 2

namespace zygisk {

struct Api;

class Module {
public:
    virtual void onLoad(Api *api, JNIEnv *env) = 0;
    virtual void preAppSpecialize(Api *api, JNIEnv *env) = 0;
    virtual void postAppSpecialize(Api *api, JNIEnv *env) = 0;
    virtual void preServerSpecialize(Api *api, JNIEnv *env) = 0;
    virtual void postServerSpecialize(Api *api, JNIEnv *env) = 0;
};

enum Option {
    FORCE_DENYLIST_UNMOUNT = 0,
    DLCLOSE_MODULE_LIBRARY = 1,
};

struct Api {
    virtual void registerModule(Module *module) = 0;
    virtual void hookJniNativeMethods(JNIEnv *env, const char *className, JNINativeMethod *methods, int numMethods) = 0;
    virtual void pltHookRegister(const char *dev, const char *symbol, void *newFunc, void **oldFunc) = 0;
    virtual void pltHookExclude(const char *dev, const char *symbol) = 0;
    virtual void pltHookCommit() = 0;
    virtual int connectCompanion() = 0;
    virtual int getModuleDir() = 0;
    virtual void setOption(Option opt) = 0;
    virtual uint32_t getFlags() = 0;
};

} // namespace zygisk

extern "C" {
    void zygisk_module_entry(zygisk::Api *api, JNIEnv *env);
}
