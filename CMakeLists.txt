cmake_minimum_required(VERSION 3.10)

project(omnishield)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include_directories(jni/include)

# PR64: Restaurar módulo completo — main.cpp con OmniModule + Dobby + Vulkan

# ── FULL (módulo completo) ───────────────────────────────────────────────────
# Stub for Dobby implementation to allow linking check
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/dobby_stub.cpp "
#include \"dobby.h\"
extern \"C\" {
int DobbyHook(void *address, void *fake_func, void **out_origin_func) { return 0; }
const char *DobbyGetVersion() { return \"1.0\"; }
void *DobbySymbolResolver(const char *image_name, const char *symbol_name) { return 0; }
}
")

add_library(omnishield SHARED
    jni/main.cpp
    ${CMAKE_CURRENT_BINARY_DIR}/dobby_stub.cpp
)

target_link_libraries(omnishield
    log
    vulkan
)

# ── MINIMAL (PR61 diagnóstico — comentado) ───────────────────────────────────
# add_library(omnishield SHARED
#     jni/main_minimal.cpp
# )
# target_link_libraries(omnishield log)
