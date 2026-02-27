// app.js — OmniShield WebUI main application
import {
  generateIMEI, generateICCID, generateIMSI, generatePhoneNumber,
  generateMAC, generateSerial, generateAndroidId, generateGsfId,
  generateWidevineId, generateBootId, generateLocation, generateAltitude,
  getTimezone, getCarrierName, getSimCountry, computeCorrelation,
  validateIMEI, validateICCID, validateIMSI, validateMAC, validatePhone,
  validateAndroidId, validateGsfId, validateWidevineId, validateBootId, validateSerial,
  US_CITIES_100, CARRIER_NAMES, IMSI_POOLS
} from './engine.js';
import { DEVICE_PROFILES, PROFILE_NAMES, getProfileByName } from './profiles.js';

// ─── KernelSU exec wrapper ──────────────────────────────────────────
// Uses dynamic import so the module loads even outside KernelSU WebView.
// 3 s timeout guards against KernelSU Next on Android 11 taking too long
// to resolve the native import (seen in some ROM builds).
let _ksuExecFn = null;
async function ksu_exec(cmd) {
  try {
    if (!_ksuExecFn) {
      const mod = await Promise.race([
        import('kernelsu'),
        new Promise((_, reject) => setTimeout(() => reject(new Error('ksu timeout')), 3000))
      ]);
      _ksuExecFn = mod.exec;
    }
    const r = await _ksuExecFn(cmd);
    return { errno: r.errno || 0, stdout: (r.stdout || '').trim() };
  } catch(e) {
    // Not in KernelSU environment or timed out — graceful degradation
    return { errno: 1, stdout: '' };
  }
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
  await ksu_exec(`mkdir -p /data/adb/.omni_data`);
  const lines = Object.entries(cfg).map(([k,v]) => `${k}=${v}`).join('\n');
  const b64 = btoa(unescape(encodeURIComponent(lines)));
  const r = await ksu_exec(`echo '${b64}' | base64 -d > "${CFG_PATH}" && chmod 644 "${CFG_PATH}"`);
  return r.errno === 0;
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
  scopedApps: [],
  recentProfiles: [],
  // computed
  imei: '', iccid: '', imsi: '', phone: '', wifiMac: '', btMac: '',
  serial: '', androidId: '', gsfId: '', widevineId: '', bootId: '',
  lat: 0, lon: 0, alt: 0, tz: '',
  carrier: '', simCountry: '', correlation: 0,
  deviceIp: '', deviceModel: ''
};

// overrides — per-field manual seeds (each "randomize" just picks a new random seed for that field)
const overrides = {};

// ─── Bootstrap ──────────────────────────────────────────────────────
async function loadState() {
  state.cfg = await readConfig();
  state.profile      = state.cfg.profile      || 'Redmi 9';
  state.seed         = parseInt(state.cfg.master_seed) || generateRandomSeed();
  state.jitter       = (state.cfg.jitter || 'true') !== 'false';
  state.networkType  = (state.cfg.network_type === 'lte' || state.cfg.network_type === 'mobile') ? 'lte' : 'wifi';
  state.seedVersion  = parseInt(state.cfg.seed_version) || 0;
  state.locationLat  = state.cfg.location_lat  ? parseFloat(state.cfg.location_lat)  : null;
  state.locationLon  = state.cfg.location_lon  ? parseFloat(state.cfg.location_lon)  : null;
  state.locationAlt  = state.cfg.location_alt  ? parseFloat(state.cfg.location_alt)  : null;
  state.proxyEnabled = state.cfg.proxy_enabled === 'true';
  state.proxyType    = state.cfg.proxy_type    || 'http';
  state.proxyHost    = state.cfg.proxy_host    || '';
  state.proxyPort    = state.cfg.proxy_port    || '';
  state.proxyUser    = state.cfg.proxy_user    || '';
  state.proxyPass    = state.cfg.proxy_pass    || '';
  state.scopedApps   = state.cfg.scoped_apps ? state.cfg.scoped_apps.split(',').filter(Boolean) : [];
  state.recentProfiles = loadRecentProfiles();

  computeAll();
  await loadSystemInfo();
}

function computeAll() {
  const { profile, seed } = state;
  let fp = getProfileByName(profile);
  // Fallback to Redmi 9 if profile is missing/invalid
  if (!fp) { state.profile = 'Redmi 9'; fp = getProfileByName('Redmi 9'); }
  const brand = (fp.brand || 'xiaomi').toLowerCase();

  state.imei       = overrides.imei       ?? generateIMEI(profile, seed);
  state.iccid      = overrides.iccid      ?? generateICCID(profile, seed);
  state.imsi       = overrides.imsi       ?? generateIMSI(profile, seed);
  state.phone      = overrides.phone      ?? generatePhoneNumber(profile, seed);
  state.wifiMac    = overrides.wifiMac    ?? generateMAC(brand, seed);
  state.btMac      = overrides.btMac      ?? generateMAC(brand, seed + 1);
  state.serial     = overrides.serial     ?? generateSerial(brand, seed, fp.securityPatch || '');
  state.androidId  = overrides.androidId  ?? generateAndroidId(seed);
  state.gsfId      = overrides.gsfId      ?? generateGsfId(seed);
  state.widevineId = overrides.widevineId ?? generateWidevineId(seed);
  state.bootId     = overrides.bootId     ?? generateBootId(seed);
  state.carrier    = getCarrierName(profile, seed);
  state.simCountry = getSimCountry(profile, seed);
  state.tz         = getTimezone(seed);

  if (state.locationLat !== null && state.locationLon !== null) {
    state.lat = state.locationLat;
    state.lon = state.locationLon;
    state.alt = state.locationAlt ?? generateAltitude(profile, seed);
  } else {
    const loc = generateLocation(profile, seed);
    state.lat = loc.lat; state.lon = loc.lon;
    state.alt = generateAltitude(profile, seed);
  }

  state.correlation = computeCorrelation(profile, seed, {
    imei: state.imei, imsi: state.imsi, iccid: state.iccid,
    phone: state.phone, wifiMac: state.wifiMac
  });
}

async function loadSystemInfo() {
  const ipRes = await ksu_exec("ip -4 addr show wlan0 2>/dev/null | grep inet | awk '{print $2}' | cut -d/ -f1 | head -1");
  const ipRes2 = ipRes.stdout || (await ksu_exec("ip -4 addr show rmnet0 2>/dev/null | grep inet | awk '{print $2}' | cut -d/ -f1 | head -1")).stdout || '';
  state.deviceIp = ipRes2 || '–';
  const modelRes = await ksu_exec('getprop ro.product.model');
  state.deviceModel = modelRes.stdout || state.profile;
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
  if (state.scopedApps.length) cfg.scoped_apps = state.scopedApps.join(',');
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
  setCell('status-ip', state.deviceIp);
  setCell('status-simcountry', state.simCountry);
  setCell('status-phone', state.phone);
  setCell('status-carrier', state.carrier);
  setCell('status-location', `${state.lat.toFixed(4)}, ${state.lon.toFixed(4)}`);
  setCell('status-tz', state.tz);
  setCell('status-network', state.networkType.toUpperCase());
  setCell('status-seed', state.seed.toString());
  setCell('status-corr-label', `${state.correlation}% coherence`);
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
  renderField('f-imei',       state.imei,       validateIMEI(state.imei));
  renderField('f-serial',     state.serial,     validateSerial(state.serial));
  renderField('f-android-id', state.androidId,  validateAndroidId(state.androidId));
  renderField('f-gsf-id',     state.gsfId,      validateGsfId(state.gsfId));
  renderField('f-widevine',   state.widevineId, validateWidevineId(state.widevineId));
  renderField('f-boot-id',    state.bootId,     validateBootId(state.bootId));
  renderField('f-fingerprint', getProfileByName(state.profile).fingerprint || '', true);
}

// ─── Network page ─────────────────────────────────────────────────────
let networkTab = 'telephony';
function renderNetwork() {
  if (networkTab === 'telephony') renderTelephonyTab();
  else renderLocationTab();
}

function renderTelephonyTab() {
  renderField('f-imsi',     state.imsi,    validateIMSI(state.imsi));
  renderField('f-iccid',    state.iccid,   validateICCID(state.iccid));
  renderField('f-phone',    state.phone,   validatePhone(state.phone));
  renderField('f-wifi-mac', state.wifiMac, validateMAC(state.wifiMac));
  renderField('f-bt-mac',   state.btMac,   validateMAC(state.btMac));
  renderField('f-carrier',  state.carrier, !!state.carrier);
  renderField('f-sim-cc',   state.simCountry, state.simCountry === 'US');

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
}

function renderSettingsTab() {
  renderScopedApps();
}

function renderScopedApps() {
  const list = document.getElementById('scoped-apps-list');
  if (!list) return;
  if (!state.scopedApps.length) {
    list.innerHTML = '<div class="text-sm" style="padding:12px 0;text-align:center">No apps in scope. Add apps above.</div>';
    return;
  }
  list.innerHTML = state.scopedApps.map(pkg => `
    <div class="scope-app-item">
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
    </div>`).join('');
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
  state.profile = name;
  // clear overrides on profile change
  for (const k in overrides) delete overrides[k];
  computeAll();
  addRecentProfile(name);
  renderDeviceTab();
  toast(`Profile set to ${name}`);
};

window.randomProfile = function() {
  const n = PROFILE_NAMES[Math.floor(Math.random() * PROFILE_NAMES.length)];
  selectProfile(n);
};

window.randomizeAllIds = function() {
  rotateSeed();
  renderIdsTab();
  toast('All IDs randomized');
};

window.applyIds = async function() {
  if (await saveConfig()) { toast('Identity changes saved'); }
  else toast('Save failed — check permissions', 'err');
};

// Per-field randomize (changes only that field via its sub-seed)
window.randomizeField = function(field) {
  const newSubSeed = Math.floor(Math.random() * 2000000000) + 1;
  const fp = getProfileByName(state.profile);
  const brand = (fp.brand || 'xiaomi').toLowerCase();
  const actions = {
    'f-imei':       () => { overrides.imei       = generateIMEI(state.profile, newSubSeed); state.imei = overrides.imei; },
    'f-serial':     () => { overrides.serial     = generateSerial(brand, newSubSeed, fp.securityPatch||''); state.serial = overrides.serial; },
    'f-android-id': () => { overrides.androidId  = generateAndroidId(newSubSeed); state.androidId = overrides.androidId; },
    'f-gsf-id':     () => { overrides.gsfId      = generateGsfId(newSubSeed); state.gsfId = overrides.gsfId; },
    'f-widevine':   () => { overrides.widevineId = generateWidevineId(newSubSeed); state.widevineId = overrides.widevineId; },
    'f-boot-id':    () => { overrides.bootId     = generateBootId(newSubSeed); state.bootId = overrides.bootId; },
    'f-imsi':       () => { overrides.imsi       = generateIMSI(state.profile, newSubSeed); state.imsi = overrides.imsi; },
    'f-iccid':      () => { overrides.iccid      = generateICCID(state.profile, newSubSeed); state.iccid = overrides.iccid; },
    'f-phone':      () => { overrides.phone      = generatePhoneNumber(state.profile, newSubSeed); state.phone = overrides.phone; },
    'f-wifi-mac':   () => { overrides.wifiMac    = generateMAC(brand, newSubSeed); state.wifiMac = overrides.wifiMac; },
    'f-bt-mac':     () => { overrides.btMac      = generateMAC(brand, newSubSeed+1); state.btMac = overrides.btMac; },
  };
  if (actions[field]) {
    actions[field]();
    state.correlation = computeCorrelation(state.profile, state.seed, { imei: state.imei, imsi: state.imsi, iccid: state.iccid, phone: state.phone, wifiMac: state.wifiMac });
    renderPage(currentPage);
  }
};

// ─── Event handlers: Network/Telephony ───────────────────────────────
window.randomizeAllTelephony = function() {
  rotateSeed();
  renderTelephonyTab();
  toast('All telephony IDs randomized');
};

window.applyTelephony = async function() {
  if (await saveConfig()) toast('Network changes saved');
  else toast('Save failed', 'err');
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
  if (await saveConfig()) toast('Location changes saved');
  else toast('Save failed', 'err');
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
window.applyProxy = async function() {
  state.proxyEnabled = document.getElementById('proxy-enabled')?.checked || false;
  state.proxyType = document.getElementById('proxy-type')?.value || 'http';
  state.proxyHost = document.getElementById('proxy-host')?.value || '';
  state.proxyPort = document.getElementById('proxy-port')?.value || '';
  state.proxyUser = document.getElementById('proxy-user')?.value || '';
  state.proxyPass = document.getElementById('proxy-pass')?.value || '';
  if (state.proxyEnabled && !state.proxyHost) { toast('Enter a proxy host', 'warn'); return; }
  if (state.proxyEnabled && !state.proxyPort) { toast('Enter a proxy port', 'warn'); return; }
  if (await saveConfig()) toast('Proxy settings saved');
  else toast('Save failed', 'err');
};

// ─── App scope management ──────────────────────────────────────────────
window.addScopedApp = async function() {
  const input = document.getElementById('app-search-input');
  const val = input?.value?.trim() || '';
  const dropdown = document.getElementById('app-dropdown');
  const selected = dropdown?.value?.trim() || '';
  const pkg = selected || val;
  if (!pkg) { toast('Enter a package name', 'warn'); return; }
  if (state.scopedApps.includes(pkg)) { toast('App already in scope', 'warn'); return; }
  state.scopedApps.push(pkg);
  if (input) input.value = '';
  if (dropdown) dropdown.value = '';
  renderScopedApps();
  await saveConfig();
  toast(`Added ${pkg} to scope`);
};

window.removeScopedApp = async function(pkg) {
  state.scopedApps = state.scopedApps.filter(p => p !== pkg);
  renderScopedApps();
  await saveConfig();
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

window.loadInstalledApps = async function() {
  const dd = document.getElementById('app-dropdown');
  if (!dd || dd.options.length > 1) return;
  dd.innerHTML = '<option value="">Loading apps...</option>';
  const {stdout} = await ksu_exec('pm list packages 2>/dev/null | sed "s/package://" | grep -v "^$" | sort | head -200');
  const pkgs = stdout ? stdout.split('\n').filter(Boolean) : [];
  dd.innerHTML = '<option value="">Select app to add...</option>' +
    pkgs.map(p => `<option value="${escAttr(p)}">${escHtml(p)}</option>`).join('');
};

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
    <br><b>Profile coherence is preserved.</b> Save config and reboot to fully apply.`,
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
  // loadState() is wrapped in try/finally so the loading screen is ALWAYS
  // removed even if ksu_exec times out, throws, or the config is missing.
  // Without this guarantee the #loading-screen stays on top and blocks every
  // click / touch event on the underlying UI.
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

  // Profile search
  document.getElementById('profile-search')?.addEventListener('input', () => renderDeviceTab());

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

  navigate('status');
});
