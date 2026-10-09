# Menu FPS: measured critical path (settled menu, ~6 FPS)

## Captures
- `D:\ps5\gt7\diagnostics-rapid\vtune-settled-2` — VTune sw hotspots + threading, 15 s settled menu.
- `D:\ps5\gt7\diagnostics-rapid\nsys-elevated-3\transition.*` — nsys, loading transition (Vulkan + CPU samples + ctxsw).
- `D:\ps5\gt7\diagnostics-rapid\nsys-elevated-4\steady.*` — nsys, steady menu (Vulkan + CPU samples + ctxsw).
- Nsight Graphics GPU Trace: not usable — under its injection the title crawls at ~0.01 FPS during loading (and `--platform` is eaten by Qt; omit it).
- Note: nsys CPU sampling with ctxsw backtraces itself causes ~500 ms stalls (present rate dropped from 70 to 26–35 per 10 s during capture). Only the ~170 ms frames were analysed.

## What the timeline shows (normal 171 ms frames)
- Worker+committer both on CPU 67% of the frame, all three of worker/committer/GPU idle only 5.6%: the driver's two-thread draw pipeline is the critical path, not the guest and not the GPU.
- GPU busy ~83 ms/frame, fed in ~62 submits spread over the whole frame (small idle gaps); it becomes the ceiling (~12 FPS) once the CPU side halves.
- GPU time per present (APS5_PROFILE_GPU): draws ~50 ms (~1000 draws), storage upload+writeback ~18 ms (alias round trips), staging ~1.6 ms.

## Committer (DrawPipeline::run -> commitDraw), share of its CPU
- write-watch walks (GetWriteWatch): ~17% (precollectImages 8.3%, CopyDrawInput 8.7%)
- Recorder::NotePendingReads: 10.4% (address-based builds note ~1200 leased-heap ranges per draw)
- image binding resolution (buildComplete): 8.4%
- DCC key scans (AllKeysEqual): ~5%
- waiting for work: 13.5%; GPU mutex: 3.4%
- per kind: template hits 28 us, address-based (BDA) builds 82 us (always miss the resource cache: HoldsLease => not reusable)

## Worker (Driver::draw), share of its CPU
- SRT runtime-source evaluation (IR interpreter per draw, under ShaderMemory::Capture/BuildPreparedShaderKey): ~22%
- waiting for room in the draw queue: 15.7%; drains 2.6%
- dispatches 12%

## Changes made (uncommitted, in C:\repos\AnyPS5-pr639)
1. Read-set dedupe: an address space's leased regions are noted as pending reads once per open batch (Recorder::ReadSetNoted). Opt-out APS5_NO_READ_SET_DEDUPE=1. Measured: committer 55.6 -> 48.5 us/commit, BDA draws 82 -> 67 us.
2. DCC scan memo keyed on write stamps (UnchangedSince). Opt-out APS5_NO_DCC_SCAN_MEMO=1. Measured: no visible change.
3. SRT evaluation memo: SrtWalker::EvaluateRuntimeSources records every guest word read and reuses the result while those words are unchanged. Opt-out APS5_NO_SRT_EVAL_MEMO=1.

## Tools
- profiling/measure-menu.ps1 — launch, settle, record present windows + per-draw costs, stop the instance it launched.
- profiling/nsys-frames.py, nsys-gaps.py, nsys-waits.py, nsys-summary.py, nsys-threads.py, nsys-user-threads.py — nsys sqlite analysis.
- profiling/vtune-topdown.py — resolved per-thread top-down tree from a VTune CSV.
