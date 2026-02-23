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
#include "vortex_profiles.h"

namespace vortex {
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
    auto it = VORTEX_PROFILES.find(profileName);
    if (it != VORTEX_PROFILES.end()) {
        std::string p = toLower(std::string(it->second.product));
        if (p.find("global") != std::string::npos || p.find("eea") != std::string::npos) return "europe";
        if (p.find("india") != std::string::npos) return "india";
        if (p.size() >= 3 && (p.substr(p.size()-3) == "_us" || p.substr(p.size()-4) == "_usa")) return "usa";
    }
    return "europe";
}

inline std::string generateValidImei(const std::string& profileName, long seed) {
    Random rng(seed);
    std::string brand = "default";
    bool isQualcomm = false;

    auto it = VORTEX_PROFILES.find(profileName);
    if (it != VORTEX_PROFILES.end()) {
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

inline std::string generateValidIccid(const std::string& profileName, long seed) {
    static const std::map<std::string, std::pair<std::string,std::string>> ICCID_MCC_MNC = {
        {"india", {"404", "20"}}, {"europe", {"234", "20"}}, {"latam", {"724", "06"}}, {"usa", {"310", "260"}}
    };
    Random rng(seed);
    std::string region = getRegionForProfile(profileName);
    auto mcc_mnc = ICCID_MCC_MNC.count(region) ? ICCID_MCC_MNC.at(region) : ICCID_MCC_MNC.at("europe");

    std::string base = "89" + mcc_mnc.first + mcc_mnc.second;
    while (base.size() < 18) base += std::to_string(rng.nextInt(10));
    base = base.substr(0, 18);
    return base + std::to_string(luhnChecksum(base));
}

inline std::string generateValidImsi(const std::string& profileName, long seed) {
    static const std::map<std::string, std::vector<std::string>> IMSI_POOLS = {
        {"europe", {"23420", "26201", "20801"}}, {"india", {"40420", "40401", "40486"}},
        {"latam", {"72406", "72410"}}, {"usa", {"310260", "310410"}}
    };
    Random rng(seed);
    std::string region = getRegionForProfile(profileName);
    const auto& pool = IMSI_POOLS.count(region) ? IMSI_POOLS.at(region) : IMSI_POOLS.at("europe");

    std::string mccMnc = pool[rng.nextInt(pool.size())];
    int remaining = 15 - mccMnc.length();
    std::string rest = std::to_string(2 + rng.nextInt(8));
    for (int i = 1; i < remaining; ++i) rest += std::to_string(rng.nextInt(10));
    return mccMnc + rest;
}

inline std::string generatePhoneNumber(const std::string& profileName, long seed) {
    static const std::map<std::string, std::string> COUNTRY_CODES = {
        {"europe", "+44"}, {"india", "+91"}, {"latam", "+55"}, {"usa", "+1"}
    };
    Random rng(seed + 777);
    std::string region = getRegionForProfile(profileName);
    std::string cc = COUNTRY_CODES.count(region) ? COUNTRY_CODES.at(region) : "+44";

    std::string local = std::to_string(2 + rng.nextInt(8));
    int len = 8 + rng.nextInt(3);
    for (int i = 1; i < len; ++i) local += std::to_string(rng.nextInt(10));
    return cc + local;
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
        char yearChar = (year == 2020) ? 'T' : (year == 2022) ? 'S' : (year == 2023) ? 'W' : 'R';

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
    Random rng(seed);
    std::vector<uint8_t> id(16);
    for(int i=0; i<16; ++i) id[i] = (uint8_t)rng.nextInt(256);
    return id;
}

inline std::string generateWidevineId(long seed) {
    auto bytes = generateWidevineBytes(seed);
    std::stringstream ss;
    ss << std::hex << std::setfill('0') << std::nouppercase;
    for(int i=0; i<16; ++i) ss << std::setw(2) << (int)bytes[i];
    return ss.str();
}

inline std::string generateBatteryTemp(long seed) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    long timeSlice = (ts.tv_sec * 10L) + (ts.tv_nsec / 100000000L);
    Random rng(seed + timeSlice);
    float temp = rng.nextFloat(340.0f, 390.0f); // 34.0C - 39.0C
    return std::to_string((int)temp);
}

inline std::string generateBatteryVoltage(long seed) {
    struct timeval tv; gettimeofday(&tv, nullptr);
    long timeMicros = (long)tv.tv_sec * 1000000L + tv.tv_usec;
    Random rng(seed + timeMicros / 500);
    long baseUv = 3850000L; // 3.85V -> µV
    return std::to_string(baseUv + (rng.nextInt(300000) - 150000));
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

} // namespace engine
} // namespace vortex
