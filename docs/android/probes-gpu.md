# P0.7 GPU memory probe: results

Probe P0.7 of [PLAN.md](PLAN.md), run on 2026-10-25. It decides the GPU memory strategy (A, A′ or B).

**P0.7 pass test:** placed memory is cached, coherent and as fast as anonymous memory for the CPU; the GPU and the CPU see each other's writes; `mprotect` works.

**Result:**
- **Stock driver:** fails. It has no `VK_EXT_map_memory_placed`, so of the placed strategies only B is possible on it. Zero-copy buffer pools through AHardwareBuffers do work on it (Galaxy Z Fold 7, with conditions): see [AHardwareBuffer-backed pools](#ahardwarebuffer-backed-pools-zero-copy-buffers-on-the-stock-driver-2026-10-26).
- **842.6 and Turnip:** pass every part of the test, with two conditions that come from the kernel's KGSL driver, not the Vulkan drivers (identical on all three):
  - **Split-VMA fault bug.** It corrupts data whenever `mprotect` splits a GPU mapping and a page is faulted in afterwards. Populating the whole mapping before the first `mprotect` avoids it, as long as nothing drops the pages later: `MADV_DONTNEED` does, and brings the corruption back.
  - **Split KGSL VMAs never merge back.** Every page boundary the dirty tracker ever toggles stays a VMA, and a process has at most `vm.max_map_count` (65,530) of them. So page-granular write tracking does not scale on GPU memory.
- **842.6** also has two driver bugs that a design can avoid: placed memory is never freed while the process lives, and `vkMapMemory` after a placed unmap returns a stale pointer.
- **Turnip (this a8xx build)** reads garbage from compute push constants.

**Recommendation:**
- Strategy **A** on **842.6**, with the conditions listed under [Recommendation](#recommendation). The one that shapes the design: dirty tracking on placed memory needs a coarse protection granularity, or a VMA budget, to stay under the VMA cap.
- **B** on the stock driver.
- A′ is not recommended: it creates and frees placed memory at every pool, so on 842.6 it leaks.

## Setup

- **Phone:** nubia NX789J (Snapdragon 8 Elite, Adreno 830, 16 GB), Android 16 build `BQ2A.250705.001`, kernel `6.6.92-android15-8`, 4 KB pages.
- **Contexts:**
  - **exe:** an `adb shell` executable in `/data/local/tmp/nxprobe-gpu/`, cpuset `/`.
  - **app:** a NativeActivity APK, `org.nxnative.probe.gpu`, `untrusted_app`, cpuset `/top-app`.
  - The app sets `KEEP_SCREEN_ON | TURN_SCREEN_ON | SHOW_WHEN_LOCKED`. Without these flags, a dozing screen with the keyguard up puts the app in `/background` (CPUs 0-1), and pinning to a big core fails.
- **Bandwidth:** pinned to cpu7 (max 4.32 GHz).
- **Concurrency:** another agent's CPU probe app (`org.nxnative.probe.cpu`) was alive during the 842.6 and Turnip app runs.
- **Memory pressure:** the first 842.6 heavy run (b and e in one process, MemAvailable down to about 1.3 GB) made the low-memory killer kill about 20 cached background apps (oom adj 955-999) at 13:37:30. The CPU probe app was not among them. Since then, e runs in its own process.

| Driver | Identity (from the driver; checked each run against the mapped library name, `driverID` and `driverInfo`) | Package |
|---|---|---|
| stock | `vulkan.adreno.so`, driverID 8, api 1.3.284, driverVersion 0x80320048, compiler E031.47.18.49 dated 02/12/26, conformance 1.3.8.0 | system (`ro.gfx.driver.1=com.qualcomm.qti.gpudrivers.sun.api35` exists, but the default loader picks the vendor driver) |
| 842.6 | `vulkan.ad07xx.so`, driverID 8, api 1.4.295, driverVersion 0x8034a006 (842.6), compiler E031.50.19.01 dated 08/28/25, conformance 1.4.1.0 | `8Elite2-842.6.zip` (user-supplied; not in the repo) |
| Turnip | `vulkan.ad08xx.so`, driverID 18 (MESA_TURNIP), "Turnip Adreno (TM) 830v2", api 1.4.335, Mesa 26.0.0-devel (git-9a720fe0e4) | `turnip_a8xx.zip` (not in the repo) |

### Loading the drivers

libadrenotools comes from github.com/bylaws/libadrenotools at commit 8fae8ce. It is cloned and built outside the repo:
- clone: `~/dm-work/android/src/libadrenotools`
- build: `~/dm-work/android/src/libadrenotools-build`
- toolchain: SDK cmake 3.22.1 and Ninja, android-35, arm64-v8a, `c++_static`, `BUILD_SHARED_LIBS=ON`, and `ANDROID_SUPPORT_FLEXIBLE_PAGE_SIZES=ON`
  - The last option makes the libraries 16 KB-aligned. Without it they are 4 KB-aligned: a phone with 16 KB pages cannot load them, and Android 17 shows a compatibility warning over a debuggable app that has them (found on a Galaxy Z Fold 7, 2026-10-26).
  - `build_app.sh` refuses any library that is not 16 KB-aligned.

`build.sh` puts `libadrenotools.so`, `libhook_impl.so`, `libmain_hook.so`, `libfile_redirect_hook.so` and `libgsl_alloc_hook.so` into the APK's `lib/arm64-v8a`. For the executable, it copies them beside it.

Exact calls that worked, as recorded in `summary.txt` (`loaded:` line):

```c
// APK, 842.6 run verbatim (the /data/app hashes change with every install):
adrenotools_open_libvulkan(RTLD_NOW, ADRENOTOOLS_DRIVER_CUSTOM, NULL,
    "/data/app/~~9afCO3TeN2zG0e-iZ88AaQ==/org.nxnative.probe.gpu-NtjDbrQ5L29yi_nXjs46MA==/lib/arm64",
    "/data/user/0/org.nxnative.probe.gpu/files/drivers/842/", "vulkan.ad07xx.so", NULL, NULL);

// APK (hookLibDir = nativeLibraryDir, found with dladdr on our own code; driver unzipped into the app's files dir)
adrenotools_open_libvulkan(RTLD_NOW, ADRENOTOOLS_DRIVER_CUSTOM /* 1 */, NULL,
    "/data/app/~~<hash>/org.nxnative.probe.gpu-<hash>/lib/arm64",
    "/data/user/0/org.nxnative.probe.gpu/files/drivers/842/",      // trailing '/' required
    "vulkan.ad07xx.so", NULL, NULL);
// Turnip: ".../files/drivers/turnip/", "vulkan.ad08xx.so"

// adb shell executable: hook libraries beside the executable
adrenotools_open_libvulkan(RTLD_NOW, ADRENOTOOLS_DRIVER_CUSTOM, NULL, "/data/local/tmp/nxprobe-gpu",
    "/data/local/tmp/nxprobe-gpu/drivers/842/", "vulkan.ad07xx.so", NULL, NULL);
// Turnip: "/data/local/tmp/nxprobe-gpu/drivers/turnip/", "vulkan.ad08xx.so"
```

- **Both drivers load in both contexts.** The exe was the uncertain case; it works.
- **A failed hook falls back silently to the system driver.** So the probe checks identity before running any test (`identity` row), and stops with exit 3 on a mismatch.
- **In the app, hwui's RenderThread also loads the stock `vulkan.adreno.so`.** Two Vulkan drivers then live in one process. Nothing went wrong in these runs.
- **The 842.6 package ships renamed dependencies** (`notgsl.so`, `notllvm-glnext.so`, ...). They load from the driver directory.

## Results per driver

Notes on the tables:
- "exe / app" gives the verdict in each context.
- The last column judges the item against the P0.7 test.
- Bandwidth: the verdict is the median, over 32 repetitions, of the paired ratio placed (or mapped) / anonymous, with a pass at ≥ 90% for each of memset, memcpy and read-sum. The exe runs and the 64 MB per-type pairs are the primary evidence, because they are steadier. The app numbers corroborate them; each app run is listed, since they spread by about ±7% between runs (see surprise 10). Figures without a note are from the final run of the final build.
- Raw results are in `~/dm-work/android/probe-results/gpu/<driver>/{exe,app}/`: `summary.txt`, `results.tsv`, `extensions.txt`, `features.txt`, `properties.txt`, `memory.txt`, `formats.txt`, `maps_*.txt`, `logcat.txt`.
  - Test e is in `app/e/`, because it needs its own process (see "freed memory" below).
  - The separated `MADV_*` split variants (`MADV_DONTNEED` alone, `MADV_FREE + MADV_PAGEOUT`) are in `app/fresh/` (stock: `exe/fresh/`), run after the main matrix. `app/vmas/` holds a separate VMA-merge run. The VMA-merge test and the combined `MADV_DONTNEED + MADV_PAGEOUT` variant are also part of every main run.

### Capabilities (all three)

| Item | stock | 842.6 | Turnip |
|---|---|---|---|
| Device extensions (full list: `extensions.txt`) | 165 | 176 | 149 |
| `VK_KHR_map_memory2` | yes | yes | no (core 1.4 `vkMapMemory2`) |
| `VK_EXT_map_memory_placed`: memoryMapPlaced / RangePlaced / UnmapReserve | **absent** | 1 / 0 / 1 | 1 / 0 / 1 |
| `minPlacedMemoryMapAlignment` | n/a | 4096 | 4096 |
| BC1-BC7 sampled, optimal tiling (16 formats) | all | all | all |
| geometryShader, depthBounds, textureCompressionBC | 1, 1, 1 | 1, 1, 1 | 1, 1, 1 |
| shaderFloat64 | 0 | 0 | 0 |
| sparseBinding, sparseResidencyBuffer | 1, 1 | 1, 1 | 1, 1 |
| multiDrawIndirect, drawIndirectFirstInstance | 1, 1 | 1, 1 | 1, 1 |
| indexTypeUint8 | 1 | 1 | 1 |
| Push descriptors (maxPushDescriptors) | KHR ext, 32 | core 1.4, 32 | core 1.4, 32 |
| Descriptor buffer (with push descriptors) | 1 (1) | 1 (1) | 1 (1) |
| `VK_EXT_external_memory_host` | no | no | no |
| `VK_KHR_pipeline_binary` | no | yes | no |
| Graphics pipeline library | no | yes | yes |
| bufferDeviceAddress (enabled and used by the BDA kernels) | works | works | works |
| Subgroup size; supportedStages | 64; 0x30 (fragment, compute) | 64; 0xf0 (fragment, compute, task, mesh) | 128; 0x3f (all graphics stages + compute) |
| Subgroup supportedOperations | 0x2bf (no clustered, no rotate-clustered) | 0x6ff | 0x6ff |
| maxMemoryAllocationSize; maxBufferSize | 1 GB; 0xfffeefff | 1 GB; 0xfffeefff | 4 GB − 1; 4 GB |
| Heaps | 14.8 GB, 4 GB | 14.8 GB, 4 GB, 8 MB | 11.1 GB (budget 9.4 GB) |
| Host-visible types (flags) | 3 HV\|C, 4-5 HV\|HC, 6 DL\|HV\|HC\|C | 3 HV\|C, 4-5 HV\|HC, 6-7 DL\|HV\|HC\|C | 0 DL\|HV\|HC, 1 DL\|HV\|HC\|C, 2 DL\|HV\|C |
| Storage-buffer memoryTypeBits | 0x41 | 0x41 | 0x7 |
| **Chosen type (item 2)** | **6: DEVICE_LOCAL\|HOST_VISIBLE\|HOST_COHERENT\|HOST_CACHED** | **6: same flags** | **1: same flags** |

Memory property flags: DL DEVICE_LOCAL, HV HOST_VISIBLE, HC HOST_COHERENT, C HOST_CACHED.

### stock (exe and app)

| Item | exe / app | Result | P0.7 |
|---|---|---|---|
| identity | PASS / PASS | as above | - |
| 2 cached+coherent type | PASS / PASS | type 6 | pass |
| 3a-3e placed mapping | SKIP | no `VK_EXT_map_memory_placed` | **fail** |
| 7 bandwidth, normal `vkMapMemory` vs anonymous (memset / memcpy / read-sum) | PASS / PASS | exe: 97 / 100 / 98% (anon best 74.3 / 33.2 / 72.3 GB/s); an earlier exe run 100 / 100 / 100%. App, three runs: 101 / 91 / 113%, 100 / 106 / 99%, 100 / 102 / 106% | pass |
| per-type bandwidth (64 MB, 7 pairs) | - | every host-visible type 98-101% of anon (memset and read-sum), the non-HOST_CACHED types 4-5 included (see surprise 8) | - |
| 5a compute sanity (push constants with and without a preceding transfer, parameter buffer, BDA) | PASS / PASS | all words right | - |
| 5 coherence, descriptors and BDA, with and without flush | PASS / PASS | 204 rounds each, 0 stale words either way | pass |
| 5b GPU writes into untouched memory (fill and kernel) | PASS / PASS | 0 wrong words | pass |
| 7 dirty tracking on a normal mapping (pre-populated) | PASS / PASS | 1 fault at the exact address, write lands, GPU sees it, PROT_NONE→RW keeps the data; 6-9 µs/fault | pass, with the conditions below |
| KGSL split-VMA fault (split.*) | FAIL / FAIL | see surprises | **condition** |
| split KGSL VMAs merge back (vmas.*) | FAIL / FAIL | 4,096 single-page unprotects in a read-only 64 MB range leave 8,193 VMAs, which stay after re-protecting and unprotecting the whole range. Anonymous control merges back to 1 | **condition** |
| freed memory | INFO | given back at once | - |

### 842.6 (app; exe identical except no b and e)

| Item | exe / app | Result | P0.7 |
|---|---|---|---|
| identity | PASS / PASS | `vulkan.ad07xx.so` mapped, E031.50 | - |
| 2 cached+coherent type | PASS / PASS | type 6 | pass |
| 3a placed 256 MB in a 5 GB PROT_NONE reservation | PASS / PASS | pointer == requested, one `/kgsl-3d0` VMA, CPU r/w, the GPU sees the CPU's writes | pass |
| 3a′ placed at 2 MB + 4 KB (minPlacedMemoryMapAlignment) | PASS / PASS | data kept, GPU check ok | pass |
| 3b one 3.5 GB allocation, placed (app only) | - / PASS | 3.5 GB, every page r/w; no bisection needed. Note: maxMemoryAllocationSize says 1 GB | pass |
| 3c two allocations back to back + memcpy across the seam | PASS / PASS | 64+64 MB and 64 MB+4 KB / 64 MB; seam memcpy (aligned and +3) ok; each half lands in its own allocation (GPU copy-out) | pass |
| 3d `vkUnmapMemory2` + RESERVE | PASS / PASS | range stays reserved (`---p`); hinted and unhinted `mmap` stay out; re-place ok; control: a hand-made hole does attract the hinted mmap | pass |
| 3d without RESERVE | INFO | the driver re-reserves anyway (not a hole, unlike the spec) | - |
| 3e total placed and touched (own process) | - / PASS | 6.00 GB in 12 chunks of 512 MB (the cap), every page re-checked | pass |
| 4 bandwidth placed vs anonymous (memset / memcpy / read-sum) | PASS / PASS | exe: 101 / 105 / 100% (anon best 62.4 / 33.3 / 41.6 GB/s); an earlier exe run 100 / 99 / 100%. App, four runs: 99 / 98 / 97%, 101 / 95 / 99%, 97 / 95 / 101%, 105 / 109 / 105% (21 reps) | pass |
| 4 bandwidth normal `vkMapMemory` vs anonymous | PASS / PASS, once FAIL | exe: 100 / 97 / 93%; an earlier exe run 100 / 100 / 100%. App, four runs: 101 / 92 / 99%, 112 / 92 / 98%, **99 / 85 / 100% (FAIL on memcpy)**, 100 / 98 / 96%. The app spread is too wide to place memcpy relative to the 90% line; the exe and per-type results are flat | pass (exe, per-type) |
| per-type, 64 MB (placed and every type) | - | 98-101% of anon for all types, placed included, exe and app | - |
| 5a compute sanity | PASS / PASS | push constants (with and without a preceding transfer), parameter buffer and BDA all right | - |
| 5 coherence: placed and mapped × descriptors and BDA × flush and no-flush | PASS / PASS (8/8) | 204 rounds each (200 × 1 MB with the destination primed in the CPU caches, 4 × 32 MB): the GPU saw every CPU write without a flush, and the CPU saw every GPU write after the fence without an invalidate | pass |
| 5b GPU writes into memory the CPU never touched | PASS / PASS (8/8) | | pass |
| 6 dirty tracking on placed and mapped pages (pre-populated) | PASS / PASS | mprotect RO on a mid-mapping page: rc 0; exactly 1 SIGSEGV (SEGV_ACCERR) at the exact address; the handler unprotects, the write lands; 8 consecutive and 32 alternate pages ok; PROT_NONE→RW keeps the data; a later GPU read sees the writes; 3-7 µs per fault (exe), 7-10 µs (app) over the runs | pass, with the conditions below |
| KGSL split-VMA fault | FAIL / FAIL | see surprises | **condition** |
| split KGSL VMAs merge back | FAIL / FAIL | placed and mapped: 8,193 VMAs left, as stock | **condition** |
| `vkMapMemory` after a placed map / unmap | WARN / WARN | DRIVER BUG: returns the old placed address, with nothing mapped there | avoidable |
| freed memory | INFO | normal mapping: given back after 0.25 s. **Placed: never given back; a hidden 1 GB KGSL mapping stays after `vkFreeMemory`** | avoidable |

### Turnip (app; exe identical except no b and e)

| Item | exe / app | Result | P0.7 |
|---|---|---|---|
| identity | PASS / PASS | `vulkan.ad08xx.so` mapped, driverID 18 | - |
| 2 cached+coherent type | PASS / PASS | type 1 | pass |
| 3a, 3a′ placed | PASS / PASS | as 842.6 | pass |
| 3b 3.5 GB single placed allocation | - / PASS | 3.5 GB in one go (maxMemoryAllocationSize 4 GB) | pass |
| 3c contiguous + seam | PASS / PASS | both variants | pass |
| 3d RESERVE | PASS / PASS | as 842.6; without RESERVE the range becomes a real hole, as the spec says | pass |
| 3e total (own process) | - / PASS | 6.00 GB (the cap) | pass |
| 4 bandwidth placed vs anonymous | PASS / PASS | exe: 98 / 100 / 95% (anon best 77.9 / 33.6 / 72.1 GB/s); an earlier exe run 99 / 99 / 100%. App, two runs: 103 / 96 / 105%, 101 / 107 / 100% | pass |
| 4 bandwidth normal `vkMapMemory` | PASS / PASS | exe: 97 / 100 / 96%; an earlier exe run 100 / 99 / 100%. App, two runs: 100 / 95 / 92%, 100 / 97 / 97% | pass |
| per-type, 64 MB | - | type 0 (HOST_COHERENT, not cached): memset 97-99%, **read-sum 0.67-0.80 GB/s = 3%** of anon; types 1, 2 and placed: 99-102% | - |
| 5a compute sanity, parameters in a storage buffer | PASS / PASS | descriptors and BDA | - |
| 5a compute sanity, **push constants** | **FAIL / FAIL** | every push-constant word reads as garbage; the values change between runs, and none of them is data the probe wrote. The same happens without any transfer command before the dispatch (`ops_push_noxfer`). With BDA pointers in push constants, nothing is written; one early run lost the device. The same SPIR-V passes on stock and 842.6 | (backend issue) |
| 5 coherence (parameters from a buffer) | PASS / PASS (8/8) | as 842.6 | pass |
| 5b fresh memory | PASS / PASS (8/8) | | pass |
| 6 dirty tracking (pre-populated) | PASS / PASS | as 842.6; 4.5-6 µs per fault (exe), 6-7.5 µs (app) | pass, with the conditions below |
| KGSL split-VMA fault | FAIL / FAIL | identical to 842.6 and stock | **condition** |
| split KGSL VMAs merge back | FAIL / FAIL | identical: 8,193 VMAs left | **condition** |
| `vkMapMemory` after placed | PASS / PASS | a correct new mapping | - |
| freed memory | INFO | given back at once, placed included | - |

## Surprises

1. **KGSL split-VMA fault bug (all three drivers, so the kernel).**
   - **Trigger:** `mprotect` on one page in the middle of a `/kgsl-3d0` mapping splits it into 3 VMAs. This persists after setting the page back to RW (the VMAs do not merge; see surprise 2).
   - **Effect:** any page *first faulted in after the split*, in a VMA that no longer starts at the object's offset 0, maps the wrong part of the GPU object. The offset is computed from the split VMA's start, so it ignores `vm_pgoff`.
   - **Measured:** for a split at +0x206000, a CPU read at object offset 0xa06000 returns object offset 0x800000 (delta −2072 KB = the split offset). A CPU store meant for 0x864040 landed at 0x65e040 (`split.*.cpu_write_after_split`). That is silent corruption of another part of the allocation, through placed and normal mappings alike.
   - **Unaffected:** pages faulted in before the split stay correct.
   - **Mitigation:** fault every page in before the first `mprotect`. Both ways work on all three drivers:
     - touching every page: 0.9-1.6 µs/page, 0.24-0.42 s/GB;
     - `madvise(MADV_POPULATE_WRITE)`: 0.09-0.17 s/GB.
   - **The dirty-tracking tests pre-populate their mappings for this reason.** Without that, the GPU-read check failed on every driver. Their result lines say so.
   - **The mitigation lasts only as long as the page table entries do.** After populate + split:
     - `MADV_DONTNEED` returns 0, drops the pages, and the next touch maps the wrong offset again: all 2,097,152 words wrong (`split.*.populated_split_dontneed_only`).
     - `MADV_FREE` fails with EINVAL on these shared mappings, so it changes nothing. `dm/host/hle_svc.c:252` uses it in UnmapPhysicalMemory.
     - `MADV_PAGEOUT` returns 0 and changes nothing. Android's cached-app compaction uses it.
     - Untested: KGSL's own reclaim of a backgrounded process. The KGSL sysfs is not readable from shell or app.
2. **Split KGSL VMAs never merge back (all three drivers, so the kernel).**
   - **Measured (`vmas.*`):** a 64 MB mapping, populated, is set read-only; then 4,096 single pages (every fourth) are made writable one at a time, as the fault handler does. That leaves 8,193 VMAs. Re-protecting the whole range and then making it all writable leaves 8,193 VMAs too.
   - **Control:** anonymous memory goes back to 1 VMA.
   - **Data:** stays correct (CPU and GPU checks).
   - **Cost:** 3-5 µs per single-page `mprotect`, the same as anonymous memory.
   - **Why:** KGSL VMAs carry flags and a `close` handler that the kernel never merges. So every page boundary the tracker ever toggles stays a VMA boundary, until the mapping goes away.
   - **Scale:** a process has at most `vm.max_map_count` VMAs (65,530 by default; not readable from shell or app). The probe app starts at about 3,600-4,200, and the real host will have more. Past the cap, `mprotect` in the fault handler fails with ENOMEM, and so does every other `mmap` in the process.
   - **Worst case:** an isolated unprotected page costs 2 VMAs (measured: 4,096 pages → 8,193 VMAs); a fully toggled range costs 1 VMA per protection unit. So the VMA count is bounded by the tracked range divided by the granularity, and the cap is reached at about 61,000 units: 240 MB of tracked range at 4 KB, about 0.95 GB at 16 KB (the Mac host's page size, which `nvn_dirty.c` uses today), about 3.8 GB at 64 KB. Scattered writes reach it sooner: about 120 MB of isolated 4 KB pages.
   - **Strategy B is not affected:** it tracks anonymous memory, which merges.
3. **Turnip a8xx: compute push constants read garbage**, with or without a transfer command earlier in the command buffer. Parameters from a storage buffer work, with and without BDA. Graphics push constants were not tested. Because of this, the probe's coherence kernels take their parameters from a buffer the GPU writes (`vkCmdUpdateBuffer`). The push-constant path is checked separately by `kdiag.ops_push.*`.
4. **842.6 never frees placed memory while the process lives.**
   - Placing a 1 GB allocation shows 2 GB of `/kgsl-3d0` mappings: the driver keeps a hidden full-size mapping of its own.
   - After `vkFreeMemory`, that hidden mapping stays and MemAvailable does not recover, with either unmap. It recovers at process exit.
   - Normal mappings on 842.6 are freed after about 0.25 s; on stock and Turnip, at once.
   - Consequence for the probe: test e (total) must run in its own process. After b's 3.5 GB in the same process, e stopped at 4.0 GB on the MemAvailable floor.
5. **842.6 `vkMapMemory` after a placed mapping returns the old placed address with nothing mapped there.** Seen after the placed map and after an unmap. Never mix placed and normal mapping of the same `VkDeviceMemory` on 842.6.
6. **842.6 always re-reserves on unmap**, even without `VK_MEMORY_UNMAP_RESERVE_BIT_EXT`. Turnip leaves a hole, as the spec says.
7. **maxMemoryAllocationSize is 1 GB on both Qualcomm drivers, yet a single 3.5 GB allocation works** on 842.6 (and placed). Turnip reports 4 GB − 1. Under A, chunking at 1 GB keeps to the advertised limit; the limit is not enforced.
8. **Every host-visible type on the Qualcomm drivers is CPU-cached in practice.** The types without HOST_CACHED (4, 5) read as fast as anonymous memory. On Turnip the non-cached type reads at 3% of anon. That Turnip result is also the control showing the bandwidth test can detect an uncached mapping.
9. **KGSL commits memory at `vkAllocateMemory`**, not at first touch: MemAvailable drops by the full size at allocation. Under A, the whole guest heap costs RAM from boot, unlike today's lazily committed anonymous reservation.
10. **App bandwidth is noisy and exe bandwidth is not.**
   - In the app, single 256 MB passes vary by up to ±40% between neighbouring repetitions (memset 28-62 GB/s); a best-of-9 ratio moved by ±12% between runs.
   - In the exe, passes within one run are steady (within about 1%), but the level moves between runs. The same 256 MB anonymous memset ran at about 38.5 GB/s in the morning's exe runs and at 62-78 GB/s in the final ones. Absolute GB/s from different runs cannot be compared; the in-run paired ratios stay at 93-105%.
   - The probe therefore uses a paired design: each operation runs on every kind back to back, the order rotates per repetition, there are 32 repetitions after 1 s of memory warm-up, and the verdict is the median of the per-pair ratios. Best-of values are reported too.
   - This replaces the pre-registered "best of 3 ≥ 90%" criterion, which in the app was decided by noise: one run gave a FAIL of 88% that a rerun did not reproduce. In the final runs the best-of values (best of 32) pass that criterion too: the lowest is 93% (Turnip exe `vkMapMemory` memset, 842.6 exe placed read-sum).
   - Even the paired median spreads by about ±7% between app runs (842.6 `vkMapMemory` memcpy: 85%, 92%, 92%, 98%). So the app numbers alone cannot place a 95% result relative to the 90% line.
   - Best-of values are fragile even in the exe: one exe run had a single repetition at 134% (memset, placed) while its median was 100%.
   - The "as fast as anonymous" verdict therefore rests on the exe medians and the 64 MB per-type pairs (97-102%), with Turnip's uncached type at 3% as the positive control. Exe medians: placed 95-105% on every driver and run; normal mapping 93-100%.
11. **Turnip's git hash differs from PLAN's source reference.** PLAN cites `e9a83d530cce`; the package is `git-9a720fe0e4`.
12. **libadrenotools exports `adrenotools_import_user_mem`** (KGSL import of a host pointer). It would let the GPU use the existing anonymous guest memory without placed mapping, but it was not tested. `VK_EXT_external_memory_host` is absent on all three drivers.

## Recommendation

**Strategy A on 842.6, with coarse-grained dirty tracking on placed memory. Stock uses B. Turnip as a second option once its push constants work.**

**Why A, not B, where placed mapping exists:**
- Placed memory on 842.6 and Turnip is cached, coherent and as fast as anonymous memory: exe paired medians 95-105%, and per-type pairs at 64 MB within 98-102%. The app runs agree within their noise.
- The GPU and CPU see each other's writes in both directions without flush or invalidate, including through BDA.
- `mprotect` write-fault tracking works on driver pages at 3-10 µs per fault. Its scale limit (surprise 2) is handled by condition 2 below.
- 6 GB places contiguously, and a 3.5 GB allocation places in one piece.
- B would add a CPU copy of every dirty page to every submit, plus a second copy of all GPU-visible guest memory.
- B's advantage, tracking anonymous memory without a VMA limit, is real but smaller: under A only texture pools need write tracking (`nvn_dirty.c` covers packaged-texture pool pages), and coarse granularity keeps them under the cap. The cost is false-dirty re-uploads, which Phase 2 must measure.

**Why not A′:**
- A′ places and frees memory per pool, and 842.6 keeps every freed placed allocation until exit.
- Each pool's fresh KGSL mapping would also need populating before the dirty tracker touches it.
- A places the heap once.

**Conditions for A (to carry into Phase 2.2):**
1. **Populate every placed chunk** with `madvise(MADV_POPULATE_WRITE)` right after placing it, before `nvn_dirty.c` can `mprotect` any page in it. This costs about 0.1-0.2 s per GB at boot.
   - **Never drop pages of placed memory.** No `MADV_DONTNEED` on placed ranges, ever: on a split VMA, the refault reads and writes the wrong part of the object. Guest "unmap" paths keep the pages. `hle_svc.c`'s `MADV_FREE` fails harmlessly on KGSL, but its `mprotect(PROT_NONE)` adds VMA boundaries (condition 2).
   - Add a startup canary: place, split, read back a GPU-written page, and refuse A (fall back to B) on a mismatch.
   - Keep canary checks in debug builds. A PTE zap the probe did not test (KGSL reclaim in the background) would bring the corruption back silently.
2. **Bound the VMA count of dirty tracking on placed memory.** Worst case, VMAs ≈ tracked bytes / protection granularity, and they never merge back.
   - Use a protection granularity of 64 KB on placed memory to start with: about 3.8 GB of tracked memory fits under the cap. The fault handler unprotects the whole 64 KB block.
   - Have the tracker count the boundaries it has created. Near a budget (for example 40,000), stop tracking further pools: treat them as always dirty, as `nvn_dirty.c` already does for pools past its table.
   - Before Phase 2.2, measure on the Mac how many distinct pages `nvn_dirty.c` unprotects per game. That says whether 16 KB would fit, and what 64 KB costs in false-dirty re-uploads.
3. **Never unmap or free placed memory on 842.6** before exit. Grow the heap with new chunks; on shrink, keep the chunks.
4. **Never `vkMapMemory` memory that has been placed** (842.6 stale pointer).
5. **Chunks of at most 1 GB** (842.6's advertised `maxMemoryAllocationSize`). Pools that cross a chunk join need a sparse buffer (`sparseBinding` and `sparseResidencyBuffer` are present on all three) or chunk-aligned heap growth.
6. **Budget RAM for the whole heap at boot** (KGSL commits at allocation).
7. **For Turnip on this a8xx build:** pass shader parameters through a buffer, not push constants. Re-test with newer Turnip builds.

**Default driver (open question 1):** 842.6.
- Its two bugs are avoided by conditions 3-4.
- Turnip's push-constant bug affects the backend's core path.
- Turnip is better on everything else it was tested on: spec-correct unmap, memory freed, 4 GB allocations, all subgroup stages.

## AHardwareBuffer-backed pools: zero-copy buffers on the stock driver (2026-10-26)

**The idea is shirai91/nx2android's (MIT):** `probes/vkzc.c`, `pool_zero_copy` in `nxnative/dm/android/gfx_vk_core.c`, and the defragmenting remap `nvn_dirty_defrag` in `nxnative/dm/gfx/nvn/nvn_dirty.c`. They report it working on a Lenovo Legion Y700 (Adreno 830). The test code here is our own (`dm/android/probe/gpu/ahb_probe.c`).

The method:
1. Allocate a BLOB `AHardwareBuffer` (`CPU_READ_OFTEN | CPU_WRITE_OFTEN | GPU_DATA_BUFFER`).
2. Get its dma-buf fd through `AHardwareBuffer_sendHandleToUnixSocket` and a socketpair.
3. `mmap(MAP_SHARED | MAP_FIXED)` the fd over the pool's guest range, copying the old contents in.
4. Import the same AHB into Vulkan as a dedicated `VkBuffer` (`VK_ANDROID_external_memory_android_hardware_buffer`).

The game's own pointer is then GPU memory, and the pool needs no shadow buffer and no upload.

**Phones:**
- **Samsung Galaxy Z Fold 7, `SM-F966B`** (the main runs, 2026-10-26).
  - Snapdragon 8 Elite, Adreno 830.
  - Build `F966BXXSDCZJ1` (Android 17, `CP2A.260605.016`), kernel `6.6.127-android15-8` (Samsung), 4 KB pages.
  - Stock driver: `vulkan.adreno.so`, `E031.47.18.56`, 06/22/26. 842.6: `E031.50.19`, loaded through libadrenotools as before.
- **nubia `NX789J`**, the phone of the P0.7 runs above: a second device, stock driver only, 2026-10-27. See [Second device](#second-device-nubia-nx789j).
  - `ro.soc.model` SM8750.
  - Build `nubia/NX789J-EEA/NX789J:16/BQ2A.250705.001-BP2A.250605.031.A3/20260519.002456`, kernel `6.6.92-android15-8-g3637f4904cf5-ab13944661-4k`, 4 KB pages.
  - Stock driver: `vulkan.adreno.so`, `E031.47.18.49`, 02/12/26.
- Turnip: SKIP (its package is gone).

**Result:** the method works on the Fold 7, on both drivers, as an adb executable and in the APK, and on the Nubia's stock driver in both contexts. Two limits are new:
- **Size: nothing above 64 MB.** On the Fold 7's build, an `isSupported` query for a larger AHB crashed the system allocator service. That is a bug in the vendor's allocator, not ours, but it means no AHB above 64 MB can be requested safely on that phone; see below.
- **Some kernel paths refuse the mapping.** It is a PFN mapping (`VM_PFNMAP | VM_IO`): `process_vm_readv`, shared futexes, `O_DIRECT` and every `madvise` fail on it.

One limit is known from KGSL: split VMAs never merge back. Unlike KGSL, mapping the fd again over the range restores one VMA. Everything else passed, and CPU bandwidth equals anonymous memory.

### The allocator crash

The first stock APK run (`stock/app/fold7-ahb/`, kept with `NOTE.txt` and `allocator_crash.txt`) queried `AHardwareBuffer_isSupported` for widths from 64 MB to 4 GB − 1.
- **What crashed:** a query above 64 MB (the 1 GB or the 2 GB one) aborted `vendor.qti.hardware.display.allocator-service`. The abort was `ubsan: mul-overflow` in `snapalloc::SnapConstraintManager::AlignmentToAlignedConstraints`, reached from `SnapAllocCore::IsSupported` through `SnapAllocCore::Allocate`.
- **What followed:** surfaceflinger aborted ("Failed to get AIDL composer service"), and the framework restarted (BootReceiver: `P|SF`). The kernel did not reboot.
- **The probe's view:** the later queries returned `DEAD_OBJECT` (-32), and the next `AHardwareBuffer_allocate` (64 MB) returned -129. Those values are artifacts of the dead service, not AHB limits.
- **Who can trigger it:** an untrusted app (uid 10050), with one `isSupported` call. That makes it a local denial of service on the Fold 7's build.

**Consequences:**
- `isSupported` is no safe pre-check: it runs the allocator's allocation path.
- The probe now caps every AHB at 64 MB (`AHB_CAP`, enforced in `ahb_alloc_desc`) and stops its AHB tests if an allocate returns -32 or -129.
- Every run since went through `~/dm-work/android/ahb/run_checked.sh`. It compares the pids of the allocator service, surfaceflinger and system_server before and after; every run's `services.txt` shows them unchanged.
- **The largest working size is unknown.** 64 MB (and 64 MB − 4 KB, 32 MB, 16 MB) was allocated, mapped, imported and checked page by page. Nothing above 64 MB was tried again, because each miss restarts the phone's UI. The log does not say which query crashed: its five `DEAD_OBJECT` lines fit either the 1 GB or the 2 GB one, so even 1 GB is not known to be safe.

### Results

Raw results are in `~/dm-work/android/probe-results/gpu/<stock|842>/<exe|app>/`:
- `fold7-ahb2/`: `--tests kdiag,ahb,fresh,vmas,dirty`;
- `fold7-ahbbw/`: bandwidth;
- `fold7-ahbsize/`, `fold7-ahbmany/`, `fold7-ahbchurn/`: the heavy tests, APK only.

Verdicts are identical in the four contexts (stock exe, stock app, 842.6 exe, 842.6 app) unless the table says otherwise. Numbers are given as stock exe / stock app / 842.6 exe / 842.6 app.

**Checks tightened after these runs (review round 1).** The Fold 7 was on its lock screen afterwards, so its tables still hold the earlier build's results. The tightened build ran on the Nubia instead ([Second device](#second-device-nubia-nx789j)): every run met the new validity conditions, and every verdict matches the earlier build's (`ahb.vmas` is still FAIL, as it should be). The changes:
- **Split tests:** every `mprotect` must succeed, and the range must have more than one VMA afterwards. The earlier runs recorded 19 and 17 pieces but did not record `mprotect` failures.
- **`ahb.vmas`:** valid only with no `mprotect` failure, a fragmented range and intact data. The earlier runs recorded 0 failures, 1 → 8,193 VMAs and 0 wrong words, so they meet this. The defrag verdicts now also require that there was something to defrag.
- **Futex wake:** the parent retries the wake (1 ms apart, up to 3 s) until it wakes the waiter, instead of a single wake after 60 ms. The earlier verdicts did not depend on the timing: the private wakes all woke the waiter, and the shared ones failed with EFAULT.
- **`ahb.churn`:** finalize's return is checked. After each cycle the range must be anonymous again. fds and dma-buf mappings may not grow by more than 2 after cycle 20. The earlier runs' progress lines meet the growth rule (stock fds 134 at cycle 20 and at the end, dma-buf mappings 7 → 7; 842.6: 137 and 137, 11 → 11).
- **`apool_finalize`:** it no longer destroys a pool it cannot save, and copies back only after the anonymous mapping succeeded.

| Check | Result |
|---|---|
| Import extension (`ahb.ext`, `ahb.ext_buffer_props`) | `VK_ANDROID_external_memory_android_hardware_buffer` and `VK_EXT_queue_family_foreign` present on both drivers. Buffer import of the AHB handle type: importable, exportable, not dedicated-only |
| `isSupported` up to the cap | 1 for 16 KB, 64 KB, 1, 4, 16, 32, 40 and 64 MB |
| Memory types the import offers (`ahb.import_types`) | `memoryTypeBits` 0x12: type 1 DEVICE_LOCAL and type 4 DEVICE_LOCAL\|HOST_VISIBLE\|HOST_COHERENT (both drivers). The probe binds type 1. Imported memory is never `vkMapMemory`'d: the CPU reaches it only through the guest mapping |
| The mapping (`ahb.vmflags`) | `/dmabuf:qcom,system`, VmFlags `rd wr sh mr mw me ms pf io de dd`: **a PFN mapping (`pf`, `io`)** |
| Chain over a `PROT_NONE` reservation (`ahb.chain_reserve`) | PASS: 64 MB at a 4 KB-aligned guest address. The GPU reads the CPU's writes (0 wrong) and the CPU reads the GPU's writes after the fence (0 wrong), with no flush or invalidate. Finalize (back to anonymous memory) keeps the data |
| Chain over part of a live RW heap range (`ahb.chain_heap`) | PASS: a 40 MB pool at offset 0x1007000 of a 128 MB range holding data. Copy out, map and copy back keep it (0 wrong for the CPU and the GPU); the neighbours are intact. Finalize keeps everything. The range goes from 1 VMA to 3 and back to 1 |
| Pool init cost, 40 MB (`ahb.pool_init_cost`) | allocate 0.30-0.41 ms, dma-buf fd 0.03-0.07 ms, `mmap` 2.8-3.1 ms, import 0.24-0.26 ms; the two copies 14.6-17.8 ms. Finalize 26-28 ms |
| Page tables (`ahb.populate`) | The whole range is mapped at `mmap`: the first read of 4,096 pages takes 0 page faults (KGSL control: 4,096). Rss does not count it |
| KGSL split-VMA bug (`ahb.split.*`) | **Not present.** 16 MB written by the GPU and split into 19 VMAs by single-page `mprotect`s, then every word read for the first time: 0 wrong. A CPU store into an untouched page after the split lands at its own offset. A mapping at fd offset 0x101000, split before the first touch: 0 wrong. On the same runs the KGSL control (`split.mapped.*`) reproduces surprise 1 on this phone too (delta −2072 KB, all four contexts) |
| `madvise` (`ahb.split.madvise`) | `POPULATE_READ`, `POPULATE_WRITE`, `COLD`, `PAGEOUT`, `FREE` and `DONTNEED` all fail with EINVAL. So `hle_svc.c:252`'s `MADV_FREE` would neither drop nor free pool pages |
| Write faults (`dirty.ahb`, `ahb.faults4096`) | PASS. The dirty-tracking test (6) passes on a 64 MB AHB: 2.8 / 3.1 / 3.2 / 3.4 µs a fault. 4,096 write faults on a read-only 32 MB AHB, one per other page: 3.5 / 4.4 / 4.5 / 4.2 µs a fault; every store landed; no other word changed; the GPU sees all 4,096 |
| VMA merging (`ahb.vmas`) | FAIL, as on KGSL: 4,096 single-page unprotects in a read-only 64 MB AHB leave 8,193 VMAs, still 8,193 after re-protecting and unprotecting the whole range. 2.3-3.1 µs per `mprotect`; data intact |
| Defrag: map the same fd again over the range (`ahb.vmas.defrag`, `ahb.faults4096.defrag`, `ahb.split.remap`) | PASS. 8,193 pieces back to 1 VMA in 8.2 / 7.5 / 9.4 / 7.7 ms for 64 MB (119-151 ms per GB); data intact for the CPU and the GPU. The process VMA count returns to its starting value. The range comes back read-write |
| Coherence without flush (`coh.ahb.desc.noflush`, `coh.ahb.bda.noflush`) | PASS: 204 rounds each (200 × 1 MB, 4 × 31 MB), descriptors and BDA, 0 stale words either way |
| Buffer device address (`ahb.bda`) | PASS: an import with `VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT` gets an address |
| GPU writes into a pool the CPU never touched (`ahb.fresh_fill`) | PASS: 32 MB fill, 0 wrong |
| 256 pools of 64 KB (`ahb.pools256`) | PASS: 0.09-0.12 ms to create a pool and 0.03-0.04 ms to free one. Each pool costs 4.00 fds and 3.0 VMAs. The GPU read every pool's CPU-written word |
| CPU bandwidth, AHB vs anonymous, 64 MB, paired median of 32 (`ahb.bw`) | PASS. memset / memcpy / read-sum: stock exe 104 / 100 / 101%, stock app 95 / 99 / 110%, 842.6 exe 102 / 98 / 102%, 842.6 app 104 / 99 / 111%. Anonymous memory, best of 32 (stock exe): 35.3 / 22.3 / 51.9 GB/s. The mapping is CPU-cached |
| Host paths on the mapping (`ahb.host.*`), with anonymous and KGSL controls | see the next table |

**The host's memory paths on an AHB mapping** (the same in all four contexts; the anonymous and KGSL controls pass every item):

| Path | AHB | What uses it in the host | Blocks zero-copy? |
|---|---|---|---|
| `process_vm_readv` on self | **FAIL: EFAULT** (page pinning refuses PFN mappings) | `vm_read_overwrite` (`dm/host/plat/plat_linux.c:13`), the host's fault-safe read. Its callers: `crash.c` (crash dumps), `hle_thread.c` (thread and wait diagnostics), `hle_fpslock.c` (reads game variables), `hle_movie.c:127,135` (checks an EventType pointer before signalling it), `probe.c` (debug probes) | **Yes, for any pool those callers read**, until `vm_read_overwrite` has a fallback (condition 2). None of them reads pool data as its job; they would see pool memory as unreadable. Reasoning, not tested: `hle_movie.c` would skip signalling an event that lived in a pool |
| `write()` from the mapping into a pipe | PASS. On a `PROT_NONE` address it returns EFAULT with no signal | nothing yet: a candidate fallback for `vm_read_overwrite` | no |
| futex, private | PASS (EAGAIN, timeout, wake) | no futex calls on guest addresses in `dm/host` (its waits use pthread condition variables) | no |
| futex, shared | **FAIL: EFAULT** | none today | no, today. A future host must keep to private futexes on pool memory |
| `read()` / `pread()` from a file into the mapping | PASS (1 MB + 3 at an odd offset; the GPU sees it) | `hle_fs.c` `ReadFile` reads straight into guest buffers | no |
| `write()` from the mapping into a file | PASS | file writes from guest buffers | no |
| `O_DIRECT` read into the mapping | FAIL: EFAULT | not used by the host | no |
| `madvise` (any advice) | FAIL: EINVAL (`ahb.split.madvise`) | `hle_svc.c:252` (`MADV_FREE` in UnmapPhysicalMemory) | no, but that call does nothing on a pool range |
| LDAXR/STLXR, and LSE LDADDAL/CASAL | PASS: 4 threads × 250,000 increments with no lost ones; 7.7-10.7 ns per op, as on anonymous and KGSL memory | the guest's atomics | no |

**Heavy tests (APK only):**

| Test | stock app | 842.6 app |
|---|---|---|
| Sizes up to the cap (`ahb.size.*`) | PASS for 16 MB, 32 MB, 64 MB − 4 KB and 64 MB: each was written page by page by the CPU, read back by the GPU from sampled pages (0 wrong), filled at the end by the GPU and read by the CPU (0 wrong). Above 64 MB: SKIP (the allocator crash) | PASS, the same four sizes, 0 wrong. Above 64 MB: SKIP |
| Many 16 KB pools (`ahb.many`) | 8,128 pools, stopped by the probe's fd budget check (run every 64 pools; it stops once fewer than 256 fds remain under `RLIMIT_NOFILE`, soft 32,768, hard 524,288): 32,644 fds were open, 124 left. The limit itself was not hit, so the allocator service never had to fail an allocation. The run: 4.00 fds and 3.01 VMAs per pool; 0.32 ms to create a pool and 0.33 ms to free one (averages; creation slows from 0.17 ms at 2,048 pools); 42.5 KB of MemAvailable per pool. The GPU read every pool. The first pool held three dma-buf fds: the 16 KB buffer and two of 28 KB | The same stop: 8,128 pools, 32,648 fds open, 120 left. 4.00 fds and 3.01 VMAs per pool; 0.32 ms to create and 0.37 ms to free; 34.7 KB of MemAvailable per pool; the GPU read every pool. The same three dma-buf fds for the first pool |
| Pool life cycle, 100 × 16 MB (`ahb.churn`: init with copy, GPU check, finalize, free) | PASS. Init 13.8 ms and finalize 20.5 ms per cycle (0.86 / 1.28 ms per MB). MemAvailable 6.14 GB at the start, 6.12 GB at the lowest, 6.20 GB at the end: the memory comes back. dma-buf mappings 7 → 7, fds 133 → 134 | PASS. Init 9.6 ms and finalize 16.8 ms per cycle (0.60 / 1.05 ms per MB). MemAvailable 5.63 → lowest 5.62 → 5.64 GB: the memory comes back. Unlike 842.6's placed memory (surprise 4), imported AHBs are freed. dma-buf mappings 11 → 11, fds 136 → 137 |

`maxMemoryAllocationCount` is 4,294,967,295 on both drivers, so the per-pool limits are the process's fds (4 per pool) and VMAs (3 per pool, against `vm.max_map_count`: unreadable from shell and app, 65,530 by kernel default).

### Second device: nubia NX789J

Stock driver `E031.47.18.49`, the review-round-1 build, 2026-10-27. Raw results are in `~/dm-work/android/probe-results/gpu/stock/<exe|app>/nubia-*/`:
- `nubia-ahb/`: `--tests ahb`, exe and app;
- `nubia-ahbchurn/`: app;
- `nubia-cohbw/`: `--tests coh,bw`, exe. This is a regression check of the shared probe code changed for the AHB tests, on ordinary mapped memory.
- `nubia-ahb-r2/` (exe) and `nubia-ahbchurn-r2/` (app): the same tests with the review-round-2 build, which only adds failure handling. Every verdict and every count (pieces, VMAs, faults, fds, dma-buf mappings) matches the runs above.

Every run kept the allocator service, surfaceflinger and system_server pids (1662, 1763, 3008). Not run on the Nubia: `ahbbw`, `ahbsize`, `ahbmany` and 842.6.

Numbers are given as exe / app.

| Check | Nubia (tightened build) | Same as the Fold 7 (earlier build)? |
|---|---|---|
| Import extension, buffer import properties | present; importable, exportable, not dedicated-only | yes |
| `isSupported` up to the cap | 1 for all eight sizes (16 KB to 64 MB) | yes |
| Memory types the import offers | `memoryTypeBits` 0x12 (types 1 and 4), type 1 bound | yes |
| The mapping | `/dmabuf:qcom,system`, `pf io` | yes |
| Chain over a `PROT_NONE` reservation, and over a live heap range | PASS, 0 wrong; the heap range goes 1 → 3 → 1 VMAs | yes |
| Pool init cost, 40 MB | allocate 0.54 / 0.25 ms, fd 0.06 / 0.06 ms, `mmap` 6.9 / 1.9 ms, import 0.18 / 0.07 ms, the two copies 24.3 / 9.1 ms; finalize 43.6 / 35.5 ms. The exe rerun (`nubia-ahb-r2`): `mmap` 0.24 ms, copies 9.4 ms, finalize 14.1 ms | same order. These timings vary more between runs than between the phones (Fold 7: `mmap` 2.8-3.1 ms, copies 14.6-17.8 ms, finalize 26-28 ms) |
| Page tables | 0 page faults on the first read of 4,096 pages (KGSL control: 4,096) | yes |
| Host paths | `process_vm_readv` EFAULT, shared futex EFAULT, `O_DIRECT` EFAULT; pipe `write()`, private futex (woken at the first wake call), file `read()` / `write()`, LDAXR/STLXR and LSE PASS. The anonymous and KGSL controls pass every item | yes. Atomics take 11.9-30.3 ns per op, against 14.1-22.0 on anonymous memory in the same runs (Fold 7: 7.7-10.7): the CPU, not the mapping |
| KGSL split-VMA bug (`ahb.split.*`, tightened) | not present: 19 pieces and 0 `mprotect` failures, 0 wrong; the store lands at its own offset; fd offset 0x101000: 17 pieces, 0 failures, 0 wrong; remap back to 1 VMA | yes |
| `madvise` | all six EINVAL, on a 19-VMA range | yes |
| Write faults | dirty test (6): 6.3 / 12.5 µs a fault; 4,096 faults: 8.7 / 10.5 µs a fault, all stores landed, the GPU sees them | verdicts yes; about 2-3× the Fold 7's 2.8-4.5 µs. The P0.7 runs measured 3-10 µs on this phone |
| VMA merging (`ahb.vmas`, tightened) | 1 → 8,193 VMAs, never merge; 0 `mprotect` failures, data intact (a valid run); 6.4 / 5.2 µs per `mprotect` | yes; `mprotect` about 2× slower |
| Defrag by remapping the fd | PASS: 12.7 / 17.8 ms per 64 MB (203 / 284 ms per GB); after the 4,096 faults 11.6 / 11.4 ms | yes; slower |
| Coherence without flush, descriptors and BDA | PASS, 204 rounds each | yes |
| BDA, GPU fill of an untouched pool | PASS | yes |
| 256 pools of 64 KB | PASS: 0.18 / 0.17 ms to create, 0.05 / 0.04 ms to free; 4.00 fds and 3.01 VMAs per pool | yes; creation slower |
| Pool life cycle, 100 × 16 MB (app, tightened) | PASS: init 6.25 ms, finalize 13.85 ms per cycle (0.39 / 0.87 ms per MB). The range was anonymous after every finalize. dma-buf mappings 8 → 8 (after cycle 20) → 8, fds 150 → 152 → 152; MemAvailable 9.08 → lowest 9.04 → 9.04 GB | yes; faster |
| Regression: `coh.mapped.*`, `bw.*` on ordinary mapped memory (exe) | the same six items as the P0.7 run of 2026-10-25; coherence 4 × PASS at "4 × 32 MB"; `bw.vkMapMemory` 100 / 99 / 100% of anonymous memory (P0.7: 97 / 100 / 98%) | n/a |

The Nubia's data agrees with the Fold 7's on every verdict, so the decision and its conditions below are unchanged. The allocator crash above 64 MB is known only from the Fold 7. Nothing above 64 MB was requested on the Nubia, and the cap applies to both phones.

### Decision

**Use it with conditions, on the stock driver, for buffer pools of up to 64 MB. It is an optimization of strategy B, not a replacement, and it is not needed for Stardew Valley.**

**What it would buy.** It replaces only strategy B's buffer shadows and their copies at commit.
- On the phone those cost about 0.27 ms a frame at the title and 0.29-0.38 ms on the farm, and 60 fps already holds (PLAN.md §2.2).
- Textures are unchanged: they still need CPU deswizzling and write tracking.
- On 842.6 the method works too, but strategy A is there already.

**Conditions:**
1. **A fixed size cap of at most 64 MB per AHB**, as a constant: never derived from `isSupported`, which crashes the allocator service above it on the Fold 7's build (the Nubia was not tested above 64 MB). Pools above the cap stay on B.
   - Untested reasoning: splitting a large pool across several AHBs would give it several `VkBuffer`s, since an imported AHB's memory binds only its own dedicated buffer. Every binding would then have to stay inside one piece.
2. **No `process_vm_readv` on pool memory.** `vm_read_overwrite` needs a fallback that works on PFN mappings. Copying through a pipe with `write()` works there and fails cleanly on unmapped memory (measured). Until then, its callers (above) treat pool memory as unreadable.
3. **Private futexes only** on pool memory. No `O_DIRECT` and no `madvise` either: `madvise` fails with EINVAL, so guest unmaps of pool memory must finalize the pool, not rely on `MADV_FREE`.
4. **VMA budget for write tracking**, as with KGSL: unprotected pages never merge back. Unlike KGSL, mapping the fd again restores one VMA (about 120-150 ms per GB). The remap leaves the whole range read-write, so the tracker must re-protect what it tracks afterwards (reasoning, not tested).
5. **fd and VMA budget per pool:** 4 fds and 3 VMAs.
   - 8,128 pools fit under the default soft fd limit (measured). The hard limit is 524,288, so the host could raise the soft limit (reasoning, not tested).
   - BotW keeps about 1,760 pools alive (`docs/agents/04-graphics.md`), about 7,000 fds.
6. **Pool init and finalize need a quiescent range** (reasoning). Between the copy out and the copy back, a write from another thread to that range would be lost. The measured costs: init 0.60-0.86 ms per MB and finalize 1.05-1.28 ms per MB (16 MB pools); 3 ms of `mmap` per 40 MB.
7. **Never `vkMapMemory`, flush or invalidate the imported memory**: the CPU uses the guest mapping. Coherence without flushes was measured that way.
8. **Watch the system services:** a test or a host path that requests an AHB should check that the allocator service survives. A dead service shows up as -32 / -129 from `AHardwareBuffer_allocate`, not as a crash in our process.

**Not run:**
- The tightened checks of review round 1 on the Fold 7 (it was on its lock screen). They ran on the Nubia, along with the `coh,bw` regression check.
- On the Nubia: `ahbbw`, `ahbsize`, `ahbmany` and 842.6.
- AHB sizes above 64 MB: each miss restarts surfaceflinger and the framework.
- The allocation failure at fd exhaustion: the shared allocator service would have had to fail an allocation.
- Turnip: its package is gone.
- What happens to the mapping when the system reclaims memory from a backgrounded app: not tested (the probe ran in the foreground).
- A `vm_read_overwrite` fallback through a pipe was measured only as single `write()` calls (4 KB on the mapping, 4 KB on a `PROT_NONE` address), not as a host function: the probe makes no host or backend changes.

## Method notes

- **Placed tests (a-e):**
  - Every check reads back through the GPU (copy-out via `vkCmdCopyBuffer`), so a mapping that looks right to the CPU but points at the wrong memory is caught.
  - 3d's "kept out" claims are validated by a control: a hand-made `munmap` hole that does attract the hinted mmap.
- **Coherence (5):**
  - The CPU writes a hash pattern without flushing. The kernel checks every word and writes a transform.
  - The CPU checks every result word right after the fence, without invalidating.
  - The 1 MB rounds first read the destination, so stale lines sit in the CPU caches.
  - The flush/invalidate runs are the control.
  - Results are only trusted where the compute sanity test (5a) passes on the same driver. That is how the Turnip push-constant bug was told apart from a coherence failure.
- **Dirty tracking (6):**
  - A SIGSEGV handler with SA_SIGINFO|SA_ONSTACK records the fault address and code and unprotects the page. It chains to the previous handler (ART's libsigchain in the app) for faults that are not ours.
  - A self-test on anonymous memory runs first.
  - The dirty test pre-populates its mapping (surprise 1), so it measures the fault path itself. The split tests cover the case without that, and what drops the pages again.
  - Each command buffer starts with a GPU-write → GPU-access memory barrier, so data written by one submission is visible to the next one's GPU reads and copies.
  - `vmas.*` measures whether the pieces merge back, with anonymous memory as the control.
- **Bandwidth (4):**
  - 256 MB buffers; NEON read-sum, `memset`, `memcpy`.
  - Pinned to cpu7; the result is INVALID if the thread is not running there.
  - Paired design: see surprise 10.
  - The per-type test (64 MB, 7 pairs) includes a placed line and shows what an uncached type looks like.

## Rerun

Source: `dm/android/probe/gpu/` (not committed).
- `gpu_probe.c`: tests.
- `ahb_probe.c`: the AHardwareBuffer pool tests (`ahb`, `ahbbw`, `ahbsize`, `ahbmany`, `ahbchurn`), included by `gpu_probe.c`.
- `vk_common.[ch]`: Vulkan bring-up, reusable for P0.8.
- `coherence.comp`, `diag.comp`: kernels.
- `gen_vkdump.py`: feature and property dumpers generated from `vulkan_core.h`.
- `build.sh`, `run_gpu.sh`.
- `build_probe_gpu.sh` / `probe_glue_gpu.c`: copies of `../common/`, with the window flags added.

```sh
dm/android/probe/gpu/build.sh                                   # -> ~/dm-work/android/probe-build/gpu/{gpu,gpu.apk}
dm/android/probe/gpu/run_gpu.sh exe stock                       # light suite, adb shell
dm/android/probe/gpu/run_gpu.sh app stock
dm/android/probe/gpu/run_gpu.sh app 842 --heavy                 # + b (3.5 GB)
dm/android/probe/gpu/run_gpu.sh app 842 --tests e               # total placed, own process
dm/android/probe/gpu/run_gpu.sh app turnip --heavy
dm/android/probe/gpu/run_gpu.sh app turnip --tests e
dm/android/probe/gpu/run_gpu.sh exe 842                         # exe loading check
dm/android/probe/gpu/run_gpu.sh exe turnip
# the deciding subsets:
dm/android/probe/gpu/run_gpu.sh app 842 --tests fresh           # KGSL split bug, mitigation and MADV_* durability (-> app/fresh/)
dm/android/probe/gpu/run_gpu.sh app 842 --tests dirty           # dirty tracking
dm/android/probe/gpu/run_gpu.sh app 842 --tests bw,types        # bandwidth
dm/android/probe/gpu/run_gpu.sh app turnip --tests kdiag        # push-constant bug
dm/android/probe/gpu/run_gpu.sh app 842 --tests free            # placed memory not freed
dm/android/probe/gpu/run_gpu.sh app 842 --tests vmas            # split KGSL VMAs never merge (also stock, turnip)
# AHardwareBuffer pools (ahb_probe.c; no AHB above 64 MB is ever requested). On another phone, STOCK_INFO / STOCK_LIB name its system driver.
STOCK_INFO=E031.47.18.56 RUN_TAG=fold7-ahb2 dm/android/probe/gpu/run_gpu.sh app stock --tests kdiag,ahb,fresh,vmas,dirty   # also exe, and 842
RUN_TAG=fold7-ahbbw dm/android/probe/gpu/run_gpu.sh exe stock --tests ahbbw                                             # bandwidth; also app, and 842
RUN_TAG=fold7-ahbchurn dm/android/probe/gpu/run_gpu.sh app 842 --tests ahbchurn   # heavy, APK only, each in its own process: ahbsize, ahbmany, ahbchurn
```

On a phone shared with other work, wrap each run in a check that the allocator service, surfaceflinger and system_server keep their pids (as `~/dm-work/android/ahb/run_checked.sh`, outside the repo, does). The probe's executable and `libprobe.so` are linked with 16 KB LOAD alignment (`-Wl,-z,max-page-size=16384`); the bundled libadrenotools libraries were already 16 KB aligned. The page-size compatibility dialog was reported on the Fold 7 with the earlier 4 KB build, and the Fold 7 has not been checked since. On the Nubia, `logcat -d` shows no `PageSizeMismatchDialog` or `PAGE_SIZE_APP_COMPAT` lines after two starts of the 16 KB build. Its buffer, which goes back to 2026-10-24, holds no such line for any app either, so this does not show that the fix was needed there. A dead allocator service shows up only as `AHardwareBuffer_allocate` errors (-32, -129).

A run with `--tests` writes to `<driver>/<mode>/<test list>/` (or `$RUN_TAG`), so it never replaces a full run.

Driver zips default to `/tmp/qcom_drv/v842/8Elite2-842.6.zip` and `/tmp/qcom_drv/turnip_a8xx/turnip_a8xx.zip`. Override them with `ZIP_842` / `ZIP_TURNIP`. Results go to `$PROBE_RESULTS`, by default `~/dm-work/android/probe-results/gpu`.
