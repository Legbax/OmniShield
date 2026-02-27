// engine.js — OmniShield generation engine (mirrors jni/omni_engine.hpp exactly)
// Uses BigInt for 64-bit precision matching C++ std::mt19937_64

// ─── MT19937-64 (matches std::mt19937_64) ────────────────────────────────────
class MT64 {
  constructor(seed) {
    this.N = 312; this.M = 156;
    this.MATRIX_A = 0xB5026F5AA96619E9n;
    this.UM = 0xFFFFFFFF80000000n;
    this.LM = 0x7FFFFFFFn;
    this.M64 = 0xFFFFFFFFFFFFFFFFn;
    this.mt = new Array(312).fill(0n);
    this.mti = 312;
    const s = BigInt(seed < 0 ? -seed : seed);
    this.mt[0] = s & this.M64;
    for (let i = 1; i < 312; i++) {
      const p = this.mt[i - 1];
      this.mt[i] = (6364136223846793005n * (p ^ (p >> 62n)) + BigInt(i)) & this.M64;
    }
  }
  gen() {
    const {N,M,MATRIX_A,UM,LM,M64} = this;
    const mag = [0n, MATRIX_A];
    if (this.mti >= N) {
      let k;
      for (k = 0; k < N - M; k++) {
        const x = (this.mt[k] & UM) | (this.mt[k+1] & LM);
        this.mt[k] = this.mt[k+M] ^ (x >> 1n) ^ mag[Number(x & 1n)];
      }
      for (; k < N - 1; k++) {
        const x = (this.mt[k] & UM) | (this.mt[k+1] & LM);
        this.mt[k] = this.mt[k+(M-N)] ^ (x >> 1n) ^ mag[Number(x & 1n)];
      }
      const x = (this.mt[N-1] & UM) | (this.mt[0] & LM);
      this.mt[N-1] = this.mt[M-1] ^ (x >> 1n) ^ mag[Number(x & 1n)];
      this.mti = 0;
    }
    let y = this.mt[this.mti++];
    y ^= (y >> 29n) & 0x5555555555555555n;
    y ^= (y << 17n) & 0x71D67FFFEDA60000n;
    y ^= (y << 37n) & 0xFFF7EEE000000000n;
    y ^= y >> 43n;
    return y & M64;
  }
}

class OmniRandom {
  constructor(seed) { this._rng = new MT64(seed); }
  nextInt(bound) { return Number(this._rng.gen() % BigInt(bound)); }
  nextFloat(min, max) {
    const v = Number(this._rng.gen() & 0xFFFFFFFFn) / 4294967295;
    return min + v * (max - min);
  }
}

// ─── TAC pools (mirrors TACS_BY_BRAND in omni_engine.hpp) ────────────────────
const TACS = {
  xiaomi:   ["86413404","86413405","35271311","35361311","86814904","86814905"],
  poco:     ["86814904","86814905","35847611","35847612","35847613","35847614"],
  redmi:    ["86413404","86413405","35271311","35271312","35271313","35271314"],
  samsung:  ["35449209","35449210","35355610","35735110","35735111","35843110","35940210"],
  google:   ["35674910","35674911","35308010","35308011","35908610","35908611"],
  oneplus:  ["86882504","86882505","35438210","35438211","35438212","35438213"],
  motorola: ["35617710","35617711","35327510","35327511","35327512","35327513"],
  nokia:    ["35720210","35720211","35489310","35489311","35654110","35654111"],
  realme:   ["86828804","86828805","35388910","35388911","35388912","35388913"],
  vivo:     ["86979604","86979605","35503210","35503211","35503212","35503213"],
  oppo:     ["86885004","86885005","35604210","35604211","35604212","35604213"],
  asus:     ["35851710","35851711","35325010","35325011","35325012","35325013"],
  "hmd global":["35720210","35720211","35489310","35489311"],
  default:  ["35271311","35449209","35674910","35438210","35617710"]
};

// OUI pools (mirrors OUIS in omni_engine.hpp)
const OUIS = [
  [0x40,0x4E,0x36],[0xF0,0x1F,0xAF],[0x18,0xDB,0x7E],[0x28,0xCC,0x01], // Qualcomm
  [0x60,0x57,0x18],[0xAC,0x37,0x43],[0x00,0x90,0x4C],                   // MediaTek
  [0xD4,0xBE,0xD9],[0xA4,0xC3,0xF0],[0xF8,0x8F,0xCA],                   // Broadcom
  [0x40,0x9B,0xCD],[0x24,0x4B,0x03]                                       // Samsung
];

// IMSI pools (USA carriers, mirrors IMSI_POOLS in omni_engine.hpp)
const IMSI_POOLS = ["310260","310410","311480","310120","311580"];
//                  T-Mobile  AT&T    Verizon  Sprint   US Cellular

const CARRIER_NAMES = {
  "310260":"T-Mobile","310410":"AT&T","311480":"Verizon Wireless",
  "310120":"T-Mobile","311580":"U.S. Cellular"
};

// US cities for GPS (mirrors US_CITIES in omni_engine.hpp)
const US_CITIES_5 = [
  {lat:40.7128, lon:-74.0060, name:"New York"},
  {lat:34.0522, lon:-118.2437, name:"Los Angeles"},
  {lat:41.8781, lon:-87.6298, name:"Chicago"},
  {lat:29.7604, lon:-95.3698, name:"Houston"},
  {lat:33.4484, lon:-112.0740, name:"Phoenix"}
];

const US_CITIES_100 = [
  {lat:40.7128,lon:-74.0060,name:"New York, NY"},
  {lat:34.0522,lon:-118.2437,name:"Los Angeles, CA"},
  {lat:41.8781,lon:-87.6298,name:"Chicago, IL"},
  {lat:29.7604,lon:-95.3698,name:"Houston, TX"},
  {lat:33.4484,lon:-112.0740,name:"Phoenix, AZ"},
  {lat:29.4241,lon:-98.4936,name:"San Antonio, TX"},
  {lat:32.7767,lon:-96.7970,name:"Dallas, TX"},
  {lat:30.3322,lon:-81.6557,name:"Jacksonville, FL"},
  {lat:30.2672,lon:-97.7431,name:"Austin, TX"},
  {lat:30.3960,lon:-86.4981,name:"Fort Worth, TX"},
  {lat:37.3382,lon:-121.8863,name:"San Jose, CA"},
  {lat:30.3322,lon:-81.6557,name:"Columbus, OH"},
  {lat:35.2271,lon:-80.8431,name:"Charlotte, NC"},
  {lat:39.7392,lon:-104.9903,name:"Denver, CO"},
  {lat:39.9526,lon:-75.1652,name:"Philadelphia, PA"},
  {lat:47.6062,lon:-122.3321,name:"Seattle, WA"},
  {lat:36.1699,lon:-115.1398,name:"Las Vegas, NV"},
  {lat:36.1627,lon:-86.7816,name:"Nashville, TN"},
  {lat:30.2241,lon:-92.0198,name:"Oklahoma City, OK"},
  {lat:35.4676,lon:-97.5164,name:"Tulsa, OK"},
  {lat:37.7749,lon:-122.4194,name:"San Francisco, CA"},
  {lat:35.1495,lon:-90.0490,name:"Memphis, TN"},
  {lat:43.0481,lon:-76.1474,name:"Louisville, KY"},
  {lat:38.2527,lon:-85.7585,name:"Baltimore, MD"},
  {lat:39.2904,lon:-76.6122,name:"Milwaukee, WI"},
  {lat:43.0389,lon:-76.1528,name:"Albuquerque, NM"},
  {lat:35.0844,lon:-106.6504,name:"Tucson, AZ"},
  {lat:32.2226,lon:-110.9747,name:"Fresno, CA"},
  {lat:36.7378,lon:-119.7871,name:"Sacramento, CA"},
  {lat:38.5816,lon:-121.4944,name:"Mesa, AZ"},
  {lat:33.4152,lon:-111.8315,name:"Kansas City, MO"},
  {lat:39.0997,lon:-94.5786,name:"Atlanta, GA"},
  {lat:33.7490,lon:-84.3880,name:"Omaha, NE"},
  {lat:41.2565,lon:-95.9345,name:"Colorado Springs, CO"},
  {lat:38.8339,lon:-104.8214,name:"Raleigh, NC"},
  {lat:35.7796,lon:-78.6382,name:"Long Beach, CA"},
  {lat:33.7701,lon:-118.1937,name:"Virginia Beach, VA"},
  {lat:36.8529,lon:-75.9780,name:"Minneapolis, MN"},
  {lat:44.9778,lon:-93.2650,name:"Tampa, FL"},
  {lat:27.9506,lon:-82.4572,name:"New Orleans, LA"},
  {lat:29.9511,lon:-90.0715,name:"Arlington, TX"},
  {lat:32.7357,lon:-97.1081,name:"Bakersfield, CA"},
  {lat:35.3733,lon:-119.0187,name:"Honolulu, HI"},
  {lat:21.3069,lon:-157.8583,name:"Anaheim, CA"},
  {lat:33.8353,lon:-117.9145,name:"Aurora, CO"},
  {lat:39.7294,lon:-104.8319,name:"Santa Ana, CA"},
  {lat:33.7455,lon:-117.8677,name:"Corpus Christi, TX"},
  {lat:27.8006,lon:-97.3964,name:"Riverside, CA"},
  {lat:33.9533,lon:-117.3962,name:"Lexington, KY"},
  {lat:38.0406,lon:-84.5037,name:"St. Louis, MO"},
  {lat:38.6270,lon:-90.1994,name:"Pittsburgh, PA"},
  {lat:40.4406,lon:-79.9959,name:"Stockton, CA"},
  {lat:37.9577,lon:-121.2908,name:"Anchorage, AK"},
  {lat:61.2181,lon:-149.9003,name:"Cincinnati, OH"},
  {lat:39.1031,lon:-84.5120,name:"St. Paul, MN"},
  {lat:44.9537,lon:-93.0900,name:"Toledo, OH"},
  {lat:41.6639,lon:-83.5552,name:"Greensboro, NC"},
  {lat:36.0726,lon:-79.7920,name:"Newark, NJ"},
  {lat:40.7357,lon:-74.1724,name:"Plano, TX"},
  {lat:33.0198,lon:-96.6989,name:"Henderson, NV"},
  {lat:36.0395,lon:-114.9817,name:"Lincoln, NE"},
  {lat:40.8136,lon:-96.7026,name:"Buffalo, NY"},
  {lat:42.8864,lon:-78.8784,name:"Fort Wayne, IN"},
  {lat:41.0793,lon:-85.1394,name:"Jersey City, NJ"},
  {lat:40.7178,lon:-74.0431,name:"Chula Vista, CA"},
  {lat:32.6401,lon:-117.0842,name:"Orlando, FL"},
  {lat:28.5383,lon:-81.3792,name:"St. Petersburg, FL"},
  {lat:27.7676,lon:-82.6403,name:"Laredo, TX"},
  {lat:27.5064,lon:-99.5075,name:"Norfolk, VA"},
  {lat:36.8508,lon:-76.2859,name:"Madison, WI"},
  {lat:43.0731,lon:-89.4012,name:"Durham, NC"},
  {lat:35.9940,lon:-78.8986,name:"Lubbock, TX"},
  {lat:33.5779,lon:-101.8552,name:"Winston-Salem, NC"},
  {lat:36.0999,lon:-80.2442,name:"Garland, TX"},
  {lat:32.9126,lon:-96.6389,name:"Glendale, AZ"},
  {lat:33.5387,lon:-112.1860,name:"Hialeah, FL"},
  {lat:25.8576,lon:-80.2781,name:"Reno, NV"},
  {lat:39.5296,lon:-119.8138,name:"Baton Rouge, LA"},
  {lat:30.4515,lon:-91.1871,name:"Irvine, CA"},
  {lat:33.6846,lon:-117.8265,name:"Chesapeake, VA"},
  {lat:36.7682,lon:-76.2875,name:"Irving, TX"},
  {lat:32.8140,lon:-96.9489,name:"Scottsdale, AZ"},
  {lat:33.4942,lon:-111.9261,name:"North Las Vegas, NV"},
  {lat:36.1989,lon:-115.1175,name:"Fremont, CA"},
  {lat:37.5485,lon:-121.9886,name:"Gilbert, AZ"},
  {lat:33.3528,lon:-111.7890,name:"San Bernardino, CA"},
  {lat:34.1083,lon:-117.2898,name:"Boise, ID"},
  {lat:43.6187,lon:-116.2146,name:"Birmingham, AL"},
  {lat:33.5207,lon:-86.8025,name:"Rochester, NY"},
  {lat:43.1566,lon:-77.6088,name:"Richmond, VA"},
  {lat:37.5407,lon:-77.4360,name:"Spokane, WA"},
  {lat:47.6588,lon:-117.4260,name:"Des Moines, IA"},
  {lat:41.5868,lon:-93.6250,name:"Montgomery, AL"},
  {lat:32.3668,lon:-86.3000,name:"Modesto, CA"},
  {lat:37.6391,lon:-120.9969,name:"Fayetteville, NC"},
  {lat:35.0527,lon:-78.8784,name:"Tacoma, WA"},
  {lat:47.2529,lon:-122.4443,name:"Shreveport, LA"},
  {lat:32.5252,lon:-93.7502,name:"Akron, OH"},
  {lat:41.0814,lon:-81.5190,name:"Aurora, IL"},
  {lat:41.7606,lon:-88.3201,name:"Little Rock, AR"},
  {lat:34.7465,lon:-92.2896,name:"Augusta, GA"}
];

// Altitude base per city (US_CITIES_5)
const US_ALT = [
  {base:10,range:25}, {base:50,range:70}, {base:180,range:20},
  {base:10,range:15}, {base:330,range:30}
];

const US_TIMEZONES = [
  "America/New_York","America/Los_Angeles","America/Chicago",
  "America/Chicago","America/Phoenix"
];

// ─── Luhn checksum (mirrors luhnChecksum in omni_engine.hpp) ─────────────────
function luhnDigit(base14) {
  let sum = 0;
  const len = base14.length;
  for (let i = len - 1; i >= 0; i--) {
    let d = parseInt(base14[i]);
    if ((len - 1 - i) % 2 === 0) { d *= 2; if (d > 9) d -= 9; }
    sum += d;
  }
  return (10 - (sum % 10)) % 10;
}

// ─── IMEI generation (exact mirror of generateValidImei) ─────────────────────
export function generateIMEI(profileName, seed) {
  const rng = new OmniRandom(seed);
  const brand = getEffectiveBrand(profileName).toLowerCase();
  const isQualcomm = isAdreno(profileName);

  let tacList = TACS[brand] || TACS.default;
  let tac;
  if (brand === 'redmi' && isQualcomm) {
    const qTacs = ["35271311","35271312","35271313","35271314"];
    tac = qTacs[rng.nextInt(qTacs.length)];
  } else {
    tac = tacList[rng.nextInt(tacList.length)];
  }
  let serial = '';
  for (let i = 0; i < 6; i++) serial += rng.nextInt(10);
  const base = tac + serial;
  return base + luhnDigit(base);
}

// ─── ICCID generation (mirrors generateValidIccid) ───────────────────────────
export function generateICCID(profileName, seed) {
  const rng = new OmniRandom(seed);
  let iccid = '89101';
  for (let i = 0; i < 13; i++) iccid += rng.nextInt(10);
  return iccid + luhnDigit(iccid);
}

// ─── IMSI generation (mirrors generateValidImsi) ─────────────────────────────
export function generateIMSI(profileName, seed) {
  const rng = new OmniRandom(seed);
  const mccMnc = IMSI_POOLS[rng.nextInt(IMSI_POOLS.length)];
  const remaining = 15 - mccMnc.length;
  let rest = String(2 + rng.nextInt(8));
  for (let i = 1; i < remaining; i++) rest += rng.nextInt(10);
  return mccMnc + rest;
}

// ─── Phone number (mirrors generatePhoneNumber, seed+777) ────────────────────
export function generatePhoneNumber(profileName, seed) {
  const rng = new OmniRandom(seed + 777);
  let npa, nxx;
  do {
    npa  = String(2 + rng.nextInt(8));
    npa += rng.nextInt(10);
    npa += rng.nextInt(10);
  } while ((npa[1] === '1' && npa[2] === '1') || npa === '555');
  do {
    nxx  = String(2 + rng.nextInt(8));
    nxx += rng.nextInt(10);
    nxx += rng.nextInt(10);
  } while (nxx[1] === '1' && nxx[2] === '1');
  let sub = '';
  for (let i = 0; i < 4; i++) sub += rng.nextInt(10);
  return '+1' + npa + nxx + sub;
}

// ─── MAC address (mirrors generateRandomMac) ─────────────────────────────────
export function generateMAC(brandIn, seed) {
  const rng = new OmniRandom(seed);
  const brand = (brandIn || '').toLowerCase();
  let oui;
  if (brand === 'samsung') {
    const pool = [[0x24,0x4B,0x03],[0x40,0x9B,0xCD],[0xD4,0xBE,0xD9]];
    oui = pool[rng.nextInt(pool.length)];
  } else if (brand === 'google') {
    const pool = [[0xF8,0x8F,0xCA],[0xA4,0xC3,0xF0]];
    oui = pool[rng.nextInt(pool.length)];
  } else if (['xiaomi','redmi','poco'].includes(brand)) {
    const pool = [[0x40,0x4E,0x36],[0xF0,0x1F,0xAF],[0x60,0x57,0x18]];
    oui = pool[rng.nextInt(pool.length)];
  } else if (brand === 'oneplus') {
    oui = [0x40,0x4E,0x36];
  } else if (brand === 'motorola') {
    oui = [0x18,0xDB,0x7E];
  } else {
    oui = OUIS[rng.nextInt(OUIS.length)];
  }
  const parts = oui.map(b => b.toString(16).padStart(2,'0'));
  for (let i = 0; i < 3; i++) parts.push(rng.nextInt(256).toString(16).padStart(2,'0'));
  return parts.join(':');
}

// ─── Serial number (mirrors generateRandomSerial) ────────────────────────────
export function generateSerial(brandIn, seed, securityPatch) {
  const rng = new OmniRandom(seed);
  const brand = (brandIn || '').toLowerCase();
  if (brand === 'samsung') {
    const plants = ['R','R','S','X'];
    const plant = plants[rng.nextInt(plants.length)];
    const factories = ['F8','58','28','R1','S1'];
    const factory = factories[rng.nextInt(factories.length)];
    let year = 2021;
    if (securityPatch && securityPatch.length >= 4) year = parseInt(securityPatch.slice(0,4)) || 2021;
    const yc = {2019:'R',2020:'S',2021:'T',2022:'U',2023:'V'}[year] || 'T';
    const months = '123456789ABC';
    const month = months[rng.nextInt(months.length)];
    let u = '';
    for (let i = 0; i < 6; i++) u += '0123456789ABCDEF'[rng.nextInt(16)];
    return plant + factory + yc + month + u;
  }
  if (brand === 'google') {
    const chars = 'ABCDEFGHJKLMNPQRSTUVWXYZ0123456789';
    let res = '';
    for (let i = 0; i < 7; i++) res += chars[rng.nextInt(chars.length)];
    return res;
  }
  const an = '0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ';
  const len = 8 + rng.nextInt(5);
  let res = '';
  for (let i = 0; i < len; i++) res += an[rng.nextInt(an.length)];
  return res;
}

// ─── Random ID (hex, mirrors generateRandomId) ───────────────────────────────
export function generateHexId(len, seed) {
  const rng = new OmniRandom(seed);
  const chars = '0123456789abcdef';
  let res = '';
  for (let i = 0; i < len; i++) res += chars[rng.nextInt(16)];
  return res;
}

// Android ID (16 hex chars, same as generateRandomId(16, seed))
export function generateAndroidId(seed) { return generateHexId(16, seed); }

// GSF ID (16 hex chars, seed+1)
export function generateGsfId(seed) { return generateHexId(16, seed + 1); }

// Widevine ID (32 hex chars, mirrors generateWidevineId)
export function generateWidevineId(seed) {
  const rng = new OmniRandom(seed);
  let res = '';
  for (let i = 0; i < 16; i++) res += rng.nextInt(256).toString(16).padStart(2,'0');
  return res;
}

// Boot ID (UUID format, seed+2)
export function generateBootId(seed) {
  const rng = new OmniRandom(seed + 2);
  const hex = (n) => rng.nextInt(256).toString(16).padStart(2,'0');
  const s = (n) => Array.from({length:n},()=>rng.nextInt(16).toString(16)).join('');
  return `${s(8)}-${s(4)}-4${s(3)}-${(8|rng.nextInt(4)).toString(16)}${s(3)}-${s(12)}`;
}

// ─── Location (mirrors generateLocationForRegion) ────────────────────────────
export function generateLocation(profileName, seed) {
  const cityRng = new OmniRandom(seed + 7777);
  const jitterRng = new OmniRandom(seed + 9999);
  const idx = cityRng.nextInt(5);
  const city = US_CITIES_5[idx];
  const jLat = (jitterRng.nextInt(4000) - 2000) / 100000;
  const jLon = (jitterRng.nextInt(4000) - 2000) / 100000;
  return { lat: city.lat + jLat, lon: city.lon + jLon, cityName: city.name };
}

export function generateAltitude(profileName, seed) {
  const cityRng = new OmniRandom(seed + 7777);
  const altRng = new OmniRandom(seed + 8888);
  const idx = cityRng.nextInt(5);
  const a = US_ALT[idx];
  return a.base + altRng.nextInt(a.range);
}

export function getTimezone(seed) {
  const rng = new OmniRandom(seed + 7777);
  return US_TIMEZONES[rng.nextInt(5)];
}

// ─── Carrier name from IMSI ───────────────────────────────────────────────────
export function getCarrierName(profileName, seed) {
  const imsi = generateIMSI(profileName, seed);
  const mccMnc = imsi.substr(0, 6);
  return CARRIER_NAMES[mccMnc] || 'T-Mobile';
}

export function getSimCountry(profileName, seed) { return 'US'; }

// ─── Helpers ─────────────────────────────────────────────────────────────────
function getEffectiveBrand(profileName) {
  // Import-free lookup using profile name string
  const lower = profileName.toLowerCase();
  if (lower.includes('galaxy') || lower.startsWith('sm-')) return 'samsung';
  if (lower.includes('pixel')) return 'google';
  if (lower.includes('poco')) return 'poco';
  if (lower.startsWith('redmi')) return 'redmi';
  if (lower.includes('mi ') || lower.startsWith('xiaomi')) return 'xiaomi';
  if (lower.includes('oneplus') || lower.startsWith('op')) return 'oneplus';
  if (lower.startsWith('moto')) return 'motorola';
  if (lower.includes('nokia')) return 'nokia';
  if (lower.includes('realme')) return 'realme';
  if (lower.includes('asus')) return 'asus';
  return 'default';
}

function isAdreno(profileName) {
  const n = profileName.toLowerCase();
  return n.includes('poco') || n.includes('mi 10') || n.includes('mi 11') ||
         n.includes('note 10 pro') || n.includes('note 9 pro') || n.includes('note 10') ||
         n.includes('poco f3') || n.includes('poco x3') || n.includes('x3 nfc') ||
         n.includes('galaxy a52') || n.includes('galaxy a72') || n.includes('galaxy s20') ||
         n.includes('oneplus') || n.includes('pixel') || n.includes('moto') ||
         n.includes('nokia 8') || n.includes('realme');
}

// ─── Validators ──────────────────────────────────────────────────────────────
export function validateIMEI(v) {
  if (!/^\d{15}$/.test(v)) return false;
  let sum = 0;
  for (let i = 0; i < 14; i++) {
    let d = parseInt(v[i]);
    if (i % 2 === 1) { d *= 2; if (d > 9) d -= 9; }
    sum += d;
  }
  return (10 - (sum % 10)) % 10 === parseInt(v[14]);
}

export function validateIMSI(v) {
  if (!/^\d{15}$/.test(v)) return false;
  const pfx = v.substr(0,6);
  return IMSI_POOLS.includes(pfx);
}

export function validateICCID(v) {
  if (!/^\d{19,20}$/.test(v) || !v.startsWith('89101')) return false;
  const base = v.slice(0,-1);
  let sum = 0;
  for (let i = 0; i < base.length; i++) {
    let d = parseInt(base[i]);
    if (i % 2 === 1) { d *= 2; if (d > 9) d -= 9; }
    sum += d;
  }
  return (10 - (sum % 10)) % 10 === parseInt(v.slice(-1));
}

export function validateMAC(v) { return /^([0-9a-f]{2}:){5}[0-9a-f]{2}$/i.test(v); }
export function validatePhone(v) { return /^\+1[2-9]\d{2}[2-9]\d{6}$/.test(v); }
export function validateAndroidId(v) { return /^[0-9a-f]{16}$/.test(v); }
export function validateGsfId(v) { return /^[0-9a-f]{16}$/.test(v); }
export function validateWidevineId(v) { return /^[0-9a-f]{32}$/.test(v); }
export function validateBootId(v) {
  return /^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/i.test(v);
}
export function validateSerial(v) { return v && v.length >= 8 && /^[A-Z0-9]+$/i.test(v); }

// ─── Correlation score ────────────────────────────────────────────────────────
export function computeCorrelation(profileName, seed, overrides = {}) {
  if (!profileName || !seed) return 0;
  let score = 0, max = 0;

  const check = (weight, condition) => { max += weight; if (condition) score += weight; };

  // Profile exists
  max += 5; score += 5; // always if we got here

  // IMEI TAC matches brand
  const imei = overrides.imei || generateIMEI(profileName, seed);
  const brand = getEffectiveBrand(profileName).toLowerCase();
  const allTacs = Object.values(TACS).flat();
  const brandTacs = TACS[brand] || TACS.default;
  check(20, brandTacs.some(t => imei.startsWith(t)));

  // IMSI is valid US carrier
  const imsi = overrides.imsi || generateIMSI(profileName, seed);
  check(20, validateIMSI(imsi));

  // ICCID valid format
  const iccid = overrides.iccid || generateICCID(profileName, seed);
  check(15, validateICCID(iccid));

  // Phone number valid NANP
  const phone = overrides.phone || generatePhoneNumber(profileName, seed);
  check(15, validatePhone(phone));

  // MAC address valid
  const mac = overrides.wifiMac || generateMAC(brand, seed);
  check(10, validateMAC(mac));

  // IMEI Luhn valid
  check(10, validateIMEI(imei));

  // Carrier coherence with IMSI
  const mccMnc = imsi.substr(0,6);
  check(5, IMSI_POOLS.includes(mccMnc));

  return Math.round((score / max) * 100);
}

// ─── UUID v4 generator (for Media DRM ID, Advertising ID) ────────────────────
export function generateUUID(seed) {
  const rng = new MT64(seed);
  const h8 = () => (Number(rng.gen() & 0xFFFFFFFFn) >>> 0).toString(16).padStart(8, '0');
  const h4 = () => Number(rng.gen() & 0xFFFFn).toString(16).padStart(4, '0');
  const a  = h8();
  const b  = h4();
  const c  = ((Number(rng.gen() & 0x0FFFn)) | 0x4000).toString(16).padStart(4, '0');
  const d  = ((Number(rng.gen() & 0x3FFFn)) | 0x8000).toString(16).padStart(4, '0');
  const e  = h8() + h4();
  return `${a}-${b}-${c}-${d}-${e}`;
}

// ─── WiFi SSID generator ──────────────────────────────────────────────────────
const _SSID_PFX = ['HOME','NETGEAR','TP-LINK','ASUS','Linksys','XFINITY','Spectrum','ATT','Verizon','MyWiFi'];
export function generateWifiSsid(seed) {
  const rng = new MT64(seed + 777);
  const pfx = _SSID_PFX[Number(rng.gen() % BigInt(_SSID_PFX.length))];
  const sfx = Number(rng.gen() & 0xFFFFn).toString(16).toUpperCase().padStart(4, '0');
  return `${pfx}-${sfx}`;
}

// ─── Gmail account generator ──────────────────────────────────────────────────
const _FN = ['alex','jordan','morgan','taylor','casey','riley','avery','blake','quinn','reese','sage','drew','cameron','harper','skyler'];
const _LN = ['smith','johnson','williams','brown','jones','davis','miller','wilson','moore','anderson','thomas','jackson','white','harris','martin'];
export function generateGmail(seed) {
  const rng = new MT64(seed + 999);
  const fn  = _FN[Number(rng.gen() % BigInt(_FN.length))];
  const ln  = _LN[Number(rng.gen() % BigInt(_LN.length))];
  const num = Number(rng.gen() % 900n) + 100;
  return `${fn}.${ln}${num}@gmail.com`;
}

export { US_CITIES_100, CARRIER_NAMES, IMSI_POOLS };
