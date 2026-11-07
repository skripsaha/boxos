# UEFI GOP and EDID mode selection — 2026-11-04

## Decision

Commit `1b48c455` makes TagBoot choose a GOP mode with one deterministic
decision:

- select the primary GOP together with its handle;
- read and validate `EFI_EDID_ACTIVE_PROTOCOL`, then discovered EDID, when the
  selected output handle exposes one;
- prefer the EDID base block's first detailed timing when a matching GOP mode
  exists and is within the 3840x2160 framebuffer contract;
- otherwise use an honest fallback: preserve a sane current mode without EDID,
  or choose the largest usable mode (preferring the EDID aspect ratio) within
  the contract;
- verify every successful `SetMode()` by re-reading the active mode;
- reject invalid stride, overflow, unsupported pixel layouts, and undersized
  framebuffer apertures.

The boot_info v4 contract remains unchanged. EDID is used by TagBoot before
`ExitBootServices()` and no firmware-owned EDID pointer is retained afterward.

## Evidence

- `make clean && make` passed; CET audit: 2318 checked, 0 violations.
- `make run UEFI=on` reached `Kernel initialization complete!`, all startup
  tests passed, and TagBoot reported:
  `GOP modes=30 current=0 selected=0 edid=none choice=current-fallback invalid=0`.
  OVMF did not expose EDID on the selected GOP handle, so the fallback was
  intentionally exercised.
- `make run` (BIOS) reached `Kernel initialization complete!`, all startup
  tests passed, and the BIOS boot path remained on boot_info v1.

## Boundary

The QEMU run does not validate the Zenbook panel's EDID association. A real
Zenbook boot with the serial line must confirm the preferred timing and the
selected mode. Multi-output GOPs without an unambiguous EDID on the selected
handle remain on the documented fallback path.
