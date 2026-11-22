# Muxless VR Bridge

Run SteamVR on a laptop’s discrete GPU when the headset’s display connector is wired to the integrated GPU.

SteamVR composes the headset image on the discrete GPU. The bridge uses a Vulkan GPU copy to place the full uncompressed frame in a DMA-BUF and queues it to the display-owning GPU through the runtime’s DRM lease. A Linux sync-file fence orders scanout after the copy. The bridge uses the headset’s advertised display mode and real KMS timing.

The headset path requires a render device with Vulkan DMA-BUF and sync-file support and a display plane that can import a compatible linear framebuffer. On Wayland the compositor must grant the runtime a DRM lease for the headset connector. SteamVR is the supported runtime. The independent SteamVR patches are available as an opt-in.

See the [portability audit](docs/portability-audit.md) for the design and compatibility requirements.

## Install

Download `Muxless-VR-Bridge-0.3.1-alpha-x86_64.AppImage` from Releases, close SteamVR and fully quit Steam, then run the following commands.

```sh
chmod +x Muxless-VR-Bridge-0.3.1-alpha-x86_64.AppImage
./Muxless-VR-Bridge-0.3.1-alpha-x86_64.AppImage --install
```

Installation checks dependencies, installs the bridge for your user, and configures SteamVR’s launch option in existing Steam profiles. If SteamVR has yet to appear in a Steam profile, the installer prints the launch option to put in **SteamVR → Properties → Launch Options**.

Start Steam on the discrete GPU:

```sh
~/.local/bin/wmr-gpu-bridge --steam
```

Fully quit an existing Steam session before using this command. Start SteamVR from your library and launch games normally. The adapter matches the compositor’s Vulkan device against the discrete GPU selected by the launcher.

The installed launcher can also wrap a command directly:

```sh
~/.local/bin/wmr-gpu-bridge --command COMMAND [ARGUMENTS...]
```

The `wmr-gpu-bridge` filename and installation receipt paths remain stable for compatibility with earlier installers and uninstallers.

## SteamVR patches

The AppImage bundles three optional repairs in a separate SteamVR patch layer. Fresh installations start with all three disabled. Updates preserve existing selections.

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

Fresh installations start with all three repairs unselected. Your selections are preserved across upgrades; a selected repair runs only on an eligible executable. Disabling a required repair can reintroduce crashes.

Show each patch's purpose, ID and current selection:

```sh
./Muxless-VR-Bridge-0.3.1-alpha-x86_64.AppImage --toggle-patches help
```

Toggle one patch using its number. For example, toggle descriptor cache ownership:

```sh
./Muxless-VR-Bridge-0.3.1-alpha-x86_64.AppImage --toggle-patches 1
```

Use `2` for shader/layout compatibility or `3` for upload staging retirement. To toggle the entire set:

```sh
./Muxless-VR-Bridge-0.3.1-alpha-x86_64.AppImage --toggle-patches all
```

`all` disables every patch if all are selected; otherwise it enables every patch. **Restart SteamVR after changing a selection.** These switches do not restart it automatically. No argument displays the same help as `help`; the installed `~/.local/bin/wmr-gpu-bridge` launcher accepts these options too. Older `SVR-001`-style IDs and the single-dash `-toggle-patches` spelling remain supported.

The separate [Room Setup exit repair](patches/steamvr/README.md#003a--room-setup-exit) is available as `--room-setup-fix apply`. Close SteamVR before applying or restoring it.

## Check an installation

- `--doctor`: discover SteamVR and report host-library, Vulkan and Wayland capabilities. Oasis is reported if installed, but its absence does not block installation. Use it when installation or GPU/display discovery fails. Advertised support is not a headset presentation test.
- `--verify` checks the receipt, layer activation settings, loader order and library loading. Use it after installation or an upgrade.
- `--verify --runtime` also looks for the installed bridge and its activation environment in the running compositor. Linux permissions can make this check unverifiable. The compositor log records the DRM lease, tested mode and buffer layout, and `first headset page flip` when scanout begins.
- `--logs --follow`: follow the newest managed command’s error log. SteamVR launches made through the installed wrapper create session directories under `${XDG_STATE_HOME:-$HOME/.local/state}/wmr-gpu-bridge/runs/`; the last three are kept. The bridge logs startup and errors, without per-frame tracing.

Pass these options to the AppImage or the installed `~/.local/bin/wmr-gpu-bridge` launcher. SteamVR’s own detailed logs remain in Steam’s `logs` directory.

If FUSE is unavailable, prefix AppImage commands with `APPIMAGE_EXTRACT_AND_RUN=1`.

## VR workload profile

On a GPU that advertises a named `VR` workload profile, the bridge can select it for the SteamVR session and restore the previous profile when the last client exits. Authorize the optional helper once after installation. The helper only changes the advertised workload profile.

```sh
sudo ~/.local/lib/wmr-gpu-bridge/0.3.1-alpha/muxless-vr-profile --authorize
```

Set `MUXLESS_VR_PROFILE=0` before launching Steam to skip profile selection. See [profile detection and authorization](runtime/README.md).

## Uninstall

Close SteamVR and fully quit Steam, then run:

```sh
./Muxless-VR-Bridge-0.3.1-alpha-x86_64.AppImage --uninstall
```

The schema-1 removal protocol supports uninstalling across package versions. It removes owned files and the bridge’s SteamVR launch wrapper, preserves modified files, logs and calibration, and does not require Oasis or a working GPU. New versions retain the same receipt location and allowed file layout so older uninstallers can remove them. See [the removal protocol](shim/appimage/REMOVAL-PROTOCOL.md).

## Build from source

For a native build, install a C++17 compiler, pkg-config, libdrm development files, Vulkan 1.4 headers and loader development files, and Wayland client development files for the diagnostic probe.

```sh
./shim/build.sh
```

Executables and both libraries are written to `shim/build/`. Native builds use the host’s C++ runtime and libc.

The laptop-window benchmark is a developer build selected with `./shim/build.sh --desktop-fixture`. It also requires `wayland-scanner`, Wayland protocols, and X11/XRandR headers.

To build the distributable AppImage, install Python 3 and rootless Podman. Uncached builds fetch the pinned container, build tools and Vulkan headers.

```sh
./shim/appimage/build.sh
```

The build verifies its pinned inputs and compiles against glibc 2.28 with static C++ runtimes. The AppImage and its SHA-256 sidecar are written to `shim/dist/` using `shim/VERSION` in the existing filename format. The bridge uses the host Vulkan stack and libdrm. The diagnostic probe also uses the Wayland client library. The packaged target is native Steam on x86-64 glibc Linux.

Run `./shim/appimage/build.sh --payload-only` to build the same portable libraries and executables in `shim/build-portable/` without assembling the AppImage.

## Source layout

- `src/`: the single accepted transport implementation: dispatch, display lease, buffer lifecycle and swapchain.
- `patches/steamvr/`: independent, build-guarded SteamVR repairs.
- `shim/launcher.cpp`: dGPU launch environment and bounded session-log retention.
- `shim/appimage/`: installer, diagnostics, removal protocol and portable build/assembly.
- `shim/third_party/`: vendored installer/hash dependencies and their licenses.

GPL-3.0-or-later. Vendored dependencies retain their notices.
