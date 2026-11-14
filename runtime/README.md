# Native VR workload-profile leases

The rendering device selected by the Vulkan client determines the target GPU. `VK_EXT_physical_device_drm` supplies its render-node device numbers. The code does not choose a GPU by vendor, PCI address, card index or model.

`vr_profile.hpp` parses the driver's named profile table. `profile_session.hpp` acquires a lease during VR-device construction, with a weak-reference cache for multiple logical devices. `profile_helper.cpp` supplies the restricted privileged operation and the unprivileged lease service. `profile_protocol.hpp` defines its abstract per-user/per-GPU endpoint. No function here runs once per rendered frame.

## Verified and unsupported driver interfaces

| Driver family | Documented interface | Behavior |
| --- | --- | --- |
| AMD `amdgpu` | Named `VR` entry in `pp_power_profile_mode` | Detect its advertised number, select it and restore the previous selection after the final lease closes. Tested on RX 6800M. The tested kernel accepts VR with the existing Auto performance level; no forced clock/power-limit setting is added. |
| NVIDIA | PowerMizer Adaptive/Auto/Prefer Maximum Performance/Prefer Consistent Performance; NVML workload profiles | The reviewed official interfaces do not define a VR workload profile. Do not relabel maximum/base clocks, compute or inference modes as VR. No automatic substitute is applied. Hardware testing remains a public-beta task. |
| Intel `i915`/`xe` | Frequency controls; Xe GuC base/power-saving profiles | No named VR mode found in the reviewed kernel interfaces. Leave policy unchanged. Hardware testing remains a public-beta task. |

Sources: [Linux AMD power-profile interface](https://www.kernel.org/doc/html/latest/gpu/amdgpu/thermal.html), [AMD's SMI profile enumeration/API](https://github.com/ROCm/amdsmi/blob/amd-mainline/include/amd_smi/amdsmi.h), [NVIDIA's official NV-CONTROL definitions](https://github.com/NVIDIA/nvidia-settings/blob/main/src/libXNVCtrl/NVCtrl.h), [NVIDIA's official NVML header/profile enumeration](https://github.com/NVIDIA/go-nvml/blob/main/pkg/nvml/nvml.h), [NVML workload-profile API](https://docs.nvidia.com/deploy/nvml-api/latest/api/group__nvmlPowerProfiles.html), [Intel Xe GuC power management](https://docs.kernel.org/gpu/xe/xe_firmware.html). [SteamVR Linux issue 469](https://github.com/ValveSoftware/SteamVR-for-Linux/issues/469) corroborates the AMD pacing/governor issue; it is not proof of a universal vendor setting.

This is capability-driven, not a claim that every GPU driver has the same feature. Missing profiles, unsupported interfaces and driver rejections do not stop VR rendering. No NVIDIA/Intel behavior is represented as hardware-validated. A future documented backend can reuse the same lease lifetime without adding vendor logic to the image handoff.

## Privilege boundary

At installation, the native helper is copied inside the versioned, receipt-owned payload. If a discrete GPU advertises VR, the installer invokes an existing administrator-authorization mechanism once to set this helper inode root-owned/setuid. Neither Steam nor the graphics layer runs as root. No system service, udev rule or global configuration file is installed.

At startup, the caller opens the selected render node itself and supplies that fd plus a same-user Unix lease socket. The helper validates that the fd is a DRM render node, resolves its kernel-owned sysfs device, opens only the fixed profile control, takes an exclusive GPU-policy lock, and permanently drops root privileges and supplementary groups. It accepts no caller-supplied sysfs path, profile number or executable command. The service detaches only after dropping privileges.

Multiple processes share the abstract per-user/per-GPU socket. The last disconnect, including a compositor crash, restores the previous profile. The service checks driver state once per second to renew VR if another tuning service reapplies a default after a DRM event. The latest external non-VR setting is preserved for restoration. A custom profile is left untouched. Frequency caps, power limits, fans and quality settings are never changed.

No GameMode, LACT, Python or vendor management command is required at runtime. Administrator authorization is installation-only. The helper remains removable by old schema-1 uninstallers because its parent directory is user-owned and all payload paths remain inside the frozen allowed subtree.

Set `MUXLESS_VR_PROFILE=0` before starting the VR session to disable automatic profile selection for diagnostics.
