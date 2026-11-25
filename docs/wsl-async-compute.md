# SimpleCompute on WSL2

The patched SimpleCompute sample rendered a 1920 × 1080 fractal on Intel UHD Graphics under Ubuntu 24.04 in WSL2. Wine selected Mesa Dozen and assigned direct and compute commands to Vulkan queue families 0 and 1. A compute dispatch of 240 × 135 × 1 workgroups completed its fence. The capture matched 2,069,860 of 2,073,600 pixels (99.82%) from the software-rendered async reference. The process reached its 45-second timeout after writing the frame, so clean shutdown remains unverified.

- [Capture](../inspection/wsl-compute/fractal-intel-uhd.png)
- [Test results](../inspection/wsl-compute/result.json)
- [Queue and adapter trace](../inspection/wsl-compute/trace-excerpt.txt)

## Build the instrumented sample

Apply [the patch](../inspection/wsl-compute/simplecompute-async.patch) to a checkout of OpenScarlett `v0.6.0-alpha`. It switches the sample to async after the first frame and records the first completed compute fence in `async-dispatch.log`. The patch context comes from Microsoft's MIT-licensed SimpleCompute sample. [OpenScarlett v0.6.0-alpha](https://github.com/californiaAlmonds/OpenScarlett/releases/tag/v0.6.0-alpha) · [Microsoft sample license](../inspection/wsl-compute/MICROSOFT-LICENSE.txt)

From a 64-bit Visual Studio Developer PowerShell, configure OpenScarlett using its tagged build instructions with `dxc.exe` available. Run these commands from this repository's root, with the OpenScarlett checkout and configured build at the paths shown:

```powershell
git -C ../OpenScarlett apply --check "$PWD/inspection/wsl-compute/simplecompute-async.patch"
git -C ../OpenScarlett apply "$PWD/inspection/wsl-compute/simplecompute-async.patch"
cmake --build ../OpenScarlett/build --config Release --target SimpleCompute_GDKX
```

Place the resulting `SimpleCompute_GDKX.exe` beside `Fractal.cso` and the four OpenScarlett shims from the same tag in a local run directory. The shims are `d3d12_xs.dll`, `kernelx.dll`, `xgameruntime.dll`, and `pixevt.dll`. [Upstream build target](https://github.com/californiaAlmonds/OpenScarlett/blob/v0.6.0-alpha/cmake/gdkx_bringup_targets.cmake)

## Run against the Intel adapter

Use Wine 10.14 with its built-in DXGI and D3D12. For this test, Mesa Dozen 26.3.1 and its display-info dependency were extracted into a local directory from the signed Noble packages. Set `VK_DRIVER_FILES` to a Dozen ICD manifest whose `library_path` points to the extracted driver, and add the extracted libraries and `/usr/lib/wsl/lib` to `LD_LIBRARY_PATH`. The [Vulkan probe](../inspection/wsl-compute/vulkan-adapters.py) should list just the Intel adapter when the manifest is pinned. [Wine 10.14 release notes](https://www.winehq.org/announce/10.14) · [Kisak Mesa Noble packages](https://ppa.launchpadcontent.net/kisak/kisak-mesa/ubuntu/dists/noble/)

With the desired Wine 10.14 runtime on PATH, open the local sample run directory in WSL2 and run:

```bash
unset VK_ICD_FILENAMES VK_ADD_DRIVER_FILES
export WINEPREFIX="$(mktemp -d)/prefix"
export WINEDLLOVERRIDES='mscoree,mshtml=;winedbg.exe=d;d3d12,d3d12core,dxgi=b;d3d12_xs,kernelx,xgameruntime,pixevt=n'
wine wineboot.exe --init
export WINE_D3D_CONFIG=renderer=vulkan VKD3D_CONFIG=virtual_heaps VKD3D_DEBUG=trace
export OPENSCARLETT_PRESENTX_HEADLESS=1 OPENSCARLETT_PRESENTX_EXIT_AFTER_FRAMES=12
export OPENSCARLETT_PRESENTX_CAPTURE_PNG="Z:$(printf '%s' "$PWD/fractal.png" | tr '/' '\\')"
timeout --kill-after=3s 45s wine ./SimpleCompute_GDKX.exe > wine.log 2>&1
```

Check `fractal.png`, the adapter and queue selections in `wine.log`, and the submitted and completed fence values in `async-dispatch.log`. In the recorded run, DXVK 2.7.1 required Vulkan 1.3 and Wine 11 needed a Vulkan instance extension missing from this Dozen driver. Wine 10.14 completed the capture with the same executable and shader used in the software-rendered run.
