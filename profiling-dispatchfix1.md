# Skipped-dispatch experiment — October 8, 2026

Base: local trial branch `try-639-on-runtree`, commit `ffa9422d`.
Changes for this experiment are uncommitted and have not been pushed.

## Changes

- Increased the typed storage image heap from 16 to 128 descriptors. Kept the per-image mip limit at 16 through a separate `StorageMipCapacity`; increasing the old shared constant alone would reserve more mips per image and preserve the overflow. Sampled heap capacity and `ShaderInfo::MaxImages` remain 64. Runtime ABI version is now 11.
- Allowed unnormalized coordinates for the existing emulated converted-image filtering path, which fetches texels and handles pixel coordinates in SPIR-V. Native sampler checks remain in place.
- Added descriptor shape details to static-image-interface failures and binding/count details to heap-overflow failures.
- Extended heap tests and added an explicit `--filter-only` GPU test for normalized and unnormalized bilinear sampling of both converted formats.

## Run and result

Built `libs` successfully with the existing MinGW build. Ran:

```powershell
& D:\ps5\gt7\tools\gt7-visible.ps1 -Seconds 450 `
  -Libs C:\repos\AnyPS5-pr639\build\core\libs\libs `
  -Tag dispatchfix1 -Env @{ APS5_WRITE_WATCH_IMPORTS = 'watch' }
```

The runner started PID 12636 and stopped that PID after 450 seconds. No other game instance was running. The new shader cache was allowed to compile. No game crash occurred.

Log: `D:\ps5\gt7\run-err-dispatchfix1.log`.

The last six reported ten-second intervals contained 22, 23, 21, 23, 23, and 23 presents: approximately **2.25 FPS**, with a repeated 2.3 FPS plateau. Final window title: 2.13 FPS. Earlier scene intervals dipped below this. There is no sustained improvement over the reported baseline.

The log no longer contains the heap-overflow failure for compute shader `0x1258d29700` or the converted-image unnormalized-sampler rejection seen for the two built-in shaders in the baseline. This removes those specific failures but does not establish that all guest-visible GPU work completes correctly.

Remaining failures include:

- Static image-interface mismatches in compute shaders `0x1258d6e800`, `0x1258d67a00`, `0x1258d61400`, and `0x1258d7ce00`.
- The large draw regression from that same interface check (about 14,000 skipped draws in several ten-second intervals).
- An unsupported native unnormalized image shape in compute shader `0x1258d12200`.
- Guest buffer views exceeding their GPU owners in three compute shaders.

The new interface diagnostics show ordinary 2D float descriptors, without conversion, packing, cube, depth-bits, or sRGB decoding, rejected against mode lists of 4, 8, or 9 entries. They do not yet establish which static field excludes the matching mode. The mountains/silhouette regression was not fixed or visually verified in this experiment.

## Validation

Passed:

- `agc_shader_device_profile_tests` (including full multi-image storage heaps, mip reservation, and overflow rejection).
- `agc_driver_runtime_images_tests`.
- `agc_driver_image_converted_unorm_tests --filter-only` (actual texel results for both formats and both coordinate modes).
- `git diff --check`.

Broader checks that did not pass:

- The default converted-image test still expects converted sampling to be refused, which conflicts with the filtering port already present at the base commit. Its new correctness coverage is available through `--filter-only`; the whole default suite is not claimed passing.
- The native unnormalized sampling test reports an incorrect value in a four-level image view (240 instead of 0). No baseline rerun was performed to date this failure; it remains unresolved.

## Interpretation and next step

Removing the targeted heap and converted-sampler skips did not lift the FPS ceiling. Remaining skips prevent ruling out missing GPU results more broadly. Fix the static image-interface regression next because it suppresses both draws and compute. This run did not enable the heavy wait timeline, so it does not measure a change in the repeated 50 ms timeout chain.
