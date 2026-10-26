# P0.7 GPU memory probe: results

Probe P0.7 of [PLAN.md](PLAN.md), run on 2026-10-25. It decides the GPU memory strategy (A, A′ or B).

**P0.7 pass test:** placed memory is cached, coherent and as fast as anonymous memory for the CPU; the GPU and the CPU see each other's writes; `mprotect` works.

**Result:**
- **Stock driver:** fails. It has no `VK_EXT_map_memory_placed`, so only strategy B is possible on it.
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
```

A run with `--tests` writes to `<driver>/<mode>/<test list>/` (or `$RUN_TAG`), so it never replaces a full run.

Driver zips default to `/tmp/qcom_drv/v842/8Elite2-842.6.zip` and `/tmp/qcom_drv/turnip_a8xx/turnip_a8xx.zip`. Override them with `ZIP_842` / `ZIP_TURNIP`. Results go to `$PROBE_RESULTS`, by default `~/dm-work/android/probe-results/gpu`.
