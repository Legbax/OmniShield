// app.js — OmniShield WebUI main application
import {
  generateIMEI, generateICCID, generateIMSI, generatePhoneNumber,
  generateMAC, generateSerial, generateAndroidId, generateGsfId,
  generateWidevineId, generateBootId, generateLocation, generateAltitude,
  getTimezone, getCarrierName, getSimCountry, computeCorrelation,
  validateIMEI, validateICCID, validateIMSI, validateMAC, validatePhone,
  validateAndroidId, validateGsfId, validateWidevineId, validateBootId, validateSerial,
  generateUUID, generateWifiSsid, generateGmail,
  US_CITIES_100, CARRIER_NAMES, IMSI_POOLS
} from './engine.js';
import { DEVICE_PROFILES, PROFILE_NAMES, getProfileByName } from './profiles.js';

// ─── JA3/TLS fingerprint presets (all Android-native clients) ─────────
// Firefox excluded: uses Gecko engine — incoherent with Android system WebView / app traffic.
// All presets here are Blink/Chromium-based or native Android HTTP stacks.
const JA3_PRESETS = [
  { name: 'Chrome 120 Android',       hash: '0700a69a2db4c9c8e5dedc5a1d14e7ce' },
  { name: 'Chrome 119 Android',       hash: 'bfbe57248732353af79a92ba6271b9d4' },
  { name: 'Samsung Internet 23',      hash: 'a0e9f5d64349fb13191bc781f81f42e1', brands: ['samsung'] },
  { name: 'OkHttp/4.12.0',            hash: 'd4e5b18d6b55c71db63d10a11e90e667' },
  { name: 'OkHttp/3.14.9 (Retrofit)', hash: 'c27a9b4a8b52c3ed95c5b1dc2e88b9f1' },
];

// ─── KernelSU exec wrapper ──────────────────────────────────────────
// The KernelSU/APatch manager injects a global `ksu` object into the
// WebView via @JavascriptInterface.  The `kernelsu` npm package is just
// a thin Promise wrapper around `ksu.exec()`.  We call the global
// directly to avoid dynamic-import timing issues that caused persistent
// "Save failed" errors on certain ROMs and KernelSU Next builds.
// Falls back to `import('kernelsu')` if the global is missing.
let _ksuExecFn = null;
// PR-UIFreeze: Reduced from 30 s → 8 s.  The old 30 s timeout per call meant
// 4 sequential calls could lock the binder/JS thread for up to 2 minutes,
// starving the system UI and freezing the entire phone.
const KSU_EXEC_TIMEOUT = 8000;

async function ksu_exec(cmd) {
  try {
    if (!_ksuExecFn) {
      // Prefer the native bridge global — instant, no import needed
      if (typeof ksu !== 'undefined' && typeof ksu.exec === 'function') {
        _ksuExecFn = (command) => new Promise((resolve, reject) => {
          const cb = '_omni_' + Date.now() + '_' + Math.random().toString(36).slice(2);
          const timer = setTimeout(() => {
            delete window[cb]; resolve({ errno: 1, stdout: '', stderr: 'timeout' });
          }, KSU_EXEC_TIMEOUT);
          window[cb] = (errno, stdout, stderr) => {
            clearTimeout(timer); delete window[cb];
            resolve({ errno, stdout, stderr: stderr || '' });
          };
          try { ksu.exec(command, '{}', cb); }
          catch(e) { clearTimeout(timer); delete window[cb]; reject(e); }
        });
      } else {
        // Fallback: dynamic import (dev environment or future managers)
        const mod = await Promise.race([
          import('kernelsu'),
          new Promise((_, rej) => setTimeout(() => rej(new Error('ksu import timeout')), 15000))
        ]);
        _ksuExecFn = mod.exec || mod.default?.exec || mod.default;
      }
    }
    if (typeof _ksuExecFn !== 'function') return { errno: 1, stdout: '', stderr: '' };
    const r = await _ksuExecFn(cmd);
    return { errno: Number(r?.errno) || 0, stdout: (r?.stdout || '').trim(), stderr: (r?.stderr || '').trim() };
  } catch(e) {
    return { errno: 1, stdout: '', stderr: '' };
  }
}

// PR-UIFreeze: Yield to the browser event loop so the OS can service touch
// events, render frames, and process binder calls between heavy operations.
function yieldToUI() {
  return new Promise(resolve => requestAnimationFrame(() => setTimeout(resolve, 0)));
}

// ─── Config management ──────────────────────────────────────────────
const CFG_PATH = '/data/adb/.omni_data/.identity.cfg';

async function readConfig() {
  const { stdout } = await ksu_exec(`cat "${CFG_PATH}" 2>/dev/null`);
  const cfg = {};
  (stdout || '').split('\n').forEach(line => {
    const eq = line.indexOf('=');
    if (eq > 0) {
      const k = line.slice(0, eq).trim();
      const v = line.slice(eq + 1).trim();
      if (k) cfg[k] = v;
    }
  });
  return cfg;
}

async function writeConfig(cfg) {
  await ksu_exec('mkdir -p /data/adb/.omni_data');
  const args = Object.entries(cfg).map(([k, v]) =>
    `'${`${k}=${v}`.replace(/'/g, "'\\''")}'`
  );
  await ksu_exec(`printf '%s\\n' ${args.join(' ')} > "${CFG_PATH}"`);
  await ksu_exec(`chmod 644 "${CFG_PATH}"`);
  // PR70c: Set SELinux context so zygote can read the config (companion is primary,
  // but this ensures the direct-read fallback also works)
  await ksu_exec(`chcon u:object_r:system_data_file:s0 /data/adb/.omni_data 2>/dev/null`);
  await ksu_exec(`chcon u:object_r:system_data_file:s0 "${CFG_PATH}" 2>/dev/null`);
  // Never trust errno alone — always verify the write succeeded
  const check = await ksu_exec(`cat "${CFG_PATH}"`);
  return check.stdout.includes('master_seed=');
}

// ─── App state ──────────────────────────────────────────────────────
const state = {
  cfg: {},
  profile: 'Redmi 9',
  seed: 0,
  jitter: true,
  networkType: 'wifi',   // 'wifi' | 'lte'
  seedVersion: 0,
  locationLat: null,
  locationLon: null,
  locationAlt: null,
  proxyEnabled: false,
  proxyType: 'http',
  proxyHost: '',
  proxyPort: '',
  proxyUser: '',
  proxyPass: '',
  webviewSpoof: false,
  scopedApps: [],
  recentProfiles: [],
  // computed — primary identifiers
  imei: '', imei2: '', iccid: '', imsi: '', phone: '', wifiMac: '', btMac: '',
  serial: '', hwSerial: '', androidId: '', ssaid: '',
  gsfId: '', widevineId: '', mediaDrmId: '', advertisingId: '',
  bootId: '', gmailAccount: '', gpuRenderer: '', ja3: null,
  // computed — telephony / network
  wifiSsid: '', wifiBssid: '', mccmnc: '', simOperator: '',
  lat: 0, lon: 0, alt: 0, tz: '',
  carrier: '', simCountry: '', correlation: 0,
  deviceIp: '', deviceModel: '', proxyIp: ''
};

// overrides — per-field manual seeds (each "randomize" just picks a new random seed for that field)
const overrides = {};

// ─── Bootstrap ──────────────────────────────────────────────────────
async function loadState() {
  state.cfg = await readConfig();
  state.profile      = state.cfg.profile      || 'Redmi 9';
  state.seed         = parseInt(state.cfg.master_seed, 10) || generateRandomSeed();
  state.jitter       = (state.cfg.jitter || 'true') !== 'false';
  state.networkType  = (state.cfg.network_type === 'lte' || state.cfg.network_type === 'mobile') ? 'lte' : 'wifi';
  state.seedVersion  = parseInt(state.cfg.seed_version) || 0;
  state.locationLat  = state.cfg.location_lat  ? parseFloat(state.cfg.location_lat)  : null;
  state.locationLon  = state.cfg.location_lon  ? parseFloat(state.cfg.location_lon)  : null;
  state.locationAlt  = state.cfg.location_alt  ? parseFloat(state.cfg.location_alt)  : null;
  state.proxyEnabled = state.cfg.proxy_enabled === 'true';
  state.proxyType    = state.cfg.proxy_type    || 'socks5';
  state.proxyHost    = state.cfg.proxy_host    || '';
  state.proxyPort    = state.cfg.proxy_port    || '';
  state.proxyUser    = state.cfg.proxy_user    || '';
  state.proxyPass    = state.cfg.proxy_pass    || '';
  state.proxyDns     = state.cfg.proxy_dns     || '';
  state.webviewSpoof = state.cfg.webview_spoof === 'true';
  state.scopedApps   = state.cfg.scoped_apps ? state.cfg.scoped_apps.split(',').filter(Boolean) : [];
  state.recentProfiles = loadRecentProfiles();

  // Restore per-field overrides persisted in config
  const ov = state.cfg;
  if (ov.override_imei)           overrides.imei          = ov.override_imei;
  if (ov.override_imei2)          overrides.imei2         = ov.override_imei2;
  if (ov.override_serial)         overrides.serial        = ov.override_serial;
  if (ov.override_hw_serial)      overrides.hwSerial      = ov.override_hw_serial;
  if (ov.override_android_id)     overrides.androidId     = ov.override_android_id;
  if (ov.override_gsf_id)         overrides.gsfId         = ov.override_gsf_id;
  if (ov.override_widevine_id)    overrides.widevineId    = ov.override_widevine_id;
  if (ov.override_media_drm_id)   overrides.mediaDrmId    = ov.override_media_drm_id;
  if (ov.override_advertising_id) overrides.advertisingId = ov.override_advertising_id;
  if (ov.override_boot_id)        overrides.bootId        = ov.override_boot_id;
  if (ov.override_gmail)          overrides.gmailAccount  = ov.override_gmail;
  if (ov.override_wifi_ssid)      overrides.wifiSsid      = ov.override_wifi_ssid;
  if (ov.override_wifi_bssid)     overrides.wifiBssid     = ov.override_wifi_bssid;
  if (ov.override_imsi)           overrides.imsi          = ov.override_imsi;
  if (ov.override_iccid)          overrides.iccid         = ov.override_iccid;
  if (ov.override_phone)          overrides.phone         = ov.override_phone;
  if (ov.override_wifi_mac)       overrides.wifiMac       = ov.override_wifi_mac;
  if (ov.override_bt_mac)         overrides.btMac         = ov.override_bt_mac;
  if (ov.ja3_idx != null)         overrides.ja3Idx        = parseInt(ov.ja3_idx) || 0;

  computeAll();
  // PR-UIFreeze: loadSystemInfo() is now called AFTER the loading screen is
  // removed (see DOMContentLoaded handler) so the UI paints immediately and
  // doesn't block the system while waiting for the ksu bridge.
}

function computeAll() {
  const { profile, seed } = state;
  let fp = getProfileByName(profile);
  // Fallback to Redmi 9 if profile is missing/invalid
  if (!fp) { state.profile = 'Redmi 9'; fp = getProfileByName('Redmi 9'); }
  const brand = (fp.brand || 'xiaomi').toLowerCase();

  state.imei       = overrides.imei       ?? generateIMEI(profile, seed);
  state.imei2      = overrides.imei2      ?? generateIMEI(profile, seed + 1);
  state.iccid      = overrides.iccid      ?? generateICCID(profile, seed);
  state.imsi       = overrides.imsi       ?? generateIMSI(profile, seed);
  state.phone      = overrides.phone      ?? generatePhoneNumber(profile, seed);
  state.wifiMac    = overrides.wifiMac    ?? generateMAC(brand, seed);
  state.btMac      = overrides.btMac      ?? generateMAC(brand, seed + 1);
  state.serial     = overrides.serial     ?? generateSerial(brand, seed, fp.securityPatch || '');
  state.hwSerial   = overrides.hwSerial   ?? generateSerial(brand, seed + 99, fp.securityPatch || '');
  state.androidId  = overrides.androidId  ?? generateAndroidId(seed);
  // Android 8+: SSAID es per-app (cada UID obtiene valor único).
  // En la UI mostramos un valor de referencia con seed diferente.
  // El C++ usa seed ^ (uid * 2654435761) para cada app real.
  state.ssaid      = generateAndroidId(seed ^ 10000);
  state.gsfId      = overrides.gsfId      ?? generateGsfId(seed);
  state.widevineId = overrides.widevineId ?? generateWidevineId(seed);
  state.mediaDrmId = overrides.mediaDrmId ?? generateUUID(seed + 31);
  state.advertisingId = overrides.advertisingId ?? generateUUID(seed + 57);
  state.bootId     = overrides.bootId     ?? generateBootId(seed);
  state.gmailAccount = overrides.gmailAccount ?? generateGmail(seed);
  state.gpuRenderer  = fp.gpuRenderer || 'Adreno 619';
  const _ja3Pool = JA3_PRESETS.filter(p => !p.brands || p.brands.includes(brand));
  state.ja3          = _ja3Pool[(seed + (overrides.ja3Idx || 0)) % _ja3Pool.length];
  state.carrier    = getCarrierName(profile, seed);
  state.simCountry = getSimCountry(profile, seed);
  state.simOperator = state.carrier;
  state.tz         = getTimezone(seed);
  state.wifiSsid   = overrides.wifiSsid   ?? generateWifiSsid(seed);
  state.wifiBssid  = overrides.wifiBssid  ?? generateMAC('default', seed + 7);
  state.mccmnc     = (state.imsi || '').substring(0, 6);

  if (state.locationLat !== null && state.locationLon !== null) {
    state.lat = state.locationLat;
    state.lon = state.locationLon;
    state.alt = state.locationAlt ?? generateAltitude(profile, seed);
  } else {
    const loc = generateLocation(profile, seed);
    state.lat = loc.lat; state.lon = loc.lon;
    state.alt = generateAltitude(profile, seed);
  }

  const corr = computeCorrelation(profile, seed, {
    imei: state.imei, imsi: state.imsi, iccid: state.iccid,
    phone: state.phone, wifiMac: state.wifiMac, serial: state.serial,
    androidId: state.androidId, gsfId: state.gsfId, bootId: state.bootId,
    widevineId: state.widevineId
  });
  state.correlation = corr.score;
  state.corrChecks  = corr.checks;
}

// PR-UIFreeze: Combined 3 sequential ksu_exec calls into a single shell
// command.  This reduces binder round-trips from 3 → 1 and eliminates the
// scenario where each 8 s timeout compounds into a 24 s system-wide freeze.
async function loadSystemInfo() {
  // Build a single shell command that fetches local IP, model, and proxy exit IP
  let cmd =
    "IP=$(ip -4 addr show wlan0 2>/dev/null|grep 'inet '|awk '{print $2}'|cut -d/ -f1|head -1);" +
    "[ -z \"$IP\" ]&&IP=$(ip -4 addr show rmnet0 2>/dev/null|grep 'inet '|awk '{print $2}'|cut -d/ -f1|head -1);" +
    "MODEL=$(getprop ro.product.model 2>/dev/null);" +
    "PROXYIP=;";

  // If proxy is enabled, query public exit IP through the SOCKS5 tunnel
  if (state.proxyEnabled && state.proxyHost && state.proxyPort) {
    const auth = state.proxyUser
      ? `${state.proxyUser}:${state.proxyPass || ''}@`
      : '';
    const proxyUrl = `socks5h://${auth}${state.proxyHost}:${state.proxyPort}`;
    // curl through proxy with short timeout; try ifconfig.me then api.ipify.org
    cmd += `PROXYIP=$(curl -s -m 5 -x '${proxyUrl}' https://api.ipify.org 2>/dev/null);` +
           `[ -z "$PROXYIP" ]&&PROXYIP=$(curl -s -m 5 -x '${proxyUrl}' https://ifconfig.me 2>/dev/null);`;
  }

  cmd += "echo \"${IP}||${MODEL}||${PROXYIP}\"";

  const { stdout } = await ksu_exec(cmd);
  const parts = (stdout || '').split('||');
  state.deviceIp    = parts[0] || '–';
  state.deviceModel = (parts[1] || '').trim() || state.profile;
  state.proxyIp     = (parts[2] || '').trim() || '';
}

// ─── Persist helpers ────────────────────────────────────────────────
async function saveConfig() {
  const cfg = {
    profile:       state.profile,
    master_seed:   String(state.seed),
    jitter:        String(state.jitter),
    network_type:  state.networkType,
    seed_version:  String(state.seedVersion),
  };
  if (state.locationLat !== null) {
    cfg.location_lat = state.lat.toFixed(6);
    cfg.location_lon = state.lon.toFixed(6);
    cfg.location_alt = state.alt.toFixed(1);
  }
  cfg.proxy_enabled = String(state.proxyEnabled);
  cfg.proxy_type    = state.proxyType;
  cfg.proxy_host    = state.proxyHost;
  cfg.proxy_port    = state.proxyPort;
  cfg.proxy_user    = state.proxyUser;
  cfg.proxy_pass    = state.proxyPass;
  cfg.proxy_dns     = state.proxyDns;
  cfg.webview_spoof = String(state.webviewSpoof);
  if (state.scopedApps.length) cfg.scoped_apps = state.scopedApps.join(',');
  // Persist per-field overrides so values survive app restarts
  if (overrides.imei)          cfg.override_imei           = overrides.imei;
  if (overrides.imei2)         cfg.override_imei2          = overrides.imei2;
  if (overrides.serial)        cfg.override_serial         = overrides.serial;
  if (overrides.hwSerial)      cfg.override_hw_serial      = overrides.hwSerial;
  if (overrides.androidId)     cfg.override_android_id     = overrides.androidId;
  if (overrides.gsfId)         cfg.override_gsf_id         = overrides.gsfId;
  if (overrides.widevineId)    cfg.override_widevine_id    = overrides.widevineId;
  if (overrides.mediaDrmId)    cfg.override_media_drm_id   = overrides.mediaDrmId;
  if (overrides.advertisingId) cfg.override_advertising_id = overrides.advertisingId;
  if (overrides.bootId)        cfg.override_boot_id        = overrides.bootId;
  if (overrides.gmailAccount)  cfg.override_gmail          = overrides.gmailAccount;
  if (overrides.wifiSsid)      cfg.override_wifi_ssid      = overrides.wifiSsid;
  if (overrides.wifiBssid)     cfg.override_wifi_bssid     = overrides.wifiBssid;
  if (overrides.imsi)          cfg.override_imsi           = overrides.imsi;
  if (overrides.iccid)         cfg.override_iccid          = overrides.iccid;
  if (overrides.phone)         cfg.override_phone          = overrides.phone;
  if (overrides.wifiMac)       cfg.override_wifi_mac       = overrides.wifiMac;
  if (overrides.btMac)         cfg.override_bt_mac         = overrides.btMac;
  if (overrides.ja3Idx != null) cfg.ja3_idx               = String(overrides.ja3Idx);
  return await writeConfig(cfg);
}

function generateRandomSeed() {
  return Math.floor(Math.random() * 2000000000) + 1;
}

function rotateSeed() {
  state.seed = generateRandomSeed();
  state.seedVersion++;
  delete Object.assign(overrides, {}); // clear all overrides
  for (const k in overrides) delete overrides[k];
  computeAll();
}

// ─── Recent profiles ────────────────────────────────────────────────
function loadRecentProfiles() {
  try { return JSON.parse(localStorage.getItem('recentProfiles') || '[]'); } catch { return []; }
}
function addRecentProfile(name) {
  let r = loadRecentProfiles().filter(p => p !== name);
  r.unshift(name);
  r = r.slice(0, 5);
  localStorage.setItem('recentProfiles', JSON.stringify(r));
  state.recentProfiles = r;
}

// ─── Toast ──────────────────────────────────────────────────────────
function toast(msg, type='ok', duration=2400) {
  const icons = {
    ok:   `<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5"><path d="M20 6L9 17l-5-5"/></svg>`,
    err:  `<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5"><circle cx="12" cy="12" r="10"/><line x1="15" y1="9" x2="9" y2="15"/><line x1="9" y1="9" x2="15" y2="15"/></svg>`,
    warn: `<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5"><path d="M10.29 3.86L1.82 18a2 2 0 001.71 3h16.94a2 2 0 001.71-3L13.71 3.86a2 2 0 00-3.42 0z"/></svg>`,
    info: `<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5"><circle cx="12" cy="12" r="10"/><line x1="12" y1="16" x2="12" y2="12"/><line x1="12" y1="8" x2="12.01" y2="8"/></svg>`
  };
  const el = document.createElement('div');
  el.className = `toast ${type}`;
  el.innerHTML = `${icons[type]||icons.info} ${msg}`;
  document.getElementById('toast-container').appendChild(el);
  setTimeout(() => { el.style.opacity = '0'; setTimeout(() => el.remove(), 300); }, duration);
}

// ─── Dialog ──────────────────────────────────────────────────────────
function showDialog(title, body, actions) {
  return new Promise(resolve => {
    const ov = document.getElementById('dialog-overlay');
    const dl = document.getElementById('dialog');
    document.getElementById('dialog-title').textContent = title;
    document.getElementById('dialog-body').innerHTML = body;
    const ac = document.getElementById('dialog-actions');
    ac.innerHTML = '';
    actions.forEach(({label, cls, value}) => {
      const b = document.createElement('button');
      b.className = `btn ${cls}`;
      b.textContent = label;
      b.onclick = () => { ov.classList.remove('open'); resolve(value); };
      ac.appendChild(b);
    });
    ov.classList.add('open');
  });
}

// ─── Navigation ──────────────────────────────────────────────────────
let currentPage = 'status';
function navigate(page) {
  document.querySelectorAll('.page').forEach(p => p.classList.remove('active'));
  document.querySelectorAll('.nav-item').forEach(n => n.classList.remove('active'));
  document.getElementById('page-' + page)?.classList.add('active');
  document.querySelector(`[data-nav="${page}"]`)?.classList.add('active');
  currentPage = page;
  renderPage(page);
}

function renderPage(page) {
  if (page === 'status')    renderStatus();
  if (page === 'identity')  renderIdentity();
  if (page === 'network')   renderNetwork();
  if (page === 'advanced')  renderAdvanced();
}

// ─── Status page ─────────────────────────────────────────────────────
function renderStatus() {
  updateGauge(state.correlation);
  setCell('status-device', state.profile);
  setCell('status-model', state.deviceModel);
  // Show proxy exit IP when proxy is active, otherwise local IP
  if (state.proxyEnabled && state.proxyIp) {
    setCell('status-ip', state.proxyIp);
    setCell('status-ip-label', 'Proxy IP');
  } else {
    setCell('status-ip', state.deviceIp);
    setCell('status-ip-label', 'IP Address');
  }
  setCell('status-simcountry', state.simCountry);
  setCell('status-phone', state.phone);
  setCell('status-carrier', state.carrier);
  setCell('status-location', `${state.lat.toFixed(4)}, ${state.lon.toFixed(4)}`);
  setCell('status-tz', state.tz);
  setCell('status-network', state.networkType.toUpperCase());
  setCell('status-seed', state.seed.toString());
  setCell('status-corr-label', `${state.correlation}% coherence`);
  renderCorrBreakdown();
}

function renderCorrBreakdown() {
  const el = document.getElementById('corr-breakdown');
  if (!el || !state.corrChecks) return;
  el.innerHTML = state.corrChecks.map(c =>
    `<div class="corr-row"><span class="corr-icon">${c.passed ? '\u2713' : '\u2717'}</span><span class="corr-name">${escHtml(c.name)}</span><span class="corr-wt${c.passed ? '' : ' fail'}">${c.passed ? '+' : '\u2212'}${c.weight}</span></div>`
  ).join('');
}

function updateGauge(score) {
  const R = 54, C = 2 * Math.PI * R;
  const el = document.getElementById('gauge-fill');
  if (!el) return;
  const pct = score / 100;
  el.style.strokeDasharray = C;
  el.style.strokeDashoffset = C * (1 - pct);
  // Color: red 0-40, amber 40-70, green 70-100
  el.style.stroke = score >= 70 ? '#34d399' : score >= 40 ? '#fbbf24' : '#f87171';
  const vEl = document.getElementById('gauge-value');
  if (vEl) { vEl.textContent = score; vEl.style.color = el.style.stroke; }
}

function setCell(id, val) {
  const el = document.getElementById(id);
  if (el) el.textContent = val || '–';
}

// ─── Identity page ────────────────────────────────────────────────────
let identityTab = 'device';
function renderIdentity() {
  if (identityTab === 'device') renderDeviceTab();
  else renderIdsTab();
}

function renderDeviceTab() {
  const list = document.getElementById('profile-list');
  if (!list) return;
  const q = document.getElementById('profile-search')?.value?.toLowerCase() || '';
  const names = PROFILE_NAMES.filter(n =>
    !q || n.toLowerCase().includes(q) ||
    (DEVICE_PROFILES[n]?.brand||'').toLowerCase().includes(q) ||
    (DEVICE_PROFILES[n]?.gpuRenderer||'').toLowerCase().includes(q)
  );
  list.innerHTML = names.map(n => {
    const fp = DEVICE_PROFILES[n];
    const sel = n === state.profile;
    return `<div class="profile-item${sel?' selected':''}" data-name="${escHtml(n)}">
      <span style="font-size:18px">${fp.icon||'📱'}</span>
      <div>
        <div class="profile-name">${escHtml(n)}</div>
        <div class="profile-brand">${escHtml(fp.manufacturer)} · ${escHtml(fp.gpuRenderer||'')} · Android ${fp.release}</div>
        <div class="profile-chip">${fp.screenWidth}×${fp.screenHeight} · ${fp.coreCount}c/${fp.ramGb}GB</div>
      </div>
      ${sel?`<svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="var(--primary)" stroke-width="3"><path d="M20 6L9 17l-5-5"/></svg>`:''}
    </div>`;
  }).join('');

  // Recent profiles
  const rec = document.getElementById('recent-list');
  if (rec) {
    rec.innerHTML = state.recentProfiles.slice(0,5).map(n => {
      const fp = DEVICE_PROFILES[n];
      if (!fp) return '';
      return `<div class="recent-profile" data-recent="${escHtml(n)}">
        <span>${fp.icon||'📱'}</span>
        <div><div style="font-size:13px;font-weight:600">${escHtml(n)}</div><div class="text-sm">${escHtml(fp.manufacturer)}</div></div>
      </div>`;
    }).filter(Boolean).join('');
  }

  // Current profile card
  const fp = getProfileByName(state.profile);
  setProfileCard(fp, state.profile);
}

function setProfileCard(fp, name) {
  const el = document.getElementById('current-profile-card');
  if (!el) return;
  el.innerHTML = `
    <div style="display:flex;align-items:center;gap:12px;margin-bottom:12px">
      <span style="font-size:32px">${fp.icon||'📱'}</span>
      <div>
        <div style="font-size:15px;font-weight:700;color:var(--text)">${escHtml(name)}</div>
        <div style="font-size:12px;color:var(--text2)">${escHtml(fp.manufacturer)} · ${escHtml(fp.brand)}</div>
      </div>
    </div>
    <div style="display:grid;grid-template-columns:1fr 1fr;gap:8px">
      ${[
        ['Model',    fp.model],
        ['Android',  fp.release],
        ['CPU',      fp.boardPlatform],
        ['GPU',      fp.gpuRenderer],
        ['Screen',   `${fp.screenWidth}×${fp.screenHeight}`],
        ['Cores/RAM',`${fp.coreCount}c / ${fp.ramGb}GB`],
        ['Security', fp.securityPatch],
        ['Build ID', fp.buildId],
      ].map(([l,v])=>`<div class="info-cell"><div class="info-cell-label">${l}</div><div class="info-cell-value">${escHtml(v||'')}</div></div>`).join('')}
    </div>`;
}

function renderIdsTab() {
  renderField('f-imei',       state.imei,        validateIMEI(state.imei));
  renderField('f-imei2',      state.imei2,       validateIMEI(state.imei2));
  renderField('f-serial',     state.serial,      validateSerial(state.serial));
  renderField('f-hw-serial',  state.hwSerial,    validateSerial(state.hwSerial));
  renderField('f-android-id', state.androidId,   validateAndroidId(state.androidId));
  renderField('f-ssaid',      state.ssaid,       validateAndroidId(state.ssaid));
  renderField('f-gsf-id',     state.gsfId,       validateGsfId(state.gsfId));
  renderField('f-widevine',   state.widevineId,  validateWidevineId(state.widevineId));
  renderField('f-media-drm',  state.mediaDrmId,  validateBootId(state.mediaDrmId));
  renderField('f-adv-id',     state.advertisingId, validateBootId(state.advertisingId));
  renderField('f-boot-id',    state.bootId,      validateBootId(state.bootId));
  renderField('f-gmail',      state.gmailAccount, /^[a-z][a-z0-9.]+@gmail\.com$/.test(state.gmailAccount));
  renderField('f-gpu',        state.gpuRenderer, !!state.gpuRenderer);
  renderField('f-fingerprint', getProfileByName(state.profile).fingerprint || '', true);
  // JA3/TLS — dual-line display (name + hash)
  const ja3 = state.ja3 || JA3_PRESETS[0];
  const ja3El   = document.getElementById('f-ja3');
  const ja3Name = document.getElementById('f-ja3-name');
  if (ja3El)   ja3El.textContent   = ja3.hash;
  if (ja3Name) ja3Name.textContent = ja3.name;
  const ja3Badge = document.getElementById('f-ja3-badge');
  if (ja3Badge) { ja3Badge.className = 'valid-badge ok'; ja3Badge.textContent = 'VALID'; }
}

// ─── Network page ─────────────────────────────────────────────────────
let networkTab = 'telephony';
function renderNetwork() {
  if (networkTab === 'telephony') renderTelephonyTab();
  else renderLocationTab();
}

function renderTelephonyTab() {
  renderField('f-imsi',       state.imsi,       validateIMSI(state.imsi));
  renderField('f-iccid',      state.iccid,      validateICCID(state.iccid));
  renderField('f-phone',      state.phone,      validatePhone(state.phone));
  renderField('f-carrier',    state.carrier,    !!state.carrier);
  renderField('f-sim-cc',     state.simCountry, state.simCountry === 'US');
  renderField('f-sim-op',     state.simOperator, !!state.simOperator);
  renderField('f-mccmnc',     state.mccmnc,     /^\d{5,6}$/.test(state.mccmnc));
  renderField('f-wifi-mac',   state.wifiMac,    validateMAC(state.wifiMac));
  renderField('f-bt-mac',     state.btMac,      validateMAC(state.btMac));
  renderField('f-wifi-ssid',  state.wifiSsid,   !!state.wifiSsid);
  renderField('f-wifi-bssid', state.wifiBssid,  validateMAC(state.wifiBssid));

  // Network type toggle
  const lteToggle = document.getElementById('toggle-lte');
  if (lteToggle) lteToggle.checked = state.networkType === 'lte';
  const netLabel = document.getElementById('net-type-label');
  if (netLabel) netLabel.textContent = state.networkType === 'lte' ? 'Spoofing as LTE (Mobile)' : 'Showing real connection type';
}

function renderLocationTab() {
  document.getElementById('loc-lat') && (document.getElementById('loc-lat').textContent = state.lat.toFixed(6));
  document.getElementById('loc-lon') && (document.getElementById('loc-lon').textContent = state.lon.toFixed(6));
  document.getElementById('loc-alt') && (document.getElementById('loc-alt').textContent = state.alt.toFixed(1) + ' m');
  document.getElementById('loc-tz')  && (document.getElementById('loc-tz').textContent  = state.tz);
  // Sensor values from profile
  const fp = getProfileByName(state.profile);
  // Safe fixed-point helper — shows '–' if value is null/undefined
  const sfx = (v, d, unit='') => (v != null ? v.toFixed(d) + unit : '–');
  const setEl = (id, v) => { const e = document.getElementById(id); if (e) e.textContent = v; };
  setEl('loc-accel-max', sfx(fp.accelMaxRange,  4, ' m/s²'));
  setEl('loc-accel-res', sfx(fp.accelResolution, 7));
  setEl('loc-gyro-max',  sfx(fp.gyroMaxRange,   6, ' rad/s'));
  setEl('loc-gyro-res',  sfx(fp.gyroResolution, 6));
  setEl('loc-mag-max',   sfx(fp.magMaxRange,    1, ' µT'));
  setEl('loc-heart',     fp.hasHeartRate  ? '✓ Present' : '✗ Absent');
  setEl('loc-baro',      fp.hasBarometer  ? '✓ Present' : '✗ Absent');
  setEl('loc-fp-wake',   fp.hasFingerprintWakeup ? '✓ Present' : '✗ Absent');

  if (window._map) {
    window._map.setView([state.lat, state.lon], 11);
    if (window._mapMarker) window._mapMarker.setLatLng([state.lat, state.lon]);
    else {
      window._mapMarker = L.marker([state.lat, state.lon], {
        icon: L.divIcon({ className:'', html:'<div style="width:18px;height:18px;border-radius:50%;background:var(--primary);border:3px solid white;box-shadow:0 2px 8px rgba(0,0,0,.5)"></div>', iconSize:[18,18], iconAnchor:[9,9] })
      }).addTo(window._map);
    }
  }
}

// ─── Advanced page ────────────────────────────────────────────────────
let advancedTab = 'proxy';
function renderAdvanced() {
  if (advancedTab === 'proxy') renderProxyTab();
  else renderSettingsTab();
}

function renderProxyTab() {
  const toggle = document.getElementById('proxy-enabled');
  if (toggle) toggle.checked = state.proxyEnabled;
  const typeSelect = document.getElementById('proxy-type');
  if (typeSelect) typeSelect.value = state.proxyType;
  const host = document.getElementById('proxy-host');
  if (host) host.value = state.proxyHost;
  const port = document.getElementById('proxy-port');
  if (port) port.value = state.proxyPort;
  const user = document.getElementById('proxy-user');
  if (user) user.value = state.proxyUser;
  const pass = document.getElementById('proxy-pass');
  if (pass) pass.value = state.proxyPass;
  const dns = document.getElementById('proxy-dns');
  if (dns) dns.value = state.proxyDns;
  checkProxyStatus();
}

function renderSettingsTab() {
  // PR71g: Sync WebView spoof toggle
  const wvToggle = document.getElementById('toggle-webview-spoof');
  if (wvToggle) wvToggle.checked = state.webviewSpoof;
  renderScopedApps();
}

function renderScopedApps() {
  const list = document.getElementById('scoped-apps-list');
  if (!list) return;
  if (!state.scopedApps.length) {
    list.innerHTML = '<div class="text-sm" style="padding:12px 0;text-align:center">No apps in scope. Add apps above.</div>';
    return;
  }
  const appSvg = '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" style="width:14px;height:14px"><rect x="3" y="3" width="18" height="18" rx="3"/><line x1="9" y1="3" x2="9" y2="21"/><line x1="15" y1="3" x2="15" y2="21"/><line x1="3" y1="9" x2="21" y2="9"/><line x1="3" y1="15" x2="21" y2="15"/></svg>';
  list.innerHTML = state.scopedApps.map(pkg => {
    const isSystem = _systemApps.has(pkg);
    const cls = isSystem ? 'system' : 'user';
    return `<div class="scope-app-item">
      <div class="scope-app-icon ${cls}">${appSvg}</div>
      <div class="scope-app-name">${escHtml(pkg)}</div>
      <div class="scope-app-actions">
        <button class="btn btn-warning btn-icon" onclick="forcestopApp('${escAttr(pkg)}')" title="Force Stop">
          <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><rect x="6" y="6" width="12" height="12"/></svg>
        </button>
        <button class="btn btn-danger btn-icon" onclick="wipeApp('${escAttr(pkg)}')" title="Wipe Data">
          <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><polyline points="3 6 5 6 21 6"/><path d="M19 6v14a2 2 0 01-2 2H7a2 2 0 01-2-2V6m3 0V4a1 1 0 011-1h4a1 1 0 011 1v2"/></svg>
        </button>
        <button class="btn btn-secondary btn-icon" onclick="removeScopedApp('${escAttr(pkg)}')" title="Remove from scope">
          <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><line x1="18" y1="6" x2="6" y2="18"/><line x1="6" y1="6" x2="18" y2="18"/></svg>
        </button>
      </div>
    </div>`;
  }).join('');
  refreshPickerAdded();
}

// ─── Field render helper ──────────────────────────────────────────────
function renderField(id, value, valid) {
  const el = document.getElementById(id);
  if (!el) return;
  el.textContent = value || '–';
  const badge = document.getElementById(id + '-badge');
  if (badge) {
    badge.className = `valid-badge ${valid ? 'ok' : 'err'}`;
    badge.textContent = valid ? 'VALID' : 'INVALID';
  }
}

// ─── Event handlers: Identity/Device ─────────────────────────────────
window.selectProfile = function(name) {
  if (!DEVICE_PROFILES[name]) return;
  if (state.profile !== name) {
    // Preserve current identity values as overrides so correlation
    // correctly shows mismatches against the new profile.
    // User can then regenerate individual fields or use "Randomize All".
    overrides.imei          = state.imei;
    overrides.imei2         = state.imei2;
    overrides.iccid         = state.iccid;
    overrides.imsi          = state.imsi;
    overrides.phone         = state.phone;
    overrides.wifiMac       = state.wifiMac;
    overrides.btMac         = state.btMac;
    overrides.serial        = state.serial;
    overrides.hwSerial      = state.hwSerial;
    overrides.androidId     = state.androidId;
    overrides.gsfId         = state.gsfId;
    overrides.widevineId    = state.widevineId;
    overrides.bootId        = state.bootId;
    overrides.gmailAccount  = state.gmailAccount;
    overrides.wifiSsid      = state.wifiSsid;
    overrides.wifiBssid     = state.wifiBssid;
  }
  state.profile = name;
  computeAll();
  addRecentProfile(name);
  renderDeviceTab();
  toast(`Profile set to ${name}`);
};

window.randomProfile = function() {
  const n = PROFILE_NAMES[Math.floor(Math.random() * PROFILE_NAMES.length)];
  selectProfile(n);
};

async function smartApply(label) {
  if (!await saveConfig()) { toast('Save failed — check permissions', 'err'); return; }
  const apps = state.scopedApps || [];
  for (const pkg of apps) {
    await ksu_exec(`am force-stop ${pkg}`);
  }
  if (apps.length > 0)
    toast(`${label} saved — ${apps.length} app(s) will reload with new profile`);
  else
    toast(`${label} saved`);
}

window.applyDevice = async function() {
  await smartApply('Device profile');
};

window.randomizeAllIds = function() {
  rotateSeed();
  renderIdsTab();
  toast('All IDs randomized');
};

window.applyIds = async function() {
  await smartApply('Identity changes');
};

// Per-field randomize (changes only that field via its sub-seed)
window.randomizeField = function(field) {
  const newSubSeed = Math.floor(Math.random() * 2000000000) + 1;
  const fp = getProfileByName(state.profile);
  const brand = (fp.brand || 'xiaomi').toLowerCase();
  const actions = {
    'f-imei':       () => { overrides.imei          = generateIMEI(state.profile, newSubSeed); state.imei = overrides.imei; },
    'f-imei2':      () => { overrides.imei2         = generateIMEI(state.profile, newSubSeed); state.imei2 = overrides.imei2; },
    'f-serial':     () => { overrides.serial        = generateSerial(brand, newSubSeed, fp.securityPatch||''); state.serial = overrides.serial; },
    'f-hw-serial':  () => { overrides.hwSerial      = generateSerial(brand, newSubSeed+1, fp.securityPatch||''); state.hwSerial = overrides.hwSerial; },
    'f-android-id': () => { overrides.androidId     = generateAndroidId(newSubSeed); state.androidId = overrides.androidId; state.ssaid = generateAndroidId(newSubSeed ^ 10000); },
    'f-gsf-id':     () => { overrides.gsfId         = generateGsfId(newSubSeed); state.gsfId = overrides.gsfId; },
    'f-widevine':   () => { overrides.widevineId    = generateWidevineId(newSubSeed); state.widevineId = overrides.widevineId; },
    'f-media-drm':  () => { overrides.mediaDrmId    = generateUUID(newSubSeed); state.mediaDrmId = overrides.mediaDrmId; },
    'f-adv-id':     () => { overrides.advertisingId = generateUUID(newSubSeed+1); state.advertisingId = overrides.advertisingId; },
    'f-boot-id':    () => { overrides.bootId        = generateBootId(newSubSeed); state.bootId = overrides.bootId; },
    'f-gmail':      () => { overrides.gmailAccount  = generateGmail(newSubSeed); state.gmailAccount = overrides.gmailAccount; },
    'f-ja3':        () => { overrides.ja3Idx = Math.floor(Math.random() * JA3_PRESETS.length); state.ja3 = JA3_PRESETS[overrides.ja3Idx]; },
    'f-imsi':       () => { overrides.imsi          = generateIMSI(state.profile, newSubSeed); state.imsi = overrides.imsi; state.mccmnc = state.imsi.substring(0,6); state.simOperator = state.carrier; },
    'f-iccid':      () => { overrides.iccid         = generateICCID(state.profile, newSubSeed); state.iccid = overrides.iccid; },
    'f-phone':      () => { overrides.phone         = generatePhoneNumber(state.profile, newSubSeed); state.phone = overrides.phone; },
    'f-wifi-mac':   () => { overrides.wifiMac       = generateMAC(brand, newSubSeed); state.wifiMac = overrides.wifiMac; },
    'f-bt-mac':     () => { overrides.btMac         = generateMAC(brand, newSubSeed+1); state.btMac = overrides.btMac; },
    'f-wifi-ssid':  () => { overrides.wifiSsid      = generateWifiSsid(newSubSeed); state.wifiSsid = overrides.wifiSsid; },
    'f-wifi-bssid': () => { overrides.wifiBssid     = generateMAC('default', newSubSeed+7); state.wifiBssid = overrides.wifiBssid; },
  };
  if (actions[field]) {
    actions[field]();
    const corr = computeCorrelation(state.profile, state.seed, {
      imei: state.imei, imsi: state.imsi, iccid: state.iccid, phone: state.phone,
      wifiMac: state.wifiMac, serial: state.serial, androidId: state.androidId,
      gsfId: state.gsfId, bootId: state.bootId, widevineId: state.widevineId
    });
    state.correlation = corr.score;
    state.corrChecks  = corr.checks;
    renderPage(currentPage);
  }
};

// ─── Inline field editing (WiFi SSID, Gmail) ─────────────────────────
// Converts the field-value div into an input for manual text entry.
// On Enter or blur, saves the value as an override and re-renders.
window.editField = function(fieldId) {
  const el = document.getElementById(fieldId);
  if (!el || el.tagName === 'INPUT') return;

  const currentValue = el.textContent === '–' ? '' : el.textContent;
  const input = document.createElement('input');
  input.type = 'text';
  input.value = currentValue;
  input.className = 'field-value editable';
  input.id = fieldId;

  const commitEdit = () => {
    const val = input.value.trim();
    if (!val) return;  // don't accept empty

    const setters = {
      'f-gmail':     v => { overrides.gmailAccount = v; state.gmailAccount = v; },
      'f-wifi-ssid': v => { overrides.wifiSsid = v; state.wifiSsid = v; },
    };
    if (setters[fieldId]) setters[fieldId](val);

    const corr = computeCorrelation(state.profile, state.seed, {
      imei: state.imei, imsi: state.imsi, iccid: state.iccid, phone: state.phone,
      wifiMac: state.wifiMac, serial: state.serial, androidId: state.androidId,
      gsfId: state.gsfId, bootId: state.bootId, widevineId: state.widevineId
    });
    state.correlation = corr.score;
    state.corrChecks  = corr.checks;
    renderPage(currentPage);
  };

  input.addEventListener('keydown', e => {
    if (e.key === 'Enter')  { e.preventDefault(); commitEdit(); }
    if (e.key === 'Escape') { renderPage(currentPage); }
  });
  input.addEventListener('blur', commitEdit);

  el.replaceWith(input);
  input.focus();
  input.select();
};

// ─── Event handlers: Network/Telephony ───────────────────────────────
window.randomizeAllTelephony = function() {
  rotateSeed();
  renderTelephonyTab();
  toast('All telephony IDs randomized');
};

window.applyTelephony = async function() {
  await smartApply('Network changes');
};

window.toggleNetworkType = function(checked) {
  state.networkType = checked ? 'lte' : 'wifi';
  renderTelephonyTab();
};

// ─── Event handlers: Location ─────────────────────────────────────────
window.randomLocation = function() {
  const city = US_CITIES_100[Math.floor(Math.random() * US_CITIES_100.length)];
  // Add small jitter ±0.01°
  state.lat = city.lat + (Math.random() - 0.5) * 0.02;
  state.lon = city.lon + (Math.random() - 0.5) * 0.02;
  state.locationLat = state.lat;
  state.locationLon = state.lon;
  state.alt = Math.floor(Math.random() * 200) + 5;
  state.locationAlt = state.alt;
  renderLocationTab();
  toast(`Location → ${city.name}`);
};

window.applyLocation = async function() {
  await smartApply('Location changes');
};

// ─── Map init ──────────────────────────────────────────────────────────
function initMap() {
  if (!document.getElementById('map') || window._map) return;
  if (typeof L === 'undefined') return;
  const map = L.map('map', { zoomControl:true, attributionControl:false });
  L.tileLayer('https://{s}.tile.openstreetmap.org/{z}/{x}/{y}.png', {
    maxZoom:18, attribution:'© OpenStreetMap contributors'
  }).addTo(map);
  map.setView([state.lat, state.lon], 11);
  window._mapMarker = L.marker([state.lat, state.lon], {
    icon: L.divIcon({ className:'', html:'<div style="width:18px;height:18px;border-radius:50%;background:var(--primary);border:3px solid white;box-shadow:0 2px 8px rgba(0,0,0,.5)"></div>', iconSize:[18,18], iconAnchor:[9,9] })
  }).addTo(map);
  map.on('click', e => {
    const {lat, lng} = e.latlng;
    state.lat = lat; state.lon = lng;
    state.locationLat = lat; state.locationLon = lng;
    if (window._mapMarker) window._mapMarker.setLatLng([lat, lng]);
    document.getElementById('loc-lat').textContent = lat.toFixed(6);
    document.getElementById('loc-lon').textContent = lng.toFixed(6);
    toast(`Coordinates set: ${lat.toFixed(4)}, ${lng.toFixed(4)}`,'info');
  });
  window._map = map;
}

// ─── Event handlers: Advanced/Proxy ───────────────────────────────────
window.togglePassVisibility = function() {
  const input = document.getElementById('proxy-pass');
  const icon = document.getElementById('pass-eye-icon');
  if (!input) return;
  const show = input.type === 'password';
  input.type = show ? 'text' : 'password';
  // Swap icon: open eye → slashed eye
  icon.innerHTML = show
    ? '<path d="M17.94 17.94A10.07 10.07 0 0 1 12 20c-7 0-11-8-11-8a18.45 18.45 0 0 1 5.06-5.94M9.9 4.24A9.12 9.12 0 0 1 12 4c7 0 11 8 11 8a18.5 18.5 0 0 1-2.16 3.19m-6.72-1.07a3 3 0 1 1-4.24-4.24"/><line x1="1" y1="1" x2="23" y2="23"/>'
    : '<path d="M1 12s4-8 11-8 11 8 11 8-4 8-11 8-11-8-11-8z"/><circle cx="12" cy="12" r="3"/>';
};

window.applyProxy = async function() {
  state.proxyEnabled = document.getElementById('proxy-enabled')?.checked || false;
  state.proxyType = document.getElementById('proxy-type')?.value || 'socks5';
  state.proxyHost = document.getElementById('proxy-host')?.value || '';
  state.proxyPort = document.getElementById('proxy-port')?.value || '';
  state.proxyUser = document.getElementById('proxy-user')?.value || '';
  state.proxyPass = document.getElementById('proxy-pass')?.value || '';
  state.proxyDns  = document.getElementById('proxy-dns')?.value || '';
  if (state.proxyEnabled && !state.proxyHost) { toast('Enter a proxy host', 'warn'); return; }
  if (state.proxyEnabled && !state.proxyPort) { toast('Enter a proxy port', 'warn'); return; }
  if (state.proxyEnabled && state.proxyType !== 'socks5') { toast('Only SOCKS5 proxies are supported', 'warn'); return; }

  if (!await saveConfig()) { toast('Save failed', 'err'); return; }

  const btn = document.querySelector('#adv-tab-proxy .btn-primary');
  const badge = document.getElementById('proxy-status-badge');

  if (state.proxyEnabled) {
    if (btn) btn.disabled = true;
    toast('Starting proxy tunnel...', 'info');
    try {
      const r = await ksu_exec('sh /data/adb/modules/omnishield/proxy_manager.sh start 2>&1');
      if (r.errno !== 0) {
        toast('Proxy failed: ' + (r.stderr || r.stdout || 'unknown error'), 'err');
        if (badge) { badge.textContent = 'ERROR'; badge.className = 'proxy-badge proxy-badge-err'; }
        // Revert config so toggle reflects reality on next load
        state.proxyEnabled = false;
        const toggle = document.getElementById('proxy-enabled');
        if (toggle) toggle.checked = false;
        await saveConfig();
        return;
      }
      // Force-stop scoped apps so they restart inside the tunnel
      const apps = state.scopedApps || [];
      for (const pkg of apps) {
        await ksu_exec(`am force-stop ${pkg}`);
      }
      if (badge) { badge.textContent = 'ACTIVE'; badge.className = 'proxy-badge proxy-badge-on'; }
      toast(`Proxy active — ${apps.length} app(s) will reload through tunnel`);
    } finally {
      if (btn) btn.disabled = false;
    }
  } else {
    await ksu_exec('sh /data/adb/modules/omnishield/proxy_manager.sh stop');
    if (badge) { badge.textContent = 'OFF'; badge.className = 'proxy-badge proxy-badge-off'; }
    toast('Proxy stopped');
  }
};

// PR72: Check proxy status on page load
async function checkProxyStatus() {
  const badge = document.getElementById('proxy-status-badge');
  if (!badge) return;
  try {
    const r = await ksu_exec('sh /data/adb/modules/omnishield/proxy_manager.sh status');
    if (r.stdout && r.stdout.trim() === 'running') {
      badge.textContent = 'ACTIVE'; badge.className = 'proxy-badge proxy-badge-on';
    } else {
      badge.textContent = 'OFF'; badge.className = 'proxy-badge proxy-badge-off';
    }
  } catch(e) {
    badge.textContent = 'OFF'; badge.className = 'proxy-badge proxy-badge-off';
  }
}

// ─── App scope management ──────────────────────────────────────────────
let _userApps = [];
let _systemApps = new Set();
let _appsLoaded = false;

window.addScopedApp = async function() {
  const input = document.getElementById('app-search-input');
  const pkg = input?.value?.trim() || '';
  if (!pkg) { toast('Enter a package name', 'warn'); return; }
  if (state.scopedApps.includes(pkg)) { toast('App already in scope', 'warn'); return; }
  state.scopedApps.push(pkg);
  if (input) input.value = '';
  renderScopedApps();
  if (await saveConfig()) toast(`Added ${pkg} — force stop the app to apply`);
  else toast('Failed to save scope — check permissions', 'err');
};

window.addScopedAppFromPicker = async function(pkg) {
  if (!pkg || state.scopedApps.includes(pkg)) return;
  state.scopedApps.push(pkg);
  renderScopedApps();
  if (await saveConfig()) toast(`Added ${pkg} — force stop the app to apply`);
  else toast('Failed to save scope — check permissions', 'err');
};

window.removeScopedApp = async function(pkg) {
  state.scopedApps = state.scopedApps.filter(p => p !== pkg);
  renderScopedApps();
  if (await saveConfig()) toast(`Removed ${pkg}`);
  else toast('Failed to save scope', 'err');
};

window.forcestopApp = async function(pkg) {
  const ok = await showDialog('Force Stop', `Force stop <b>${escHtml(pkg)}</b>?<br><span class="text-sm">The app will close immediately.</span>`,
    [{label:'Cancel',cls:'btn-secondary',value:false},{label:'Force Stop',cls:'btn-warning',value:true}]);
  if (!ok) return;
  const r = await ksu_exec(`am force-stop ${pkg}`);
  toast(r.errno === 0 ? `${pkg} stopped` : 'Force stop failed', r.errno === 0 ? 'ok' : 'err');
};

window.wipeApp = async function(pkg) {
  const ok = await showDialog('Wipe Data', `<span style="color:var(--error)">Permanently wipe ALL data</span> for <b>${escHtml(pkg)}</b>?<br><span class="text-sm">This cannot be undone.</span>`,
    [{label:'Cancel',cls:'btn-secondary',value:false},{label:'Wipe Data',cls:'btn-danger',value:true}]);
  if (!ok) return;
  const r = await ksu_exec(`pm clear ${pkg}`);
  toast(r.errno === 0 ? `${pkg} wiped` : 'Wipe failed', r.errno === 0 ? 'ok' : 'err');
};

function parsePkgList(stdout) {
  return (stdout || '').split('\n')
    .map(l => l.replace(/^package:/, '').trim())
    .filter(Boolean)
    .sort();
}

async function loadAppLists() {
  const [userRes, sysRes] = await Promise.all([
    ksu_exec('pm list packages -3 2>/dev/null'),
    ksu_exec('pm list packages -s 2>/dev/null')
  ]);
  _userApps = parsePkgList(userRes.stdout);
  const sysList = parsePkgList(sysRes.stdout);
  _systemApps = new Set(sysList);
  _appsLoaded = true;
  return { user: _userApps, system: sysList };
}

function renderPickerList(user, system, filter) {
  const q = (filter || '').toLowerCase();
  const fUser = q ? user.filter(p => p.toLowerCase().includes(q)) : user;
  const fSys = q ? system.filter(p => p.toLowerCase().includes(q)) : system;
  if (!fUser.length && !fSys.length) {
    return '<div class="app-picker-empty">No apps match the filter</div>';
  }
  const addIcon = '<svg class="app-picker-add-icon" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><line x1="12" y1="5" x2="12" y2="19"/><line x1="5" y1="12" x2="19" y2="12"/></svg>';
  let html = '';
  if (fUser.length) {
    html += `<div class="app-picker-group user"><svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" style="width:12px;height:12px"><path d="M20 21v-2a4 4 0 00-4-4H8a4 4 0 00-4 4v2"/><circle cx="12" cy="7" r="4"/></svg> Installed Apps <span class="count">${fUser.length}</span></div>`;
    html += fUser.map(p =>
      `<div class="app-picker-item${state.scopedApps.includes(p) ? ' added' : ''}" data-pkg="${escAttr(p)}" onclick="addScopedAppFromPicker('${escAttr(p)}')"><span class="app-picker-dot user"></span><span class="app-picker-pkg">${escHtml(p)}</span>${addIcon}</div>`
    ).join('');
  }
  if (fSys.length) {
    html += `<div class="app-picker-group system"><svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" style="width:12px;height:12px"><rect x="2" y="3" width="20" height="14" rx="2"/><line x1="8" y1="21" x2="16" y2="21"/><line x1="12" y1="17" x2="12" y2="21"/></svg> System Apps <span class="count">${fSys.length}</span></div>`;
    html += fSys.map(p =>
      `<div class="app-picker-item${state.scopedApps.includes(p) ? ' added' : ''}" data-pkg="${escAttr(p)}" onclick="addScopedAppFromPicker('${escAttr(p)}')"><span class="app-picker-dot system"></span><span class="app-picker-pkg">${escHtml(p)}</span>${addIcon}</div>`
    ).join('');
  }
  return html;
}

function refreshPickerAdded() {
  document.querySelectorAll('.app-picker-item').forEach(el => {
    el.classList.toggle('added', state.scopedApps.includes(el.dataset.pkg));
  });
}

window.toggleAppPicker = async function() {
  const panel = document.getElementById('app-picker-panel');
  if (!panel) return;
  if (panel.classList.contains('open')) { closeAppPicker(); return; }
  panel.classList.add('open');
  const list = document.getElementById('app-picker-list');
  const search = document.getElementById('app-picker-search');
  if (search) search.value = '';
  if (!_appsLoaded) {
    list.innerHTML = '<div class="app-picker-loading"><span class="spinner"></span>Loading apps…</div>';
    const { user, system } = await loadAppLists();
    list.innerHTML = renderPickerList(user, system, '');
  } else {
    list.innerHTML = renderPickerList(_userApps, [..._systemApps].sort(), '');
  }
  if (search) search.focus();
};

window.closeAppPicker = function() {
  const panel = document.getElementById('app-picker-panel');
  if (panel) panel.classList.remove('open');
};

// Live search filter for app picker
document.addEventListener('input', (e) => {
  if (e.target.id !== 'app-picker-search') return;
  const list = document.getElementById('app-picker-list');
  if (!list || !_appsLoaded) return;
  list.innerHTML = renderPickerList(_userApps, [..._systemApps].sort(), e.target.value);
});

// ─── Reboot ────────────────────────────────────────────────────────────
window.rebootDevice = async function() {
  const ok = await showDialog('Reboot Device',
    'Reboot now? This will apply all changes and restart your device.<br><span style="color:var(--warning)">Save any open work first.</span>',
    [{label:'Cancel',cls:'btn-secondary',value:false},{label:'Reboot Now',cls:'reboot-btn btn',value:true}]);
  if (!ok) return;
  saveConfig();
  toast('Rebooting...', 'warn', 1500);
  setTimeout(() => ksu_exec('reboot'), 1600);
};

// ─── Wipe Google Traces ───────────────────────────────────────────────
window.wipeGoogleTraces = async function() {
  const ok = await showDialog('Wipe Google Traces',
    `This will force stop and wipe data for:<br>
    <ul style="margin:8px 0 0 16px;line-height:2">
      <li><b>WebView provider</b> — cookies, localStorage, cache</li>
      <li><b>Google Play Store</b> — device fingerprint, account cache</li>
      <li><b>Google Play Services</b> — device registration, checkin</li>
      <li><b>Google Services Framework</b> — GSF ID reset</li>
    </ul>
    <br><span style="color:var(--error)">The UI will close immediately.</span>
    Reopen KernelSU Manager to continue.`,
    [{label:'Cancel',cls:'btn-secondary',value:false},
     {label:'Wipe & Close',cls:'destroy-btn btn',value:true}]);
  if (!ok) return;

  toast('Wiping Google traces in 2 seconds...', 'warn', 2000);

  // Detect the active WebView provider package, then background the
  // destructive commands with nohup so they survive WebView process death.
  // PR97: Always explicitly target com.android.webview (AOSP built-in) AND the
  // dynamically resolved active provider ($WV). Previously only $WV was targeted,
  // leaving com.android.webview data untouched on devices where the active
  // provider differs (e.g. Chrome as WebView, or Google WebView active while
  // AOSP WebView still holds cached data from previous sessions).
  await ksu_exec(
    `nohup sh -c "` +
    `sleep 2; ` +
    `WV=$(cmd webviewupdate current-webview-package 2>/dev/null | tr -d '\\n' || echo com.google.android.webview); ` +
    `[ -z "\\$WV" ] && WV=com.google.android.webview; ` +
    `am force-stop com.android.vending 2>/dev/null; ` +
    `am force-stop com.google.android.gms 2>/dev/null; ` +
    `am force-stop com.google.android.gsf 2>/dev/null; ` +
    `am force-stop com.android.webview 2>/dev/null; ` +
    `am force-stop \\$WV 2>/dev/null; ` +
    `pm clear --user 0 com.google.android.gsf 2>/dev/null; ` +
    `pm clear --user 0 com.google.android.gms 2>/dev/null; ` +
    `pm clear --user 0 com.android.vending 2>/dev/null; ` +
    `pm clear --user 0 com.android.webview 2>/dev/null; ` +
    `pm clear --user 0 \\$WV 2>/dev/null; ` +
    `rm -rf /data/user/0/com.android.webview/ /data/data/com.android.webview/ /data/user_de/0/com.android.webview/ 2>/dev/null; ` +
    `rm -rf /data/user/0/\\$WV/ /data/data/\\$WV/ /data/user_de/0/\\$WV/ 2>/dev/null` +
    `" >/dev/null 2>&1 &`
  );
};

// ─── Destroy Identity ──────────────────────────────────────────────────
window.destroyIdentity = async function() {
  const ok = await showDialog('Destroy Identity',
    `This will:<br>
    <ul style="margin:8px 0 0 16px;line-height:2">
      <li>Force stop all scoped apps</li>
      <li>Wipe data for all scoped apps</li>
      <li>Randomize device profile</li>
      <li>Randomize all IDs &amp; telephony</li>
      <li>Set a random US location</li>
    </ul>
    <br><b>Profile coherence is preserved.</b> Apps will reload with the new identity on next launch.`,
    [{label:'Cancel',cls:'btn-secondary',value:false},{label:'Destroy Identity',cls:'destroy-btn btn',value:true}]);
  if (!ok) return;

  // 1. Force stop all scoped apps
  for (const pkg of state.scopedApps) {
    await ksu_exec(`am force-stop ${pkg}`);
    await sleep(200);
  }

  // 2. Wipe data for all scoped apps
  for (const pkg of state.scopedApps) {
    await ksu_exec(`pm clear ${pkg}`);
    await sleep(200);
  }

  // 3. Random profile
  const newProfile = PROFILE_NAMES[Math.floor(Math.random() * PROFILE_NAMES.length)];
  state.profile = newProfile;
  addRecentProfile(newProfile);

  // 4. Rotate all seeds (coherent new identity)
  for (const k in overrides) delete overrides[k];
  state.seed = generateRandomSeed();
  state.seedVersion++;

  // 5. Random US location
  const city = US_CITIES_100[Math.floor(Math.random() * US_CITIES_100.length)];
  state.lat = city.lat + (Math.random() - 0.5) * 0.02;
  state.lon = city.lon + (Math.random() - 0.5) * 0.02;
  state.locationLat = state.lat;
  state.locationLon = state.lon;
  state.alt = Math.floor(Math.random() * 200) + 5;
  state.locationAlt = state.alt;

  computeAll();
  saveConfig();
  toast(`Identity destroyed → ${newProfile}`, 'warn', 4000);
  renderPage(currentPage);
  navigate('status');
};

function sleep(ms) { return new Promise(r => setTimeout(r, ms)); }

// ─── Refresh status ────────────────────────────────────────────────────
window.refreshStatus = async function() {
  await loadSystemInfo();
  computeAll();
  renderStatus();
  toast('Status refreshed', 'info');
};

// ─── Utils ────────────────────────────────────────────────────────────
function escHtml(s) { return String(s||'').replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;').replace(/"/g,'&quot;'); }
function escAttr(s) { return String(s||'').replace(/'/g,'&#39;').replace(/"/g,'&quot;'); }


// ─── Init ─────────────────────────────────────────────────────────────
document.addEventListener('DOMContentLoaded', async () => {
  // PR-UIFreeze: loadState() is wrapped in try/finally so the loading screen
  // is ALWAYS removed even if ksu_exec times out, throws, or the config is
  // missing.  yieldToUI() calls give the OS event loop a chance to breathe
  // between heavy operations, preventing the system-wide freeze.
  try {
    await loadState();
  } catch(e) {
    console.error('[OmniShield] init error:', e);
    // State defaults are already set at declaration time, so computeAll()
    // below will still produce valid generated values.
  } finally {
    const loader = document.getElementById('loading-screen');
    if (loader) {
      loader.classList.add('hidden');
      setTimeout(() => loader.remove(), 600);
    }
  }

  // PR-UIFreeze: Yield to let the browser paint the first frame with the
  // loading screen removed before doing more work.
  await yieldToUI();

  // ── Event listeners are registered AFTER the finally block so they are
  //    always attached regardless of whether loadState() succeeded. ──────

  // Navigation
  document.querySelectorAll('.nav-item').forEach(btn => {
    btn.addEventListener('click', () => navigate(btn.dataset.nav));
  });

  // Identity tabs
  document.querySelectorAll('#identity-tabs .tab-btn').forEach(btn => {
    btn.addEventListener('click', () => {
      identityTab = btn.dataset.tab;
      document.querySelectorAll('#identity-tabs .tab-btn').forEach(b => b.classList.toggle('active', b === btn));
      document.querySelectorAll('#identity-content .tab-panel').forEach(p => p.classList.toggle('active', p.id === 'id-tab-' + identityTab));
      renderIdentity();
    });
  });

  // Network tabs
  document.querySelectorAll('#network-tabs .tab-btn').forEach(btn => {
    btn.addEventListener('click', () => {
      networkTab = btn.dataset.tab;
      document.querySelectorAll('#network-tabs .tab-btn').forEach(b => b.classList.toggle('active', b === btn));
      document.querySelectorAll('#network-content .tab-panel').forEach(p => p.classList.toggle('active', p.id === 'net-tab-' + networkTab));
      renderNetwork();
      if (networkTab === 'location') setTimeout(initMap, 100);
    });
  });

  // Advanced tabs
  document.querySelectorAll('#advanced-tabs .tab-btn').forEach(btn => {
    btn.addEventListener('click', () => {
      advancedTab = btn.dataset.tab;
      document.querySelectorAll('#advanced-tabs .tab-btn').forEach(b => b.classList.toggle('active', b === btn));
      document.querySelectorAll('#advanced-content .tab-panel').forEach(p => p.classList.toggle('active', p.id === 'adv-tab-' + advancedTab));
      renderAdvanced();
    });
  });

  // Profile search — PR-UIFreeze: debounced to avoid re-rendering 40+ DOM
  // elements on every keystroke, which caused layout thrashing on slow devices.
  let _profileSearchTimer = 0;
  document.getElementById('profile-search')?.addEventListener('input', () => {
    clearTimeout(_profileSearchTimer);
    _profileSearchTimer = setTimeout(renderDeviceTab, 180);
  });

  // Profile list click (delegated)
  document.getElementById('profile-list')?.addEventListener('click', e => {
    const item = e.target.closest('.profile-item');
    if (item) selectProfile(item.dataset.name);
  });

  // Recent profiles click
  document.getElementById('recent-list')?.addEventListener('click', e => {
    const item = e.target.closest('.recent-profile');
    if (item) selectProfile(item.dataset.recent);
  });

  // Dialog close on overlay click
  document.getElementById('dialog-overlay')?.addEventListener('click', e => {
    if (e.target === document.getElementById('dialog-overlay')) {
      document.getElementById('dialog-overlay').classList.remove('open');
    }
  });

  // LTE toggle
  document.getElementById('toggle-lte')?.addEventListener('change', e => toggleNetworkType(e.target.checked));

  // PR71g: WebView spoof toggle — persists immediately on change
  document.getElementById('toggle-webview-spoof')?.addEventListener('change', async e => {
    state.webviewSpoof = e.target.checked;
    if (await saveConfig()) toast(state.webviewSpoof ? 'WebView spoofing enabled — force stop apps to apply' : 'WebView spoofing disabled');
    else toast('Failed to save setting', 'err');
  });

  navigate('status');

  // PR-UIFreeze: Load system info (IP, model) in the background AFTER the
  // UI is already visible and interactive.  When it resolves, refresh the
  // status page so the real values replace the defaults.
  loadSystemInfo().then(() => {
    if (currentPage === 'status') renderStatus();
  }).catch(() => {});
});
