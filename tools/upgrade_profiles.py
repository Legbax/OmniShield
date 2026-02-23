import re
import sys

def upgrade_profiles(input_file, output_file):
    with open(input_file, 'r') as f:
        content = f.read()

    # Define new struct fields
    extra_fields = [
        "gpuVendor", "gpuRenderer", "gpuVersion",
        "screenWidth", "screenHeight", "screenDensity"
    ]

    # Parse existing profiles
    # Regex to find: { "Name", { ... } }
    # We assume the format generated previously:
    # { "Name", {
    #     "val",
    #     "val",
    #     ...
    # } },

    # We need to capture the name and the block of values.
    # The values are comma separated strings in quotes.

    profiles = []

    # Find all profile blocks
    # Logic: Look for { "Name", {
    # Then capture until } },

    block_pattern = re.compile(r'\{\s*"([^"]+)",\s*\{(.*?)\}\s*\},', re.DOTALL)
    matches = block_pattern.findall(content)

    for name, body in matches:
        # Extract values
        # We need to preserve the order of existing values
        # The body is a list of "value", strings.

        # Simple split by comma, but handle newlines and spaces
        # Better: findall "([^"]*)"

        vals = re.findall(r'"([^"]*)"', body)

        # Map known fields to dict for logic
        # Current struct has 30 fields
        keys = [
            "manufacturer", "brand", "model", "device", "product", "hardware",
            "board", "bootloader", "fingerprint", "buildId", "tags", "type",
            "radioVersion", "incremental", "securityPatch", "release",
            "boardPlatform", "eglDriver", "openGlEs", "hardwareChipname",
            "zygote", "vendorFingerprint", "display", "buildDescription",
            "buildFlavor", "buildHost", "buildUser", "buildDateUtc",
            "buildVersionCodename", "buildVersionPreviewSdk"
        ]

        data = dict(zip(keys, vals))

        # Logic for new fields
        platform = data.get("boardPlatform", "").lower()
        hardware = data.get("hardware", "").lower()

        gpu_vendor = "Qualcomm"
        gpu_renderer = "Adreno (TM) 618" # Default
        gpu_version = "OpenGL ES 3.2 V@0502.0 (GIT@0000000, I00000000, 1600000000) (Date:01/01/2021)"

        width = "1080"
        height = "2400"
        density = "440"

        if "lahaina" in platform:
            gpu_renderer = "Adreno (TM) 660"
        elif "kona" in platform:
            gpu_renderer = "Adreno (TM) 650"
        elif "msmnile" in platform:
            gpu_renderer = "Adreno (TM) 640"
        elif "lito" in platform:
            gpu_renderer = "Adreno (TM) 620"
        elif "holi" in platform:
            gpu_renderer = "Adreno (TM) 642L"
        elif "atoll" in platform or "sm7150" in hardware:
            gpu_renderer = "Adreno (TM) 618"
        elif "bengal" in platform or "trinket" in platform:
            gpu_renderer = "Adreno (TM) 610"
        elif "sdm670" in platform:
            gpu_renderer = "Adreno (TM) 615"
        elif "mt6833" in platform or "mt6833" in hardware:
            gpu_vendor = "ARM"
            gpu_renderer = "Mali-G57 MC2"
        elif "mt6768" in platform or "mt6769" in platform or "mt6768" in hardware:
            gpu_vendor = "ARM"
            gpu_renderer = "Mali-G52 MC2"
        elif "mt6785" in platform:
            gpu_vendor = "ARM"
            gpu_renderer = "Mali-G76 MC4"
        elif "mt6765" in platform:
            gpu_vendor = "ARM"
            gpu_renderer = "Mali-G52 MC1"
        elif "exynos9610" in platform:
            gpu_vendor = "ARM"
            gpu_renderer = "Mali-G72 MP3"
        elif "exynos850" in platform:
            gpu_vendor = "ARM"
            gpu_renderer = "Mali-G52"
        elif "exynos9825" in platform:
            gpu_vendor = "ARM"
            gpu_renderer = "Mali-G76 MP12"

        data["gpuVendor"] = gpu_vendor
        data["gpuRenderer"] = gpu_renderer
        data["gpuVersion"] = gpu_version
        data["screenWidth"] = width
        data["screenHeight"] = height
        data["screenDensity"] = density

        profiles.append((name, vals + [gpu_vendor, gpu_renderer, gpu_version, width, height, density]))

    # Write output
    with open(output_file, 'w') as f:
        f.write("#pragma once\n")
        f.write("#include <string>\n")
        f.write("#include <map>\n\n")

        f.write("struct DeviceFingerprint {\n")

        all_keys = [
            "manufacturer", "brand", "model", "device", "product", "hardware",
            "board", "bootloader", "fingerprint", "buildId", "tags", "type",
            "radioVersion", "incremental", "securityPatch", "release",
            "boardPlatform", "eglDriver", "openGlEs", "hardwareChipname",
            "zygote", "vendorFingerprint", "display", "buildDescription",
            "buildFlavor", "buildHost", "buildUser", "buildDateUtc",
            "buildVersionCodename", "buildVersionPreviewSdk",
            "gpuVendor", "gpuRenderer", "gpuVersion",
            "screenWidth", "screenHeight", "screenDensity"
        ]

        for field in all_keys:
            f.write(f"    const char* {field};\n")
        f.write("};\n\n")

        f.write("static const std::map<std::string, DeviceFingerprint> VORTEX_PROFILES = {\n")

        for name, vals in profiles:
            f.write(f'    {{ "{name}", {{\n')
            for val in vals:
                f.write(f'        "{val}",\n')
            f.write("    } },\n")

        f.write("};\n")

if __name__ == "__main__":
    upgrade_profiles("jni/vortex_profiles.h", "jni/vortex_profiles.h")
