# Muxless VR Bridge

Muxless VR Bridge lets SteamVR and Oasis use a Windows Mixed Reality headset connected to an integrated GPU while SteamVR and games render on the discrete GPU. It targets Linux hybrid-GPU muxless laptops with HDMI wired to the iGPU.

**Current release** · [0.1 for Linux x86-64](https://github.com/vibudh007/muxless-vr-bridge/releases/tag/v0.1)

The AppImage installs both the bridge and the SteamVR shader workaround required on the tested setup. The workaround activates in the SteamVR compositor only when the known shader and incompatible pipeline layout match.

## Install and play

### 1. Install the bridge

Install **SteamVR** and **Oasis Driver for Windows Mixed Reality** through Steam, then fully quit Steam and SteamVR. Download the AppImage from the release page and run these commands in your download directory:

```sh
chmod +x Muxless-VR-Bridge-0.1-x86_64.AppImage
./Muxless-VR-Bridge-0.1-x86_64.AppImage --install
```

Installation is per user. The AppImage requires glibc 2.28 or newer and uses the host's Vulkan, DRM, and Wayland support. If AppImage mounting fails, rerun the AppImage command with `APPIMAGE_EXTRACT_AND_RUN=1`.

### 2. Start Steam on the dGPU

Fully quit Steam, then run:

```sh
~/.local/bin/wmr-gpu-bridge --steam
```

Start SteamVR, then launch your game normally.

## Verify and troubleshoot

Pick the command that matches what you’re trying to check.

| Command | When to use it | What it checks |
| --- | --- | --- |
| `~/.local/bin/wmr-gpu-bridge --doctor` | Installation fails or SteamVR won’t start. | Checks for SteamVR, Oasis, required system libraries, and GPU and display capabilities. |
| `~/.local/bin/wmr-gpu-bridge --verify` | You’ve just installed or upgraded the bridge. | Checks that the installed files match their recorded checksums and that the Vulkan layer can load. SteamVR can be closed. |
| `~/.local/bin/wmr-gpu-bridge --verify --runtime` | SteamVR is running and you want to confirm the bridge is sending frames to the headset. | Checks the installation and reports whether the bridge is actively presenting. Keep the headset awake while running it. |
| `~/.local/bin/wmr-gpu-bridge --logs --follow` | You’re reproducing a freeze, crash, or display problem. | Streams the bridge log as events happen. Press `Ctrl+C` to stop watching. |

The bridge keeps logs from the last three SteamVR sessions in `~/.local/state/wmr-gpu-bridge/runs/` by default.

## Uninstall

Close SteamVR and Steam. From the directory containing the AppImage, run the uninstaller:

```sh
./Muxless-VR-Bridge-0.1-x86_64.AppImage --uninstall
```

This removes both components. The bridge uses receipts to track installed files. See the [removal protocol](shim/appimage/REMOVAL-PROTOCOL.md) for details.

## Tested hardware and known limits

The test system is an Asus ROG Strix G15 Advantage Edition (2021), which has an AMD Radeon RX 6800M, AMD Vega 8 integrated graphics internally wired to the HDMI port, a Lenovo Explorer WMR headset over HDMI, COSMIC Desktop Environment, SteamVR Beta 2.19.2 and Oasis 1.0.4. SteamVR Home and the dashboard rendered correctly in the headset with SteamVR beta 2.19.2. Other hardware and Linux configurations still need testing.

## Build from source

Run these commands from the repository root.

### Build the AppImage

Use this if you want an installer you can run with `--install` or share with someone else.

```sh
./shim/appimage/build.sh
```

Requires Python 3, rootless Podman, and host Vulkan headers, including Vulkan Video headers and the Vulkan registry (`vk.xml`). The script builds the binaries in a container and writes the AppImage and its checksum to `shim/dist/`.

### Build the native binaries

Use this for local development. The script compiles the Vulkan layer and launcher into `shim/build/`.

```sh
./shim/build.sh
```

Requires a C++17 compiler, Python 3, `pkg-config`, libdrm development files, the Vulkan loader and headers, and the Vulkan registry (`vk.xml`).

Both scripts expect the Vulkan registry under `/usr/share/vulkan/registry/`. The AppImage script also expects the host headers under `/usr/include/vulkan/` and `/usr/include/vk_video/`.

## License

Project code is licensed under [GPL-3.0-or-later](LICENSE). Vendored dependencies retain their own notices under `shim/third_party/`.
