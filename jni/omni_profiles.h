#pragma once
#include <string>
#include <cstdint>

struct DeviceFingerprint {
    // ── 36 strings (posiciones 0–35) ────────────────────────────────────────
    const char* manufacturer;
    const char* brand;
    const char* model;
    const char* device;
    const char* product;
    const char* hardware;
    const char* board;
    const char* bootloader;
    const char* fingerprint;
    const char* buildId;
    const char* tags;
    const char* type;
    const char* radioVersion;
    const char* incremental;
    const char* securityPatch;
    const char* release;
    const char* boardPlatform;
    const char* eglDriver;
    const char* openGlEs;
    const char* hardwareChipname;
    const char* zygote;
    const char* vendorFingerprint;
    const char* display;
    const char* buildDescription;
    const char* buildFlavor;
    const char* buildHost;
    const char* buildUser;
    const char* buildDateUtc;
    const char* buildVersionCodename;
    const char* buildVersionPreviewSdk;
    const char* gpuVendor;
    const char* gpuRenderer;
    const char* gpuVersion;
    const char* screenWidth;
    const char* screenHeight;
    const char* screenDensity;
    // ── int / int32_t (posiciones 36–41) ────────────────────────────────────
    // ORDEN: core_count, ram_gb, luego los 4 int32_t de Camera2 (PR44)
    // Este orden coincide con generate_profiles.py → int_fields
    int      core_count;
    int      ram_gb;
    int32_t  pixelArrayWidth;       // PR44 rear  — SENSOR_INFO_PIXEL_ARRAY_SIZE w
    int32_t  pixelArrayHeight;      // PR44 rear  — SENSOR_INFO_PIXEL_ARRAY_SIZE h
    int32_t  frontPixelArrayWidth;  // PR44 front — SENSOR_INFO_PIXEL_ARRAY_SIZE w
    int32_t  frontPixelArrayHeight; // PR44 front — SENSOR_INFO_PIXEL_ARRAY_SIZE h
    // ── floats (posiciones 42–54) ────────────────────────────────────────────
    // ORDEN: sensores PR38+39, luego Camera2 PR44 rear, luego Camera2 PR44 front
    // Este orden coincide con generate_profiles.py → float_fields
    float accelMaxRange;              // PR38+39 — acelerómetro
    float accelResolution;            // PR38+39
    float gyroMaxRange;               // PR38+39 — giroscopio
    float gyroResolution;             // PR38+39
    float magMaxRange;                // PR38+39 — magnetómetro
    float sensorPhysicalWidth;        // PR44 rear  — SENSOR_INFO_PHYSICAL_SIZE w (mm)
    float sensorPhysicalHeight;       // PR44 rear  — SENSOR_INFO_PHYSICAL_SIZE h (mm)
    float focalLength;                // PR44 rear  — LENS_INFO_AVAILABLE_FOCAL_LENGTHS (mm)
    float aperture;                   // PR44 rear  — LENS_INFO_AVAILABLE_APERTURES (f-num)
    float frontSensorPhysicalWidth;   // PR44 front — SENSOR_INFO_PHYSICAL_SIZE w (mm)
    float frontSensorPhysicalHeight;  // PR44 front — SENSOR_INFO_PHYSICAL_SIZE h (mm)
    float frontFocalLength;           // PR44 front — LENS_INFO_AVAILABLE_FOCAL_LENGTHS (mm)
    float frontAperture;              // PR44 front — LENS_INFO_AVAILABLE_APERTURES (f-num)
    // ── bools (posiciones 55–57) ─────────────────────────────────────────────
    // Este orden coincide con generate_profiles.py → bool_fields
    bool hasHeartRateSensor;
    bool hasBarometerSensor;
    bool hasFingerprintWakeupSensor;
};

// PR60: Array estático POD en .rodata — cero guard variables, cero heap.
// Un static const de tipos trivialmente construibles (const char*, int, float, bool)
// se inicializa en tiempo de compilación: no hay guard variable, no hay riesgo fork.
inline const DeviceFingerprint* findProfile(const std::string& name) {
    struct Entry { const char* n; DeviceFingerprint fp; };
    static const Entry TABLE[] = {
    { "Redmi 9", {
        "Xiaomi", "Redmi", "Redmi 9", "lancelot",
        "lancelot_global", "mt6768", "lancelot", "V12.5.6.0.RJCMIXM",
        "Redmi/lancelot_global/lancelot:11/RP1A.200720.011/V12.5.6.0.RJCMIXM:user/release-keys", "RP1A.200720.011", "release-keys", "user",
        "MOLY.LR12A.R3.MP.V84.P47", "V12.5.6.0.RJCMIXM", "2021-10-05", "11",
        "mt6768", "mali", "196610", "MT6768",
        "zygote64_32", "Redmi/lancelot_global/lancelot:11/RP1A.200720.011/V12.5.6.0.RJCMIXM:user/release-keys", "RP1A.200720.011", "lancelot_global-user 11 RP1A.200720.011 V12.5.6.0.RJCMIXM release-keys",
        "lancelot_global-user", "pangu-build-component-system-175411", "builder", "1633392000",
        "REL", "0", "ARM", "Mali-G52 MC2",
        "OpenGL ES 3.2 v1.r21p0-01rel0.a51a0c509f2714d8e5acbde47570a4b2", "1080", "2340", "395", 8, 4,
        4000, 3000, 3264, 2448,                                  // PR44: pixelArray rear/front (int32_t)
        39.2266f, 0.0011974f, 34.906586f, 0.001064f, 4912.0f,  // BMA4xy (Bosch),
        6.40f, 4.80f, 4.74f, 1.79f,                              // PR44: rear physSize + focal + aperture
        4.00f, 3.00f, 2.18f, 2.45f,                              // PR44: front physSize + focal + aperture
        false, false, false                                      // PR38+39: sensor presence bools
    } },
    { "Redmi 10X 4G", {
        "Xiaomi", "Redmi", "M2004J7AC", "merlin",
        "merlin", "mt6769", "merlin", "V12.5.3.0.QJJCNXM",
        "Redmi/merlin/merlin:11/RP1A.200720.011/V12.5.3.0.QJJCNXM:user/release-keys", "RP1A.200720.011", "release-keys", "user",
        "MOLY.LR12A.R3.MP.V110.6", "V12.5.3.0.QJJCNXM", "2021-07-01", "11",
        "mt6769", "mali", "196610", "MT6769",
        "zygote64_32", "Redmi/merlin/merlin:11/RP1A.200720.011/V12.5.3.0.QJJCNXM:user/release-keys", "RP1A.200720.011", "merlin-user 11 RP1A.200720.011 V12.5.3.0.QJJCNXM release-keys",
        "merlin-user", "pangu-build-component-system-175411", "builder", "1622505600",
        "REL", "0", "ARM", "Mali-G52 MC2",
        "OpenGL ES 3.2 v1.r21p0-01rel0.a51a0c509f2714d8e5acbde47570a4b2", "1080", "2400", "403", 8, 4,
        8000, 6000, 4608, 3456,                                  // PR44: pixelArray rear/front (int32_t)
        39.2266f, 0.0011974f, 34.906586f, 0.001064f, 4912.0f,  // BMA4xy (Bosch),
        6.40f, 4.80f, 4.74f, 1.79f,                              // PR44: rear physSize + focal + aperture
        4.00f, 3.00f, 2.18f, 2.45f,                              // PR44: front physSize + focal + aperture
        false, false, false                                      // PR38+39: sensor presence bools
    } },
    { "Galaxy A31", {
        "samsung", "samsung", "SM-A315F", "a31",
        "a31sqz", "mt6768", "mt6768", "A315FXXU4CUH1",
        "samsung/a31sqz/a31:11/RP1A.200720.012/A315FXXU4CUH1:user/release-keys", "RP1A.200720.012", "release-keys", "user",
        "MOLY.LR12A.R3.MP.V84.P47", "A315FXXU4CUH1", "2021-08-01", "11",
        "mt6768", "mali", "196610", "MT6768",
        "zygote64_32", "samsung/a31sqz/a31:11/RP1A.200720.012/A315FXXU4CUH1:user/release-keys", "RP1A.200720.012", "a31sqz-user 11 RP1A.200720.012 A315FXXU4CUH1 release-keys",
        "a31sqz-user", "SWDD8100", "dpi", "1627776000",
        "REL", "0", "ARM", "Mali-G52 MC2",
        "OpenGL ES 3.2 v1.r21p0-01rel0.a51a0c509f2714d8e5acbde47570a4b2", "1080", "2400", "411", 8, 4,
        8000, 6000, 5184, 3880,                                  // PR44: pixelArray rear/front (int32_t)
        156.9064f, 0.004788f, 34.906586f, 0.001064f, 1200.0f,  // BMI160 (Bosch) - Samsung usa BMI160 en la mayoría de la serie A,
        6.40f, 4.80f, 4.28f, 2.0f,                              // PR44: rear physSize + focal + aperture
        4.61f, 3.46f, 2.49f, 2.2f,                              // PR44: front physSize + focal + aperture
        false, false, true                                      // PR38+39: sensor presence bools
    } },
    { "Galaxy A21s", {
        "samsung", "samsung", "SM-A217F", "a21s",
        "a21ssqz", "exynos850", "exynos850", "A217FXXU4CUJ1",
        "samsung/a21ssqz/a21s:11/RP1A.200720.012/A217FXXU4CUJ1:user/release-keys", "RP1A.200720.012", "release-keys", "user",
        "g850-210401-5286", "A217FXXU4CUJ1", "2021-07-01", "11",
        "exynos850", "mali", "196610", "Exynos850",
        "zygote64_32", "samsung/a21ssqz/a21s:11/RP1A.200720.012/A217FXXU4CUJ1:user/release-keys", "RP1A.200720.012", "a21ssqz-user 11 RP1A.200720.012 A217FXXU4CUJ1 release-keys",
        "a21ssqz-user", "SWDD7700", "dpi", "1625097600",
        "REL", "0", "ARM", "Mali-G52 MC1",
        "OpenGL ES 3.2 v1.r25p0-01rel0.a51a0c509f2714d8e5acbde47570a4b2", "720", "1600", "270", 8, 4,
        8000, 6000, 4128, 3096,                                  // PR44: pixelArray rear/front (int32_t)
        156.9064f, 0.004788f, 34.906586f, 0.001064f, 1200.0f,  // BMI160 (Bosch),
        6.40f, 4.80f, 4.28f, 2.0f,                              // PR44: rear physSize + focal + aperture
        3.67f, 2.75f, 1.93f, 2.4f,                              // PR44: front physSize + focal + aperture
        false, false, true                                      // PR38+39: sensor presence bools
    } },
    { "POCO M3 Pro 5G", {
        "Xiaomi", "POCO", "M2103K19PG", "camellia",
        "camellia_global", "mt6833", "camellia", "V12.5.2.0.RKRMIXM",
        "POCO/camellia_global/camellia:11/RP1A.200720.011/V12.5.2.0.RKRMIXM:user/release-keys", "RP1A.200720.011", "release-keys", "user",
        "MOLY.LR12A.R3.MP.V73.6", "V12.5.2.0.RKRMIXM", "2021-07-01", "11",
        "mt6833", "mali", "196610", "MT6833",
        "zygote64_32", "POCO/camellia_global/camellia:11/RP1A.200720.011/V12.5.2.0.RKRMIXM:user/release-keys", "RP1A.200720.011", "camellia_global-user 11 RP1A.200720.011 V12.5.2.0.RKRMIXM release-keys",
        "camellia_global-user", "pangu-build-component-system-176812", "builder", "1625097600",
        "REL", "0", "ARM", "Mali-G57 MC2",
        "OpenGL ES 3.2 v1.r21p0-01rel0.a51a0c509f2714d8e5acbde47570a4b2", "1080", "2400", "404", 8, 6,
        9248, 6944, 5184, 3880,                                  // PR44: pixelArray rear/front (int32_t)
        39.2266f, 0.0011974f, 34.906586f, 0.001064f, 2000.0f,  // BMA253 (Bosch),
        7.40f, 5.56f, 5.09f, 2.0f,                              // PR44: rear physSize + focal + aperture
        4.61f, 3.46f, 2.49f, 2.2f,                              // PR44: front physSize + focal + aperture
        false, false, false                                      // PR38+39: sensor presence bools
    } },
    { "Galaxy A32 5G", {
        "samsung", "samsung", "SM-A326B", "a32x",
        "a32xsqz", "mt6853", "mt6853", "A326BXXU4AUH1",
        "samsung/a32xsqz/a32x:11/RP1A.200720.012/A326BXXU4AUH1:user/release-keys", "RP1A.200720.012", "release-keys", "user",
        "MOLY.LR12A.R3.MP.V84.6", "A326BXXU4AUH1", "2021-08-01", "11",
        "mt6853", "mali", "196610", "MT6853",
        "zygote64_32", "samsung/a32xsqz/a32x:11/RP1A.200720.012/A326BXXU4AUH1:user/release-keys", "RP1A.200720.012", "a32xsqz-user 11 RP1A.200720.012 A326BXXU4AUH1 release-keys",
        "a32xsqz-user", "SWDD8201", "dpi", "1627776000",
        "REL", "0", "ARM", "Mali-G57 MC3",
        "OpenGL ES 3.2 v1.r26p0-01rel0.a51a0c509f2714d8e5acbde47570a4b2", "720", "1600", "270", 8, 4,
        8000, 6000, 4128, 3096,                                  // PR44: pixelArray rear/front (int32_t)
        156.9064f, 0.004788f, 34.906586f, 0.001064f, 1200.0f,  // BMI160 (Bosch),
        6.40f, 4.80f, 4.28f, 2.0f,                              // PR44: rear physSize + focal + aperture
        3.67f, 2.75f, 1.93f, 2.4f,                              // PR44: front physSize + focal + aperture
        false, false, true                                      // PR38+39: sensor presence bools
    } },
    { "Realme 8", {
        "realme", "realme", "RMX3085", "RMX3085",
        "RMX3085", "mt6785", "RMX3085", "1630454400000",
        "realme/RMX3085/RMX3085:11/RP1A.200720.011/1630454400000:user/release-keys", "RP1A.200720.011", "release-keys", "user",
        "MOLY.LR12A.R3.MP.V89.6", "1630454400000", "2021-09-01", "11",
        "mt6785", "mali", "196610", "MT6785",
        "zygote64_32", "realme/RMX3085/RMX3085:11/RP1A.200720.011/1630454400000:user/release-keys", "RP1A.200720.011", "RMX3085-user 11 RP1A.200720.011 1630454400000 release-keys",
        "RMX3085-user", "oppo-build-03", "oppo", "1630454400",
        "REL", "0", "ARM", "Mali-G76 MC4",
        "OpenGL ES 3.2 v1.r26p0-01rel0.a51a0c509f2714d8e5acbde47570a4b2", "1080", "2400", "411", 8, 6,
        9248, 6944, 4608, 3456,                                  // PR44: pixelArray rear/front (int32_t)
        39.2266f, 0.0011974f, 34.906586f, 0.001064f, 2000.0f,  // BMA253 (Bosch),
        7.40f, 5.56f, 5.09f, 2.0f,                              // PR44: rear physSize + focal + aperture
        4.00f, 3.00f, 2.18f, 2.45f,                              // PR44: front physSize + focal + aperture
        false, false, false                                      // PR38+39: sensor presence bools
    } },
    { "Redmi 9 Prime", {  // ✅ mt6768 Mali-G52 MC2
        "Xiaomi", "Redmi", "M2004J19I", "javalin",
        "javalin_in", "mt6768", "javalin", "unknown",
        "Redmi/javalin_in/javalin:11/RP1A.200720.011/V12.5.2.0.RJCINXM:user/release-keys", "RP1A.200720.011", "release-keys", "user",
        "MOLY.LR12A.R3.MP.V84.P47", "V12.5.2.0.RJCINXM", "2021-10-01", "11",
        "mt6768", "mali", "196610", "mt6768",
        "zygote64_32", "Redmi/javalin_in/javalin:11/RP1A.200720.011/V12.5.2.0.RJCINXM:user/release-keys", "RP1A.200720.011.V12.5.2.0.RJCINXM", "javalin_in-user 11 RP1A.200720.011 V12.5.2.0.RJCINXM release-keys",
        "javalin_in-user", "ubuntu-server", "builder", "1625097600",
        "REL", "0", "ARM", "Mali-G52 MC2",
        "OpenGL ES 3.2 v1.r26p0-01rel0", "1080", "2340", "440", 8, 4,
        4000, 3000, 3264, 2448,
        78.4532f, 0.0012f, 34.9065f, 0.0005f, 4915.2f,
        5.64f, 4.23f, 4.74f, 1.79f,
        3.67f, 2.76f, 3.54f, 2.0f,
        false, false, true
    } },
    { "Redmi Note 9", {  // ✅ mt6768 Mali-G52 MC2 (Helio G85 China merlin)
        "Xiaomi", "Redmi", "M2003J15SC", "merlin",
        "merlin", "mt6768", "merlin", "unknown",
        "Redmi/merlin/merlin:11/RP1A.200720.011/V12.5.3.0.RJCCNXM:user/release-keys", "RP1A.200720.011", "release-keys", "user",
        "MOLY.LR12A.R3.MP.V84.P47", "V12.5.3.0.RJCCNXM", "2021-10-01", "11",
        "mt6768", "mali", "196610", "mt6768",
        "zygote64_32", "Redmi/merlin/merlin:11/RP1A.200720.011/V12.5.3.0.RJCCNXM:user/release-keys", "RP1A.200720.011.V12.5.3.0.RJCCNXM", "merlin-user 11 RP1A.200720.011 V12.5.3.0.RJCCNXM release-keys",
        "merlin-user", "ubuntu-server", "builder", "1625097600",
        "REL", "0", "ARM", "Mali-G52 MC2",
        "OpenGL ES 3.2 v1.r26p0-01rel0", "1080", "2340", "440", 8, 4,
        4000, 3000, 3264, 2448,
        78.4532f, 0.0012f, 34.9065f, 0.0005f, 4915.2f,
        5.64f, 4.23f, 4.74f, 1.79f,
        3.67f, 2.76f, 3.54f, 2.0f,
        false, false, true
    } },
    { "Tecno Spark 6", {  // ✅ mt6768 Mali-G52 MC2
        "Tecno", "Tecno", "KE7", "KE7",
        "KE7-GL", "mt6768", "KE7", "unknown",
        "Tecno/KE7-GL/KE7:11/RP1A.200720.011/210420V321:user/release-keys", "RP1A.200720.011", "release-keys", "user",
        "MOLY.LR12A.R3.MP.V84.P47", "210420V321", "2021-10-01", "11",
        "mt6768", "mali", "196610", "mt6768",
        "zygote64_32", "Tecno/KE7-GL/KE7:11/RP1A.200720.011/210420V321:user/release-keys", "RP1A.200720.011.210420V321", "KE7-GL-user 11 RP1A.200720.011 210420V321 release-keys",
        "KE7-GL-user", "ubuntu-server", "builder", "1625097600",
        "REL", "0", "ARM", "Mali-G52 MC2",
        "OpenGL ES 3.2 v1.r26p0-01rel0", "1080", "2340", "440", 8, 4,
        4000, 3000, 3264, 2448,
        78.4532f, 0.0012f, 34.9065f, 0.0005f, 4915.2f,
        5.64f, 4.23f, 4.74f, 1.79f,
        3.67f, 2.76f, 3.54f, 2.0f,
        false, false, true
    } },
    { "Vivo S1", {  // ✅ mt6768 Mali-G52 MC2
        "vivo", "vivo", "vivo 1907", "1907",
        "1907", "mt6768", "1907", "unknown",
        "vivo/1907/1907:11/RP1A.200720.012/compiler:user/release-keys", "RP1A.200720.012", "release-keys", "user",
        "MOLY.LR12A.R3.MP.V84.P47", "compiler", "2021-10-01", "11",
        "mt6768", "mali", "196610", "mt6768",
        "zygote64_32", "vivo/1907/1907:11/RP1A.200720.012/compiler:user/release-keys", "RP1A.200720.012.compiler", "1907-user 11 RP1A.200720.012 compiler release-keys",
        "1907-user", "ubuntu-server", "builder", "1625097600",
        "REL", "0", "ARM", "Mali-G52 MC2",
        "OpenGL ES 3.2 v1.r26p0-01rel0", "1080", "2340", "440", 8, 4,
        4000, 3000, 3264, 2448,
        78.4532f, 0.0012f, 34.9065f, 0.0005f, 4915.2f,
        5.64f, 4.23f, 4.74f, 1.79f,
        3.67f, 2.76f, 3.54f, 2.0f,
        false, false, true
    } },
    { "Vivo Y19", {  // ✅ mt6768 Mali-G52 MC2
        "vivo", "vivo", "vivo 1915", "1915",
        "1915", "mt6768", "1915", "unknown",
        "vivo/1915/1915:11/RP1A.200720.012/compiler:user/release-keys", "RP1A.200720.012", "release-keys", "user",
        "MOLY.LR12A.R3.MP.V84.P47", "compiler", "2021-10-01", "11",
        "mt6768", "mali", "196610", "mt6768",
        "zygote64_32", "vivo/1915/1915:11/RP1A.200720.012/compiler:user/release-keys", "RP1A.200720.012.compiler", "1915-user 11 RP1A.200720.012 compiler release-keys",
        "1915-user", "ubuntu-server", "builder", "1625097600",
        "REL", "0", "ARM", "Mali-G52 MC2",
        "OpenGL ES 3.2 v1.r26p0-01rel0", "1080", "2340", "440", 8, 4,
        4000, 3000, 3264, 2448,
        78.4532f, 0.0012f, 34.9065f, 0.0005f, 4915.2f,
        5.64f, 4.23f, 4.74f, 1.79f,
        3.67f, 2.76f, 3.54f, 2.0f,
        false, false, true
    } },
    { "Vivo Y20G", {  // ✅ mt6769 Mali-G52 MC2
        "vivo", "vivo", "V2037", "V2037",
        "V2037", "mt6769", "V2037", "unknown",
        "vivo/V2037/V2037:11/RP1A.200720.012/compiler:user/release-keys", "RP1A.200720.012", "release-keys", "user",
        "MOLY.LR12A.R3.MP.V84.P47", "compiler", "2021-10-01", "11",
        "mt6769", "mali", "196610", "mt6769",
        "zygote64_32", "vivo/V2037/V2037:11/RP1A.200720.012/compiler:user/release-keys", "RP1A.200720.012.compiler", "V2037-user 11 RP1A.200720.012 compiler release-keys",
        "V2037-user", "ubuntu-server", "builder", "1625097600",
        "REL", "0", "ARM", "Mali-G52 MC2",
        "OpenGL ES 3.2 v1.r26p0-01rel0", "1080", "2340", "440", 8, 4,
        4000, 3000, 3264, 2448,
        78.4532f, 0.0012f, 34.9065f, 0.0005f, 4915.2f,
        5.64f, 4.23f, 4.74f, 1.79f,
        3.67f, 2.76f, 3.54f, 2.0f,
        false, false, true
    } },
    { "Samsung Galaxy A31", {  // ✅ mt6768 Mali-G52 MC2 (variant a31nnxx)
        "samsung", "samsung", "SM-A315F", "a31",
        "a31nnxx", "mt6768", "a31", "unknown",
        "samsung/a31nnxx/a31:11/RP1A.200720.012/A315FXXU1CUD1:user/release-keys", "RP1A.200720.012", "release-keys", "user",
        "MOLY.LR12A.R3.MP.V84.P47", "A315FXXU1CUD1", "2021-10-01", "11",
        "mt6768", "mali", "196610", "mt6768",
        "zygote64_32", "samsung/a31nnxx/a31:11/RP1A.200720.012/A315FXXU1CUD1:user/release-keys", "RP1A.200720.012.A315FXXU1CUD1", "a31nnxx-user 11 RP1A.200720.012 A315FXXU1CUD1 release-keys",
        "a31nnxx-user", "ubuntu-server", "builder", "1625097600",
        "REL", "0", "ARM", "Mali-G52 MC2",
        "OpenGL ES 3.2 v1.r26p0-01rel0", "1080", "2340", "440", 8, 4,
        4000, 3000, 3264, 2448,
        78.4532f, 0.0012f, 34.9065f, 0.0005f, 4915.2f,
        5.64f, 4.23f, 4.74f, 1.79f,
        3.67f, 2.76f, 3.54f, 2.0f,
        false, false, true
    } },
    { "POCO M2", {  // ✅ mt6768 Mali-G52 MC2
        "POCO", "POCO", "M2004J19PI", "shiva",
        "shiva_in", "mt6768", "shiva", "unknown",
        "POCO/shiva_in/shiva:11/RP1A.200720.011/V12.5.3.0.RJRINXM:user/release-keys", "RP1A.200720.011", "release-keys", "user",
        "MOLY.LR12A.R3.MP.V84.P47", "V12.5.3.0.RJRINXM", "2021-10-01", "11",
        "mt6768", "mali", "196610", "mt6768",
        "zygote64_32", "POCO/shiva_in/shiva:11/RP1A.200720.011/V12.5.3.0.RJRINXM:user/release-keys", "RP1A.200720.011.V12.5.3.0.RJRINXM", "shiva_in-user 11 RP1A.200720.011 V12.5.3.0.RJRINXM release-keys",
        "shiva_in-user", "ubuntu-server", "builder", "1625097600",
        "REL", "0", "ARM", "Mali-G52 MC2",
        "OpenGL ES 3.2 v1.r26p0-01rel0", "1080", "2340", "440", 8, 4,
        4000, 3000, 3264, 2448,
        78.4532f, 0.0012f, 34.9065f, 0.0005f, 4915.2f,
        5.64f, 4.23f, 4.74f, 1.79f,
        3.67f, 2.76f, 3.54f, 2.0f,
        false, false, true
    } },
    { "Realme Narzo 20", {  // ✅ mt6768 Mali-G52 MC2
        "realme", "realme", "RMX2193", "RMX2193",
        "RMX2193", "mt6768", "RMX2193", "unknown",
        "realme/RMX2193/RMX2193:11/RP1A.200720.011/1623412341234:user/release-keys", "RP1A.200720.011", "release-keys", "user",
        "MOLY.LR12A.R3.MP.V84.P47", "1623412341234", "2021-10-01", "11",
        "mt6768", "mali", "196610", "mt6768",
        "zygote64_32", "realme/RMX2193/RMX2193:11/RP1A.200720.011/1623412341234:user/release-keys", "RP1A.200720.011.1623412341234", "RMX2193-user 11 RP1A.200720.011 1623412341234 release-keys",
        "RMX2193-user", "ubuntu-server", "builder", "1625097600",
        "REL", "0", "ARM", "Mali-G52 MC2",
        "OpenGL ES 3.2 v1.r26p0-01rel0", "1080", "2340", "440", 8, 4,
        4000, 3000, 3264, 2448,
        78.4532f, 0.0012f, 34.9065f, 0.0005f, 4915.2f,
        5.64f, 4.23f, 4.74f, 1.79f,
        3.67f, 2.76f, 3.54f, 2.0f,
        false, false, true
    } },
    { "Realme Narzo 10", {  // ✅ mt6768 Mali-G52 MC2
        "realme", "realme", "RMX2040", "RMX2040",
        "RMX2040", "mt6768", "RMX2040", "unknown",
        "realme/RMX2040/RMX2040:11/RP1A.200720.011/1652695552124:user/release-keys", "RP1A.200720.011", "release-keys", "user",
        "MOLY.LR12A.R3.MP.V84.P47", "1652695552124", "2021-10-01", "11",
        "mt6768", "mali", "196610", "mt6768",
        "zygote64_32", "realme/RMX2040/RMX2040:11/RP1A.200720.011/1652695552124:user/release-keys", "RP1A.200720.011.1652695552124", "RMX2040-user 11 RP1A.200720.011 1652695552124 release-keys",
        "RMX2040-user", "ubuntu-server", "builder", "1625097600",
        "REL", "0", "ARM", "Mali-G52 MC2",
        "OpenGL ES 3.2 v1.r26p0-01rel0", "1080", "2340", "440", 8, 4,
        4000, 3000, 3264, 2448,
        78.4532f, 0.0012f, 34.9065f, 0.0005f, 4915.2f,
        5.64f, 4.23f, 4.74f, 1.79f,
        3.67f, 2.76f, 3.54f, 2.0f,
        false, false, true
    } },
    { "Realme 6i", {  // ✅ mt6768 Mali-G52 MC2
        "realme", "realme", "RMX2040", "RMX2040",
        "RMX2040_Global", "mt6768", "RMX2040", "unknown",
        "realme/RMX2040_Global/RMX2040:11/RP1A.200720.011/1620000000:user/release-keys", "RP1A.200720.011", "release-keys", "user",
        "MOLY.LR12A.R3.MP.V84.P47", "1620000000", "2021-10-01", "11",
        "mt6768", "mali", "196610", "mt6768",
        "zygote64_32", "realme/RMX2040_Global/RMX2040:11/RP1A.200720.011/1620000000:user/release-keys", "RP1A.200720.011.1620000000", "RMX2040_Global-user 11 RP1A.200720.011 1620000000 release-keys",
        "RMX2040_Global-user", "ubuntu-server", "builder", "1625097600",
        "REL", "0", "ARM", "Mali-G52 MC2",
        "OpenGL ES 3.2 v1.r26p0-01rel0", "1080", "2340", "440", 8, 4,
        4000, 3000, 3264, 2448,
        78.4532f, 0.0012f, 34.9065f, 0.0005f, 4915.2f,
        5.64f, 4.23f, 4.74f, 1.79f,
        3.67f, 2.76f, 3.54f, 2.0f,
        false, false, true
    } },
    { "Moto G31", {  // ✅ mt6769 Mali-G52 MC2
        "motorola", "motorola", "moto g31", "cotone",
        "cotone_global", "mt6769", "cotone", "unknown",
        "motorola/cotone_global/cotone:11/RRW31.Q3-26-62-2/2d6f8:user/release-keys", "RRW31.Q3-26-62-2", "release-keys", "user",
        "MOLY.LR12A.R3.MP.V84.P47", "2d6f8", "2021-10-01", "11",
        "mt6769", "mali", "196610", "mt6769",
        "zygote64_32", "motorola/cotone_global/cotone:11/RRW31.Q3-26-62-2/2d6f8:user/release-keys", "RRW31.Q3-26-62-2.2d6f8", "cotone_global-user 11 RRW31.Q3-26-62-2 2d6f8 release-keys",
        "cotone_global-user", "ubuntu-server", "builder", "1625097600",
        "REL", "0", "ARM", "Mali-G52 MC2",
        "OpenGL ES 3.2 v1.r26p0-01rel0", "1080", "2340", "440", 8, 4,
        4000, 3000, 3264, 2448,
        78.4532f, 0.0012f, 34.9065f, 0.0005f, 4915.2f,
        5.64f, 4.23f, 4.74f, 1.79f,
        3.67f, 2.76f, 3.54f, 2.0f,
        false, false, true
    } },
    { "Galaxy A32 4G", {  // ✅ mt6769 Mali-G52 MC2
        "samsung", "samsung", "SM-A325F", "a32",
        "a32nnxx", "mt6769", "a32", "unknown",
        "samsung/a32nnxx/a32:11/RP1A.200720.012/A325FXXU1AUCC:user/release-keys", "RP1A.200720.012", "release-keys", "user",
        "MOLY.LR12A.R3.MP.V84.P47", "A325FXXU1AUCC", "2021-10-01", "11",
        "mt6769", "mali", "196610", "mt6769",
        "zygote64_32", "samsung/a32nnxx/a32:11/RP1A.200720.012/A325FXXU1AUCC:user/release-keys", "RP1A.200720.012.A325FXXU1AUCC", "a32nnxx-user 11 RP1A.200720.012 A325FXXU1AUCC release-keys",
        "a32nnxx-user", "ubuntu-server", "builder", "1625097600",
        "REL", "0", "ARM", "Mali-G52 MC2",
        "OpenGL ES 3.2 v1.r26p0-01rel0", "1080", "2340", "440", 8, 4,
        4000, 3000, 3264, 2448,
        78.4532f, 0.0012f, 34.9065f, 0.0005f, 4915.2f,
        5.64f, 4.23f, 4.74f, 1.79f,
        3.67f, 2.76f, 3.54f, 2.0f,
        false, false, true
    } },
    { "Realme C25s", {  // ✅ mt6769 Mali-G52 MC2
        "realme", "realme", "RMX3195", "RMX3195",
        "RMX3195", "mt6769", "RMX3195", "unknown",
        "realme/RMX3195/RMX3195:11/RP1A.200720.011/1620000000:user/release-keys", "RP1A.200720.011", "release-keys", "user",
        "MOLY.LR12A.R3.MP.V84.P47", "1620000000", "2021-10-01", "11",
        "mt6769", "mali", "196610", "mt6769",
        "zygote64_32", "realme/RMX3195/RMX3195:11/RP1A.200720.011/1620000000:user/release-keys", "RP1A.200720.011.1620000000", "RMX3195-user 11 RP1A.200720.011 1620000000 release-keys",
        "RMX3195-user", "ubuntu-server", "builder", "1625097600",
        "REL", "0", "ARM", "Mali-G52 MC2",
        "OpenGL ES 3.2 v1.r26p0-01rel0", "1080", "2340", "440", 8, 4,
        4000, 3000, 3264, 2448,
        78.4532f, 0.0012f, 34.9065f, 0.0005f, 4915.2f,
        5.64f, 4.23f, 4.74f, 1.79f,
        3.67f, 2.76f, 3.54f, 2.0f,
        false, false, true
    } },
    { "Infinix Note 10", {  // ✅ mt6769 Mali-G52 MC2
        "Infinix", "Infinix", "X693", "X693",
        "X693-GL", "mt6769", "X693", "unknown",
        "Infinix/X693-GL/X693:11/RP1A.200720.011/210515V262:user/release-keys", "RP1A.200720.011", "release-keys", "user",
        "MOLY.LR12A.R3.MP.V84.P47", "210515V262", "2021-10-01", "11",
        "mt6769", "mali", "196610", "mt6769",
        "zygote64_32", "Infinix/X693-GL/X693:11/RP1A.200720.011/210515V262:user/release-keys", "RP1A.200720.011.210515V262", "X693-GL-user 11 RP1A.200720.011 210515V262 release-keys",
        "X693-GL-user", "ubuntu-server", "builder", "1625097600",
        "REL", "0", "ARM", "Mali-G52 MC2",
        "OpenGL ES 3.2 v1.r26p0-01rel0", "1080", "2340", "440", 8, 4,
        4000, 3000, 3264, 2448,
        78.4532f, 0.0012f, 34.9065f, 0.0005f, 4915.2f,
        5.64f, 4.23f, 4.74f, 1.79f,
        3.67f, 2.76f, 3.54f, 2.0f,
        false, false, true
    } },
    { "Redmi Note 8 2021", {  // ✅ mt6769 Mali-G52 MC2
        "Xiaomi", "Redmi", "M2101K7AG", "biloba",
        "biloba_global", "mt6769", "biloba", "unknown",
        "Redmi/biloba_global/biloba:11/RP1A.200720.011/V12.5.8.0.RCUMIXM:user/release-keys", "RP1A.200720.011", "release-keys", "user",
        "MOLY.LR12A.R3.MP.V84.P47", "V12.5.8.0.RCUMIXM", "2021-10-01", "11",
        "mt6769", "mali", "196610", "mt6769",
        "zygote64_32", "Redmi/biloba_global/biloba:11/RP1A.200720.011/V12.5.8.0.RCUMIXM:user/release-keys", "RP1A.200720.011.V12.5.8.0.RCUMIXM", "biloba_global-user 11 RP1A.200720.011 V12.5.8.0.RCUMIXM release-keys",
        "biloba_global-user", "ubuntu-server", "builder", "1625097600",
        "REL", "0", "ARM", "Mali-G52 MC2",
        "OpenGL ES 3.2 v1.r26p0-01rel0", "1080", "2340", "440", 8, 4,
        4000, 3000, 3264, 2448,
        78.4532f, 0.0012f, 34.9065f, 0.0005f, 4915.2f,
        5.64f, 4.23f, 4.74f, 1.79f,
        3.67f, 2.76f, 3.54f, 2.0f,
        false, false, true
    } },
    { "Galaxy A22 4G", {  // ✅ mt6769 Mali-G52 MC2
        "samsung", "samsung", "SM-A225F", "a22",
        "a22nnxx", "mt6769", "a22", "unknown",
        "samsung/a22nnxx/a22:11/RP1A.200720.012/A225FXXU1AUF4:user/release-keys", "RP1A.200720.012", "release-keys", "user",
        "MOLY.LR12A.R3.MP.V84.P47", "A225FXXU1AUF4", "2021-10-01", "11",
        "mt6769", "mali", "196610", "mt6769",
        "zygote64_32", "samsung/a22nnxx/a22:11/RP1A.200720.012/A225FXXU1AUF4:user/release-keys", "RP1A.200720.012.A225FXXU1AUF4", "a22nnxx-user 11 RP1A.200720.012 A225FXXU1AUF4 release-keys",
        "a22nnxx-user", "ubuntu-server", "builder", "1625097600",
        "REL", "0", "ARM", "Mali-G52 MC2",
        "OpenGL ES 3.2 v1.r26p0-01rel0", "1080", "2340", "440", 8, 4,
        4000, 3000, 3264, 2448,
        78.4532f, 0.0012f, 34.9065f, 0.0005f, 4915.2f,
        5.64f, 4.23f, 4.74f, 1.79f,
        3.67f, 2.76f, 3.54f, 2.0f,
        false, false, true
    } },
    { "Moto G41", {  // ✅ mt6769 Mali-G52 MC2
        "motorola", "motorola", "moto g41", "corfu",
        "corfu_global", "mt6769", "corfu", "unknown",
        "motorola/corfu_global/corfu:11/RRU31.Q3-36-39/18a2d:user/release-keys", "RRU31.Q3-36-39", "release-keys", "user",
        "MOLY.LR12A.R3.MP.V84.P47", "18a2d", "2021-10-01", "11",
        "mt6769", "mali", "196610", "mt6769",
        "zygote64_32", "motorola/corfu_global/corfu:11/RRU31.Q3-36-39/18a2d:user/release-keys", "RRU31.Q3-36-39.18a2d", "corfu_global-user 11 RRU31.Q3-36-39 18a2d release-keys",
        "corfu_global-user", "ubuntu-server", "builder", "1625097600",
        "REL", "0", "ARM", "Mali-G52 MC2",
        "OpenGL ES 3.2 v1.r26p0-01rel0", "1080", "2340", "440", 8, 4,
        4000, 3000, 3264, 2448,
        78.4532f, 0.0012f, 34.9065f, 0.0005f, 4915.2f,
        5.64f, 4.23f, 4.74f, 1.79f,
        3.67f, 2.76f, 3.54f, 2.0f,
        false, false, true
    } },
    { "Realme Narzo 30A", {  // ✅ mt6768 Mali-G52 MC2
        "realme", "realme", "RMX3171", "RMX3171",
        "RMX3171", "mt6768", "RMX3171", "unknown",
        "realme/RMX3171/RMX3171:11/RP1A.200720.011/1620000000:user/release-keys", "RP1A.200720.011", "release-keys", "user",
        "MOLY.LR12A.R3.MP.V84.P47", "1620000000", "2021-10-01", "11",
        "mt6768", "mali", "196610", "mt6768",
        "zygote64_32", "realme/RMX3171/RMX3171:11/RP1A.200720.011/1620000000:user/release-keys", "RP1A.200720.011.1620000000", "RMX3171-user 11 RP1A.200720.011 1620000000 release-keys",
        "RMX3171-user", "ubuntu-server", "builder", "1625097600",
        "REL", "0", "ARM", "Mali-G52 MC2",
        "OpenGL ES 3.2 v1.r26p0-01rel0", "1080", "2340", "440", 8, 4,
        4000, 3000, 3264, 2448,
        78.4532f, 0.0012f, 34.9065f, 0.0005f, 4915.2f,
        5.64f, 4.23f, 4.74f, 1.79f,
        3.67f, 2.76f, 3.54f, 2.0f,
        false, false, true
    } },
    { "Realme Narzo 50A", {  // ✅ mt6769 Mali-G52 MC2
        "realme", "realme", "RMX3430", "RMX3430",
        "RMX3430", "mt6769", "RMX3430", "unknown",
        "realme/RMX3430/RMX3430:11/RP1A.200720.011/1630000000:user/release-keys", "RP1A.200720.011", "release-keys", "user",
        "MOLY.LR12A.R3.MP.V84.P47", "1630000000", "2021-10-01", "11",
        "mt6769", "mali", "196610", "mt6769",
        "zygote64_32", "realme/RMX3430/RMX3430:11/RP1A.200720.011/1630000000:user/release-keys", "RP1A.200720.011.1630000000", "RMX3430-user 11 RP1A.200720.011 1630000000 release-keys",
        "RMX3430-user", "ubuntu-server", "builder", "1625097600",
        "REL", "0", "ARM", "Mali-G52 MC2",
        "OpenGL ES 3.2 v1.r26p0-01rel0", "1080", "2340", "440", 8, 4,
        4000, 3000, 3264, 2448,
        78.4532f, 0.0012f, 34.9065f, 0.0005f, 4915.2f,
        5.64f, 4.23f, 4.74f, 1.79f,
        3.67f, 2.76f, 3.54f, 2.0f,
        false, false, true
    } },
    { "Infinix Hot 10s", {  // ✅ mt6769 Mali-G52 MC2
        "Infinix", "Infinix", "X689B", "X689B",
        "X689B-GL", "mt6769", "X689B", "unknown",
        "Infinix/X689B-GL/X689B:11/RP1A.200720.011/210401V182:user/release-keys", "RP1A.200720.011", "release-keys", "user",
        "MOLY.LR12A.R3.MP.V84.P47", "210401V182", "2021-10-01", "11",
        "mt6769", "mali", "196610", "mt6769",
        "zygote64_32", "Infinix/X689B-GL/X689B:11/RP1A.200720.011/210401V182:user/release-keys", "RP1A.200720.011.210401V182", "X689B-GL-user 11 RP1A.200720.011 210401V182 release-keys",
        "X689B-GL-user", "ubuntu-server", "builder", "1625097600",
        "REL", "0", "ARM", "Mali-G52 MC2",
        "OpenGL ES 3.2 v1.r26p0-01rel0", "1080", "2340", "440", 8, 4,
        4000, 3000, 3264, 2448,
        78.4532f, 0.0012f, 34.9065f, 0.0005f, 4915.2f,
        5.64f, 4.23f, 4.74f, 1.79f,
        3.67f, 2.76f, 3.54f, 2.0f,
        false, false, true
    } },
    { "Infinix Note 11i", {  // ✅ mt6769 Mali-G52 MC2
        "Infinix", "Infinix", "X697", "X697",
        "X697-GL", "mt6769", "X697", "unknown",
        "Infinix/X697-GL/X697:11/RP1A.200720.011/211020V120:user/release-keys", "RP1A.200720.011", "release-keys", "user",
        "MOLY.LR12A.R3.MP.V84.P47", "211020V120", "2021-10-01", "11",
        "mt6769", "mali", "196610", "mt6769",
        "zygote64_32", "Infinix/X697-GL/X697:11/RP1A.200720.011/211020V120:user/release-keys", "RP1A.200720.011.211020V120", "X697-GL-user 11 RP1A.200720.011 211020V120 release-keys",
        "X697-GL-user", "ubuntu-server", "builder", "1625097600",
        "REL", "0", "ARM", "Mali-G52 MC2",
        "OpenGL ES 3.2 v1.r26p0-01rel0", "1080", "2340", "440", 8, 4,
        4000, 3000, 3264, 2448,
        78.4532f, 0.0012f, 34.9065f, 0.0005f, 4915.2f,
        5.64f, 4.23f, 4.74f, 1.79f,
        3.67f, 2.76f, 3.54f, 2.0f,
        false, false, true
    } },
    { "Tecno Pova 2", {  // ✅ mt6769 Mali-G52 MC2
        "Tecno", "Tecno", "LE7", "LE7",
        "LE7-GL", "mt6769", "LE7", "unknown",
        "Tecno/LE7-GL/LE7:11/RP1A.200720.011/210604V405:user/release-keys", "RP1A.200720.011", "release-keys", "user",
        "MOLY.LR12A.R3.MP.V84.P47", "210604V405", "2021-10-01", "11",
        "mt6769", "mali", "196610", "mt6769",
        "zygote64_32", "Tecno/LE7-GL/LE7:11/RP1A.200720.011/210604V405:user/release-keys", "RP1A.200720.011.210604V405", "LE7-GL-user 11 RP1A.200720.011 210604V405 release-keys",
        "LE7-GL-user", "ubuntu-server", "builder", "1625097600",
        "REL", "0", "ARM", "Mali-G52 MC2",
        "OpenGL ES 3.2 v1.r26p0-01rel0", "1080", "2340", "440", 8, 4,
        4000, 3000, 3264, 2448,
        78.4532f, 0.0012f, 34.9065f, 0.0005f, 4915.2f,
        5.64f, 4.23f, 4.74f, 1.79f,
        3.67f, 2.76f, 3.54f, 2.0f,
        false, false, true
    } },
    { "Tecno Spark 8 Pro", {  // ✅ mt6769 Mali-G52 MC2
        "Tecno", "Tecno", "KG8", "KG8",
        "KG8-GL", "mt6769", "KG8", "unknown",
        "Tecno/KG8-GL/KG8:11/RP1A.200720.011/211115V145:user/release-keys", "RP1A.200720.011", "release-keys", "user",
        "MOLY.LR12A.R3.MP.V84.P47", "211115V145", "2021-10-01", "11",
        "mt6769", "mali", "196610", "mt6769",
        "zygote64_32", "Tecno/KG8-GL/KG8:11/RP1A.200720.011/211115V145:user/release-keys", "RP1A.200720.011.211115V145", "KG8-GL-user 11 RP1A.200720.011 211115V145 release-keys",
        "KG8-GL-user", "ubuntu-server", "builder", "1625097600",
        "REL", "0", "ARM", "Mali-G52 MC2",
        "OpenGL ES 3.2 v1.r26p0-01rel0", "1080", "2340", "440", 8, 4,
        4000, 3000, 3264, 2448,
        78.4532f, 0.0012f, 34.9065f, 0.0005f, 4915.2f,
        5.64f, 4.23f, 4.74f, 1.79f,
        3.67f, 2.76f, 3.54f, 2.0f,
        false, false, true
    } },
    { "Realme 6i Pro", {  // ⚠️ mt6785 Mali-G76 MC4
        "realme", "realme", "RMX2001", "RMX2001",
        "RMX2001", "mt6785", "RMX2001", "unknown",
        "realme/RMX2001/RMX2001:11/RP1A.200720.011/1639110559567:user/release-keys", "RP1A.200720.011", "release-keys", "user",
        "MOLY.LR12A.R3.MP.V89.6", "1639110559567", "2021-10-01", "11",
        "mt6785", "mali", "196610", "mt6785",
        "zygote64_32", "realme/RMX2001/RMX2001:11/RP1A.200720.011/1639110559567:user/release-keys", "RP1A.200720.011.1639110559567", "RMX2001-user 11 RP1A.200720.011 1639110559567 release-keys",
        "RMX2001-user", "ubuntu-server", "builder", "1625097600",
        "REL", "0", "ARM", "Mali-G76 MC4",
        "OpenGL ES 3.2 v1.r26p0-01rel0", "1080", "2340", "440", 8, 4,
        4000, 3000, 3264, 2448,
        78.4532f, 0.0012f, 34.9065f, 0.0005f, 4915.2f,
        5.64f, 4.23f, 4.74f, 1.79f,
        3.67f, 2.76f, 3.54f, 2.0f,
        false, false, true
    } },
    { "Tecno Camon 17", {  // ⚠️ mt6785 Mali-G76 MC4
        "Tecno", "Tecno", "CG7", "CG7",
        "CG7-GL", "mt6785", "CG7", "unknown",
        "Tecno/CG7-GL/CG7:11/RP1A.200720.011/210512V345:user/release-keys", "RP1A.200720.011", "release-keys", "user",
        "MOLY.LR12A.R3.MP.V89.6", "210512V345", "2021-10-01", "11",
        "mt6785", "mali", "196610", "mt6785",
        "zygote64_32", "Tecno/CG7-GL/CG7:11/RP1A.200720.011/210512V345:user/release-keys", "RP1A.200720.011.210512V345", "CG7-GL-user 11 RP1A.200720.011 210512V345 release-keys",
        "CG7-GL-user", "ubuntu-server", "builder", "1625097600",
        "REL", "0", "ARM", "Mali-G76 MC4",
        "OpenGL ES 3.2 v1.r26p0-01rel0", "1080", "2340", "440", 8, 4,
        4000, 3000, 3264, 2448,
        78.4532f, 0.0012f, 34.9065f, 0.0005f, 4915.2f,
        5.64f, 4.23f, 4.74f, 1.79f,
        3.67f, 2.76f, 3.54f, 2.0f,
        false, false, true
    } },
    { nullptr, {} }

    };
    for (int i = 0; TABLE[i].n; i++)
        if (name == TABLE[i].n) return &TABLE[i].fp;
    return nullptr;
}
