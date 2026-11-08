# WMR GPU Bridge

A Linux Vulkan layer for SteamVR on hybrid-GPU systems where the headset's display connector is wired to the integrated GPU. SteamVR and games render on the discrete GPU. The bridge routes headset presentation to the GPU that owns the connector. On the tested setup, the direct-scanout backend presents from a shared image with zero explicit frame copies.

## Alpha status

The current release candidate is **0.14.0-portability-alpha**. The tested configuration is an AMD Radeon RX 6800M, AMD integrated graphics, Lenovo Explorer over HDMI, COSMIC Wayland, SteamVR beta 2.19.1 and 2.19.2, and Oasis 1.0.4. The [SteamVR shader workaround](compat/steamvr/README.md) is required on this tested setup. With it disabled, the compositor stalled just after the first frame. With it enabled, Home and the dashboard stayed running. Initial stutter subsided after a few seconds.

Other GPU combinations, compositors, distros, and headsets require validation. Monado support is deferred. SteamVR's built-in Desktop view and some dashboard interactions have caused compositor crashes on the tested setup. Stable 90 Hz under load and the target of at most 0.1 ms of added latency against a directly connected dGPU remain unverified.

## Install and run

Close SteamVR before installing or removing the bridge. Use the x86-64 AppImage and its matching checksum.

```sh
sha256sum -c WMR-GPU-Bridge-0.14.0-portability-alpha-x86_64.AppImage.sha256
chmod +x WMR-GPU-Bridge-0.14.0-portability-alpha-x86_64.AppImage
./WMR-GPU-Bridge-0.14.0-portability-alpha-x86_64.AppImage --install
```

The AppImage installs the bridge. On the tested setup, also [build and install the SteamVR shader workaround](compat/steamvr/README.md) before launching SteamVR. The two components are installed separately.

The installer uses per-user paths. The build targets glibc 2.28 or newer and uses host Vulkan, DRM, and Wayland support. SteamVR and any headset driver, including Oasis for the tested WMR setup, are installed separately. If AppImage mounting is unavailable, prefix the command with `APPIMAGE_EXTRACT_AND_RUN=1`.

Fully quit Steam and start it from **Steam (dGPU for VR)** or run `~/.local/bin/wmr-gpu-bridge --steam`. Launch SteamVR and then your game normally. The launcher also accepts Steam arguments after `--steam`. If entering the SteamVR launch option manually, use the quoted absolute path to the installed `wmr-gpu-bridge` launcher followed by `%command%`.

```sh
~/.local/bin/wmr-gpu-bridge --doctor
~/.local/bin/wmr-gpu-bridge --verify --runtime
~/.local/bin/wmr-gpu-bridge --logs --follow
./WMR-GPU-Bridge-0.14.0-portability-alpha-x86_64.AppImage --uninstall
```

The bridge retains the last three SteamVR sessions under `${XDG_STATE_HOME:-$HOME/.local/state}/wmr-gpu-bridge/runs/`. Use `--verify` to check installed files and `--verify --runtime` while SteamVR is presenting to check live status. The installer and remover track file ownership using the [removal protocol](shim/appimage/REMOVAL-PROTOCOL.md).

## Build from source

For a native build, install a C++17 compiler, Python 3, pkg-config, libdrm development files, and the Vulkan loader, headers, and registry (`vk.xml`). For the portable AppImage, also install rootless Podman. The packaging script mounts the Vulkan headers, video headers, and registry from the host. It checks the vendored source hashes and uses a pinned container base and AppImage tool digest.

```sh
./shim/build.sh
./shim/appimage/build.sh
```

The native launcher and layer are written to `shim/build/`. The AppImage and checksum are written to `shim/dist/`. The standalone display and image-sharing probes are in `prototype/`.

Third-party source notices are retained under `shim/third_party/`.

The project's own code is licensed under [GPL-3.0-or-later](LICENSE). Vendored dependencies retain their own licenses.
