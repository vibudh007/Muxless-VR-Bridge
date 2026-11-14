# SteamVR runtime patchset

This library is packaged with Muxless VR Bridge but developed separately from its generic display transport. The transport does not call into these repairs. The launcher activates the repair layer; the library identifies `vrcompositor` by executable name and full SHA-256 before enabling any build-dependent hook.

| SteamVR compositor | SHA-256 | Eligible patches |
| --- | --- | --- |
| 2.19.2 | `7695e718ee0d070df596663d737b6a183aa650a2c66b90a035cc7f227a7a49e1` | 001, 002, 006 |
| 2.19.3 | `b3747b541c86f683286f13fcce9ed7761e0258ecef3827c5ce915167226947fe` | 001, 006 |
| Any other executable | Any other hash | None |

An eligible patch also needs to be selected. A SteamVR update does not inherit eligibility merely because its version string looks similar. A verified new executable must be reviewed and explicitly added to `tested_build.hpp`.

## 001 — Descriptor cache ownership

`descriptor_arena.hpp` tracks application descriptor pools, allocates backing pools when required, routes sets to their owners, and handles free/reset/destruction. This repairs exhaustion without a fixed arbitrary increase of every pool. The native pool’s public handle remains unchanged.

## 002 — Shader/layout compatibility

`shader_adapter.hpp` and `shader_compat.hpp` recognize the affected shader/layout combination and repair construction of that pipeline. There are no per-frame shader-repair hooks. The 2.19.2 repair is not assumed valid for 2.19.3.

## 006 — Upload staging retirement

`transfer_ring.hpp` tracks the identified upload reservation and the submissions using it, retaining synchronization until GPU completion before retiring a reservation. It prevents cumulative staging exhaustion. Its build-dependent assumptions are protected by the executable allowlist.

## Controls

`--toggle-patches [help | all | 1 | 2 | 3]` on the AppImage or installed launcher changes installation-owned settings. Both `-toggle-patches` and `SVR-001`-style IDs are accepted. IDs `1`, `2`, and `3` select SVR-001, SVR-002, and SVR-006 respectively; the older IDs remain accepted. Restart SteamVR to apply them. Diagnostics are not enabled by these switches.

Settings live under `PREFIX/lib/wmr-gpu-bridge/patches/`, within the frozen removal protocol’s ownership scope. The launcher passes that directory through `MUXLESS_STEAMVR_PATCH_CONFIG`. Settings are checksummed in the install receipt and changed with a write-ahead ownership record; upgrades preserve them. The original development XDG location is a fallback only when that environment variable is absent.

No game files are modified.
