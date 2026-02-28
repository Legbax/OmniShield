#ifndef _SYS_SYSTEM_PROPERTIES_H
#define _SYS_SYSTEM_PROPERTIES_H

#ifdef __cplusplus
extern "C" {
#endif

#define PROP_NAME_MAX 32
#define PROP_VALUE_MAX 92

typedef struct prop_info prop_info;

int __system_property_set(const char *key, const char *value);
int __system_property_get(const char *key, char *value);
const prop_info *__system_property_find(const char *name);
void __system_property_read_callback(const prop_info *pi,
    void (*callback)(void *cookie, const char *name, const char *value, unsigned serial),
    void *cookie);
int __system_property_foreach(
    void (*callback)(const prop_info *pi, void *cookie),
    void *cookie);

#ifdef __cplusplus
}
#endif

#endif
