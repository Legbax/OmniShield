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
        "Moto Edge Plus": 6.7, "Moto Edge": 6.7, "OnePlus 8T": 6.55, "ASUS ZenFone 7": 6.67
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

        width = 1080
        height = 2400
        if name in ["Redmi 9", "Moto Edge Plus", "Moto Edge"]:
            height = 2340

        diag = diagonals.get(name, 6.5)
        density = calculate_dpi(width, height, diag)

        gpu_vendor = "Qualcomm"
        gpu_renderer = "Adreno (TM) 618"
        gpu_version = "OpenGL ES 3.2 V@0502.0 (GIT@5f4e5c9, Ia3b7920, 1600000000) (Date:10/20/2020)"

        if "lahaina" in platform: gpu_renderer = "Adreno (TM) 660"
        elif "kona" in platform: gpu_renderer = "Adreno (TM) 650"
        elif "lito" in platform: gpu_renderer = "Adreno (TM) 620"
        elif "mt6833" in platform: gpu_vendor = "ARM"; gpu_renderer = "Mali-G57 MC2"
        elif "mt6768" in hardware or "mt6769" in platform: gpu_vendor = "ARM"; gpu_renderer = "Mali-G52 MC2"

        if gpu_vendor == "ARM":
            gpu_version = "OpenGL ES 3.2 v1.r21p0-01rel0.a51a0c509f2714d8e5acbde47570a4b2"

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
