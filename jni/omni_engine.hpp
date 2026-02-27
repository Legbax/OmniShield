#pragma once

#include <string>
#include <vector>
#include <map>
#include <random>
#include <sstream>
#include <iomanip>
#include <ctime>
#include <algorithm>
#include <sys/time.h>
#include <cmath>
#include "omni_profiles.h"

namespace omni {
namespace engine {

// TACs mapped by brand
static const std::map<std::string, std::vector<std::string>> TACS_BY_BRAND = {
    {"xiaomi",   {"86413404", "86413405", "35271311", "35361311", "86814904", "86814905"}},
    {"poco",     {"86814904", "86814905", "35847611", "35847612", "35847613", "35847614"}},
    {"redmi",    {"86413404", "86413405", "35271311", "35271312", "35271313", "35271314"}},
    {"samsung",  {"35449209", "35449210", "35355610", "35735110", "35735111", "35843110", "35940210"}},
    {"google",   {"35674910", "35674911", "35308010", "35308011", "35908610", "35908611"}},
    {"oneplus",  {"86882504", "86882505", "35438210", "35438211", "35438212", "35438213"}},
    {"motorola", {"35617710", "35617711", "35327510", "35327511", "35327512", "35327513"}},
    {"nokia",    {"35720210", "35720211", "35489310", "35489311", "35654110", "35654111"}},
    {"realme",   {"86828804", "86828805", "35388910", "35388911", "35388912", "35388913"}},
    {"vivo",     {"86979604", "86979605", "35503210", "35503211", "35503212", "35503213"}},
    {"oppo",     {"86885004", "86885005", "35604210", "35604211", "35604212", "35604213"}},
    {"asus",     {"35851710", "35851711", "35325010", "35325011", "35325012", "35325013"}},
    {"hmd global", {"35720210", "35720211", "35489310", "35489311"}},
    {"default",  {"35271311", "35449209", "35674910", "35438210", "35617710"}}
};

// OUI pools
static const std::vector<std::vector<uint8_t>> OUIS = {
    {0x40, 0x4E, 0x36}, {0xF0, 0x1F, 0xAF}, {0x18, 0xDB, 0x7E}, {0x28, 0xCC, 0x01}, // Qualcomm
    {0x60, 0x57, 0x18}, {0xAC, 0x37, 0x43}, {0x00, 0x90, 0x4C},                     // MediaTek
    {0xD4, 0xBE, 0xD9}, {0xA4, 0xC3, 0xF0}, {0xF8, 0x8F, 0xCA},                     // Broadcom
    {0x40, 0x9B, 0xCD}, {0x24, 0x4B, 0x03}                                          // Samsung
};

inline std::string toLower(const std::string& str) {
    std::string s = str;
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c){ return std::tolower(c); });
    return s;
}

// 1. LUHN FIX: Paridad par para base de 14 dígitos
inline int luhnChecksum(const std::string& number) {
    int sum = 0, len = number.length();
    for (int i = len - 1; i >= 0; i--) {
        int d = number[i] - '0';
        if ((len - 1 - i) % 2 == 0) { d *= 2; if (d > 9) d -= 9; }
        sum += d;
    }
    return (10 - (sum % 10)) % 10;
}

class Random {
    std::mt19937_64 rng;
public:
    Random(long seed) : rng(seed) {}
    Random() : rng(std::random_device{}()) {}

    int nextInt(int bound) {
        return std::uniform_int_distribution<int>(0, bound - 1)(rng);
    }

    int nextInt() {
        return std::uniform_int_distribution<int>()(rng);
    }

    float nextFloat(float min, float max) {
        return std::uniform_real_distribution<float>(min, max)(rng);
    }
};

inline std::string getRegionForProfile(const std::string& profileName) {
    (void)profileName;  // Todos los perfiles operan exclusivamente en USA
    return "usa";
}

inline std::string generateValidImei(const std::string& profileName, long seed) {
    Random rng(seed);
    std::string brand = "default";
    bool isQualcomm = false;

    auto it = getDeviceProfiles().find(profileName);
    if (it != getDeviceProfiles().end()) {
        brand = toLower(it->second.brand);
        isQualcomm = (toLower(std::string(it->second.eglDriver)) == "adreno");
    }

    const std::vector<std::string>* tacList = TACS_BY_BRAND.count(brand) ? &TACS_BY_BRAND.at(brand) : &TACS_BY_BRAND.at("default");
    std::string tac;

    if (brand == "redmi" && isQualcomm) {
        std::vector<std::string> qcomTacs = {"35271311", "35271312", "35271313", "35271314"};
        tac = qcomTacs[rng.nextInt(qcomTacs.size())];
    } else {
        tac = (*tacList)[rng.nextInt(tacList->size())];
    }

    std::string serial = "";
    for(int i=0; i<6; ++i) serial += std::to_string(rng.nextInt(10));
    std::string base = tac + serial;
    return base + std::to_string(luhnChecksum(base));
}

// ICCID: Estándar ITU-T E.118 (89...)
inline std::string generateValidIccid(const std::string& profileName, long seed) {
    // PR41: Solo USA — country code 1
    static const std::map<std::string, std::string> ICCID_PREFIX = {
        {"usa", "89101"},
    };
    Random rng(seed);
    std::string region = getRegionForProfile(profileName);
    std::string prefix = ICCID_PREFIX.count(region) ? ICCID_PREFIX.at(region) : "89101";
    std::string iccid = prefix;
    int remaining = 18 - (int)prefix.size();  // base 18 + 1 check digit = 19 total (ITU-T E.118)
    for(int i = 0; i < remaining; ++i) iccid += std::to_string(rng.nextInt(10));
    return iccid + std::to_string(luhnChecksum(iccid));
}

inline std::string generateValidImsi(const std::string& profileName, long seed) {
    // PR41: Pool USA expandido — 5 carriers cubren >98% del mercado real
    static const std::map<std::string, std::vector<std::string>> IMSI_POOLS = {
        {"usa", {"310260", "310410", "311480", "310120", "311580"}}
        //       T-Mobile   AT&T      Verizon   Sprint    US Cellular
    };
    Random rng(seed);
    std::string region = getRegionForProfile(profileName);
    const auto& pool = IMSI_POOLS.count(region) ? IMSI_POOLS.at(region) : IMSI_POOLS.at("usa");
    std::string mccMnc = pool[rng.nextInt(pool.size())];
    int remaining = 15 - mccMnc.length();
    std::string rest = std::to_string(2 + rng.nextInt(8));
    for (int i = 1; i < remaining; ++i) rest += std::to_string(rng.nextInt(10));
    return mccMnc + rest;
}

inline std::string generatePhoneNumber(const std::string& profileName, long seed) {
    // PR41: Solo USA NANP
    static const std::map<std::string,std::string> CC={{"usa","+1"}};
    Random rng(seed+777);
    std::string region=getRegionForProfile(profileName);
    std::string cc=CC.count(region)?CC.at(region):"+1";

    // PR42: NANP completo — estructura NPA-NXX-XXXX
    // NPA (área code, dígitos 1-3): primer dígito 2-9, excluir N11 y 555
    // NXX (exchange, dígitos 4-6): primer dígito también 2-9 (regla NANP)
    // XXXX (subscriber, dígitos 7-10): libre 0-9
    std::string npa, nxx;

    // Generar NPA válido (excluir N11: d2==1&&d3==1, y 555)
    do {
        npa  = std::to_string(2 + rng.nextInt(8));   // d1: 2-9
        npa += std::to_string(rng.nextInt(10));        // d2: 0-9
        npa += std::to_string(rng.nextInt(10));        // d3: 0-9
    } while ((npa[1] == '1' && npa[2] == '1') || npa == "555");

    // Generar NXX válido (d4 también debe ser 2-9, excluir N11)
    do {
        nxx  = std::to_string(2 + rng.nextInt(8));   // d4: 2-9
        nxx += std::to_string(rng.nextInt(10));        // d5: 0-9
        nxx += std::to_string(rng.nextInt(10));        // d6: 0-9
    } while (nxx[1] == '1' && nxx[2] == '1');

    // XXXX subscriber (dígitos 7-10): libre
    std::string sub = "";
    for (int i = 0; i < 4; ++i) sub += std::to_string(rng.nextInt(10));

    return cc + npa + nxx + sub;
}

inline std::string generateRandomMac(const std::string& brandIn, long seed) {
    Random rng(seed);
    std::string brand = toLower(brandIn);
    std::vector<uint8_t> oui;

    if (brand == "samsung") {
        std::vector<std::vector<uint8_t>> pool = {{0x24, 0x4B, 0x03}, {0x40, 0x9B, 0xCD}, {0xD4, 0xBE, 0xD9}};
        oui = pool[rng.nextInt(pool.size())];
    } else if (brand == "google") {
        std::vector<std::vector<uint8_t>> pool = {{0xF8, 0x8F, 0xCA}, {0xA4, 0xC3, 0xF0}};
        oui = pool[rng.nextInt(pool.size())];
    } else if (brand == "xiaomi" || brand == "redmi" || brand == "poco") {
        std::vector<std::vector<uint8_t>> pool = {{0x40, 0x4E, 0x36}, {0xF0, 0x1F, 0xAF}, {0x60, 0x57, 0x18}};
        oui = pool[rng.nextInt(pool.size())];
    } else if (brand == "oneplus") { oui = {0x40, 0x4E, 0x36};
    } else if (brand == "motorola") { oui = {0x18, 0xDB, 0x7E};
    } else { oui = OUIS[rng.nextInt(OUIS.size())]; }

    std::stringstream ss;
    ss << std::hex << std::setfill('0') << std::nouppercase;
    for (size_t i = 0; i < oui.size(); ++i) ss << std::setw(2) << (int)oui[i] << ":";
    for (int i = 0; i < 3; ++i) ss << std::setw(2) << rng.nextInt(256) << (i == 2 ? "" : ":");
    return ss.str();
}

inline std::string generateRandomSerial(const std::string& brandIn, long seed, const std::string& securityPatch) {
    Random rng(seed);
    std::string brand = toLower(brandIn);
    if (brand == "samsung") {
        std::vector<char> plantPrefixes = {'R', 'R', 'S', 'X'};
        char plant = plantPrefixes[rng.nextInt(plantPrefixes.size())];
        std::vector<std::string> factories = {"F8", "58", "28", "R1", "S1"};
        std::string factory = factories[rng.nextInt(factories.size())];

        int year = 2021;
        if (securityPatch.size() >= 4) try { year = std::stoi(securityPatch.substr(0, 4)); } catch (...) {}
        char yearChar = (year == 2019) ? 'R' : (year == 2020) ? 'S' : (year == 2021) ? 'T' : (year == 2022) ? 'U' : (year == 2023) ? 'V' : 'T';

        std::string monthChars = "123456789ABC";
        char month = monthChars[rng.nextInt(monthChars.length())];

        std::string unique = "";
        for(int i=0; i<6; ++i) unique += "0123456789ABCDEF"[rng.nextInt(16)];
        return std::string(1, plant) + factory + std::string(1, yearChar) + std::string(1, month) + unique;
    }

    std::string alphaNum = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    if (brand == "google") {
        std::string chars = "ABCDEFGHJKLMNPQRSTUVWXYZ0123456789";
        std::string res = "";
        for(int i=0; i<7; ++i) res += chars[rng.nextInt(chars.length())];
        return res;
    } else {
        int len = 8 + rng.nextInt(5);
        std::string res = "";
        for(int i=0; i<len; ++i) res += alphaNum[rng.nextInt(alphaNum.length())];
        return res;
    }
}

inline std::string generateRandomId(int len, long seed) {
    Random rng(seed);
    std::string chars = "0123456789abcdef";
    std::string res = "";
    for(int i=0; i<len; ++i) res += chars[rng.nextInt(chars.length())];
    return res;
}

inline std::vector<uint8_t> generateWidevineBytes(long seed) {
    Random rng(seed); std::vector<uint8_t> id(16);
    for(int i=0; i<16; ++i) id[i] = (uint8_t)rng.nextInt(256);
    return id;
}
inline std::string generateWidevineId(long seed) {
    std::vector<uint8_t> bytes = generateWidevineBytes(seed);
    std::stringstream ss;
    ss << std::hex << std::setfill('0') << std::nouppercase;
    for(int i=0; i<16; ++i) ss << std::setw(2) << (int)bytes[i];
    return ss.str();
}

// Temperatura: Estática basada en semilla
inline std::string generateBatteryTemp(long seed) {
    Random rng(seed);
    int temp = 330 + rng.nextInt(30);
    return std::to_string(temp);
}

// Voltaje: Estática basada en semilla
inline std::string generateBatteryVoltage(long seed) {
    Random rng(seed);
    int uv = (rng.nextInt(400) + 3700) * 1000;
    return std::to_string(uv);
}

inline std::string generateTls13CipherSuites(long seed) {
    std::vector<std::string> suites = {"TLS_AES_128_GCM_SHA256", "TLS_AES_256_GCM_SHA384", "TLS_CHACHA20_POLY1305_SHA256"};
    Random rng(seed + 999);
    for (int i = suites.size() - 1; i > 0; --i) std::swap(suites[i], suites[rng.nextInt(i + 1)]);
    std::string res = suites[0];
    for (size_t i = 1; i < suites.size(); ++i) res += ":" + suites[i];
    return res;
}

inline std::string generateTls12CipherSuites(long seed) {
    std::vector<std::string> suites = {"ECDHE-ECDSA-AES128-GCM-SHA256", "ECDHE-RSA-AES128-GCM-SHA256", "ECDHE-ECDSA-AES256-GCM-SHA384", "ECDHE-RSA-AES256-GCM-SHA384"};
    Random rng(seed + 1001);
    for (int i = suites.size() - 1; i > 0; --i) std::swap(suites[i], suites[rng.nextInt(i + 1)]);
    std::string res = suites[0];
    for (size_t i = 1; i < suites.size(); ++i) res += ":" + suites[i];
    return res;
}

inline const char* getGlVersionForProfile(const DeviceFingerprint& fp) { return fp.gpuVersion; }

// PR38+39: GPS — coordenadas determinísticas coherentes con región del perfil.
// Genera lat/lon dentro del rango geográfico correcto (ciudad representativa ± ~2km).
// Usando el mismo g_masterSeed garantiza coherencia entre sesiones sin cambiar identidad.
inline void generateLocationForRegion(const std::string& profileName, long seed,
                                       double& outLat, double& outLon) {
    Random rng(seed + 9999LL);
    std::string region = getRegionForProfile(profileName);

    // PR41: Pool de 5 ciudades USA — selección determinística por seed
    static const double US_CITIES[][2] = {
        {40.7128, -74.0060},   // New York
        {34.0522, -118.2437},  // Los Angeles
        {41.8781, -87.6298},   // Chicago
        {29.7604, -95.3698},   // Houston
        {33.4484, -112.0740},  // Phoenix
    };
    Random cityRng(seed + 7777LL);
    int cityIdx = cityRng.nextInt(5);
    double baseLat = US_CITIES[cityIdx][0];
    double baseLon = US_CITIES[cityIdx][1];

    // Jitter determinístico ±0.02° (≈ 2km de radio) — simula posición específica
    double jitterLat = (rng.nextInt(4000) - 2000) / 100000.0;
    double jitterLon = (rng.nextInt(4000) - 2000) / 100000.0;
    outLat = baseLat + jitterLat;
    outLon = baseLon + jitterLon;
}

// PR38+39: Altitud determinística realista por región (metros sobre nivel del mar)
inline double generateAltitudeForRegion(const std::string& profileName, long seed) {
    Random rng(seed + 8888LL);
    (void)profileName;
    // PR41: Altitudes USA realistas — varía según ciudad (seed-deterministic)
    // NYC ~10-35m, LA ~50-120m, Chicago ~180-200m, Houston ~10-25m, Phoenix ~330-360m
    static const double US_ALT_BASE[] = {10.0, 50.0, 180.0, 10.0, 330.0};
    static const int    US_ALT_RANGE[] = {25, 70, 20, 15, 30};
    Random cityRng(seed + 7777LL);  // Mismo seed que GPS → misma ciudad
    int idx = cityRng.nextInt(5);
    return US_ALT_BASE[idx] + (rng.nextInt(US_ALT_RANGE[idx]));
}

// PR42: Timezone coherente con ciudad GPS — mismo seed que cityRng (seed+7777)
// Houston usa Central Time (America/Chicago). Phoenix es MST sin DST (America/Phoenix).
inline std::string getTimezoneForProfile(long seed) {
    static const std::string US_CITY_TZ[5] = {
        "America/New_York",    // idx=0: New York
        "America/Los_Angeles", // idx=1: Los Angeles
        "America/Chicago",     // idx=2: Chicago
        "America/Chicago",     // idx=3: Houston (Central Time = Chicago)
        "America/Phoenix"      // idx=4: Phoenix (MST, no DST)
    };
    Random cityRng(seed + 7777LL);  // MISMO seed+offset que generateLocationForRegion
    return US_CITY_TZ[cityRng.nextInt(5)];
}

// PR41: Mapeo MCC/MNC → nombre comercial del carrier USA
// Usado por main.cpp para gsm.sim.operator.alpha
inline std::string getCarrierNameForImsi(const std::string& profileName, long seed) {
    static const std::map<std::string, std::string> CARRIER_NAMES = {
        {"310260", "T-Mobile"},
        {"310410", "AT&T"},
        {"311480", "Verizon Wireless"},
        {"310120", "T-Mobile"},       // Sprint fusionado → reporta T-Mobile en red
        {"311580", "U.S. Cellular"},
    };
    std::string imsi = generateValidImsi(profileName, seed);
    std::string mccMnc = (imsi.substr(0,3) == "310" || imsi.substr(0,3) == "311")
                         ? imsi.substr(0,6) : imsi.substr(0,5);
    auto it = CARRIER_NAMES.find(mccMnc);
    return (it != CARRIER_NAMES.end()) ? it->second : "T-Mobile";
}

// PR42: Samsung Qualcomm recibe RIL Qualcomm, no Samsung
// Afectados: Galaxy A52 (atoll), A72 (atoll), S20 FE (kona), A52s (sm7325)
// Samsung Exynos siguen con "Samsung RIL v3.0"
inline std::string getRilVersionForProfile(const std::string& profileName) {
    auto it = getDeviceProfiles().find(profileName);
    if (it != getDeviceProfiles().end()) {
        std::string plat = toLower(std::string(it->second.boardPlatform));
        std::string brand = toLower(std::string(it->second.brand));
        // MediaTek — cualquier marca
        if (plat.find("mt6") != std::string::npos || plat.find("mt8") != std::string::npos)
            return "android mtk-ril 1.0";
        // Samsung Exynos exclusivamente → RIL Samsung
        if (brand == "samsung" && plat.find("exynos") != std::string::npos)
            return "Samsung RIL v3.0";
        // Todo lo demás (Qualcomm, incluido Samsung Qualcomm) → AOSP RIL
    }
    return "android qualcomm ril 1.0";
}

// PR38+39: Seed version tracking — permite detectar rotación de seed desde la UI.
// La UI companion escribe seed_version=N en el config cuando rota el master_seed.
// El módulo compara el seed_version almacenado vs el del config para invalidar
// caches derivadas del seed anterior (location, serial, android_id, etc.).
// El seed_version=0 (default) significa "sin rotación configurada".
inline bool isSeedVersionChanged(long currentSeedVersion, long storedSeedVersion) {
    return currentSeedVersion != 0 && currentSeedVersion != storedSeedVersion;
}

} // namespace engine
} // namespace omni
