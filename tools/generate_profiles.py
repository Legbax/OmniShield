import re
import sys

def parse_profiles(content):
    # Improved Regex
    # Matches: "Profile Name" to DeviceFingerprint( ... )
    # We match until the closing parenthesis that appears at the start of a line (with indentation).
    # This avoids stopping at parentheses inside strings like "Pixel 4a (5G)".

    # Structure:
    # "Key" to DeviceFingerprint(
    #    ...
    # )

    pattern = re.compile(r'"([^"]+)"\s+to\s+DeviceFingerprint\s*\((.*?)\n\s*\)', re.DOTALL)

    matches = pattern.findall(content)

    profiles = []

    for name, body in matches:
        # Parse fields: key = "value"
        field_pattern = re.compile(r'(\w+)\s*=\s*"([^"]*)"')
        fields = dict(field_pattern.findall(body))

        # PR32: Extracción de campos enteros (coreCount/ramGb) que no están entre comillas en Kotlin
        core_count_match = re.search(r'coreCount\s*=\s*(\d+)', body)
        ram_gb_match     = re.search(r'ramGb\s*=\s*(\d+)', body)
        if core_count_match:
            fields["core_count"] = core_count_match.group(1)
        if ram_gb_match:
            fields["ram_gb"] = ram_gb_match.group(1)

        # PR38+39: Extracción de campos float y bool (sensor metadata)
        # Valores como: accelMaxRange = 78.4532f, hasHeartRateSensor = false
        float_fields = ["accelMaxRange", "accelResolution", "gyroMaxRange",
                        "gyroResolution", "magMaxRange"]
        for ff in float_fields:
            # Match: field = 123.456f (opcional f)
            match = re.search(fr'{ff}\s*=\s*([\d\.]+f?)', body)
            if match: fields[ff] = match.group(1)

        bool_fields = ["hasHeartRateSensor", "hasBarometerSensor",
                       "hasFingerprintWakeupSensor"]
        for bf in bool_fields:
            # Match: field = true/false
            match = re.search(fr'{bf}\s*=\s*(true|false)', body)
            if match: fields[bf] = match.group(1)

        if fields:
            profiles.append((name, fields))
        else:
            print(f"Warning: Failed to parse fields for {name}")

    return profiles

def generate_header(input_file, output_file):
    with open(input_file, 'r') as f:
        content = f.read()

    profiles = parse_profiles(content)

    print(f"Found {len(profiles)} profiles.")

    with open(output_file, 'w') as f:
        f.write("#pragma once\n")
        f.write("#include <string>\n")
        f.write("#include <map>\n\n")

        f.write("struct DeviceFingerprint {\n")
        ordered_fields = [
            "manufacturer", "brand", "model", "device", "product", "hardware",
            "board", "bootloader", "fingerprint", "buildId", "tags", "type",
            "radioVersion", "incremental", "securityPatch", "release",
            "boardPlatform", "eglDriver", "openGlEs", "hardwareChipname",
            "zygote", "vendorFingerprint", "display", "buildDescription",
            "buildFlavor", "buildHost", "buildUser", "buildDateUtc",
            "buildVersionCodename", "buildVersionPreviewSdk", "gpuVendor",
            "gpuRenderer", "gpuVersion", "screenWidth", "screenHeight",
            "screenDensity"
        ]

        int_fields = ["core_count", "ram_gb"]
        float_fields = ["accelMaxRange", "accelResolution", "gyroMaxRange",
                        "gyroResolution", "magMaxRange"]
        bool_fields = ["hasHeartRateSensor", "hasBarometerSensor",
                       "hasFingerprintWakeupSensor"]

        for field in ordered_fields:
            f.write(f"    const char* {field};\n")
        for field in int_fields:
            f.write(f"    int {field};\n")
        for field in float_fields:
            f.write(f"    float {field};\n")
        for field in bool_fields:
            f.write(f"    bool {field};\n")
        f.write("};\n\n")

        f.write("static const std::map<std::string, DeviceFingerprint> G_DEVICE_PROFILES = {\n")

        for name, fields in profiles:
            f.write(f'    {{ "{name}", {{\n')
            for field in ordered_fields:
                val = fields.get(field, "")
                val = val.replace('"', '\\"')
                f.write(f'        "{val}",\n')
            for field in int_fields:
                default_val = "8" if field == "core_count" else "6"
                val = fields.get(field, default_val)
                f.write(f'        {val},\n')
            for field in float_fields:
                val = fields.get(field, "0.0f")
                f.write(f'        {val},\n')
            for field in bool_fields:
                val = fields.get(field, "false")
                f.write(f'        {val},\n')
            f.write("    } },\n")

        f.write("};\n")

if __name__ == "__main__":
    # ADVERTENCIA (PR29): Este script requiere un archivo externo 'DeviceData.kt.txt'
    # que NO está incluido en el repositorio. Dicho archivo debe tener el formato
    # Kotlin DSL: "Device Name" to DeviceFingerprint(field = "value", ...)
    # Si editas omni_profiles.h directamente (formato C++ posicional), este script
    # NO puede regenerar el header desde ese archivo — son formatos incompatibles.
    # Para uso: proveer DeviceData.kt.txt externo antes de ejecutar.
    import os
    if not os.path.exists("DeviceData.kt.txt"):
        print("ERROR: 'DeviceData.kt.txt' no encontrado en el directorio actual.")
        print("Este archivo fuente externo es requerido y no está incluido en el repo.")
        print("Si editas omni_profiles.h directamente, no necesitas ejecutar este script.")
        sys.exit(1)
    generate_header("DeviceData.kt.txt", "jni/omni_profiles.h")
