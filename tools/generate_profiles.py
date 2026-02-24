import re
import sys

def generate_header(input_file, output_file):
    with open(input_file, 'r') as f:
        content = f.read()

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

        if fields:
            profiles.append((name, fields))
        else:
            print(f"Warning: Failed to parse fields for {name}")

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
            "buildVersionCodename", "buildVersionPreviewSdk"
        ]

        for field in ordered_fields:
            f.write(f"    const char* {field};\n")
        f.write("};\n\n")

        f.write("static const std::map<std::string, DeviceFingerprint> G_DEVICE_PROFILES = {\n")

        for name, fields in profiles:
            f.write(f'    {{ "{name}", {{\n')
            for field in ordered_fields:
                val = fields.get(field, "")
                val = val.replace('"', '\\"')
                f.write(f'        "{val}",\n')
            f.write("    } },\n")

        f.write("};\n")

if __name__ == "__main__":
    generate_header("DeviceData.kt.txt", "jni/omni_profiles.h")
