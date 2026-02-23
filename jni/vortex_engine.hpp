#pragma once

#include <string>
#include <vector>
#include <map>
#include <random>
#include <sstream>
#include <iomanip>
#include <ctime>
#include <algorithm>
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
    int sum = 0;
    int len = number.length();
    for (int i = len - 1; i >= 0; i--) {
        int d = number[i] - '0';
        if ((len - 1 - i) % 2 != 0) {
             d *= 2;
             if (d > 9) d -= 9;
        }
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

inline std::string generateValidImei(const std::string& profileName, long seed) {
    Random rng(seed);
    std::string brand = "default";
    auto it = VORTEX_PROFILES.find(profileName);
    if (it != VORTEX_PROFILES.end()) brand = toLower(it->second.brand);

    const std::vector<std::string>* tacList;
    if (TACS_BY_BRAND.count(brand)) tacList = &TACS_BY_BRAND.at(brand);
    else tacList = &TACS_BY_BRAND.at("default");

    std::string tac = (*tacList)[rng.nextInt(tacList->size())];
    std::string serial = "";
    for(int i=0; i<6; ++i) serial += std::to_string(rng.nextInt(10));

    std::string base = tac + serial;
    return base + std::to_string(luhnChecksum(base));
}

inline std::string generateValidImsi(const std::string& mccMnc, long seed) {
    Random rng(seed);
    int first = 2 + rng.nextInt(8);
    std::string rest = "";
    for(int i=0; i<8; ++i) rest += std::to_string(rng.nextInt(10));
    return mccMnc + std::to_string(first) + rest;
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
    ss << std::hex << std::uppercase << std::setfill('0');
    for (size_t i = 0; i < oui.size(); ++i) ss << std::setw(2) << (int)oui[i] << ":";
    for (int i = 0; i < 3; ++i) ss << std::setw(2) << rng.nextInt(256) << (i == 2 ? "" : ":");
    return ss.str();
}

inline std::string generateRandomSerial(const std::string& brandIn, long seed, const std::string& securityPatch) {
    Random rng(seed);
    std::string brand = toLower(brandIn);
    std::string alphaNum = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";

    if (brand == "samsung") {
        std::vector<std::string> factories = {"F8", "58", "28", "R1", "S1"};
        std::string factory = factories[rng.nextInt(factories.size())];
        int year = 2021;
        if (securityPatch.size() >= 4) try { year = std::stoi(securityPatch.substr(0, 4)); } catch (...) {}

        char yearChar = 'R';
        switch(year) {
            case 2020: yearChar = 'T'; break;
            case 2021: yearChar = 'R'; break;
            case 2022: yearChar = 'S'; break;
            case 2023: yearChar = 'W'; break;
            case 2024: yearChar = 'X'; break;
            case 2025: yearChar = 'Y'; break;
            default: yearChar = 'R'; break;
        }
        std::string monthChars = "123456789ABC";
        char month = monthChars[rng.nextInt(monthChars.length())];
        std::string uniqueChars = "0123456789ABCDEF";
        std::string unique = "";
        for(int i=0; i<6; ++i) unique += uniqueChars[rng.nextInt(uniqueChars.length())];
        return "R" + factory + std::string(1, yearChar) + std::string(1, month) + unique;
    } else if (brand == "google") {
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
    Random rng(seed);
    std::stringstream ss;
    ss << std::hex << std::setfill('0');
    for(int i=0; i<16; ++i) ss << std::setw(2) << rng.nextInt(256);
    return ss.str();
}

inline std::string generateJA3CipherSuites(long seed) {
    // Android 11 Typical BoringSSL Cipher List (approximated)
    // TLS_AES_128_GCM_SHA256:TLS_AES_256_GCM_SHA384:TLS_CHACHA20_POLY1305_SHA256:...
    // We shuffle slightly or return a fixed robust list typical of the device generation.
    // Fixed standard set for Android 11 to avoid fingerprinting via ordering oddities.
    return "TLS_AES_128_GCM_SHA256:TLS_AES_256_GCM_SHA384:TLS_CHACHA20_POLY1305_SHA256:ECDHE-ECDSA-AES128-GCM-SHA256:ECDHE-RSA-AES128-GCM-SHA256:ECDHE-ECDSA-AES256-GCM-SHA384:ECDHE-RSA-AES256-GCM-SHA384:ECDHE-ECDSA-CHACHA20-POLY1305:ECDHE-RSA-CHACHA20-POLY1305:ECDHE-RSA-AES128-SHA:ECDHE-RSA-AES256-SHA:AES128-GCM-SHA256:AES256-GCM-SHA384:AES128-SHA:AES256-SHA";
}

inline std::string generateBatteryTemp(long seed) {
    Random rng(seed + std::time(nullptr)/10); // Slight variation over time
    float temp = rng.nextFloat(312.0f, 338.0f); // 31.2 - 33.8 C in decicelsius
    return std::to_string((int)temp);
}

inline std::string generateBatteryVoltage(long seed) {
    Random rng(seed + std::time(nullptr)/20);
    int mv = rng.nextInt(400) + 3800; // 3800 - 4200 mV
    return std::to_string(mv);
}

} // namespace engine
} // namespace vortex
