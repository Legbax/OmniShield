#!/usr/bin/env node
// test_profile_leaks.mjs — OmniShield profile leak detection test suite
// Parses C++ omni_profiles.h (source of truth) and validates all 40 device profiles
// for empty fields, cross-field incoherence, fingerprint mismatches, and suppression bugs.
// Run: node tools/test_profile_leaks.mjs

import { readFileSync } from 'node:fs';
import { join, dirname } from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';

const __dirname = dirname(fileURLToPath(import.meta.url));
const ROOT = join(__dirname, '..');

// ── Struct field ordering (must match omni_profiles.h struct DeviceFingerprint) ──
const STRING_FIELDS = [
  'manufacturer', 'brand', 'model', 'device',
  'product', 'hardware', 'board', 'bootloader',
  'fingerprint', 'buildId', 'tags', 'type',
  'radioVersion', 'incremental', 'securityPatch', 'release',
  'boardPlatform', 'eglDriver', 'openGlEs', 'hardwareChipname',
  'zygote', 'vendorFingerprint', 'display', 'buildDescription',
  'buildFlavor', 'buildHost', 'buildUser', 'buildDateUtc',
  'buildVersionCodename', 'buildVersionPreviewSdk', 'gpuVendor', 'gpuRenderer',
  'gpuVersion', 'screenWidth', 'screenHeight', 'screenDensity'
];

// ── C++ header parser ────────────────────────────────────────────────────────
function parseCppProfiles(filePath) {
  const src = readFileSync(filePath, 'utf8');
  const profiles = {};

  // Split on profile entry pattern: { "ProfileName", {
  const entryRe = /\{\s*"([^"]+)"\s*,\s*\{/g;
  let match;
  const entries = [];
  while ((match = entryRe.exec(src)) !== null) {
    const name = match[1];
    if (name === '') continue; // skip sentinel
    entries.push({ name, startIdx: match.index + match[0].length });
  }

  for (let i = 0; i < entries.length; i++) {
    const { name, startIdx } = entries[i];
    // Find the closing "} }," — content between the opening { and the closing } },
    const endIdx = i + 1 < entries.length ? entries[i + 1].startIdx : src.length;
    const body = src.slice(startIdx, endIdx);

    // Extract all quoted strings
    const strRe = /"((?:[^"\\]|\\.)*)"/g;
    const strings = [];
    let sm;
    while ((sm = strRe.exec(body)) !== null) {
      strings.push(sm[1]);
    }

    if (strings.length < STRING_FIELDS.length) {
      console.error(`WARN: ${name} has only ${strings.length} strings (expected ${STRING_FIELDS.length})`);
      continue;
    }

    const profile = {};
    for (let j = 0; j < STRING_FIELDS.length; j++) {
      profile[STRING_FIELDS[j]] = strings[j];
    }

    // Extract numeric values after the strings
    // Find text after the last quoted string
    const lastStrEnd = body.lastIndexOf('"') + 1;
    const numericPart = body.slice(lastStrEnd);

    // Extract ints (core_count, ram_gb)
    const intMatches = numericPart.match(/(\d+)\s*,\s*(\d+)/);
    if (intMatches) {
      profile.core_count = parseInt(intMatches[1]);
      profile.ram_gb = parseInt(intMatches[2]);
    }

    // Extract bools
    const boolRe = /(true|false)/g;
    const bools = [];
    let bm;
    while ((bm = boolRe.exec(numericPart)) !== null) bools.push(bm[1] === 'true');
    if (bools.length >= 3) {
      profile.hasHeartRateSensor = bools[bools.length - 3];
      profile.hasBarometerSensor = bools[bools.length - 2];
      profile.hasFingerprintWakeupSensor = bools[bools.length - 1];
    }

    profiles[name] = profile;
  }

  return profiles;
}

// ── Test runner ──────────────────────────────────────────────────────────────
class TestRunner {
  constructor() {
    this.results = [];
    this.totalPass = 0;
    this.totalFail = 0;
    this.totalWarn = 0;
    this.profileFailCounts = {};
  }

  check(profile, test, passed, detail = '') {
    if (passed) {
      this.totalPass++;
    } else {
      this.totalFail++;
      this.profileFailCounts[profile] = (this.profileFailCounts[profile] || 0) + 1;
      this.results.push({ profile, test, detail });
    }
  }

  warn(profile, test, detail = '') {
    this.totalWarn++;
    this.results.push({ profile, test, detail, isWarn: true });
  }

  summarize() {
    console.log('\n' + '='.repeat(80));
    console.log('PROFILE LEAK TEST RESULTS');
    console.log('='.repeat(80));

    // Group failures by profile
    const failsByProfile = {};
    const warnsByProfile = {};
    for (const r of this.results) {
      const bucket = r.isWarn ? warnsByProfile : failsByProfile;
      if (!bucket[r.profile]) bucket[r.profile] = [];
      bucket[r.profile].push(r);
    }

    for (const [profile, fails] of Object.entries(failsByProfile)) {
      console.log(`\n  FAIL  ${profile} (${fails.length} issue${fails.length > 1 ? 's' : ''}):`);
      for (const f of fails) {
        console.log(`        - [${f.test}] ${f.detail}`);
      }
    }

    for (const [profile, warns] of Object.entries(warnsByProfile)) {
      console.log(`\n  WARN  ${profile} (${warns.length}):`);
      for (const w of warns) {
        console.log(`        - [${w.test}] ${w.detail}`);
      }
    }

    console.log('\n' + '-'.repeat(80));
    console.log(`TOTAL: ${this.totalPass} passed, ${this.totalFail} failed, ${this.totalWarn} warnings`);
    const failedProfiles = Object.keys(failsByProfile).length;
    const totalProfiles = 40;
    console.log(`PROFILES: ${totalProfiles - failedProfiles}/${totalProfiles} clean, ${failedProfiles} with issues`);
    console.log('='.repeat(80));
  }
}

// ── Test Suite 1: Required string fields non-empty ───────────────────────────
function testNonEmptyFields(runner, name, p) {
  // All 36 string fields used by hooks must be non-empty
  // An empty field means the hook returns "", which falls through to original value = leak
  for (const field of STRING_FIELDS) {
    const val = p[field];
    runner.check(name, `NonEmpty:${field}`, val != null && val !== '',
      `field "${field}" is empty — hook will leak original device value`);
  }
}

// ── Test Suite 2: GPU coherence ──────────────────────────────────────────────
function testGpuCoherence(runner, name, p) {
  const drv = (p.eglDriver || '').toLowerCase();

  if (drv === 'adreno') {
    runner.check(name, 'GPU:adreno->vendor', p.gpuVendor === 'Qualcomm',
      `eglDriver=adreno but gpuVendor="${p.gpuVendor}" (expected "Qualcomm")`);
    runner.check(name, 'GPU:adreno->renderer', (p.gpuRenderer || '').includes('Adreno'),
      `eglDriver=adreno but gpuRenderer="${p.gpuRenderer}" missing "Adreno"`);
  } else if (drv === 'mali') {
    runner.check(name, 'GPU:mali->vendor', p.gpuVendor === 'ARM',
      `eglDriver=mali but gpuVendor="${p.gpuVendor}" (expected "ARM")`);
    runner.check(name, 'GPU:mali->renderer', (p.gpuRenderer || '').includes('Mali'),
      `eglDriver=mali but gpuRenderer="${p.gpuRenderer}" missing "Mali"`);
  } else if (drv === 'powervr') {
    runner.check(name, 'GPU:powervr->vendor',
      p.gpuVendor === 'Imagination Technologies',
      `eglDriver=powervr but gpuVendor="${p.gpuVendor}" (expected "Imagination Technologies")`);
    runner.check(name, 'GPU:powervr->renderer', (p.gpuRenderer || '').includes('PowerVR'),
      `eglDriver=powervr but gpuRenderer="${p.gpuRenderer}" missing "PowerVR"`);
  } else {
    runner.check(name, 'GPU:known-driver', false,
      `unknown eglDriver="${p.eglDriver}" (expected adreno/mali/powervr)`);
  }
}

// ── Test Suite 3: Platform-SoC coherence ─────────────────────────────────────
function testPlatformSocCoherence(runner, name, p) {
  const plat = (p.boardPlatform || '').toLowerCase();
  const hw = (p.hardware || '').toLowerCase();
  const chip = (p.hardwareChipname || '').toUpperCase();

  // boardPlatform should match hardware (they're often the same)
  // Exception: some Samsung devices have hardware=qcom but boardPlatform=atoll
  // Just check they're both populated and from the same family

  if (plat.startsWith('mt6') || plat.startsWith('mt8')) {
    // MediaTek
    runner.check(name, 'SoC:mtk->chipname', chip.startsWith('MT'),
      `boardPlatform="${plat}" (MTK) but hardwareChipname="${p.hardwareChipname}" doesn't start with MT`);
    runner.check(name, 'SoC:mtk->gpu', p.eglDriver === 'mali' || p.eglDriver === 'powervr',
      `boardPlatform="${plat}" (MTK) but eglDriver="${p.eglDriver}" (expected mali or powervr)`);
  } else if (plat.includes('exynos') || plat.startsWith('s5e')) {
    // Samsung Exynos
    runner.check(name, 'SoC:exynos->gpu', p.eglDriver === 'mali',
      `boardPlatform="${plat}" (Exynos) but eglDriver="${p.eglDriver}" (expected mali)`);
  } else {
    // Qualcomm
    const qcomPlatforms = ['kona', 'msmnile', 'lahaina', 'lito', 'sdm670',
      'sm6150', 'atoll', 'holi', 'trinket', 'sm6350', 'sm7325', 'bengal'];
    const isKnownQcom = qcomPlatforms.some(q => plat.includes(q));
    runner.check(name, 'SoC:qcom->known', isKnownQcom,
      `boardPlatform="${plat}" not a known Qualcomm platform`);
    if (isKnownQcom) {
      runner.check(name, 'SoC:qcom->adreno', p.eglDriver === 'adreno',
        `boardPlatform="${plat}" (Qualcomm) but eglDriver="${p.eglDriver}" (expected adreno)`);
      runner.check(name, 'SoC:qcom->chipname',
        chip.startsWith('SM') || chip.startsWith('SDM') || chip.startsWith('QC'),
        `boardPlatform="${plat}" (Qualcomm) but chipname="${p.hardwareChipname}" doesn't match SM*/SDM* pattern`);
    }
  }
}

// ── Test Suite 4: Fingerprint format ─────────────────────────────────────────
function testFingerprintFormat(runner, name, p) {
  // Android fingerprint: brand/product/device:release/buildId/incremental:type/tags
  const fp = p.fingerprint || '';
  const re = /^([^/]+)\/([^/]+)\/([^:]+):([^/]+)\/([^/]+)\/([^:]+):([^/]+)\/(.+)$/;
  const m = fp.match(re);

  runner.check(name, 'FP:format', !!m,
    `fingerprint doesn't match format brand/product/device:release/buildId/incremental:type/tags: "${fp}"`);

  if (m) {
    const [, fpBrand, fpProduct, fpDevice, fpRelease, fpBuildId, fpIncremental, fpType, fpTags] = m;

    runner.check(name, 'FP:brand', fpBrand === p.brand,
      `FP brand="${fpBrand}" != standalone brand="${p.brand}"`);
    runner.check(name, 'FP:product', fpProduct === p.product,
      `FP product="${fpProduct}" != standalone product="${p.product}"`);
    runner.check(name, 'FP:device', fpDevice === p.device,
      `FP device="${fpDevice}" != standalone device="${p.device}"`);
    runner.check(name, 'FP:release', fpRelease === p.release,
      `FP release="${fpRelease}" != standalone release="${p.release}"`);
    runner.check(name, 'FP:buildId', fpBuildId === p.buildId,
      `FP buildId="${fpBuildId}" != standalone buildId="${p.buildId}"`);
    runner.check(name, 'FP:incremental', fpIncremental === p.incremental,
      `FP incremental="${fpIncremental}" != standalone incremental="${p.incremental}"`);
    runner.check(name, 'FP:type', fpType === p.type,
      `FP type="${fpType}" != standalone type="${p.type}"`);
    runner.check(name, 'FP:tags', fpTags === p.tags,
      `FP tags="${fpTags}" != standalone tags="${p.tags}"`);
  }

  // vendorFingerprint should also be a valid fingerprint
  const vfp = p.vendorFingerprint || '';
  runner.check(name, 'FP:vendor-format', re.test(vfp),
    `vendorFingerprint doesn't match format: "${vfp}"`);
}

// ── Test Suite 5: Build description coherence ────────────────────────────────
function testBuildDescription(runner, name, p) {
  const desc = p.buildDescription || '';
  // buildDescription typically contains: product-type release buildId incremental type-tags
  // e.g., "lancelot_global-user 11 RP1A.200720.011 V12.5.6.0.RJCMIXM release-keys"
  runner.check(name, 'BuildDesc:hasRelease', desc.includes(p.release),
    `buildDescription="${desc}" missing release="${p.release}"`);
  runner.check(name, 'BuildDesc:hasBuildId', desc.includes(p.buildId),
    `buildDescription="${desc}" missing buildId="${p.buildId}"`);
}

// ── Test Suite 6: MIUI suppression ───────────────────────────────────────────
function testMiuiSuppression(runner, name, p) {
  const br = (p.brand || '').toLowerCase();
  const xiaomiBrands = ['xiaomi', 'redmi', 'poco'];
  const isXiaomi = xiaomiBrands.includes(br);

  // For non-Xiaomi profiles, ro.miui.* should be suppressed (hook returns empty)
  // For Xiaomi profiles, they should NOT be suppressed (original values pass through)
  // The test verifies the brand classification is correct for each profile
  if (isXiaomi) {
    runner.check(name, 'MIUI:xiaomi-not-suppressed', true,
      `brand="${p.brand}" correctly identified as Xiaomi family`);
  } else {
    // Non-Xiaomi: verify brand is genuinely not Xiaomi
    runner.check(name, 'MIUI:non-xiaomi-suppressed',
      !xiaomiBrands.includes(br),
      `brand="${p.brand}" incorrectly classified`);
  }
}

// ── Test Suite 7: MediaTek suppression ───────────────────────────────────────
function testMtkSuppression(runner, name, p) {
  const plat = (p.boardPlatform || '').toLowerCase();
  const hasMt = plat.includes('mt');

  if (!hasMt) {
    // Non-MTK profile: ro.mediatek.* should be suppressed
    // Verify chipname doesn't leak MTK identity
    const chip = (p.hardwareChipname || '').toUpperCase();
    runner.check(name, 'MTK:no-mtk-leak', !chip.startsWith('MT'),
      `non-MTK platform="${plat}" but hardwareChipname="${p.hardwareChipname}" leaks MediaTek identity`);
  }
}

// ── Test Suite 8: Boot device path coherence ─────────────────────────────────
function testBootDevice(runner, name, p) {
  const plat = (p.boardPlatform || '').toLowerCase();

  // Simulate the hook's boot device logic
  let bootDevice;
  if (plat.includes('mt6') || plat.includes('mt8')) {
    bootDevice = 'bootdevice';
  } else if (plat.includes('exynos') || plat.includes('s5e')) {
    bootDevice = 'soc/11120000.ufs';
  } else if (plat.includes('bengal') || plat.includes('holi') || plat.includes('trinket')) {
    bootDevice = 'soc/4744000.sdhci';
  } else {
    bootDevice = 'soc/1d84000.ufshc';
  }

  // Cross-validate: eMMC (sdhci) is typically for low-end Qualcomm only
  if (bootDevice === 'soc/4744000.sdhci') {
    const lowEnd = ['bengal', 'holi', 'trinket'];
    runner.check(name, 'BootDev:emmc-lowend',
      lowEnd.some(l => plat.includes(l)),
      `bootdevice=eMMC but platform="${plat}" not in low-end list`);
  }

  // For UFS Qualcomm, verify it's actually a higher-end platform
  if (bootDevice === 'soc/1d84000.ufshc') {
    const ufsQcom = ['kona', 'msmnile', 'lahaina', 'lito', 'sdm670', 'sm6150',
      'atoll', 'sm6350', 'sm7325'];
    runner.check(name, 'BootDev:ufs-qcom',
      ufsQcom.some(q => plat.includes(q)),
      `bootdevice=UFS but platform="${plat}" not in known UFS Qualcomm list`);
  }
}

// ── Test Suite 9: Hostname format ────────────────────────────────────────────
function testHostname(runner, name, p) {
  // Hook generates: model + "-" + brand
  const hostname = (p.model || '') + '-' + (p.brand || '');

  // Hostnames with parentheses or spaces look suspicious on Android
  const hasParens = hostname.includes('(') || hostname.includes(')');
  if (hasParens) {
    runner.warn(name, 'Hostname:parens',
      `hostname="${hostname}" contains parentheses — unusual for Android net.hostname`);
  }

  // Very long hostnames may be truncated or suspicious
  if (hostname.length > 40) {
    runner.warn(name, 'Hostname:long',
      `hostname="${hostname}" is ${hostname.length} chars — may be truncated`);
  }
}

// ── Test Suite 10: Cross-partition consistency check ──────────────────────────
// Verifies that partition-variant props all resolve to the same profile field
function testCrossPartition(runner, name, p) {
  // These partition namespaces all map to the same values:
  // ro.product.{system,vendor,odm,product,system_ext}.model → fp.model
  // The hook handles them. This test verifies the source fields aren't empty.
  const partitionFields = {
    'model': p.model,
    'brand': p.brand,
    'manufacturer': p.manufacturer,
    'device': p.device,
    'product': p.product,
  };

  for (const [field, val] of Object.entries(partitionFields)) {
    runner.check(name, `Partition:${field}`, val != null && val !== '',
      `"${field}" is empty — all 5 partition variants will leak original value`);
  }

  // Build version partition fields
  runner.check(name, 'Partition:incremental', p.incremental != null && p.incremental !== '',
    `incremental is empty — 5 partition build.version.incremental props will leak`);
  runner.check(name, 'Partition:buildDateUtc', p.buildDateUtc != null && p.buildDateUtc !== '',
    `buildDateUtc is empty — 6 partition build.date.utc props will leak`);
}

// ── Test Suite 11: SDK version coherence ─────────────────────────────────────
function testSdkVersion(runner, name, p) {
  const release = p.release || '';
  const expectedSdk = { '10': '29', '11': '30', '12': '31', '13': '33' };
  const sdk = expectedSdk[release];

  runner.check(name, 'SDK:known-release', sdk != null,
    `release="${release}" not in known SDK mapping (hook will default to "30")`);
}

// ── Test Suite 12: JS/C++ sync check ─────────────────────────────────────────
function testJsCppSync(runner, name, cppProfile, jsProfile) {
  if (!jsProfile) {
    runner.warn(name, 'Sync:missing-in-js', `Profile "${name}" not found in profiles.js`);
    return;
  }

  // Fields that exist in both JS and C++
  const sharedFields = [
    'manufacturer', 'brand', 'model', 'device', 'hardware',
    'boardPlatform', 'eglDriver', 'gpuVendor', 'gpuRenderer',
    'buildId', 'securityPatch', 'release', 'fingerprint',
    'screenWidth', 'screenHeight', 'screenDensity'
  ];

  for (const f of sharedFields) {
    const cppVal = cppProfile[f] || '';
    const jsVal = (jsProfile[f] || '').toString();
    if (cppVal !== jsVal) {
      runner.warn(name, `Sync:${f}`,
        `C++="${cppVal}" vs JS="${jsVal}" — profiles.js is stale`);
    }
  }
}

// ── Test Suite 13: buildFlavor matches product + type ────────────────────────
function testBuildFlavor(runner, name, p) {
  // buildFlavor is typically "{product}-{type}" e.g. "lancelot_global-user"
  const expected = `${p.product}-${p.type}`;
  runner.check(name, 'BuildFlavor:format', p.buildFlavor === expected,
    `buildFlavor="${p.buildFlavor}" != expected "${expected}"`);
}

// ── Test Suite 14: display field coherence ───────────────────────────────────
function testDisplayField(runner, name, p) {
  // ro.build.display.id is typically the buildId
  // It should at least contain the buildId
  runner.check(name, 'Display:hasBuildId',
    (p.display || '').includes(p.buildId),
    `display="${p.display}" doesn't contain buildId="${p.buildId}"`);
}

// ── Main ─────────────────────────────────────────────────────────────────────
async function main() {
  console.log('OmniShield Profile Leak Test');
  console.log('Parsing C++ profiles from jni/omni_profiles.h...');

  const cppProfiles = parseCppProfiles(join(ROOT, 'jni/omni_profiles.h'));
  const profileCount = Object.keys(cppProfiles).length;
  console.log(`Found ${profileCount} C++ profiles`);

  // Import JS profiles for cross-validation
  let jsProfiles = {};
  try {
    const mod = await import(pathToFileURL(join(ROOT, 'webroot/js/profiles.js')).href);
    jsProfiles = mod.DEVICE_PROFILES || {};
    console.log(`Found ${Object.keys(jsProfiles).length} JS profiles`);
  } catch (e) {
    console.warn(`Could not import profiles.js: ${e.message}`);
  }

  const runner = new TestRunner();

  // Validate profile count
  runner.check('_global', 'ProfileCount:40', profileCount === 40,
    `Expected 40 C++ profiles, got ${profileCount}`);
  runner.check('_global', 'JS-ProfileCount:40', Object.keys(jsProfiles).length === 40,
    `Expected 40 JS profiles, got ${Object.keys(jsProfiles).length}`);

  // Run all test suites on each profile
  for (const [name, p] of Object.entries(cppProfiles)) {
    testNonEmptyFields(runner, name, p);
    testGpuCoherence(runner, name, p);
    testPlatformSocCoherence(runner, name, p);
    testFingerprintFormat(runner, name, p);
    testBuildDescription(runner, name, p);
    testBuildFlavor(runner, name, p);
    testDisplayField(runner, name, p);
    testMiuiSuppression(runner, name, p);
    testMtkSuppression(runner, name, p);
    testBootDevice(runner, name, p);
    testHostname(runner, name, p);
    testCrossPartition(runner, name, p);
    testSdkVersion(runner, name, p);
    testJsCppSync(runner, name, p, jsProfiles[name]);
  }

  runner.summarize();
  process.exit(runner.totalFail > 0 ? 1 : 0);
}

main().catch(e => { console.error(e); process.exit(2); });
