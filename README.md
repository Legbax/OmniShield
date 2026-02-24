# OmniShield: Privacy & Integrity for Social Apps

![OmniShield Banner](https://raw.githubusercontent.com/Legbax/OmniShield/main/readmebanner.png)

> **Developer Legba**

OmniShield (formerly Vortex) is an advanced privacy and security solution designed to provide a perfect symbiosis between application identity and environment stealth. Specifically optimized for the **Redmi 9 (Lancelot)** running Android 11, the project utilizes a deep hardening stack to evade detections in critical social applications such as Snapchat, Instagram, and Tinder.

---

## 🚀 Key Features

### 🛡️ Stealth Hardening & Detection Evasion
* **Kernel Integration:** Leverages KernelSU Next, SusFS, and Tricky Store for deep system-level hiding of mounts and files.
* **Hardware Attestation:** Supports **STRONG INTEGRITY** through custom Keybox management with Tricky Store.
* **Log Stripping:** Hardened logging that strictly respects production build configurations, removing unconditional logs to prevent log-based detection.

### 🆔 Comprehensive Device Spoofing
OmniShield allows for the manipulation of critical identifiers to maintain a consistent digital identity:
* **Hardware IDs:** Spoofing of IMEI (Slot 1 & 2), Serial Number, and MediaDRM Device ID.
* **Software IDs:** Android ID, SSAID (specifically for Snapchat), GSF ID, and Google Advertising ID (GAID).
* **Network Identity:** Spoofing of SIM Operator, SIM Country ISO, IMSI, ICCID, and Mobile Number.
* **Connectivity:** WiFi and Bluetooth MAC address spoofing, along with WiFi SSID and BSSID customization.

### 🌐 Social App Hardening
* **SSL Pinning Bypass:** Full bypass for OkHttp, TrustManager, and SSLContext, enabling traffic analysis and interception.
* **Sensor Spoofing:** Brand-aware sensor data manipulation (Vendor, Maximum Range) to prevent hardware-based discrepancies.

---

## 🎨 Customization Options

### 📱 Device Profiles
The app includes **40 high-fidelity fingerprints** directly integrated for various manufacturers including Xiaomi, Samsung, OnePlus, Pixel, Motorola, and more.
* **Randomize All:** Instantly generates a coherent identity, synchronizing all hardware and network IDs with a single tap.
* **Recent Profiles:** Quick access to previously used configurations for seamless switching.

### ⚙️ UI & UX (Dalí Edition)
* **Modern Interface:** A consistent purple neon dark theme utilizing Material Design 3 components.
* **Real-time Monitoring:** A dedicated Status dashboard featuring an Evasion Score indicator, Proxy status, and active IP address visualization.
* **Advanced Controls:** Granular application selection with dedicated switches to force stop or clear data for specific apps.

---

## ⚠️ Environment Requirements

To ensure maximum stealth effectiveness, OmniShield requires the following technical stack:
- **Device:** Redmi 9 (Lancelot).
- **OS:** MIUI 12.5.6.0.RJCMIXM (Android 11, SDK 30).
- **Root Solution:** KernelSU Next + SusFS + Shamiko + PIF Next.

---

## 📄 License & Disclaimer
OmniShield is intended for educational and cybersecurity research purposes only. The use of this software to manipulate third-party applications must comply with local laws and the respective platforms' terms of service.

**Developer:** [Legba](https://github.com/Legbax)