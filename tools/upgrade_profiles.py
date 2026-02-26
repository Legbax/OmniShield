import re
import sys
import math

def calculate_dpi(w, h, diag):
    return str(int(math.sqrt(w**2 + h**2) / diag))

def upgrade_profiles(input_file, output_file):
    with open(input_file, 'r') as f:
        content = f.read()

    diagonals = {
        "Redmi 9": 6.53, "POCO X3 Pro": 6.67, "POCO X3 NFC": 6.67, "Mi 10T": 6.67,
        "Redmi Note 10 Pro": 6.67, "Mi 11": 6.81, "Redmi Note 10": 6.43,
        "Galaxy A52": 6.5, "Galaxy A21s": 6.5, "Galaxy A31": 6.4, "Galaxy A12": 6.5,
        "Moto Edge Plus": 6.7, "Moto Edge": 6.7, "OnePlus 8T": 6.55, "ASUS ZenFone 7": 6.67,
        # PR33: Diagonales faltantes que causaban fallback a default 6.5" incorrecto
        "POCO F3": 6.67,            # SM8250 kona — 1080×2400 → math=394 DPI ✅
        "Nokia 5.4": 6.39,          # SM6115 bengal — 1080×2400 → math=411, override→409
        "Moto G Power 2021": 6.5,   # SM6115 bengal — 1080×2400 → math=404 ✅ (explícito)
        # PR34: 10 diagonales adicionales completando el catálogo de 40 dispositivos
        "Redmi Note 9 Pro": 6.67,   # SM7125 atoll — 1080×2400 → math=394 DPI ✅
        "OnePlus Nord": 6.44,       # SM7250 lito — 1080×2400 → math=408 DPI ✅
        "OnePlus 8": 6.55,          # SM8250 kona — 1080×2400 → math=401 DPI ✅
        "Mi 11 Lite": 6.55,         # SM7150 atoll — 1080×2400 → math=401 DPI ✅
        "Realme 8 Pro": 6.4,        # SM7125 atoll — 1080×2400 → math=411 DPI ✅
        "Realme 8": 6.4,            # MT6785 — 1080×2400 → math=411 DPI ✅
        "Realme GT Master": 6.43,   # SM7325 — 1080×2400 → math=409 DPI ✅
        "Galaxy M31": 6.4,          # Exynos9611 — 1080×2340 → math=402, override→403
        "Redmi 10X 4G": 6.53,       # MT6769 — 1080×2400 → math=403 DPI ✅
        "OnePlus N10 5G": 6.49,     # SM6350 — 1080×2400 → math=405 DPI ✅
    }

    # PR33: Valores canónicos que difieren del cálculo matemático puro.
    # Se aplican antes de calculate_dpi() y nunca deben eliminarse.
    # Fuentes: firmware dumps verificados / especificaciones oficiales de fabricante.
    dpi_overrides = {
        "Mi 11":    "395",   # Xiaomi MIUI ro.sf.lcd_density FHD+ (math 6.81" = 386)
        "Nokia 5.4": "409",  # Nokia spec oficial 409 ppi (math 6.39" = 411)
        # PR34: Override canónico para Galaxy M31 (spec Samsung 403 ppi, math 2340px = 402)
        "Galaxy M31": "403", # Samsung SM-M315F 6.4" 1080×2340 → spec 403 ppi (math=402)
        # PR40: HD+ Samsung — int(sqrt(720²+1600²)/6.5) = 269, spec Samsung = 270
        "Galaxy A21s":  "270",  # SM-A217F 6.5" HD+ 720×1600 — Samsung spec 270 ppi
        "Galaxy A32 5G": "270", # SM-A326B 6.5" HD+ 720×1600 — Samsung spec 270 ppi
        "Galaxy A12":   "270",  # SM-A125F 6.5" HD+ 720×1600 — Samsung spec 270 ppi
        # PR40: Pixel — excluidos de diagonals dict (spec Google directa, no math)
        "Pixel 5":      "432",  # redfin  6.0" OLED 1080×2340 — Google spec 432 ppi
        "Pixel 4a 5G":  "413",  # bramble 6.2" OLED 1080×2340 — Google spec 413 ppi
        "Pixel 4a":     "443",  # sunfish 5.81" OLED 1080×2340 — Google spec 443 ppi
        "Pixel 3a XL":  "400",  # bonito  6.0" FHD  1080×2160 — Google spec 400 ppi
    }

    block_pattern = re.compile(r'\{\s*"([^"]+)",\s*\{(.*?)\}\s*\},', re.DOTALL)
    matches = block_pattern.findall(content)
    profiles = []

    for name, body in matches:
        vals = re.findall(r'"([^"]*)"', body)

        keys = [
            "manufacturer", "brand", "model", "device", "product", "hardware",
            "board", "bootloader", "fingerprint", "buildId", "tags", "type",
            "radioVersion", "incremental", "securityPatch", "release",
            "boardPlatform", "eglDriver", "openGlEs", "hardwareChipname",
            "zygote", "vendorFingerprint", "display", "buildDescription",
            "buildFlavor", "buildHost", "buildUser", "buildDateUtc",
            "buildVersionCodename", "buildVersionPreviewSdk"
        ]

        base_vals = vals[:30] if len(vals) >= 30 else vals
        data = dict(zip(keys, base_vals))

        if name == "Realme GT Master":
            base_vals[7] = "S.20211201"

        platform = data.get("boardPlatform", "").lower()
        hardware = data.get("hardware", "").lower()

        # PR40: Corrección de dimensiones reales por tipo de pantalla
        width_overrides = {
            "Galaxy A21s": 720, "Galaxy A32 5G": 720, "Galaxy A12": 720
        }
        height_overrides = {
            "Redmi 9": 2340, "Moto Edge Plus": 2340, "Moto Edge": 2340, "Galaxy M31": 2340,
            "Galaxy A21s": 1600, "Galaxy A32 5G": 1600, "Galaxy A12": 1600,
            "Pixel 5": 2340, "Pixel 4a 5G": 2340, "Pixel 4a": 2340, "Pixel 3a XL": 2160
        }

        width = width_overrides.get(name, 1080)
        height = height_overrides.get(name, 2400)

        diag = diagonals.get(name, 6.5)
        # PR33: Aplicar override si existe; si no, calcular matemáticamente
        density = dpi_overrides.get(name) or calculate_dpi(width, height, diag)

        gpu_vendor = "Qualcomm"
        gpu_renderer = "Adreno (TM) 618"
        gpu_version = "OpenGL ES 3.2 V@0502.0 (GIT@5f4e5c9, Ia3b7920, 1600000000) (Date:10/20/2020)"

        # Qualcomm platforms (mapeo completo sincronizado con omni_profiles.h)
        if "lahaina" in platform: gpu_renderer = "Adreno (TM) 660"
        elif "kona" in platform: gpu_renderer = "Adreno (TM) 650"
        elif "msmnile" in platform: gpu_renderer = "Adreno (TM) 640"
        elif "lito" in platform: gpu_renderer = "Adreno (TM) 620"
        elif "atoll" in platform: gpu_renderer = "Adreno (TM) 618"
        elif "sm7325" in platform: gpu_renderer = "Adreno (TM) 642L"
        elif "sm6350" in platform: gpu_renderer = "Adreno (TM) 619L"
        elif "sm6150" in platform: gpu_renderer = "Adreno (TM) 612"
        elif "holi" in platform: gpu_renderer = "Adreno (TM) 619"
        elif "bengal" in platform or "trinket" in platform: gpu_renderer = "Adreno (TM) 610"
        elif "sdm670" in platform: gpu_renderer = "Adreno (TM) 615"
        # IMPORTANTE: msmnile = Snapdragon 855/860 = Adreno 640 (NO 650)
        # kona ya captura msmnile arriba por el OR, pero si un perfil tiene bp=msmnile
        # explícitamente, el elif "kona" or "msmnile" lo cubre.

        # MediaTek platforms
        elif "mt6853" in platform: gpu_vendor = "ARM"; gpu_renderer = "Mali-G57 MC3"
        elif "mt6833" in platform: gpu_vendor = "ARM"; gpu_renderer = "Mali-G57 MC2"
        elif "mt6785" in platform: gpu_vendor = "ARM"; gpu_renderer = "Mali-G76 MC4"
        elif "mt6768" in hardware or "mt6769" in platform: gpu_vendor = "ARM"; gpu_renderer = "Mali-G52 MC2"
        elif "mt6765" in platform: gpu_vendor = "Imagination Technologies"; gpu_renderer = "PowerVR GE8320"

        # Exynos platforms
        elif "exynos9825" in platform: gpu_vendor = "ARM"; gpu_renderer = "Mali-G76 MP12"
        elif "exynos9611" in platform: gpu_vendor = "ARM"; gpu_renderer = "Mali-G72 MP3"
        elif "exynos850" in platform: gpu_vendor = "ARM"; gpu_renderer = "Mali-G52 MC1"

        if gpu_vendor == "ARM":
            gpu_version = "OpenGL ES 3.2 v1.r21p0-01rel0.a51a0c509f2714d8e5acbde47570a4b2"
        elif gpu_vendor == "Imagination Technologies":
            gpu_version = "OpenGL ES 3.2 build 1.13@5776728"

        profiles.append((name, base_vals + [gpu_vendor, gpu_renderer, gpu_version, str(width), str(height), density]))

    with open(output_file, 'w') as f:
        f.write("#pragma once\n#include <string>\n#include <map>\n\nstruct DeviceFingerprint {\n")
        all_keys = keys + ["gpuVendor", "gpuRenderer", "gpuVersion", "screenWidth", "screenHeight", "screenDensity"]
        for field in all_keys: f.write(f"    const char* {field};\n")
        int_fields = ["core_count", "ram_gb"]
        for field in int_fields: f.write(f"    int {field};\n")
        f.write("};\n\nstatic const std::map<std::string, DeviceFingerprint> G_DEVICE_PROFILES = {\n")

        for name, vals in profiles:
            f.write(f'    {{ "{name}", {{\n')
            for val in vals: f.write(f'        "{val}",\n')
            # Append default int values (these will be manually maintained)
            f.write('        8,\n')   # core_count default
            f.write('        6,\n')   # ram_gb default
            f.write("    } },\n")
        f.write("};\n")

if __name__ == "__main__":
    upgrade_profiles("jni/omni_profiles.h", "jni/omni_profiles.h")
