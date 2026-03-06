cmake_minimum_required(VERSION 3.10)

project(omnishield)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include_directories(jni/include)

# PR64: Restaurar módulo completo — main.cpp con OmniModule + Dobby + Vulkan

# PR132: Build real Dobby (static) — replaces no-op stub that caused
# all DobbyHook calls to silently fail (PR131 diagnosis: rc=0 but orig=0x0).
add_subdirectory(external/dobby dobby_build)

# ── FULL (módulo completo) ───────────────────────────────────────────────────
add_library(omnishield SHARED
    jni/main.cpp
)

target_link_libraries(omnishield
    dobby_static
    log
    vulkan
)

# ── MINIMAL (PR61 diagnóstico — comentado) ───────────────────────────────────
# add_library(omnishield SHARED
#     jni/main_minimal.cpp
# )
# target_link_libraries(omnishield log)
