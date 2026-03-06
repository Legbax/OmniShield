# Google Maps location spoofing: PR119 quick guide

This project now uses a PR119 "Radar Doppler" strategy in Binder (`my_jbbinder_ontransact`) to mutate location parcels without relying on a fixed transaction code.

## What changed in PR119

1. **No hard dependency on PR118 symbol resolution** (`Location_readFromParcel`).
2. **No strict `code == 1` dependency** for location callbacks.
3. **Heuristic Binder token detection** (`location` / `fused`) + dynamic parcel mutation.
4. **Doppler fallback scan** over parcel offsets when known AOSP/GMS offsets drift.
5. **Producer auto-scope** for early location sources:
   - `com.mediatek.location.ppe.main`
   - `com.xiaomi.location.fused`
   - `com.android.location.fused`

## Required scope (recommended)

Keep these packages inside `scoped_apps`:

- `com.google.android.apps.maps`
- `com.google.android.gms`
- `com.mediatek.location.ppe.main`
- `com.xiaomi.location.fused`
- `com.android.location.fused`

> Note: PR119 already auto-scopes the producer processes above as a safety net.

## Logcat verification

When opening Google Maps, check:

```bash
adb logcat | grep -E "PR119|PR115|PR105|scope"
```

Expected indicators:

- `[PR115] JavaBBinder::onTransact hooked OK ...`
- `[PR119] mutated BEFORE orig: ...`
- `[PR119][scope] producer auto-scope: ...` (on producer processes)

If PR119 logs never appear in Maps/GMS/producer processes, verify that the module is loaded in those processes and that `debug_mode=true` in config.
