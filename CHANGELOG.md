# Changelog

## 0.1.1 — 2026-11-25

First source release.

- Added PE import and export inspection, a loader preflight, and Windows Direct3D 12 triangle fixtures with startup and frame shims.
- Added Xbox shader container parsing, two desktop pixel shader proofs, and DXIL, WARP, and pixel-readback validation.
- Recorded SimpleTriangle, SimpleTexture, and synchronous SimpleCompute runs on Ubuntu 24.04 WSL2 using public OpenScarlett v0.6.0-alpha samples.
- Recorded async SimpleCompute runs on llvmpipe and Intel UHD Graphics. The Intel capture matched 99.82% of the llvmpipe frame and the compute fence completed.
- Documented the Wine 10.14 and Mesa Dozen GPU path. Async rendering passes, and the capture process still reaches its shutdown timeout.

## Planned for 0.1.2

Verify clean shutdown after async frame capture and make the WSL2 test repeatable.
