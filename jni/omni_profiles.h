#pragma once
#include <string>
#include <map>
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

static const std::map<std::string, DeviceFingerprint> G_DEVICE_PROFILES = {
    { "Redmi 9", {
        "Xiaomi", "Redmi", "Redmi 9", "lancelot",
        "lancelot_global", "mt6768", "lancelot", "V12.5.6.0.RJCMIXM",
        "Redmi/lancelot_global/lancelot:11/RP1A.200720.011/V12.5.6.0.RJCMIXM:user/release-keys", "RP1A.200720.011", "release-keys", "user",
        "MOLY.LR12A.R3.MP.V84.P47", "V12.5.6.0.RJCMIXM", "2021-10-05", "11",
        "mt6768", "mali", "196610", "MT6768",
        "zygote64_32", "Redmi/lancelot_global/lancelot:11/RP1A.200720.011/V12.5.6.0.RJCMIXM:user/release-keys", "RP1A.200720.011", "lancelot_global-user 11 RP1A.200720.011 V12.5.6.0.RJCMIXM release-keys",
        "lancelot_global-user", "pangu-build-component-system-177793", "builder", "1632960000",
        "REL", "0", "ARM", "Mali-G52 MC2",
        "OpenGL ES 3.2 v1.r26p0-01rel0.812488876c6978508e75b7509d43763d", "1080", "2340", "395", 8, 4,
        4224, 3168, 3264, 2448,                                  // PR44: pixelArray rear/front (int32_t)
        39.2266f, 0.0011974f, 34.906586f, 0.001064f, 4912.0f,  // BMA4xy (Bosch)
        4.80f, 3.60f, 2.75f, 2.2f,                              // PR44: rear physSize + focal + aperture
        3.20f, 2.40f, 2.02f, 2.0f,                              // PR44: front physSize + focal + aperture
        false, false, false                                      // PR38+39: sensor presence bools
    } },
    { "POCO X3 Pro", {
        "Xiaomi", "POCO", "M2102J20SG", "vayu",
        "vayu_global", "qcom", "vayu", "V12.5.3.0.RJUMIXM",
        "POCO/vayu_global/vayu:11/RKQ1.200826.002/V12.5.3.0.RJUMIXM:user/release-keys", "RKQ1.200826.002", "release-keys", "user",
        "MPSS.HI.3.1.c3-00186-SM8150_GEN_PACK-1", "V12.5.3.0.RJUMIXM", "2021-10-01", "11",
        "msmnile", "adreno", "196610", "SM8150-AC",
        "zygote64_32", "POCO/vayu_global/vayu:11/RKQ1.200826.002/V12.5.3.0.RJUMIXM:user/release-keys", "RKQ1.200826.002", "vayu_global-user 11 RKQ1.200826.002 V12.5.3.0.RJUMIXM release-keys",
        "vayu_global-user", "c3-miui-ota-bd98", "builder", "1622630400",
        "REL", "0", "Qualcomm", "Adreno (TM) 640",
        "OpenGL ES 3.2 V@0502.0 (GIT@5f4e5c9, Ia3b7920, 1600000000)", "1080", "2400", "394", 8, 8,
        8000, 6000, 5184, 3880,                                  // PR44: pixelArray rear/front (int32_t)
        78.4532f, 0.0023946f, 34.906586f, 0.001064f, 4912.0f,  // LSM6DSO (ST Micro)
        6.40f, 4.80f, 4.74f, 1.6f,                              // PR44: rear physSize + focal + aperture
        4.61f, 3.46f, 2.49f, 2.2f,                              // PR44: front physSize + focal + aperture
        false, false, false                                      // PR38+39: sensor presence bools
    } },
    { "Mi 10T", {
        "Xiaomi", "Xiaomi", "M2007J3SY", "apollo",
        "apollo_global", "qcom", "apollo", "V12.5.1.0.RJDMIXM",
        "Xiaomi/apollo_global/apollo:11/RKQ1.200826.002/V12.5.1.0.RJDMIXM:user/release-keys", "RKQ1.200826.002", "release-keys", "user",
        "MPSS.HI.3.2.c1.1-00085-SM8250_GEN_PACK-1", "V12.5.1.0.RJDMIXM", "2021-07-01", "11",
        "kona", "adreno", "196610", "SM8250",
        "zygote64_32", "Xiaomi/apollo_global/apollo:11/RKQ1.200826.002/V12.5.1.0.RJDMIXM:user/release-keys", "RKQ1.200826.002", "apollo_global-user 11 RKQ1.200826.002 V12.5.1.0.RJDMIXM release-keys",
        "apollo_global-user", "c3-miui-ota-bd05", "builder", "1622112000",
        "REL", "0", "Qualcomm", "Adreno (TM) 650",
        "OpenGL ES 3.2 V@0502.0 (GIT@5f4e5c9, Ia3b7920, 1600000000)", "1080", "2400", "394", 8, 8,
        9248, 6944, 5184, 3880,                                  // PR44: pixelArray rear/front (int32_t)
        78.4532f, 0.0023946f, 34.906586f, 0.001064f, 4912.0f,  // LSM6DSO (ST Micro)
        7.40f, 5.56f, 5.09f, 2.0f,                              // PR44: rear physSize + focal + aperture
        4.61f, 3.46f, 2.49f, 2.2f,                              // PR44: front physSize + focal + aperture
        false, true, false                                      // PR38+39: sensor presence bools
    } },
    { "Redmi Note 10 Pro", {
        "Xiaomi", "Redmi", "M2101K6G", "sweet",
        "sweet_global", "qcom", "sweet", "V12.5.4.0.RKGMIXM",
        "Redmi/sweet_global/sweet:11/RKQ1.200826.002/V12.5.4.0.RKGMIXM:user/release-keys", "RKQ1.200826.002", "release-keys", "user",
        "MPSS.AT.4.0-00026-SM7150_GEN_PACK-1", "V12.5.4.0.RKGMIXM", "2021-08-01", "11",
        "atoll", "adreno", "196610", "SM7150-AB",
        "zygote64_32", "Redmi/sweet_global/sweet:11/RKQ1.200826.002/V12.5.4.0.RKGMIXM:user/release-keys", "RKQ1.200826.002", "sweet_global-user 11 RKQ1.200826.002 V12.5.4.0.RKGMIXM release-keys",
        "sweet_global-user", "pangu-build-component-system-178104", "builder", "1627776000",
        "REL", "0", "Qualcomm", "Adreno (TM) 618",
        "OpenGL ES 3.2 V@0502.0 (GIT@5f4e5c9, Ia3b7920, 1600000000)", "1080", "2400", "394", 8, 6,
        12032, 9024, 4608, 3456,                                  // PR44: pixelArray rear/front (int32_t)
        78.4532f, 0.0023946f, 34.906586f, 0.001064f, 4912.0f,  // LSM6DSO (ST Micro)
        9.60f, 7.20f, 5.58f, 1.9f,                              // PR44: rear physSize + focal + aperture
        4.00f, 3.00f, 2.18f, 2.45f,                              // PR44: front physSize + focal + aperture
        false, false, false                                      // PR38+39: sensor presence bools
    } },
    { "Redmi Note 9 Pro", {
        "Xiaomi", "Redmi", "M2003J6B2G", "joyeuse",
        "joyeuse_global", "qcom", "joyeuse", "V12.5.1.0.RJZMIXM",
        "Redmi/joyeuse_global/joyeuse:11/RKQ1.200826.002/V12.5.1.0.RJZMIXM:user/release-keys", "RKQ1.200826.002", "release-keys", "user",
        "MPSS.AT.4.0-00050-SM7150_GEN_PACK-1", "V12.5.1.0.RJZMIXM", "2021-07-01", "11",
        "atoll", "adreno", "196610", "SM7125",
        "zygote64_32", "Redmi/joyeuse_global/joyeuse:11/RKQ1.200826.002/V12.5.1.0.RJZMIXM:user/release-keys", "RKQ1.200826.002", "joyeuse_global-user 11 RKQ1.200826.002 V12.5.1.0.RJZMIXM release-keys",
        "joyeuse_global-user", "pangu-build-component-system-175632", "builder", "1622112000",
        "REL", "0", "Qualcomm", "Adreno (TM) 618",
        "OpenGL ES 3.2 V@0502.0 (GIT@5f4e5c9, Ia3b7920, 1600000000)", "1080", "2400", "394", 8, 6,
        9248, 6944, 4608, 3456,                                  // PR44: pixelArray rear/front (int32_t)
        78.4532f, 0.0023946f, 34.906586f, 0.001064f, 4912.0f,  // LSM6DSO (ST Micro)
        7.40f, 5.56f, 5.09f, 1.79f,                              // PR44: rear physSize + focal + aperture
        4.00f, 3.00f, 2.18f, 2.45f,                              // PR44: front physSize + focal + aperture
        false, false, false                                      // PR38+39: sensor presence bools
    } },
    { "Redmi Note 10", {
        "Xiaomi", "Redmi", "M2101K7AI", "mojito",
        "mojito_global", "qcom", "mojito", "V12.5.6.0.RKFMIXM",
        "Redmi/mojito_global/mojito:11/RKQ1.200826.002/V12.5.6.0.RKFMIXM:user/release-keys", "RKQ1.200826.002", "release-keys", "user",
        "MPSS.AT.4.0-00055-SM6150_GEN_PACK-1", "V12.5.6.0.RKFMIXM", "2021-09-01", "11",
        "sm6150", "adreno", "196610", "SM6150",
        "zygote64_32", "Redmi/mojito_global/mojito:11/RKQ1.200826.002/V12.5.6.0.RKFMIXM:user/release-keys", "RKQ1.200826.002", "mojito_global-user 11 RKQ1.200826.002 V12.5.6.0.RKFMIXM release-keys",
        "mojito_global-user", "pangu-build-component-system-179001", "builder", "1630454400",
        "REL", "0", "Qualcomm", "Adreno (TM) 612",
        "OpenGL ES 3.2 V@0502.0 (GIT@5f4e5c9, Ia3b7920, 1600000000)", "1080", "2400", "409", 8, 4,
        9248, 6944, 5184, 3880,                                  // PR44: pixelArray rear/front (int32_t)
        39.2266f, 0.0011974f, 34.906586f, 0.001064f, 2000.0f,  // BMA253 (Bosch),
        7.40f, 5.56f, 5.09f, 2.0f,                              // PR44: rear physSize + focal + aperture
        4.61f, 3.46f, 2.49f, 2.2f,                              // PR44: front physSize + focal + aperture
        false, false, false                                      // PR38+39: sensor presence bools
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
    { "POCO X3 NFC", {
        "Xiaomi", "POCO", "M2007J20CG", "surya",
        "surya_global", "qcom", "surya", "V12.5.2.0.RJGMIXM",
        "POCO/surya_global/surya:11/RKQ1.200826.002/V12.5.2.0.RJGMIXM:user/release-keys", "RKQ1.200826.002", "release-keys", "user",
        "MPSS.AT.4.0-00026-SM7150_GEN_PACK-1", "V12.5.2.0.RJGMIXM", "2021-10-01", "11",
        "atoll", "adreno", "196610", "SM7150-AB",
        "zygote64_32", "POCO/surya_global/surya:11/RKQ1.200826.002/V12.5.2.0.RJGMIXM:user/release-keys", "RKQ1.200826.002", "surya_global-user 11 RKQ1.200826.002 V12.5.2.0.RJGMIXM release-keys",
        "surya_global-user", "c3-miui-ota-bd77", "builder", "1622630400",
        "REL", "0", "Qualcomm", "Adreno (TM) 618",
        "OpenGL ES 3.2 V@0502.0 (GIT@5f4e5c9, Ia3b7920, 1600000000)", "1080", "2400", "394", 8, 6,
        9248, 6944, 5184, 3880,                                  // PR44: pixelArray rear/front (int32_t)
        78.4532f, 0.0023946f, 34.906586f, 0.001064f, 4912.0f,  // LSM6DSO (ST Micro),
        7.40f, 5.56f, 5.09f, 2.0f,                              // PR44: rear physSize + focal + aperture
        4.61f, 3.46f, 2.49f, 2.2f,                              // PR44: front physSize + focal + aperture
        false, false, false                                      // PR38+39: sensor presence bools
    } },
    { "Mi 11 Lite", {
        "Xiaomi", "Xiaomi", "M2101K9AG", "courbet",
        "courbet_global", "qcom", "courbet", "V12.5.3.0.RKAMIXM",
        "Xiaomi/courbet_global/courbet:11/RKQ1.200826.002/V12.5.3.0.RKAMIXM:user/release-keys", "RKQ1.200826.002", "release-keys", "user",
        "MPSS.AT.4.0-00026-SM7150_GEN_PACK-1", "V12.5.3.0.RKAMIXM", "2021-08-01", "11",
        "atoll", "adreno", "196610", "SM7150-AB",
        "zygote64_32", "Xiaomi/courbet_global/courbet:11/RKQ1.200826.002/V12.5.3.0.RKAMIXM:user/release-keys", "RKQ1.200826.002", "courbet_global-user 11 RKQ1.200826.002 V12.5.3.0.RKAMIXM release-keys",
        "courbet_global-user", "pangu-build-component-system-180421", "builder", "1627776000",
        "REL", "0", "Qualcomm", "Adreno (TM) 618",
        "OpenGL ES 3.2 V@0502.0 (GIT@5f4e5c9, Ia3b7920, 1600000000)", "1080", "2400", "401", 8, 6,
        9248, 6944, 4608, 3456,                                  // PR44: pixelArray rear/front (int32_t)
        78.4532f, 0.0023946f, 34.906586f, 0.001064f, 4912.0f,  // LSM6DSO (ST Micro),
        7.40f, 5.56f, 4.74f, 2.0f,                              // PR44: rear physSize + focal + aperture
        4.00f, 3.00f, 2.18f, 2.45f,                              // PR44: front physSize + focal + aperture
        false, false, false                                      // PR38+39: sensor presence bools
    } },
    { "Mi 11", {
        "Xiaomi", "Xiaomi", "M2011K2G", "venus",
        "venus_global", "qcom", "venus", "V12.5.6.0.RKBMIXM",
        "Xiaomi/venus_global/venus:11/RKQ1.200826.002/V12.5.6.0.RKBMIXM:user/release-keys", "RKQ1.200826.002", "release-keys", "user",
        "MPSS.HI.3.3.c1-00076-SM8350_GEN_PACK-1", "V12.5.6.0.RKBMIXM", "2021-10-01", "11",
        "lahaina", "adreno", "196610", "SM8350",
        "zygote64_32", "Xiaomi/venus_global/venus:11/RKQ1.200826.002/V12.5.6.0.RKBMIXM:user/release-keys", "RKQ1.200826.002", "venus_global-user 11 RKQ1.200826.002 V12.5.6.0.RKBMIXM release-keys",
        "venus_global-user", "pangu-build-component-system-182133", "builder", "1633046400",
        "REL", "0", "Qualcomm", "Adreno (TM) 660",
        "OpenGL ES 3.2 V@0502.0 (GIT@5f4e5c9, Ia3b7920, 1600000000)", "1080", "2400", "395", 8, 8,
        12032, 9024, 5184, 3880,                                  // PR44: pixelArray rear/front (int32_t)
        78.4532f, 0.0023946f, 34.906586f, 0.001064f, 4912.0f,  // LSM6DSO (ST Micro),
        9.60f, 7.20f, 5.58f, 1.85f,                              // PR44: rear physSize + focal + aperture
        4.61f, 3.46f, 2.49f, 2.2f,                              // PR44: front physSize + focal + aperture
        false, true, true                                      // PR38+39: sensor presence bools
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
    { "POCO F3", {
        "Xiaomi", "POCO", "M2012K11AG", "alioth",
        "alioth_global", "qcom", "alioth", "V12.5.5.0.RKHMIXM",
        "POCO/alioth_global/alioth:11/RKQ1.200826.002/V12.5.5.0.RKHMIXM:user/release-keys", "RKQ1.200826.002", "release-keys", "user",
        "MPSS.HI.3.2.c1.1-00085-SM8250_GEN_PACK-1", "V12.5.5.0.RKHMIXM", "2021-09-01", "11",
        "kona", "adreno", "196610", "SM8250",
        "zygote64_32", "POCO/alioth_global/alioth:11/RKQ1.200826.002/V12.5.5.0.RKHMIXM:user/release-keys", "RKQ1.200826.002", "alioth_global-user 11 RKQ1.200826.002 V12.5.5.0.RKHMIXM release-keys",
        "alioth_global-user", "c3-miui-ota-bd88", "builder", "1630454400",
        "REL", "0", "Qualcomm", "Adreno (TM) 650",
        "OpenGL ES 3.2 V@0502.0 (GIT@5f4e5c9, Ia3b7920, 1600000000)", "1080", "2400", "394", 8, 8,
        8000, 6000, 5184, 3880,                                  // PR44: pixelArray rear/front (int32_t)
        78.4532f, 0.0023946f, 34.906586f, 0.001064f, 4912.0f,  // LSM6DSO (ST Micro),
        6.40f, 4.80f, 4.74f, 1.79f,                              // PR44: rear physSize + focal + aperture
        4.61f, 3.46f, 2.49f, 2.2f,                              // PR44: front physSize + focal + aperture
        false, false, false                                      // PR38+39: sensor presence bools
    } },
    { "Galaxy A52", {
        "samsung", "samsung", "SM-A525F", "a52x",
        "a52xsqz", "qcom", "atoll", "A525FXXU4AUH1",
        "samsung/a52xsqz/a52x:11/RP1A.200720.012/A525FXXU4AUH1:user/release-keys", "RP1A.200720.012", "release-keys", "user",
        "MPSS.AT.4.0-00050-SM7150_GEN_PACK-1", "A525FXXU4AUH1", "2021-08-01", "11",
        "atoll", "adreno", "196610", "SM7125",
        "zygote64_32", "samsung/a52xsqz/a52x:11/RP1A.200720.012/A525FXXU4AUH1:user/release-keys", "RP1A.200720.012", "a52xsqz-user 11 RP1A.200720.012 A525FXXU4AUH1 release-keys",
        "a52xsqz-user", "SWDD7390", "dpi", "1627776000",
        "REL", "0", "Qualcomm", "Adreno (TM) 618",
        "OpenGL ES 3.2 V@0502.0 (GIT@5f4e5c9, Ia3b7920, 1600000000)", "1080", "2400", "404", 8, 6,
        9248, 6944, 6528, 4896,                                  // PR44: pixelArray rear/front (int32_t)
        78.4532f, 0.0023946f, 34.906586f, 0.001064f, 4912.0f,  // LSM6DSR (ST Micro),
        7.40f, 5.56f, 5.23f, 1.8f,                              // PR44: rear physSize + focal + aperture
        6.40f, 4.80f, 3.56f, 2.2f,                              // PR44: front physSize + focal + aperture
        false, false, true                                      // PR38+39: sensor presence bools
    } },
    { "Galaxy A72", {
        "samsung", "samsung", "SM-A725F", "a72",
        "a72sqz", "qcom", "atoll", "A725FXXU3AUH2",
        "samsung/a72sqz/a72:11/RP1A.200720.012/A725FXXU3AUH2:user/release-keys", "RP1A.200720.012", "release-keys", "user",
        "MPSS.AT.4.0-00050-SM7150_GEN_PACK-1", "A725FXXU3AUH2", "2021-08-01", "11",
        "atoll", "adreno", "196610", "SM7125",
        "zygote64_32", "samsung/a72sqz/a72:11/RP1A.200720.012/A725FXXU3AUH2:user/release-keys", "RP1A.200720.012", "a72sqz-user 11 RP1A.200720.012 A725FXXU3AUH2 release-keys",
        "a72sqz-user", "SWDD7391", "dpi", "1627776000",
        "REL", "0", "Qualcomm", "Adreno (TM) 618",
        "OpenGL ES 3.2 V@0502.0 (GIT@5f4e5c9, Ia3b7920, 1600000000)", "1080", "2400", "393", 8, 8,
        9248, 6944, 6528, 4896,                                  // PR44: pixelArray rear/front (int32_t)
        78.4532f, 0.0023946f, 34.906586f, 0.001064f, 4912.0f,  // LSM6DSR (ST Micro),
        7.40f, 5.56f, 5.36f, 1.8f,                              // PR44: rear physSize + focal + aperture
        6.40f, 4.80f, 3.56f, 2.2f,                              // PR44: front physSize + focal + aperture
        false, false, true                                      // PR38+39: sensor presence bools
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
    { "Galaxy S20 FE", {
        "samsung", "samsung", "SM-G781B", "r8q",
        "r8qsqz", "qcom", "kona", "G781BXXU3CUJ2",
        "samsung/r8qsqz/r8q:11/RP1A.200720.012/G781BXXU3CUJ2:user/release-keys", "RP1A.200720.012", "release-keys", "user",
        "MPSS.HI.3.2.c1.1-00085-SM8250_GEN_PACK-1", "G781BXXU3CUJ2", "2021-10-01", "11",
        "kona", "adreno", "196610", "SM8250",
        "zygote64_32", "samsung/r8qsqz/r8q:11/RP1A.200720.012/G781BXXU3CUJ2:user/release-keys", "RP1A.200720.012", "r8qsqz-user 11 RP1A.200720.012 G781BXXU3CUJ2 release-keys",
        "r8qsqz-user", "SWDD5130", "dpi", "1633046400",
        "REL", "0", "Qualcomm", "Adreno (TM) 650",
        "OpenGL ES 3.2 V@0502.0 (GIT@5f4e5c9, Ia3b7920, 1600000000)", "1080", "2400", "404", 8, 8,
        4032, 3024, 6528, 4896,                                  // PR44: pixelArray rear/front (int32_t)
        78.4532f, 0.0023946f, 34.906586f, 0.001064f, 4912.0f,  // LSM6DSO (ST Micro),
        7.20f, 5.40f, 4.07f, 1.8f,                              // PR44: rear physSize + focal + aperture
        6.40f, 4.80f, 3.56f, 2.2f,                              // PR44: front physSize + focal + aperture
        false, true, true                                      // PR38+39: sensor presence bools
    } },
    { "Galaxy A51", {
        "samsung", "samsung", "SM-A515F", "a51",
        "a51sqz", "exynos9611", "exynos9611", "A515FXXU4CUG1",
        "samsung/a51sqz/a51:11/RP1A.200720.012/A515FXXU4CUG1:user/release-keys", "RP1A.200720.012", "release-keys", "user",
        "A515FXXU4CUG1", "A515FXXU4CUG1", "2021-07-01", "11",
        "exynos9611", "mali", "196610", "Exynos9611",
        "zygote64_32", "samsung/a51sqz/a51:11/RP1A.200720.012/A515FXXU4CUG1:user/release-keys", "RP1A.200720.012", "a51sqz-user 11 RP1A.200720.012 A515FXXU4CUG1 release-keys",
        "a51sqz-user", "21R3NF12", "dpi", "1625097600",
        "REL", "0", "ARM", "Mali-G72 MP3",
        "OpenGL ES 3.2 v1.r25p0-01rel0.a51a0c509f2714d8e5acbde47570a4b2", "1080", "2400", "404", 8, 4,
        8000, 6000, 5184, 3880,                                  // PR44: pixelArray rear/front (int32_t)
        156.9064f, 0.004788f, 34.906586f, 0.001064f, 1200.0f,  // BMI160 (Bosch),
        6.40f, 4.80f, 4.28f, 2.0f,                              // PR44: rear physSize + focal + aperture
        5.04f, 3.78f, 2.73f, 2.4f,                              // PR44: front physSize + focal + aperture
        false, false, true                                      // PR38+39: sensor presence bools
    } },
    { "Galaxy M31", {
        "samsung", "samsung", "SM-M315F", "m31",
        "m31sqz", "exynos9611", "exynos9611", "M315FXXU4CUG1",
        "samsung/m31sqz/m31:11/RP1A.200720.012/M315FXXU4CUG1:user/release-keys", "RP1A.200720.012", "release-keys", "user",
        "M315FXXU4CUG1", "M315FXXU4CUG1", "2021-11-01", "11",
        "exynos9611", "mali", "196610", "Exynos9611",
        "zygote64_32", "samsung/m31sqz/m31:11/RP1A.200720.012/M315FXXU4CUG1:user/release-keys", "RP1A.200720.012", "m31sqz-user 11 RP1A.200720.012 M315FXXU4CUG1 release-keys",
        "m31sqz-user", "21R3NF12", "dpi", "1636934400",
        "REL", "0", "ARM", "Mali-G72 MP3",
        "OpenGL ES 3.2 v1.r25p0-01rel0.a51a0c509f2714d8e5acbde47570a4b2", "1080", "2340", "403", 8, 6,
        9248, 6944, 5184, 3880,                                  // PR44: pixelArray rear/front (int32_t)
        156.9064f, 0.004788f, 34.906586f, 0.001064f, 1200.0f,  // BMI160 (Bosch),
        7.40f, 5.56f, 4.28f, 2.0f,                              // PR44: rear physSize + focal + aperture
        4.61f, 3.46f, 2.49f, 2.2f,                              // PR44: front physSize + focal + aperture
        false, false, true                                      // PR38+39: sensor presence bools
    } },
    { "Galaxy A12", {
        "samsung", "samsung", "SM-A125F", "a12",
        "a12sqz", "mt6765", "mt6765", "A125FXXU5BUJ1",
        "samsung/a12sqz/a12:11/RP1A.200720.012/A125FXXU5BUJ1:user/release-keys", "RP1A.200720.012", "release-keys", "user",
        "MOLY.LR12A.R3.MP.V56.6", "A125FXXU5BUJ1", "2021-10-01", "11",
        "mt6765", "powervr", "196610", "MT6765",
        "zygote64_32", "samsung/a12sqz/a12:11/RP1A.200720.012/A125FXXU5BUJ1:user/release-keys", "RP1A.200720.012", "a12sqz-user 11 RP1A.200720.012 A125FXXU5BUJ1 release-keys",
        "a12sqz-user", "SWDD8800", "dpi", "1633046400",
        "REL", "0", "Imagination Technologies", "PowerVR GE8320",
        "OpenGL ES 3.2 build 1.13@5776728", "720", "1600", "270", 8, 4,
        8000, 6000, 3264, 2448,                                  // PR44: pixelArray rear/front (int32_t)
        39.2266f, 0.0011974f, 34.906586f, 0.001064f, 2000.0f,  // BMA253 (Bosch),
        6.40f, 4.80f, 4.00f, 2.0f,                              // PR44: rear physSize + focal + aperture
        3.20f, 2.40f, 2.02f, 2.0f,                              // PR44: front physSize + focal + aperture
        false, false, false                                      // PR38+39: sensor presence bools
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
    { "Galaxy F62", {
        "samsung", "samsung", "SM-E625F", "e1q",
        "e1qsqz", "exynos9825", "exynos9825", "E625FXXU2BUG1",
        "samsung/e1qsqz/e1q:11/RP1A.200720.012/E625FXXU2BUG1:user/release-keys", "RP1A.200720.012", "release-keys", "user",
        "E625FXXU2BUG1", "E625FXXU2BUG1", "2021-07-01", "11",
        "exynos9825", "mali", "196610", "Exynos9825",
        "zygote64_32", "samsung/e1qsqz/e1q:11/RP1A.200720.012/E625FXXU2BUG1:user/release-keys", "RP1A.200720.012", "e1qsqz-user 11 RP1A.200720.012 E625FXXU2BUG1 release-keys",
        "e1qsqz-user", "SWDD5830", "dpi", "1625097600",
        "REL", "0", "ARM", "Mali-G76 MP12",
        "OpenGL ES 3.2 v1.r23p0-01rel0.a51a0c509f2714d8e5acbde47570a4b2", "1080", "2400", "393", 8, 6,
        9216, 6912, 6528, 4896,                                  // PR44: pixelArray rear/front (int32_t)
        156.9064f, 0.004788f, 34.906586f, 0.001064f, 1200.0f,  // BMI160 (Bosch),
        7.40f, 5.56f, 4.28f, 2.0f,                              // PR44: rear physSize + focal + aperture
        6.40f, 4.80f, 3.56f, 2.2f,                              // PR44: front physSize + focal + aperture
        false, false, true                                      // PR38+39: sensor presence bools
    } },
    { "OnePlus 8T", {
        "OnePlus", "OnePlus", "KB2001", "OnePlus8T",
        "OnePlus8T", "qcom", "kona", "2107142215",
        "OnePlus/kebab/OnePlus8T:11/RP1A.200720.011/2107142215:user/release-keys", "RP1A.200720.011", "release-keys", "user",
        "MPSS.HI.3.2.c1.1-00085-SM8250_GEN_PACK-1", "2107142215", "2021-07-05", "11",
        "kona", "adreno", "196610", "SM8250",
        "zygote64_32", "OnePlus/kebab/OnePlus8T:11/RP1A.200720.011/2107142215:user/release-keys", "RP1A.200720.011", "kebab-user 11 RP1A.200720.011 2107142215 release-keys",
        "kebab-user", "builder01.oneplus.com", "builduser", "1626307200",
        "REL", "0", "Qualcomm", "Adreno (TM) 650",
        "OpenGL ES 3.2 V@0502.0 (GIT@5f4e5c9, Ia3b7920, 1600000000)", "1080", "2400", "401", 8, 12,
        8000, 6000, 4608, 3456,                                  // PR44: pixelArray rear/front (int32_t)
        78.4532f, 0.0023946f, 34.906586f, 0.001064f, 4912.0f,  // LSM6DSO (ST Micro),
        6.40f, 4.80f, 5.58f, 1.56f,                              // PR44: rear physSize + focal + aperture
        4.00f, 3.00f, 2.18f, 2.45f,                              // PR44: front physSize + focal + aperture
        false, true, true                                      // PR38+39: sensor presence bools
    } },
    { "OnePlus Nord", {
        "OnePlus", "OnePlus", "AC2003", "OnePlus Nord",
        "avicii", "qcom", "lito", "2105101635",
        "OnePlus/avicii/OnePlus Nord:11/RKQ1.201022.002/2105101635:user/release-keys", "RKQ1.201022.002", "release-keys", "user",
        "MPSS.VT.5.2-00075-SM7250_GEN_PACK-1", "2105101635", "2021-06-05", "11",
        "lito", "adreno", "196610", "SM7250",
        "zygote64_32", "OnePlus/avicii/OnePlus Nord:11/RKQ1.201022.002/2105101635:user/release-keys", "RKQ1.201022.002", "avicii-user 11 RKQ1.201022.002 2105101635 release-keys",
        "avicii-user", "builder02.oneplus.com", "builduser", "1620604800",
        "REL", "0", "Qualcomm", "Adreno (TM) 620",
        "OpenGL ES 3.2 V@0502.0 (GIT@5f4e5c9, Ia3b7920, 1600000000)", "1080", "2400", "408", 8, 8,
        8000, 6000, 6528, 4896,                                  // PR44: pixelArray rear/front (int32_t)
        78.4532f, 0.0023946f, 34.906586f, 0.001064f, 4912.0f,  // LSM6DSO (ST Micro),
        6.40f, 4.80f, 4.97f, 2.45f,                              // PR44: rear physSize + focal + aperture
        6.40f, 4.80f, 3.56f, 2.2f,                              // PR44: front physSize + focal + aperture
        false, false, true                                      // PR38+39: sensor presence bools
    } },
    { "OnePlus N10 5G", {
        "OnePlus", "OnePlus", "BE2025", "OnePlus N10 5G",
        "billie", "qcom", "sm6350", "2104132208",
        "OnePlus/billie/OnePlus N10 5G:11/RP1A.200720.011/2104132208:user/release-keys", "RP1A.200720.011", "release-keys", "user",
        "MPSS.VT.5.2-00040-SM6350_GEN_PACK-1", "2104132208", "2021-04-05", "11",
        "sm6350", "adreno", "196610", "SM6350",
        "zygote64_32", "OnePlus/billie/OnePlus N10 5G:11/RP1A.200720.011/2104132208:user/release-keys", "RP1A.200720.011", "billie-user 11 RP1A.200720.011 2104132208 release-keys",
        "billie-user", "builder03.oneplus.com", "builduser", "1617235200",
        "REL", "0", "Qualcomm", "Adreno (TM) 619L",
        "OpenGL ES 3.2 V@0502.0 (GIT@5f4e5c9, Ia3b7920, 1600000000)", "1080", "2400", "405", 8, 6,
        9248, 6944, 4608, 3456,                                  // PR44: pixelArray rear/front (int32_t)
        78.4532f, 0.0023946f, 34.906586f, 0.001064f, 4912.0f,  // LSM6DSO (ST Micro),
        7.40f, 5.56f, 5.09f, 2.0f,                              // PR44: rear physSize + focal + aperture
        4.00f, 3.00f, 2.18f, 2.45f,                              // PR44: front physSize + focal + aperture
        false, false, true                                      // PR38+39: sensor presence bools
    } },
    { "OnePlus 8", {
        "OnePlus", "OnePlus", "IN2013", "OnePlus8",
        "instantnoodle", "qcom", "kona", "2105100150",
        "OnePlus/instantnoodle/OnePlus8:11/RP1A.200720.011/2105100150:user/release-keys", "RP1A.200720.011", "release-keys", "user",
        "MPSS.HI.3.2.c1.1-00085-SM8250_GEN_PACK-1", "2105100150", "2021-06-05", "11",
        "kona", "adreno", "196610", "SM8250",
        "zygote64_32", "OnePlus/instantnoodle/OnePlus8:11/RP1A.200720.011/2105100150:user/release-keys", "RP1A.200720.011", "instantnoodle-user 11 RP1A.200720.011 2105100150 release-keys",
        "instantnoodle-user", "builder04.oneplus.com", "builduser", "1620518400",
        "REL", "0", "Qualcomm", "Adreno (TM) 650",
        "OpenGL ES 3.2 V@0502.0 (GIT@5f4e5c9, Ia3b7920, 1600000000)", "1080", "2400", "401", 8, 8,
        8000, 6000, 4608, 3456,                                  // PR44: pixelArray rear/front (int32_t)
        78.4532f, 0.0023946f, 34.906586f, 0.001064f, 4912.0f,  // LSM6DSO (ST Micro),
        6.40f, 4.80f, 5.58f, 1.78f,                              // PR44: rear physSize + focal + aperture
        4.00f, 3.00f, 2.18f, 2.45f,                              // PR44: front physSize + focal + aperture
        false, true, true                                      // PR38+39: sensor presence bools
    } },
    { "Pixel 5", {
        "Google", "google", "Pixel 5", "redfin",
        "redfin", "redfin", "redfin", "r8.0.0-6692804",
        "google/redfin/redfin:11/RQ3A.210805.001.A1/7474174:user/release-keys", "RQ3A.210805.001.A1", "release-keys", "user",
        "MPSS.VT.5.2-00075-SM7250_GEN_PACK-1", "7474174", "2021-08-05", "11",
        "lito", "adreno", "196610", "SM7250",
        "zygote64_32", "google/redfin/redfin:11/RQ3A.210805.001.A1/7474174:user/release-keys", "RQ3A.210805.001.A1", "redfin-user 11 RQ3A.210805.001.A1 7474174 release-keys",
        "redfin-user", "abfarm-release-rbe-32-00025", "android-build", "1627776000",
        "REL", "0", "Qualcomm", "Adreno (TM) 620",
        "OpenGL ES 3.2 V@0502.0 (GIT@5f4e5c9, Ia3b7920, 1600000000)", "1080", "2340", "432", 8, 8,
        4056, 3040, 3264, 2448,                                  // PR44: pixelArray rear/front (int32_t)
        78.4532f, 0.0023946f, 34.906586f, 0.001064f, 4912.0f,  // LSM6DSO (ST Micro),
        5.76f, 4.29f, 4.44f, 1.7f,                              // PR44: rear physSize + focal + aperture
        3.20f, 2.40f, 2.02f, 2.0f,                              // PR44: front physSize + focal + aperture
        false, true, false                                      // PR38+39: sensor presence bools
    } },
    { "Pixel 4a 5G", {
        "Google", "google", "Pixel 4a (5G)", "bramble",
        "bramble", "bramble", "bramble", "b2-0.3-7214727",
        "google/bramble/bramble:11/RQ3A.210705.001/7380771:user/release-keys", "RQ3A.210705.001", "release-keys", "user",
        "g7250-00195-210614-B-7352378", "7380771", "2021-07-05", "11",
        "lito", "adreno", "196610", "SM7250",
        "zygote64_32", "google/bramble/bramble:11/RQ3A.210705.001/7380771:user/release-keys", "RQ3A.210705.001", "bramble-user 11 RQ3A.210705.001 7380771 release-keys",
        "bramble-user", "abfarm-release-rbe-64.hot.corp.google.com", "android-build", "1625616000",
        "REL", "0", "Qualcomm", "Adreno (TM) 620",
        "OpenGL ES 3.2 V@0502.0 (GIT@5f4e5c9, Ia3b7920, 1600000000)", "1080", "2340", "413", 8, 6,
        4056, 3040, 3264, 2448,                                  // PR44: pixelArray rear/front (int32_t)
        78.4532f, 0.0023946f, 34.906586f, 0.001064f, 4912.0f,  // LSM6DSO (ST Micro),
        5.76f, 4.29f, 4.44f, 2.0f,                              // PR44: rear physSize + focal + aperture
        3.20f, 2.40f, 2.02f, 2.0f,                              // PR44: front physSize + focal + aperture
        false, true, false                                      // PR38+39: sensor presence bools
    } },
    { "Pixel 4a", {
        "Google", "google", "Pixel 4a", "sunfish",
        "sunfish", "sunfish", "sunfish", "s5-0.5-6765805",
        "google/sunfish/sunfish:11/RQ3A.210805.001/7390230:user/release-keys", "RQ3A.210805.001", "release-keys", "user",
        "MPSS.AT.4.0-00022-SM7150_GEN_PACK-1", "7390230", "2021-08-05", "11",
        "atoll", "adreno", "196610", "SM7150",
        "zygote64_32", "google/sunfish/sunfish:11/RQ3A.210805.001/7390230:user/release-keys", "RQ3A.210805.001", "sunfish-user 11 RQ3A.210805.001 7390230 release-keys",
        "sunfish-user", "abfarm-release-rbe-32-00027", "android-build", "1627776000",
        "REL", "0", "Qualcomm", "Adreno (TM) 618",
        "OpenGL ES 3.2 V@0502.0 (GIT@5f4e5c9, Ia3b7920, 1600000000)", "1080", "2340", "443", 8, 6,
        4056, 3040, 3264, 2448,                                  // PR44: pixelArray rear/front (int32_t)
        78.4532f, 0.0023946f, 34.906586f, 0.001064f, 4912.0f,  // LSM6DSO (ST Micro),
        5.76f, 4.29f, 4.44f, 2.0f,                              // PR44: rear physSize + focal + aperture
        3.20f, 2.40f, 2.02f, 2.0f,                              // PR44: front physSize + focal + aperture
        false, true, false                                      // PR38+39: sensor presence bools
    } },
    { "Pixel 3a XL", {
        "Google", "google", "Pixel 3a XL", "bonito",
        "bonito", "bonito", "bonito", "b4s4-0.2-5613699",
        "google/bonito/bonito:11/RP1A.200720.011/6734798:user/release-keys", "RP1A.200720.011", "release-keys", "user",
        "MPSS.AT.3.0-00079-SDM670_GEN_PACK-1", "6734798", "2021-05-05", "11",
        "sdm670", "adreno", "196610", "SDM670",
        "zygote64_32", "google/bonito/bonito:11/RP1A.200720.011/6734798:user/release-keys", "RP1A.200720.011", "bonito-user 11 RP1A.200720.011 6734798 release-keys",
        "bonito-user", "abfarm-release-rbe-32-00010", "android-build", "1619827200",
        "REL", "0", "Qualcomm", "Adreno (TM) 615",
        "OpenGL ES 3.2 V@0502.0 (GIT@5f4e5c9, Ia3b7920, 1600000000)", "1080", "2160", "400", 8, 4,
        4032, 3024, 3264, 2448,                                  // PR44: pixelArray rear/front (int32_t)
        78.4532f, 0.0023946f, 34.906586f, 0.001064f, 4912.0f,  // LSM6DSO (ST Micro),
        5.52f, 4.14f, 4.40f, 2.0f,                              // PR44: rear physSize + focal + aperture
        3.20f, 2.40f, 2.02f, 2.0f,                              // PR44: front physSize + focal + aperture
        false, true, false                                      // PR38+39: sensor presence bools
    } },
    { "Moto G Power 2021", {
        "motorola", "motorola", "moto g power (2021)", "borneo",
        "borneo", "qcom", "bengal", "2b4fae",
        "motorola/borneo/borneo:11/RRQ31.Q3-47-22/2b4fae:user/release-keys", "RRQ31.Q3-47-22", "release-keys", "user",
        "MPSS.AT.4.0-00055-SM6115_GEN_PACK-1", "2b4fae", "2021-05-01", "11",
        "bengal", "adreno", "196610", "SM6115",
        "zygote64_32", "motorola/borneo/borneo:11/RRQ31.Q3-47-22/2b4fae:user/release-keys", "RRQ31.Q3-47-22", "borneo-user 11 RRQ31.Q3-47-22 2b4fae release-keys",
        "borneo-user", "buildbot-motoauto06.mcd.mot.com", "hudsoncm", "1619827200",
        "REL", "0", "Qualcomm", "Adreno (TM) 610",
        "OpenGL ES 3.2 V@0502.0 (GIT@5f4e5c9, Ia3b7920, 1600000000)", "1080", "2400", "404", 8, 4,
        8000, 6000, 3264, 2448,                                  // PR44: pixelArray rear/front (int32_t)
        78.4532f, 0.0023946f, 34.906586f, 0.001064f, 4912.0f,  // ICM-42688 (TDK),
        6.40f, 4.80f, 4.74f, 2.0f,                              // PR44: rear physSize + focal + aperture
        3.20f, 2.40f, 2.02f, 2.0f,                              // PR44: front physSize + focal + aperture
        false, false, false                                      // PR38+39: sensor presence bools
    } },
    { "Moto G Stylus 2021", {
        "motorola", "motorola", "moto g stylus (2021) 5G", "nairo",
        "nairo_retail", "qcom", "holi", "1.0.0.0",
        "motorola/nairo_retail/nairo:11/RPNS31.Q1-45-20/1.0.0.0:user/release-keys", "RPNS31.Q1-45-20", "release-keys", "user",
        "MPSS.AT.4.0-00055-SM4350_GEN_PACK-1", "1.0.0.0", "2021-05-01", "11",
        "holi", "adreno", "196610", "SM4350",
        "zygote64_32", "motorola/nairo_retail/nairo:11/RPNS31.Q1-45-20/1.0.0.0:user/release-keys", "RPNS31.Q1-45-20", "nairo_retail-user 11 RPNS31.Q1-45-20 1.0.0.0 release-keys",
        "nairo_retail-user", "moto-build-prod-02", "moto", "1622505600",
        "REL", "0", "Qualcomm", "Adreno (TM) 619",
        "OpenGL ES 3.2 V@0502.0 (GIT@5f4e5c9, Ia3b7920, 1600000000)", "1080", "2400", "386", 8, 4,
        8000, 6000, 4608, 3456,                                  // PR44: pixelArray rear/front (int32_t)
        78.4532f, 0.0023946f, 34.906586f, 0.001064f, 4912.0f,  // ICM-42688 (TDK),
        6.40f, 4.80f, 4.74f, 2.0f,                              // PR44: rear physSize + focal + aperture
        4.00f, 3.00f, 2.18f, 2.45f,                              // PR44: front physSize + focal + aperture
        false, false, false                                      // PR38+39: sensor presence bools
    } },
    { "Moto Edge", {
        "motorola", "motorola", "moto edge", "racer",
        "racer_retail", "qcom", "lito", "1.0.0.0",
        "motorola/racer_retail/racer:11/RRAS31.Q1-16-113/1.0.0.0:user/release-keys", "RRAS31.Q1-16-113", "release-keys", "user",
        "MPSS.VT.5.2-00075-SM7250_GEN_PACK-1", "1.0.0.0", "2021-04-01", "11",
        "lito", "adreno", "196610", "SM7250",
        "zygote64_32", "motorola/racer_retail/racer:11/RRAS31.Q1-16-113/1.0.0.0:user/release-keys", "RRAS31.Q1-16-113", "racer_retail-user 11 RRAS31.Q1-16-113 1.0.0.0 release-keys",
        "racer_retail-user", "moto-build-prod-03", "moto", "1617235200",
        "REL", "0", "Qualcomm", "Adreno (TM) 620",
        "OpenGL ES 3.2 V@0502.0 (GIT@5f4e5c9, Ia3b7920, 1600000000)", "1080", "2340", "384", 8, 6,
        9248, 6944, 5184, 3880,                                  // PR44: pixelArray rear/front (int32_t)
        78.4532f, 0.0023946f, 34.906586f, 0.001064f, 4912.0f,  // LSM6DSO (ST Micro),
        7.40f, 5.56f, 5.09f, 2.0f,                              // PR44: rear physSize + focal + aperture
        5.04f, 3.78f, 2.73f, 2.4f,                              // PR44: front physSize + focal + aperture
        false, false, false                                      // PR38+39: sensor presence bools
    } },
    { "Moto Edge Plus", {
        "motorola", "motorola", "motorola edge+", "sofiar",
        "sofiar_retail", "qcom", "kona", "1.0.0.0",
        "motorola/sofiar_retail/sofiar:11/RROS31.Q1-13-52/1.0.0.0:user/release-keys", "RROS31.Q1-13-52", "release-keys", "user",
        "MPSS.HI.3.2.c1.1-00125-SM8250_GEN_PACK-1", "1.0.0.0", "2021-05-01", "11",
        "kona", "adreno", "196610", "SM8250-AB",
        "zygote64_32", "motorola/sofiar_retail/sofiar:11/RROS31.Q1-13-52/1.0.0.0:user/release-keys", "RROS31.Q1-13-52", "sofiar_retail-user 11 RROS31.Q1-13-52 1.0.0.0 release-keys",
        "sofiar_retail-user", "moto-build-prod-04", "moto", "1619827200",
        "REL", "0", "Qualcomm", "Adreno (TM) 650",
        "OpenGL ES 3.2 V@0502.0 (GIT@5f4e5c9, Ia3b7920, 1600000000)", "1080", "2340", "384", 8, 12,
        12032, 9024, 5184, 3880,                                  // PR44: pixelArray rear/front (int32_t)
        78.4532f, 0.0023946f, 34.906586f, 0.001064f, 4912.0f,  // LSM6DSO (ST Micro),
        9.60f, 7.20f, 5.58f, 1.8f,                              // PR44: rear physSize + focal + aperture
        5.04f, 3.78f, 2.73f, 2.4f,                              // PR44: front physSize + focal + aperture
        false, true, false                                      // PR38+39: sensor presence bools
    } },
    { "Nokia 8.3 5G", {
        "HMD Global", "Nokia", "Nokia 8.3 5G", "nokia_8.3_5g",
        "BVUB_00WW", "qcom", "lito", "00WW_3_510",
        "Nokia/BVUB_00WW/nokia_8.3_5g:11/RKQ1.200928.002/00WW_3_510:user/release-keys", "RKQ1.200928.002", "release-keys", "user",
        "MPSS.VT.5.2-00075-SM7250_GEN_PACK-1", "00WW_3_510", "2021-08-01", "11",
        "lito", "adreno", "196610", "SM7250",
        "zygote64_32", "Nokia/BVUB_00WW/nokia_8.3_5g:11/RKQ1.200928.002/00WW_3_510:user/release-keys", "RKQ1.200928.002", "BVUB_00WW-user 11 RKQ1.200928.002 00WW_3_510 release-keys",
        "BVUB_00WW-user", "hmd-build-01", "hmd", "1627776000",
        "REL", "0", "Qualcomm", "Adreno (TM) 620",
        "OpenGL ES 3.2 V@0502.0 (GIT@5f4e5c9, Ia3b7920, 1600000000)", "1080", "2400", "386", 8, 6,
        9248, 6936, 5520, 4140,                                  // PR44: pixelArray rear/front (int32_t)
        78.4532f, 0.0023946f, 34.906586f, 0.001064f, 4912.0f,  // LSM6DSR (ST Micro),
        7.40f, 5.56f, 5.09f, 2.0f,                              // PR44: rear physSize + focal + aperture
        4.80f, 3.60f, 2.75f, 2.0f,                              // PR44: front physSize + focal + aperture
        false, true, false                                      // PR38+39: sensor presence bools
    } },
    { "Nokia 5.4", {
        "HMD Global", "Nokia", "Nokia 5.4", "nokia_5.4",
        "CAV_00WW", "qcom", "bengal", "00WW_2_440",
        "Nokia/CAV_00WW/nokia_5.4:11/RKQ1.200928.002/00WW_2_440:user/release-keys", "RKQ1.200928.002", "release-keys", "user",
        "MPSS.AT.4.0-00055-SM6115_GEN_PACK-1", "00WW_2_440", "2021-06-01", "11",
        "bengal", "adreno", "196610", "SM6115",
        "zygote64_32", "Nokia/CAV_00WW/nokia_5.4:11/RKQ1.200928.002/00WW_2_440:user/release-keys", "RKQ1.200928.002", "CAV_00WW-user 11 RKQ1.200928.002 00WW_2_440 release-keys",
        "CAV_00WW-user", "hmd-build-02", "hmd", "1622505600",
        "REL", "0", "Qualcomm", "Adreno (TM) 610",
        "OpenGL ES 3.2 V@0502.0 (GIT@5f4e5c9, Ia3b7920, 1600000000)", "1080", "2400", "409", 8, 4,
        8000, 6000, 4608, 3456,                                  // PR44: pixelArray rear/front (int32_t)
        78.4532f, 0.0023946f, 34.906586f, 0.001064f, 4912.0f,  // ICM-42688 (TDK),
        6.40f, 4.80f, 4.74f, 2.0f,                              // PR44: rear physSize + focal + aperture
        4.00f, 3.00f, 2.18f, 2.45f,                              // PR44: front physSize + focal + aperture
        false, false, false                                      // PR38+39: sensor presence bools
    } },
    { "Realme 8 Pro", {
        "realme", "realme", "RMX3091", "RMX3091",
        "RMX3091", "qcom", "atoll", "1636363205855",
        "realme/RMX3091/RMX3091:11/RP1A.200720.011/1636363205855:user/release-keys", "RP1A.200720.011", "release-keys", "user",
        "MPSS.AT.4.0-00050-SM7150_GEN_PACK-1", "1636363205855", "2021-11-01", "11",
        "atoll", "adreno", "196610", "SM7125",
        "zygote64_32", "realme/RMX3091/RMX3091:11/RP1A.200720.011/1636363205855:user/release-keys", "RP1A.200720.011", "RMX3091-user 11 RP1A.200720.011 1636363205855 release-keys",
        "RMX3091-user", "oppo-build-01", "oppo", "1635724800",
        "REL", "0", "Qualcomm", "Adreno (TM) 618",
        "OpenGL ES 3.2 V@0502.0 (GIT@5f4e5c9, Ia3b7920, 1600000000)", "1080", "2400", "411", 8, 8,
        12032, 9024, 4608, 3456,                                  // PR44: pixelArray rear/front (int32_t)
        39.2266f, 0.0011974f, 34.906586f, 0.001064f, 2000.0f,  // BMA253 (Bosch),
        9.60f, 7.20f, 5.58f, 1.88f,                              // PR44: rear physSize + focal + aperture
        4.00f, 3.00f, 2.18f, 2.45f,                              // PR44: front physSize + focal + aperture
        false, false, false                                      // PR38+39: sensor presence bools
    } },
    { "Realme GT Master", {
        "realme", "realme", "RMX3363", "RE58B2L1",
        "RMX3363", "qcom", "sm7325", "S.20211201",
        "realme/RMX3363/RE58B2L1:11/RP1A.200720.011/1638316800000:user/release-keys", "RP1A.200720.011", "release-keys", "user",
        "MPSS.VT.5.2-00095-SM7325_GEN_PACK-1", "1638316800000", "2021-12-01", "11",
        "sm7325", "adreno", "196610", "SM7325",
        "zygote64_32", "realme/RMX3363/RE58B2L1:11/RP1A.200720.011/1638316800000:user/release-keys", "RP1A.200720.011", "RMX3363-user 11 RP1A.200720.011 1638316800000 release-keys",
        "RMX3363-user", "oppo-build-02", "oppo", "1638316800",
        "REL", "0", "Qualcomm", "Adreno (TM) 642L",
        "OpenGL ES 3.2 V@0502.0 (GIT@5f4e5c9, Ia3b7920, 1600000000)", "1080", "2400", "409", 8, 8,
        9248, 6944, 4608, 3456,                                  // PR44: pixelArray rear/front (int32_t)
        39.2266f, 0.0011974f, 34.906586f, 0.001064f, 2000.0f,  // BMA253 (Bosch),
        7.40f, 5.56f, 5.09f, 1.8f,                              // PR44: rear physSize + focal + aperture
        4.00f, 3.00f, 2.18f, 2.45f,                              // PR44: front physSize + focal + aperture
        false, false, false                                      // PR38+39: sensor presence bools
    } },
    { "ASUS ZenFone 7", {
        "asus", "asus", "ASUS_I002D", "ASUS_I002D",
        "WW_I002D", "qcom", "kona", "18.0840.2101.26-0",
        "asus/WW_I002D/ASUS_I002D:11/RKQ1.200826.002/18.0840.2101.26-0:user/release-keys", "RKQ1.200826.002", "release-keys", "user",
        "M3.13.24.51-Sake_0000100", "18.0840.2101.26-0", "2021-09-01", "11",
        "kona", "adreno", "196610", "SM8250-AB",
        "zygote64_32", "asus/WW_I002D/ASUS_I002D:11/RKQ1.200826.002/18.0840.2101.26-0:user/release-keys", "RKQ1.200826.002", "WW_I002D-user 11 RKQ1.200826.002 18.0840.2101.26-0 release-keys",
        "WW_I002D-user", "android-build", "jenkins", "1629859200",
        "REL", "0", "Qualcomm", "Adreno (TM) 650",
        "OpenGL ES 3.2 V@0502.0 (GIT@5f4e5c9, Ia3b7920, 1600000000)", "1080", "2400", "394", 8, 8,
        9248, 6944, 9248, 6944,                                  // PR44: pixelArray rear/front (int32_t)
        78.4532f, 0.0023946f, 34.906586f, 0.001064f, 4912.0f,  // LSM6DSO (ST Micro),
        7.40f, 5.56f, 4.09f, 2.0f,                              // PR44: rear physSize + focal + aperture
        7.40f, 5.56f, 4.09f, 2.0f,                              // PR44: front physSize + focal + aperture
        false, true, false                                      // PR38+39: sensor presence bools
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
};
