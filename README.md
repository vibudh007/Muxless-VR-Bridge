# Muxless VR Bridge

Muxless VR Bridge lets SteamVR and Oasis use a Windows Mixed Reality headset connected to an integrated GPU while SteamVR and games render on the discrete GPU. It targets Linux hybrid-GPU systems such as muxless laptops with HDMI wired to the iGPU. On the tested setup, the bridge presents the finished frame from a shared image with zero explicit frame copies.

**Current release** · [0.1 for Linux x86-64](https://github.com/vibudh007/muxless-vr-bridge/releases/tag/v0.1)

The AppImage installs both the bridge and the SteamVR shader workaround required on the tested setup. The workaround activates in the SteamVR compositor only when the known shader and incompatible pipeline layout match.

## Install and play

### 1. Install the bridge

Install **SteamVR** and **Oasis Driver for Windows Mixed Reality** through Steam, then fully quit Steam and SteamVR. Download the AppImage and its matching `.sha256` file from the release page and run these commands in your download directory:

```sh
image=Muxless-VR-Bridge-0.1-x86_64.AppImage
sha256sum -c "$image.sha256" &&
  chmod +x "$image" &&
  "./$image" --install
```

Installation is per user. The AppImage requires glibc 2.28 or newer and uses the host's Vulkan, DRM, and Wayland support. If AppImage mounting fails, rerun the AppImage command with `APPIMAGE_EXTRACT_AND_RUN=1`.

### 2. Start Steam on the dGPU

Fully quit Steam, then start it on the discrete GPU. Use either command, or any launcher that starts Steam with `DRI_PRIME=1`:

```sh
DRI_PRIME=1 steam
```

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

Close SteamVR and Steam. From the directory containing the AppImage, run the uninstaller:

```sh
./Muxless-VR-Bridge-0.1-x86_64.AppImage --uninstall
```

This removes both components. The bridge uses receipts to track installed files. See the [removal protocol](shim/appimage/REMOVAL-PROTOCOL.md) for details.

## Tested hardware and known limits

The test system uses an AMD Radeon RX 6800M, AMD integrated graphics, a Lenovo Explorer over HDMI, COSMIC Wayland, and Oasis 1.0.4. SteamVR Home and the dashboard rendered correctly in the headset with SteamVR beta 2.19.2. The built-in Desktop view has caused compositor crashes. Other hardware and Linux configurations still need testing.

## Build from source

For the native bridge build, install a C++17 compiler, Python 3, pkg-config, libdrm development files, the Vulkan loader and headers, and the Vulkan registry (`vk.xml`). The AppImage build also needs rootless Podman and host Vulkan Video headers. It checks the vendored source hashes and uses a pinned container base and AppImage tool digest.

```sh
./shim/build.sh
./shim/appimage/build.sh
```

The native build writes to `shim/build/`. The AppImage and checksum are written to `shim/dist/`. Standalone display and image-sharing probes are in `prototype/`.

## License

Project code is licensed under [GPL-3.0-or-later](LICENSE). Vendored dependencies retain their own notices under `shim/third_party/`.
