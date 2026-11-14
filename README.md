# Muxless VR Bridge

Run SteamVR on a laptop’s discrete GPU when the headset’s display connector is wired to the integrated GPU.

The bridge renders SteamVR’s final image in discrete-GPU memory, copies the completed image into an exportable scanout buffer, and presents that buffer through the display-owning GPU’s DRM lease. The display GPU does not render the scene. Resolution, refresh rate and game quality remain unchanged.

This package contains the accepted minimal bridge and a **separate SteamVR patchset**. SteamVR and Oasis are required for the currently supported WMR setup. Development and headset testing use Mesa, Wayland and a Lenovo Explorer; other GPU/driver combinations need compatible Vulkan, DMA-BUF, synchronization and DRM-leasing support. Universal hardware compatibility and the 0.1 ms added-latency target have not been demonstrated.

## Install

Download `Muxless-VR-Bridge-0.2-x86_64.AppImage` from Releases, close SteamVR and fully quit Steam, then run:

```sh
chmod +x Muxless-VR-Bridge-0.2-x86_64.AppImage
./Muxless-VR-Bridge-0.2-x86_64.AppImage --install
```

Installation checks dependencies, discovers native Steam libraries, installs both layers for your user and configures SteamVR’s launch option in existing Steam profiles. No application-menu shortcut is required. If the dGPU driver exposes a named VR workload profile, installation asks for administrator authorization once for the bundled native profile helper. Steam, SteamVR and games remain unprivileged. If SteamVR has not appeared in a Steam profile yet, the installer prints the launch option to put in **SteamVR → Properties → Launch Options**.

Start Steam on the discrete GPU:

```sh
~/.local/bin/wmr-gpu-bridge --steam
```

Start SteamVR from your library and launch games normally. You may instead start Steam using `DRI_PRIME=1 steam`; the installed SteamVR launch option activates the bridge. Fully quit an existing Steam session before changing its GPU environment.

The installed launcher can also wrap a command directly:

```sh
~/.local/bin/wmr-gpu-bridge --command COMMAND [ARGUMENTS...]
```

The `wmr-gpu-bridge` filename and installation receipt paths remain stable for compatibility with earlier installers and uninstallers.

## Automatic GPU VR mode

When the bridge creates its VR render device, it detects the selected dGPU from Vulkan's DRM properties and requests the driver's named `VR` workload profile, if available. It reads the profile ID from the driver; GPU models, PCI addresses and profile numbers are not hardcoded. It changes no resolution, refresh rate, image quality, frequency limit, power limit or fan setting.

The bundled `muxless-vr-profile` helper needs one-time administrator authorization during `--install` because Linux restricts these driver controls. After opening the validated control, it permanently drops root privileges. A native, unprivileged lease service shares the setting across contexts/processes and restores the previous profile after the last VR client exits, including a compositor crash. It has no per-frame work and does not require GameMode, LACT or Python at runtime. Its files remain within the existing uninstall ownership scope.

AMD's `amdgpu` driver exposes this named profile and is tested on the RX 6800M. The researched NVIDIA and Intel Linux interfaces do not expose an equivalent named VR mode: they are left unchanged. Maximum-performance, fixed-clock, compute and inference modes are not silently substituted. Public beta testing should report unsupported or failed capabilities as such. A user-selected custom profile is preserved.

For a diagnostic comparison, set `MUXLESS_VR_PROFILE=0` before launching Steam through the bridge. Restart the entire VR session when changing this option.

## SteamVR patches

The AppImage bundles three repairs in a separate SteamVR patch layer. They address compositor failures independently of the display bridge and do not modify SteamVR or game files.

### 1 — Descriptor cache ownership

**Problem:** On the verified SteamVR builds, dashboard content and other compositor activity can exhaust a Vulkan descriptor pool. Descriptors tell shaders which images and buffers to use. When the compositor's fixed pool runs out, a failed allocation can crash SteamVR.

**Repair:** The patch tracks which backing pool owns each descriptor set, adds backing pools when the affected allocation needs capacity, and routes frees, resets and destruction to the correct owners. It also reclaims empty backing pools. This manages the allocation lifecycle rather than permanently inflating every pool or discarding live allocations.

### 2 — Shader/layout compatibility

**Problem:** The verified SteamVR 2.19.2 executable contains a shader/pipeline-layout combination with an incompatible uniform-buffer access. On the tested graphics stack, it can crash the compositor soon after the first frame or when displaying overlays.

**Repair:** The patch recognizes that specific shader and incompatible layout, then adapts the shader during pipeline construction. It does not rewrite unrelated shaders or run a shader-repair pass on every frame. This repair is eligible only for the verified **2.19.2** binary; it stays inactive on **2.19.3**.

### 3 — Upload staging retirement

**Problem:** The affected compositor upload allocator can retain already-used staging reservations until its fixed upload space is exhausted. Uploading new dashboard or application textures can then lead to an assertion or compositor crash.

**Repair:** The patch tracks the identified staging allocation, the commands using it and their GPU-completion fences. It permits reuse only after the tracked work has completed and the allocator identity and reservation boundaries still match. It polls completion rather than idling the entire GPU, and disables its intervention if its safety checks fail.

### Supported builds and safeguards

Patches 1 and 3 are eligible for the verified SteamVR **2.19.2 and 2.19.3** compositor executables. Patch 2 is eligible only for the verified **2.19.2** executable. Eligibility uses the executable's full SHA-256, not just its displayed version. Unknown or updated binaries pass through without these build-dependent repairs until reviewed. This prevents an old repair from blindly modifying a new upstream implementation; it does not automatically determine whether every upstream bug is fixed.

See [patch applicability](patches/steamvr/README.md) for the exact allowlist.

### How to use the patches

Installation selects all three repairs by default. Your selections are preserved across upgrades; a selected repair runs only on an eligible executable. Disabling a required repair can reintroduce crashes.

Show each patch's purpose, ID and current selection:

```sh
./Muxless-VR-Bridge-0.2-x86_64.AppImage --toggle-patches help
```

Toggle one patch using its number. For example, toggle descriptor cache ownership:

```sh
./Muxless-VR-Bridge-0.2-x86_64.AppImage --toggle-patches 1
```

Use `2` for shader/layout compatibility or `3` for upload staging retirement. To toggle the entire set:

```sh
./Muxless-VR-Bridge-0.2-x86_64.AppImage --toggle-patches all
```

`all` disables every patch if all are selected; otherwise it enables every patch. **Restart SteamVR after changing a selection.** These switches do not restart it automatically. No argument displays the same help as `help`; the installed `~/.local/bin/wmr-gpu-bridge` launcher accepts these options too. Older `SVR-001`-style IDs and the single-dash `-toggle-patches` spelling remain supported.

## Check an installation

- `--doctor`: discover SteamVR/Oasis paths and report host-library, Vulkan and Wayland capabilities. Use it when installation or GPU/display discovery fails. Advertised support is not a headset presentation test.
- `--verify`: check installed file hashes, registration and library loading. Use it after installation or an upgrade.
- `--verify --runtime`: attempt to check that the installed bridge is mapped in a running compositor. Linux may deny access to capability-protected compositor processes, causing an unverified result even while the bridge is running. This does **not** prove correct images, frame delivery or latency.
- `--logs --follow`: follow the newest managed command’s error log. SteamVR launches made through the installed wrapper create session directories under `${XDG_STATE_HOME:-$HOME/.local/state}/wmr-gpu-bridge/runs/`; the last three are kept. The bridge logs startup and errors, without per-frame tracing.

Pass these options to the AppImage or the installed `~/.local/bin/wmr-gpu-bridge` launcher. SteamVR’s own detailed logs remain in Steam’s `logs` directory.

If FUSE is unavailable, prefix AppImage commands with `APPIMAGE_EXTRACT_AND_RUN=1`.

## Uninstall

Close SteamVR and fully quit Steam, then run:

```sh
./Muxless-VR-Bridge-0.2-x86_64.AppImage --uninstall
```

The schema-1 removal protocol supports uninstalling across package versions. It removes owned files and the bridge’s SteamVR launch wrapper, preserves modified files, logs and calibration, and does not require Oasis or a working GPU. New versions retain the same receipt location and allowed file layout so older uninstallers can remove them. See [the removal protocol](shim/appimage/REMOVAL-PROTOCOL.md).

## Build from source

For a native build, install a C++17 compiler, Python 3, pkg-config, libdrm development files, Vulkan headers/loader development files and Wayland client development files:

```sh
./shim/build.sh
```

Executables and both libraries are written to `shim/build/`. Native builds use the host’s C++ runtime and libc.

To build the distributable AppImage, also install rootless Podman and host Vulkan Video headers (`/usr/include/vk_video`). Internet access is needed to fetch the pinned container/tool when uncached:

```sh
./shim/appimage/build.sh
```

This verifies vendored source hashes, compiles against a pinned glibc 2.28 container with static C++ runtimes, and assembles the AppImage using a checksum-pinned tool. Output is `shim/dist/Muxless-VR-Bridge-0.2-x86_64.AppImage` and its SHA-256 sidecar. Host graphics drivers and Vulkan/Wayland/libdrm libraries are used at runtime; they are not bundled. x86-64 glibc Linux is the packaged target; musl systems and sandboxed Steam are not validated.

## Source layout

- `runtime/`: native, capability-detected GPU profile leases and the restricted installation helper.
- `src/`: the single accepted transport implementation: dispatch, display lease, buffer lifecycle and swapchain.
- `patches/steamvr/`: independent, build-guarded SteamVR repairs.
- `shim/launcher.cpp`: dGPU launch environment and bounded session-log retention.
- `shim/appimage/`: installer, diagnostics, removal protocol and portable build/assembly.
- `shim/third_party/`: vendored installer/hash dependencies and their licenses.

GPL-3.0-or-later. Vendored dependencies retain their notices.
