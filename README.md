# Xbox Linux Compatibility

An experimental compatibility layer for Xbox Series X|S applications on desktop PCs. The long-term goal is a bootable Linux gaming distro with a controller-first game library and the compatibility runtime built in.

## Prototype status

- A Windows test fixture presents SimpleTriangle through desktop Direct3D 12.
- Two translated pixel shaders passed DXIL validation, WARP pipeline creation, and pixel readback against reference output.
- The first frame from a Scarlett executable and the Linux graphics test are pending.

## Next milestones

1. Build Microsoft's SimpleTriangle sample for the Xbox Series X|S target and inventory its imports.
2. Present a reference-matching frame from the console build on Windows through the compatibility layer.
3. Test the desktop graphics path on Linux with Wine and vkd3d-proton.
4. Validate the Scarlett sample on Linux, then bundle the runtime with the launcher in a bootable image.

Use the tested sample builds and their results to track compatibility as the project grows.
