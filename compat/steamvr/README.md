# SteamVR shader workaround

The AppImage includes this workaround in its Vulkan layer. It matches a specific SteamVR compositor shader and replaces it in memory when the pipeline layout lacks the required vertex uniform binding. SteamVR files are untouched and game processes are outside its scope.

For diagnosis, start Steam with `DISABLE_WMR_STEAMVR_COMPAT=1` in its environment to disable the shader workaround. The bridge continues to manage headset presentation.
