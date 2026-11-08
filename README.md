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

Clone the repository and run the build commands from its root:

```sh
git clone https://github.com/vibudh007/Muxless-VR-Bridge.git
cd Muxless-VR-Bridge
```

The AppImage and native builds are independent. SteamVR, Oasis, and a connected headset are needed for use and live testing. Both builds can run without them.

### Build the AppImage

Use this build for the Linux x86-64 installer. The host needs Bash, Python 3, a working rootless Podman setup, and Vulkan headers and registry files. The build pulls its container from Docker Hub, installs packages from Rocky Linux repositories, and downloads AppImage tooling and its runtime from GitHub.

On Fedora, install the build dependencies:

```sh
sudo dnf install git bash python3 podman vulkan-headers
```

On other distributions, package names may differ. The script expects these host paths:

| Required path | Contents |
| --- | --- |
| `/usr/include/vulkan/` | Vulkan C headers |
| `/usr/include/vk_video/` | Vulkan Video headers |
| `/usr/share/vulkan/registry/vk.xml` | Vulkan command and type registry |

Confirm that Podman works as your normal user, then build:

```sh
podman info --format '{{.Host.Security.Rootless}}'
./shim/appimage/build.sh
```

The first command should print `true`. Run the build without `sudo`. The script uses the container's compiler and development libraries and verifies the downloaded AppImage tool against its recorded digest. The AppImage and its checksum are written to `shim/dist/`.

To check that the resulting AppImage starts without installing it:

```sh
APPIMAGE_EXTRACT_AND_RUN=1 ./shim/dist/Muxless-VR-Bridge-0.1-x86_64.AppImage --version
```

### Build the native binaries

Use this build for local development. You’ll need Bash, Python 3, a C++17 compiler and linker, `pkg-config`, libdrm development files, Vulkan headers, the Vulkan loader development library, and the registry at `/usr/share/vulkan/registry/vk.xml`.

On Fedora:

```sh
sudo dnf install git bash python3 gcc-c++ pkgconf-pkg-config libdrm-devel vulkan-loader-devel vulkan-headers
```

Then build and check the launcher:

```sh
./shim/build.sh
./shim/build/wmr-gpu-bridge --version
```

This produces `shim/build/libmuxless_vr_bridge.so` and `shim/build/wmr-gpu-bridge`. Use the AppImage build to get the installer.

## License

Project code is licensed under [GPL-3.0-or-later](LICENSE). Vendored dependencies retain their own notices under `shim/third_party/`.
