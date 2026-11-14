# Removal protocol 1 (frozen compatibility surface)

The receipt location, allowed paths and ownership rules remain stable so existing AppImages can remove new installations. Vulkan layer names may evolve; legacy removal relies on the receipt, not those names.

## Discovery and receipt

- State: `$XDG_STATE_HOME/wmr-gpu-bridge/install-receipt.json`, default `~/.local/state/wmr-gpu-bridge/install-receipt.json`.
- Receipt **schema remains 1**. Required legacy fields: `prefix`, `manifest`, `files` (array of absolute `path` and lowercase SHA256 `sha256`). `version` identifies the installed payload, never the remover's own version. New `removal_protocol: 1` and other metadata are additive.
- Keep versioned files beneath `PREFIX/lib/wmr-gpu-bridge/`; wrapper remains `PREFIX/bin/wmr-gpu-bridge`; registration remains the existing XDG Vulkan manifest. New resource types must fit this ownership contract or explicitly solve/test legacy removal before shipping.
- A legacy desktop entry may be a symlink at the stable XDG applications path, targeting an owned `steam.desktop` inside the versioned payload. Record alias **first**, before its target. This permits historical canonical-root checks and prevents a dangling alias when its target is removed. Additional `kind` and `target` fields let new removers detect a modified/dangling alias; old removers ignore them.
- Keep every retained owned file in the receipt across upgrades/downgrades. A successful upgrade may remove unchanged old payload files and their entries; modified old files remain recorded. Do not derive removal from the AppImage's embedded version. Keep the same XDG state location for discovery; custom XDG state cannot be rediscovered if the caller changes it.

## Installation and interruption

Write ownership intent before each payload replacement. Transitional `previous_sha256` (and desktop `previous_target`) allow the new remover to recognize either side of a recorded interrupted replacement. Remove these transitional fields only on a successful final receipt commit. Normal copy exceptions clean the temporary copy. The journal tracks ownership across interrupted file replacements.

## Removal

1. Lock state and refuse active VR; require Steam closed only if an owned launch option must be edited.
2. Validate receipt schema, hashes and allowed path scope. Reject malformed/untracked authority. Do not run executables selected by receipt contents.
3. Disable activation before removing files, preserving other configuration values.
4. Remove only this wrapper from SteamVR launch options; preserve surrounding arguments. Profile discovery does not depend on OpenVR runtime/Oasis registration.
5. Unlink matching desktop alias before payload; remove matching owned files, retaining modified files and the receipt if leftovers remain. New removal also handles missing payloads/dangling owned aliases.
6. Preserve logs, external drivers, calibration, udev and Bluetooth. No GPU probe, packaged payload, Oasis or existing SteamVR installation is needed for removal.
7. Repeated removal succeeds when the receipt and managed registration/wrapper are already absent; refuse untracked residual registration rather than guessing ownership.

## Bundled patchset

The combined Vulkan registration uses `layers` instead of the earlier single `layer`. It remains at the same receipt-owned path. Both libraries are inside the versioned payload. Persistent patch selections are ordinary receipt-owned files under `PREFIX/lib/wmr-gpu-bridge/patches/`, within the legacy allowed subtree. Toggle operations lock installation state, validate ownership, journal the new and previous checksums, write atomically and commit the new checksum. The immutable 0.1 remover has been tested against this layout.

## Native VR profile helper

The optional root-owned/setuid helper is an ordinary checksum-owned file inside the versioned managed library directory; that parent remains user-owned. Old schema-1 uninstallers can unlink it without elevation. No global service, udev rule or system configuration file is installed. Administrator authorization changes the helper inode's ownership/mode, not its bytes. Additive `required_mode: root-setuid` metadata lets new verification check authorization; old removers ignore it. The unprivileged profile lease service exits after its last client disconnects and restores the driver profile. Its Unix endpoint is abstract, with no filesystem artifact to remove.
