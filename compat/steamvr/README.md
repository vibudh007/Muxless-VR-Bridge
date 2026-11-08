# SteamVR shader workaround

This is a separately installed Vulkan layer for the observed invalid uniform-buffer access in a known SteamVR compositor vertex shader. It leaves the generic bridge and its AppImage independent of SteamVR shaders. It changes no vendor files and has no game-specific rules.

The adapter selects vrcompositor and recognizes the exact shader fingerprint. The replacement is used only in an observed pipeline layout without the required vertex uniform binding. Other shaders and layouts pass through. It does not inflate descriptor pools.

This workaround is required on the tested RX 6800M / Lenovo Explorer setup with SteamVR beta 2.19.2. Other configurations may work without it.

Build and install for the current user while SteamVR is stopped:

```sh
bash compat/steamvr/build.sh
python3 compat/steamvr/setup.py install
```

The build uses the same glibc 2.28 container baseline as the generic AppImage. The separate registration is disabled with `DISABLE_WMR_STEAMVR_COMPAT=1`. To remove it, close SteamVR and run `python3 compat/steamvr/setup.py uninstall`. The setup command only removes its receipt-owned files. Use this adapter with the generic bridge version 0.14.0 or newer. Older versions already contain the compatibility hook.

Remove the adapter when the SteamVR or driver fix has been verified on this setup.
