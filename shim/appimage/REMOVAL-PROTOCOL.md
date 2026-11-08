# Removal protocol 1 (frozen compatibility surface)

Introduced in 0.10.0-installer-review. Historical WMR names remain stable identifiers so old AppImages can manage new installations. Product scope is generic SteamVR presentation; these names do not imply Oasis is required.

## Discovery and receipt

- State: `$XDG_STATE_HOME/wmr-gpu-bridge/install-receipt.json`, default `~/.local/state/wmr-gpu-bridge/install-receipt.json`.
- Receipt **schema remains 1**. Required legacy fields: `prefix`, `manifest`, `files` (array of absolute `path` and lowercase SHA256 `sha256`). `version` identifies the installed payload, never the remover's own version. New `removal_protocol: 1` and other metadata are additive.
- Keep versioned files beneath `PREFIX/lib/wmr-gpu-bridge/`; wrapper remains `PREFIX/bin/wmr-gpu-bridge`; registration remains the existing XDG Vulkan manifest. New resource types must fit this ownership contract or explicitly solve/test legacy removal before shipping.
- Desktop entry is a symlink at the stable XDG applications path, targeting an owned `steam.desktop` inside the versioned payload. Record alias **first**, before its target. This permits historical canonical-root checks and prevents a dangling alias when its target is removed. Additional `kind` and `target` fields let new removers detect a modified/dangling alias; old removers ignore them.
- Keep all still-owned versioned files in the receipt across upgrades/downgrades. Do not derive removal from the AppImage's embedded version. Keep the same XDG state location for discovery; custom XDG state cannot be rediscovered if the caller changes it.

## Installation and interruption

Write ownership intent before each payload replacement. Transitional `previous_sha256` (and desktop `previous_target`) allow the new remover to recognize either side of a recorded interrupted replacement. Remove these transitional fields only on a successful final receipt commit. Normal copy exceptions clean the temporary copy. This is a removal journal, not a claim of power-loss durability: directory fsync, crash-at-every-instruction recovery and stale temporary cleanup remain release-hardening work.

## Removal

1. Lock state and refuse active VR; require Steam closed only if an owned launch option must be edited.
2. Validate receipt schema, hashes and allowed path scope. Reject malformed/untracked authority. Do not run executables selected by receipt contents.
3. Disable activation before removing files, preserving other configuration values.
4. Remove only this wrapper from SteamVR launch options; preserve surrounding arguments. Profile discovery does not depend on OpenVR runtime/Oasis registration.
5. Unlink matching desktop alias before payload; remove matching owned files, retaining modified files and the receipt if leftovers remain. New removal also handles missing payloads/dangling owned aliases.
6. Preserve logs, external drivers, calibration, udev and Bluetooth. No GPU probe, packaged payload, Oasis or existing SteamVR installation is needed for removal.
7. Repeated removal succeeds when the receipt and managed registration/wrapper are already absent; refuse untracked residual registration rather than guessing ownership.

## Required compatibility test

`tools/test_uninstall_matrix.py` extracts and executes actual managers from retained AppImages in temporary HOME/XDG/prefix trees, under a private PID/proc namespace. It tests candidate→history and history→candidate as required passing pairs, and records unchanged history→history failures explicitly. It also covers upgrade/downgrade, future additive metadata, custom prefix, repeated removal and a real failed install with an unrelated collision retained. `tools/test_appimage_manager.cpp` exercises actual removal functions for modified/unsafe/partial receipt cases and process guards.

The 6 November matrix has 19 AppImages and 361 pairs. All 37 pairs involving the new candidate pass, including all 18 historical removers removing the new install and the new remover removing all 18 old installs. There are 77 failures strictly between historical builds. Those immutable binaries/layouts were incompatible before this protocol; they were not rewritten. This does not prove future versions that violate this contract will work, nor every partial historic install can be removed by every historic executable.
