# Whole-frame capture and replay

Goal: capture one frame of a running title at the AGC driver boundary, then replay it in a
standalone executable (`agc_frame_replay`) through the real driver, so a rendering experiment costs
seconds instead of a full boot.

## Usage

Capture (driver environment, nothing else changes when neither variable is set):

| Variable | Meaning |
| --- | --- |
| `APS5_CAPTURE_TRIGGER=<file>` | capture the next frame whenever `<file>` appears (the driver deletes it); each capture goes to `<APS5_CAPTURE_DIR or .>/frame-capture-<flips>` |
| `APS5_CAPTURE_FRAME=<n>` | capture the frame after the n-th flip submission (0: the first frame), into `APS5_CAPTURE_DIR` (default `frame-capture-<n>`) |
| `APS5_CAPTURE_DIR=<dir>` | output directory (parent directory in trigger mode) |
| `APS5_CAPTURE_REFERENCE=<min width>` | after the frame, wait for it and dump every cached storage image / render target at least that wide into `<capture>/reference` |
| `APS5_CAPTURE_ALL_MEMORY=1` | snapshot every registered guest mapping instead of the GPU-visible ones |
| `APS5_CAPTURE_DRAIN_MS=<ms>` | how long the capture waits for the GPU to drain (default 10000) |

The driver logs `[capture]` lines: the snapshot size and time, aliased ranges, command memory
added late, and the final event / CPU-write totals.

Replay:

```
agc_frame_replay <capture dir> [--images <dir> [--min-width <px>] [--image <address>]...]
                 [--memory <address>:<bytes>:<file>]... [--after-draw <n> | --after-dispatch <n>]
                 [--after-dir <dir>] [--prepare-shaders] [--verbose]
agc_frame_replay --compare <reference dir> <replay dir>
```

Image dumps are `storage_<address>_<w>x<h>_t<tile>_f<VkFormat>.raw`: a 20-byte header (`IMG1`,
width, height, pitch, VkFormat) and the linear rows of the first mip and layer. `--compare` reports,
per file, identical / different (texel count, largest byte difference, first texel) / missing.

Tests: `agc_driver_frame_replay` (arena memory, `APS5_CAPTURE_ALL_MEMORY`),
`agc_driver_frame_replay_direct` (GPU-mapped direct memory under the default filter, one input
written through a CPU-only alias), `agc_driver_frame_replay_trigger` (trigger file). Each captures a
two-queue frame (registers set in the previous frame, a cross-queue label wait, a CPU write between
submissions, a shader registered before the frame and one during it, both overwritten in guest
memory afterwards), replays it with `agc_frame_replay`, and requires identical output memory,
identical storage images and an identical image dump right after the second dispatch.

## Submission flow (what the capture has to reproduce)

1. Guest code calls `sceAgcDriverSubmitDcb/Acb` (and the Multi variants), all of which end in
   `Driver::Submit(Packet{addr, dw_num}, queue)` (`Execution/src/Driver/Queues/Submission.cpp`).
   `SuspendPoint` enqueues a boundary that resets queue 0 state at its next execution.
2. `Submit` copies the command stream out of guest memory *at submit time* (`copySegment`):
   INDIRECT_BUFFER / COND_INDIRECT_BUFFER chains are flattened, predicated IBs are inlined, REWIND
   stops the copy and leaves a tail that is re-read from guest memory once the CPU sets the
   control word. Indirect register lists are read at submit time too (`readRegisterLists`).
   Flip and rendering-wait packets (AnyPS5's own encodings `0xc004105c` and `0xc0021018`) reserve
   `IVideoOutput` objects at submit time.
3. The submission is queued on a per-queue worker (`QueueWorker.cpp`), which runs
   `Driver::execute` (`PacketExecution.cpp`): every packet updates `QueueState` (shader / context /
   user-config register banks, constant RAM, index / indirect bases, predication), or draws,
   dispatches, waits on memory, writes labels, or flips. Everything a packet reads that is not a
   register comes from guest memory at execution time: shader code, descriptors, vertex / index
   data, indirect arguments, labels, textures. Draws resolve their programs against the shader
   registry (`RegisterShader`); unregistered compute programs are read raw from memory.
4. Results reach guest memory three ways: the GPU writes host-imported guest pages directly; the
   driver stores bytes with the CPU (`GuestMemory::Write/WriteChanged`, all through `storeOwn`,
   which stamps the write tracker as a driver store); or results stay in host images (storage
   images / render targets, `StorageTexture`) until something reads the memory (the flush hook,
   `StorageTexture::FlushPending`).

So a frame is fully determined by: guest memory at frame start, the driver state that outlives a
frame, the ordered submit / suspend / shader-registration calls with their queue ids, and the CPU's
writes to guest memory between those calls.

## What is captured

The capture starts at the first driver call (submit, suspend or shader registration) after the
arming flip and ends with the submission that carries the next flip. Every recorded entry point
holds the capture mutex for its whole call, so events are written in the order the driver accepts
them.

At the start, on the calling game thread:

1. Drain: wait (bounded by `APS5_CAPTURE_DRAIN_MS`) until every accepted submission completed,
   then under the GPU mutex wait for the device, store every pending storage image
   (`FlushAllPending`) and wait again, so guest memory holds every GPU result. A frame whose GPU
   work waits on a CPU write the blocked thread would make later cannot drain; the capture then
   proceeds with work in flight and records `drained 0`.
2. Memory snapshot: the GPU-visible guest mappings (libkernel's protection records with a GPU read
   or write bit, 0x30, enumerated by the new `KernelProtectedRanges_nid_postfix`), restricted to
   their committed, host-readable pages, each run stored with its guest address. Command memory a
   submit reads from outside those mappings is added the first time it is read ("late" ranges).
3. Driver state that outlives a frame: the `QueueState` of every queue (all three register banks,
   saved context, constant RAM, index / indirect bases, index type, instance count, predication),
   the pending queue 0 reset, the shader registry (addresses, type, code words, header bytes of
   every entry) and the video output handles.
4. Aliases: `KernelDirectMappings_nid_postfix` lists which guest ranges map the same direct memory;
   captured ranges with an alias elsewhere are listed in the manifest.

During the frame, every driver entry point appends an event to `events.bin`: the CPU writes since
the previous event, then the call itself (queue, command address and size, plus the flattened words
for inspection; or the registered shader's bytes). The file is flushed per event and the manifest
is written at the start and rewritten at the end (`complete 1`), so an interrupted capture still
replays up to its last event.

### CPU writes between submissions

The write tracker in `GuestMemory.cpp` already walks (and resets) the dirty bits of every range
the driver validates. The capture adds:

- a dirty-page observer, called for every page any walk reports (and for pages a fresh commit
  replaced), whichever thread walked them;
- a store observer, called by `storeOwn` with the bytes the driver itself stored;
- `CollectForCapture`: at each event a CPU collect of every captured mapping (identical in effect to
  the driver's own collects: blocks are stamped as CPU writes exactly when their pages are dirty),
  plus a non-stamping walk of the blocks the tracker excluded (host imports, whose dirty bits still
  record CPU stores but which no collect consumes).

The capture keeps a byte-exact shadow of every page it has seen change (loaded lazily from
`memory.bin`). At each event it compares the dirty pages with their shadows, records the differing
byte runs and updates the shadows. Driver stores update the shadow when they happen, so labels,
write-backs and DMA results that the replayed driver produces itself are not replayed as CPU
writes. A dirty page whose alias is captured makes the capture compare the alias too, and the
uncaptured side of an alias is walked for writes. Ranges no walk can read are compared whole.

## Files

`manifest.txt` (text): `aps5-frame-capture 1`, `frame`, `drained`, `complete`,
`null-pixel-program`, `events`, `submissions`, `cpu-write-bytes`, `memory-bytes`, `output <handle>`,
`mapping <begin> <end> <prot> <late>`, `run <begin> <end> <memory.bin offset>`,
`alias <first> <second> <bytes>`. `state.bin`: queue states, queue 0 reset, registry. `events.bin`:
`AFEV`, version, then records (kind, CPU write run count, payload size, runs, payload).
`memory.bin`: the snapshot runs back to back.

Captured data is title data: it stays on the capturing machine and is never committed.

## Replay

1. Map every run at its captured guest address: inside the guest arena (libc reserves
   `0x2_0000_0000 .. 0xfc_0000_0000` at load, with write watching) through
   `GuestArenaMarkUsed` + `GuestArenaCommit`, outside it with `VirtualAlloc` of whole 64 KiB
   allocation granules that the replay process left free (otherwise the run is left out with a
   warning; memory the replay process uses is never written over); copy the snapshot bytes;
   register the mappings with `GuestAllocations`. Guest addresses are absolute, so
   nothing in command streams or descriptors is relocated.
2. Restore the driver state (`Driver::RestoreCaptureState`): queue states, the queue 0 reset, and
   the shader registry, published once. Registered shaders are prepared at first use by default
   (only what the frame draws is compiled); `--prepare-shaders` prepares all of them up front as
   the title did at registration.
3. Register a headless `IVideoOutput` for every captured handle (flips complete at once, rendering
   waits return at once).
4. Play the events in order on one thread: apply the event's CPU writes (plain stores, so the
   replayed write tracker sees them as CPU writes; pages the snapshot left reserved are committed
   first), then make the same driver call with the same guest command address. The driver re-reads
   command buffers, indirect buffers, register lists and REWIND tails from the replayed memory.
   A submission whose command words lie in a run that was left out (late command memory in the
   capturing process's own heap, say) is submitted from a copy of its recorded words when they are
   the whole submission; otherwise the replay stops and names the missing range.
5. After the last event: wait for the frame (until it makes no progress, no new draw or dispatch,
   for 60 s; a cold shader cache can take minutes), flush every pending storage image, then dump
   what was asked. `--after-draw` / `--after-dispatch` dump from the queue worker right after that
   packet was recorded (the recorder is synced first; a pipelined draw is drained first).

## Global driver state and how replay rebuilds it

| State | Where | Replay |
| --- | --- | --- |
| Queue register banks, constant RAM, bases, predication | `Driver::queues` | restored from `state.bin` |
| Queue 0 reset after a suspend | `Driver::resetGraphics` | restored |
| Shader registry | `Driver::shaders` | restored from recorded bytes |
| Prepared shaders (registration, `ResolveShaderAbi`, graphics ABIs) | registry entries | not recorded; prepared at first use (or all with `--prepare-shaders`) |
| Pipeline cache, draw / dispatch caches, recipes | `Driver`, `PipelineCache` | rebuilt (pure caches) |
| Storage-image / texture caches | `ShaderResources.cpp` | rebuilt from guest memory (pending results were flushed before the snapshot) |
| DCC key memos, unit shadows | `DccMetadata.cpp`, `UnitShadow.cpp` | rebuilt from guest memory |
| Label store history, deferred labels | `Driver::labelStores`, `DeferredLabels` | start empty (the drain emptied them) |
| Write tracker generations | `GuestMemory.cpp` | start fresh; replayed CPU writes are real page writes |
| Depth surfaces (host-only images, HTILE clear state) | `DepthSurface.cpp` | **not restored**: created on first use and cleared to the target's clear value |
| Video outputs | `Driver::outputs` | headless stand-ins |

## Known fidelity gaps

- CPU/GPU races: CPU writes are recorded at driver-call boundaries. A write the title makes while
  the GPU executes a submission is replayed before the next call, and the interleaving of GPU work
  across queues is the replay machine's. Pages the CPU and the GPU both write between two events
  carry the GPU's bytes in the recorded CPU write.
- Depth / stencil content that survives from earlier frames lives only in host images and is lost;
  a frame that does not clear its depth targets first replays against cleared depth.
- Direct memory is replayed as private memory: aliased mappings become independent copies, so a
  GPU write through one alias is not seen through the other (CPU writes reach both, through the
  recorded writes). The title's section-backed write tracking becomes write-watch tracking.
- GPU writes into memory the snapshot did not include (CPU-only mappings under the default filter)
  are not observed; the GPU cannot reach such memory on the console.
- The null pixel program lives in the driver image, whose base can differ between the title and the
  replay; the captured null program is restored at its captured address next to the replay's own.
- Shader preparation is lazy by default, which changes timing, not results.
- The capture itself perturbs the frame: the drain, the per-event walks over all captured memory
  (the game thread is held in its submit meanwhile) and the re-armed write tracking.
