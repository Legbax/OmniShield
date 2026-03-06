#pragma once

#include <sys/types.h>
#include <stddef.h>
#include "pac_kit.h"

// PR132: Do NOT include platform.h here — it includes dobby/common.h which
// includes this file, creating a circular dependency that leaves OSMemory
// undefined. Forward-declare only what we need for the Android branch.
#if defined(ANDROID)
#include <sys/mman.h>
#include <unistd.h>
#endif

#if defined(__arm64e__) && __has_feature(ptrauth_calls)
#include <ptrauth.h>
#endif

namespace features {

template <typename T> inline T arm_thumb_fix_addr(T &addr) {
#if defined(__arm__) || defined(__aarch64__)
  addr = (T)((uintptr_t)addr & ~1);
#endif
  return addr;
}

namespace apple {
template <typename T> inline T arm64e_pac_strip(T &addr) {
  return pac_strip(addr);
}

template <typename T> inline T arm64e_pac_sign(T &addr) {
  return pac_sign(addr);
}

template <typename T> inline T arm64e_pac_strip_and_sign(T &addr) {
  return pac_strip_and_sign(addr);
}
} // namespace apple

namespace android {
inline void make_memory_readable(void *address, size_t size) {
#if defined(ANDROID)
  // PR132: Use mprotect directly to avoid circular header dependency with OSMemory.
  long pagesize = sysconf(_SC_PAGESIZE);
  uintptr_t addr = reinterpret_cast<uintptr_t>(address);
  void *page = reinterpret_cast<void *>(addr & ~(pagesize - 1));
  mprotect(page, pagesize, PROT_READ | PROT_EXEC);
#endif
}
} // namespace android
} // namespace features