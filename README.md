# WMR GPU Bridge

WMR GPU Bridge lets SteamVR use a headset connected to an integrated GPU while SteamVR and games render on the discrete GPU. It targets Linux hybrid-GPU systems such as muxless laptops with HDMI wired to the iGPU. On the tested setup, the bridge presents the finished frame from a shared image with zero explicit frame copies.

**Current release:** [0.14.0 portability alpha for Linux x86-64](https://github.com/vibudh007/wmr-gpu-bridge/releases/tag/v0.14.0-portability-alpha)

**Required on the tested setup:** Install both the bridge AppImage and the separate SteamVR shader workaround before starting SteamVR. The AppImage contains the bridge. Download both prebuilt components and their checksums from the release page.

## Install and play

### 1. Install the bridge

Close SteamVR. Download the AppImage and its matching `.sha256` file from the release page and run these commands in your download directory:

```sh
image=WMR-GPU-Bridge-0.14.0-portability-alpha-x86_64.AppImage
sha256sum -c "$image.sha256" &&
  chmod +x "$image" &&
  "./$image" --install
```

Installation is per user. The AppImage requires glibc 2.28 or newer and uses the host's Vulkan, DRM, and Wayland support. Install SteamVR and your headset driver separately. The tested WMR headset uses Oasis. If AppImage mounting fails, rerun the AppImage command with `APPIMAGE_EXTRACT_AND_RUN=1`.

### 2. Install the SteamVR shader workaround

Keep SteamVR closed. Download the prebuilt shader workaround archive and its matching `.sha256` file from the same release page. Python 3 is required to register the bundled library.

```sh
adapter=wmr-steamvr-shader-workaround-0.14.0-x86_64
sha256sum -c "$adapter.tar.gz.sha256" &&
  tar -xzf "$adapter.tar.gz" &&
  python3 "$adapter/setup.py" install
```

The workaround is a separate Vulkan layer. It applies an in-memory replacement when a specific SteamVR compositor shader would read a uniform buffer missing from its pipeline layout. It changes no SteamVR files and has no game-specific rules. See the [adapter documentation](compat/steamvr/README.md) for details.

### 3. Start Steam on the dGPU

Fully quit Steam, then open **Steam (dGPU for VR)** from the application menu or run:

```sh
~/.local/bin/wmr-gpu-bridge --steam
```

Start SteamVR from the library, then launch your game normally. If you manage SteamVR launch options manually, use the quoted absolute path to the installed `wmr-gpu-bridge` launcher followed by `%command%`.

## Verify and troubleshoot

```sh
~/.local/bin/wmr-gpu-bridge --doctor
~/.local/bin/wmr-gpu-bridge --verify
~/.local/bin/wmr-gpu-bridge --verify --runtime
~/.local/bin/wmr-gpu-bridge --logs --follow
```

`--verify` checks the installation. Run `--verify --runtime` while the headset is awake and SteamVR is presenting. The last three SteamVR sessions are retained under `${XDG_STATE_HOME:-$HOME/.local/state}/wmr-gpu-bridge/runs/`.

## Uninstall

Close SteamVR. In the directory containing the extracted adapter folder, remove the shader workaround:

```sh
python3 wmr-steamvr-shader-workaround-0.14.0-x86_64/setup.py uninstall
```

In the directory containing the AppImage, remove the bridge:

```sh
./WMR-GPU-Bridge-0.14.0-portability-alpha-x86_64.AppImage --uninstall
```

The bridge uses receipts to track installed files. See the [removal protocol](shim/appimage/REMOVAL-PROTOCOL.md) for details.

## Tested hardware and known limits

The test system uses an AMD Radeon RX 6800M, AMD integrated graphics, a Lenovo Explorer over HDMI, COSMIC Wayland, and Oasis 1.0.4. On SteamVR beta 2.19.2, v14 stalled just after the first frame with the shader workaround disabled. With the workaround enabled, Home and the dashboard stayed running during the retest, and the initial stutter subsided after a few seconds.

Other GPU combinations, Linux distributions, compositors, and headsets still need validation. SteamVR’s built-in Desktop view has caused compositor crashes on the test system. Stable 90 Hz under load and the target of at most 0.1 ms added latency versus a directly connected dGPU remain unverified. Monado support is deferred.

## Build from source

For the native bridge build, install a C++17 compiler, Python 3, pkg-config, libdrm development files, the Vulkan loader and headers, and the Vulkan registry (`vk.xml`). The AppImage build also needs rootless Podman and host Vulkan Video headers. It checks the vendored source hashes and uses a pinned container base and AppImage tool digest.

```sh
./shim/build.sh
./shim/appimage/build.sh
bash compat/steamvr/build.sh
```

The native build writes to `shim/build/`. The AppImage and checksum are written to `shim/dist/`. Standalone display and image-sharing probes are in `prototype/`.

## License

Project code is licensed under [GPL-3.0-or-later](LICENSE). Vendored dependencies retain their own notices under `shim/third_party/`.
