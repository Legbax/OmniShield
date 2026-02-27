#pragma once

#include <jni.h>
#include <stdint.h>

#define ZYGISK_API_VERSION 3

namespace zygisk {

enum Option {
    FORCE_DENYLIST_UNMOUNT = 0,
    DLCLOSE_MODULE_LIBRARY = 1,
};

struct Api {
    // CRÍTICO: Sin registerModule — vtable[0] es hookJniNativeMethods
    virtual bool hookJniNativeMethods(JNIEnv *env, const char *className,
                                      JNINativeMethod *methods, int numMethods) = 0;
    virtual void pltHookRegister(const char *executable_path, const char *symbol,
                                 void *new_func, void **old_func) = 0;
    virtual void pltHookExclude(const char *executable_path, const char *symbol) = 0;
    virtual bool pltHookCommit() = 0;
    virtual int connectCompanion() = 0;
    virtual void setOption(Option opt) = 0;
    virtual int getModuleDir() = 0;
    virtual uint32_t getFlags() = 0;
};

struct AppSpecializeArgs {
    jint &uid;
    jint &gid;
    jintArray &gids;
    jint &runtime_flags;
    jobjectArray &rlimits;
    jint &mount_external;
    jstring &se_info;
    jstring &nice_name;
    jintArray &fds_to_close;
    jintArray &fds_to_ignore;
    jboolean &is_child_zygote;
    jstring &instruction_set;
    jstring &app_data_dir;
    jboolean &is_top_app;
    jobjectArray &pkg_data_info_list;
    jobjectArray &whitelisted_data_info_list;
    jboolean &mount_data_dirs;
    jboolean &mount_storage_dirs;
};

struct ServerSpecializeArgs {
    jint &uid;
    jint &gid;
    jintArray &gids;
    jint &runtime_flags;
    jlong &permitted_capabilities;
    jlong &effective_capabilities;
};

class Module {
public:
    // Sin destructor virtual — PR54 confirmado
    virtual void onLoad(Api *api, JNIEnv *env) {}
    virtual void preAppSpecialize(AppSpecializeArgs *args) {}
    virtual void postAppSpecialize(const AppSpecializeArgs *args) {}
    virtual void preServerSpecialize(ServerSpecializeArgs *args) {}
    virtual void postServerSpecialize(const ServerSpecializeArgs *args) {}
};

} // namespace zygisk

#define REGISTER_ZYGISK_MODULE(clazz) \
static clazz _module_instance; \
extern "C" __attribute__((visibility("default"))) \
void zygisk_module_entry(int32_t* api_version, void** v_module) { \
    *api_version = ZYGISK_API_VERSION; \
    *v_module = &_module_instance; \
}
