# Google Maps location spoofing: PR119 quick guide

This project now uses a PR119 "Radar Doppler" strategy in Binder (`my_jbbinder_ontransact`) to mutate location parcels without relying on a fixed transaction code.

## What changed in PR119

1. **No hard dependency on PR118 symbol resolution** (`Location_readFromParcel`).
2. **No strict `code == 1` dependency** for location callbacks.
3. **No strict `getLastLocation/getCurrentLocation` transaction code dependency** in IPC request detection (`PR105c`).
4. **Heuristic Binder token detection** (`location` / `fused`) + dynamic parcel mutation.
5. **Doppler fallback scan** over parcel offsets when known AOSP/GMS offsets drift.
6. **Producer auto-scope** for early location sources:
   - `com.mediatek.location.ppe.main`
   - `com.xiaomi.location.fused`
   - `com.android.location.fused`
7. **PR120 Ashmem tracer** logs when scoped processes use `/dev/ashmem` file descriptors via `open/read/pread/mmap/mmap64`.
   - PR120 uses FD classification cache (ashmem/non-ashmem) to avoid repeated probes in hot `read()` paths.
   - PR120 resolves FD symlinks with direct syscall `readlinkat` (no dependency on `orig_readlinkat` hook availability).

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
adb logcat | grep -E "PR120|PR119|PR115|PR105|PR105c|scope"
```

Expected indicators:

- `[PR115] JavaBBinder::onTransact hooked OK ...`
- `[PR105c] location request candidate: ...`
- `[PR119] mutated BEFORE orig: ...`
- `[PR119][scope] producer auto-scope: ...` (on producer processes)
- `[PR120] ashmem fd observed via mmap/read/...` (evidence of shared-memory path)
- `[PR120-PULSE] mmap/mmap64 hook active ...` (confirms mmap hooks are executing even before ashmem classification logs)

If startup shows `PR120: mmap symbol unresolved` / `mmap64 symbol unresolved`, PR120 now tries a resolver chain (`DobbySymbolResolver` → `dlsym(RTLD_DEFAULT)` → `dlopen(libc)+dlsym`, plus `__mmap2` alias fallback for mmap64-compatible paths).
If logs show identical addresses for both hooks (e.g. `mmap` and `mmap64` at the same pointer), PR120 uses **single-hook mode** to avoid double-hooking the same symbol.
If PR119 logs never appear in Maps/GMS/producer processes, verify that the module is loaded in those processes and that `debug_mode=true` in config.
