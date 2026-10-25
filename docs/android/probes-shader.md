# P0.8 shader probe: results

Probe P0.8 of [PLAN.md](PLAN.md), run on 2026-10-25. It checks whether Stardew Valley's shaders, translated by `dmsh_xlate` (TargetApi.Vulkan), work on the phone's Vulkan drivers, and it settles the Y direction and NaN `min`/`max` questions for the Vulkan shader chain.

**P0.8 pass test (PLAN.md):** `spirv-val` is clean, pipelines build on the device, compile times are recorded, and a test pattern shows the Y direction and NaN `min`/`max` behaviour.

**Result: pass on stock, 842.6 and KosmicKrisp. Turnip passes T1, T2 and T4, and passes T3 only with `TU_DEBUG=sysmem`.**
- **Validity:** `spirv-val` is clean for all 65 modules, both stripped and original, for vulkan1.1 and vulkan1.3.
- **Pipelines:** every module creates a VkShaderModule, and all 73 pipelines build on stock, 842.6, Turnip and KosmicKrisp, from both the stripped and the original modules (0 failures anywhere).
- **Y direction and winding:** identical on every driver that renders correctly. The details are under [T3](#t3-y-direction-and-winding).
- **NaN behaviour:** in a compute shader, `min`, `max` and `clamp` with NaN give the Maxwell FMNMX result on all four drivers, for both the `F*` and the `N*` instructions. The graphics stages, where all of Stardew's min/max ops are, were not tested.

**Translator options to fix before Phase 2:**
- **Descriptor layout:** the shared descriptor set 0 collides between stages in 59 of 73 pairs.
- **ClipDistance/CullDistance:** the oversized declarations break a device limit on every driver.
- **Capabilities:** `Float64` and fragment-shader-interlock are declared but unsupported on every driver.

None of the three stops a driver from building the pipelines, but each one is a validation error. The details are under [Requirements](#requirements-for-the-vulkan-shader-chain).

**Surprise:** the Turnip a8xx build loses every render pass that it runs in GMEM mode. `TU_DEBUG=sysmem` fixes it.

## Setup

- **Phone:** nubia NX789J (Snapdragon 8 Elite, Adreno 830), Android 16. This is the same phone and the same driver identities as [P0.7](probes-gpu.md#setup). Each run checks the driver against the mapped library, the `driverID` and the `driverInfo`:

| Driver | Identity | Loaded as |
|---|---|---|
| stock | driverID 8, api 1.3.284, compiler E031.47.18.49 (02/12/26) | system `vulkan.adreno.so` |
| 842.6 | driverID 8, api 1.4.295, compiler E031.50.19.01 (08/28/25) | libadrenotools, `vulkan.ad07xx.so` |
| Turnip | driverID 18, "Turnip Adreno (TM) 830v2", api 1.4.335, Mesa 26.0.0-devel (git-9a720fe0e4) | libadrenotools, `vulkan.ad08xx.so` |
| KosmicKrisp (Mac) | driverID 28, Apple M5, api 1.4.354, Mesa 26.2.5 | Homebrew loader, Khronos validation layer, `VK_LAYER_DUPLICATE_MESSAGE_LIMIT=0` |

- **Shaders:** Stardew Valley v1310720, 65 modules (42 vertex, 23 pixel), translated by `dmsh_xlate`.
  - **original:** the translator's output.
  - **stripped:** the same modules after `dm/android/tools/spv_strip_caps.py`, which removes the `Float64`, `GroupNonUniform*` and `FragmentShaderPixelInterlockEXT` capabilities and the interlock extension.
  - No module uses an instruction that needs those capabilities: `spirv-dis` finds no `OpGroupNonUniform*`, interlock instruction or 64-bit float type in any of the 65 originals.
  - The modules and everything derived from them stay outside the repo and are pushed to the phone only for the duration of a run.
- **Contexts:**
  - **app:** a NativeActivity APK, `org.nxnative.probe.shader`, built and driven through the P0.7 harness (`dm/android/probe/gpu/`, unchanged), cpuset `/top-app`.
  - **exe:** an `adb shell` executable in `/data/local/tmp/nxprobe-shader/`.
  - **mac:** a macOS build of the same source, run with the validation layer.
- **Caches:**
  - Every phone run starts with the app's `cache/` and `code_cache/` empty.
  - `MESA_SHADER_CACHE_DISABLE=true` is set unless `--mesa-cache` is given.
  - Each T2 phase uses a fresh `VkDevice`.
  - No driver wrote files of its own: after every run, `cache/`, `code_cache/` and the exe's `HOME` were empty (`appdirs_after.txt`, `home_after.txt`).
- **Concurrency:** another agent's CPU probe apps (`org.nxnative.probe.cpu` and `org.nxnative.probe.cputag`) were alive during most phone runs.

### Pairing (`gen_manifest.py`)

The generator reads the modules, their `.res.json` and `spirv-cross --reflect`; the cross-check against both is empty, so they all agree. It also reads the game's own effect definitions:
- **The prewarm list:** `dm/games/0100E65002BB8000/pso_manifest.tsv` has 4 render-pipeline rows, all of them SpriteBatch's single pair (`4300f337…` + `d94db265…`).
- **The MonoGame MGFX v8 effects:** 7 effects with 80 passes, read from the `main` NSO's data segment (LZ4) and from `RomFS/Content/Effects/*.xnb`. They reference 67 shaders, and every one of them is among the 65 modules.
  - Each pass names its vertex and pixel shader.
  - In the pass of `ShadowRemove.xnb`, vertex shader index 255 means "keep the bound vertex shader". That shader is SpriteBatch's (`4300f337…`), the one the prewarm list records.
  - The 8 LZX-compressed font XNBs are skipped; fonts carry no effects.

Every one of the 73 pairs comes from game evidence:

| Source | Pairs |
|---|---|
| MGFX passes | 64 |
| MGFX passes in two effects | 7 |
| prewarm list + MGFX (SpriteBatch) | 1 |
| MGFX pass with "the bound vertex shader" (ShadowRemove + SpriteBatch's vertex shader) | 1 |

All 23 pixel shaders and all 42 vertex shaders are covered. Every pixel shader input is written by its vertex shader, with the same location, the same component type and at least as many components.

The interface fallback was never needed. It would have been ambiguous anyway, because every pixel shader is covered by 11, 29, 40 or 42 vertex shaders, depending on how many inputs it has. Its rule is in `analysis.txt`:
- among the vertex shaders that cover the pixel shader, pick the one with the fewest outputs the pixel shader does not read;
- break ties with the smaller key.

**Whether all 73 pairs occur at run time is not established.** 72 of the 73 pairs come from the six MonoGame built-in effects compiled into `main` (AlphaTestEffect, BasicEffect, DualTextureEffect, EnvironmentMapEffect, SkinnedEffect and SpriteBatch); the 73rd is the game's own `ShadowRemove`. The Mac prewarm recording, from boot to the shop and a save load, saw only SpriteBatch. So the T2 totals cover more pipelines than the game is known to use.

`manifest.txt` gives, per module:
- the stage and entry point;
- the descriptor bindings with their exact Vulkan types;
- the vertex inputs (location, component type, count);
- the stage interface;
- the fragment outputs.

`analysis.txt` adds the pairing evidence, descriptor collisions, min/max forms, capabilities and a trace of `gl_Position`. The probe builds:
- set layouts from the bindings;
- vertex attributes from the inputs: float inputs get `R32G32B32A32_SFLOAT`-style formats, `uvec4` inputs get `_UINT` formats;
- RGBA8 colour attachments, one per fragment output;
- dynamic viewport and scissor.

## Results per driver

T1 is `vkCreateShaderModule` for all 65 modules. T2 creates a graphics pipeline for each of the 73 pairs, timed per pipeline. Every phase after cold runs on a fresh `VkDevice`:
- **cold:** no pipeline cache, on the device T1 used (T1 only creates and destroys shader modules, in under 2 ms on every phone driver);
- **cold2:** cold again, on another fresh device;
- **fill:** an empty `VkPipelineCache`;
- **warm:** the same cache, on the same device;
- **reload:** a new cache from fill's serialized data;
- **disk:** the cache data written by an earlier process.

Each phase creates all 73 pipelines and reports the total plus the per-pipeline p50 and max, in ms. The times cover `vkCreateGraphicsPipelines` only; `vkCreatePipelineCache` (deserializing the data in reload and disk) is not timed. "Hits" counts `VK_PIPELINE_CREATION_FEEDBACK_APPLICATION_PIPELINE_CACHE_HIT_BIT`.

**A T2 PASS means the driver built the pipeline, not that it is valid.** The phone runs have no validation layer, and the drivers accept pipelines the layer rejects. Every one of these pipelines breaks `maxCombinedClipAndCullDistances` (see [Validation](#validation-kosmickrisp-with-the-khronos-layer)), and a stricter driver could refuse them.

### Pinned to a prime core

These are exe runs under `taskset 80`, which pins the process to cpu7 (4.32 GHz). These are the comparable numbers:

| Driver | Modules | T1 | Pipelines | cold (total, p50, max) | cold2 total | warm (total, p50, max; hits) | reload total | Cache data |
|---|---|---|---|---|---|---|---|---|
| stock | stripped | 65/65 | 73/73 | 387.4, 4.37, 15.31 | 374.8 | 4.8, 0.06, 0.13; 73 | 4.5 | 520,342 B |
| stock | original | 65/65 | 73/73 | 386.5, 4.37, 15.26 | 373.6 | 4.8, 0.06, 0.13; 73 | 4.6 | 520,364 B |
| 842.6 | stripped | 65/65 | 73/73 | 395.4, 4.50, 15.46 | 381.7 | 4.8, 0.06, 0.13; 73 | 4.8 | 520,244 B |
| 842.6 | original | 65/65 | 73/73 | 395.5, 4.49, 15.41 | 380.1 | 4.8, 0.06, 0.13; 73 | 4.9 | 520,280 B |
| Turnip | stripped | 65/65 | 73/73 | 294.6, 2.75, 17.66 | 290.9 | 0.3, 0.00, 0.04; **0** | 0.3 | 853,501 B |
| Turnip | original | 65/65 | 73/73 | 293.6, 2.75, 17.57 | 290.2 | 0.3, 0.00, 0.03; **0** | 0.3 | 853,501 B |
| KosmicKrisp (Mac) | stripped | 65/65 | 73/73 | 129.1, 1.46, 14.89 * | 110.2 | 1.0, 0.01, 0.02; 73 | 6.2 | 2,318,725 B |
| KosmicKrisp (Mac) | original | 65/65 | 73/73 | 113.9, 1.40, 3.70 * | 111.4 | 1.0, 0.01, 0.02; 73 | 6.2 | 2,318,729 B |

\* **KosmicKrisp:** the Mac rows are not pinned (Apple M5). Metal keeps its own compiled-shader cache across processes, outside Mesa's cache. The first run on this Mac took 2,728 ms cold (p50 28.5, max 90.6). That run's files were overwritten by later runs.

**Original vs stripped:** no difference on any driver. On the phone, cold totals agree within 1 %. On KosmicKrisp, cold2 agrees within 1 %. The stripped cold total is higher only because of its first pipeline (pair 0, 14.9 ms against about 1.5 ms for the others).
- Every driver accepts the original modules, even though none supports what they declare:
  - `Float64`, with `shaderFloat64` = 0 everywhere;
  - `FragmentShaderPixelInterlockEXT`, with no `VK_EXT_fragment_shader_interlock` anywhere.
- KosmicKrisp's own SPIR-V front end warns "Unsupported SPIR-V capability" for both.
- The probe enables only what each driver reports.

### Unpinned: what the app gets from the scheduler

These are app runs of the stripped modules. The table shows cold totals over every run of each driver:

| Driver | Runs | Cold total min / median / max | p50 (median) | Worst pipeline | warm and reload totals |
|---|---|---|---|---|---|
| stock | 11 | 382 / 1,095 / 1,158 | 10.4 | 63.5 | 4.5-17.7 |
| 842.6 | 9 | 1,020 / 1,037 / 1,163 | 11.8 | 65.6 | 12.6-19.6 |
| Turnip | 9 | 736 / 843 / 906 | 5.4 | 77.1 | 0.8-1.9 |

**The spread is CPU placement, not the driver.** The same exe pinned to cpu0 (the 6-core cluster) took much longer cold:

| Driver | Cold on cpu0 (ms) | Cold on cpu7 (ms) | Slowdown |
|---|---|---|---|
| stock | 1,296 and 1,388 | 387 | 3.3-3.6× |
| 842.6 | 1,441 | 395 | 3.6× |
| Turnip | 1,066 | 295 | 3.6× |

Polling during one app run (a fast one, 383 ms) found the busy thread on cpu6 at 4.32 GHz. The slow runs were not polled. The likely explanation is the clock:
- The clock limits alone account for only 1.2× (4.32 against 3.53 GHz), against the 3.3-3.6× measured on cpu0.
- Snapshots between runs showed cpu0-5 at 0.96-1.36 GHz.
- So the governor probably does not raise the small cluster for a burst this short. This is an inference, not a measurement.

Within the app, the median is the realistic figure for one thread with no affinity. Every run records the CPU frequency caps in `cpufreq.txt`.

### Pipeline cache

| Driver | disk phase (another process's data) | Data from the stock driver loaded into this driver |
|---|---|---|
| stock | 6.0 ms, 73 hits | - |
| 842.6 | 8.6 ms, 73 hits | ignored without an error: 1,001 ms, 0 hits (a full compile) |
| Turnip | 0.6 ms, 0 hits reported | ignored without an error: 921 ms |

**Turnip never sets the cache-hit bit:**
- In warm, reload and disk it reports `VALID` only, and the per-stage flags are 0.
- Its timings show the cache works: 0.3 ms against 295 ms cold.
- **Rule:** judge cache effectiveness by time, not by feedback.

## T3: Y direction and winding

`ydir.vert` draws one triangle, (-1,0) (-1,-4) (3,0), that covers clip-space y < 0. It is counter-clockwise in NDC with y up. `ydir.frag` writes `(1, (FragCoord.y-0.5)/255, 0, 1)`.

The target is a 64×64 RGBA8 image, read back by rows (row 0 is first in memory). It is drawn 7 times:
- with a positive and a negative viewport height;
- with cull none, BACK and FRONT, front face CCW;
- then a repeat of the first draw.

Each draw clears to its own colour, so a pass that did not run shows up. A draw counts only if every pixel is either its colour or its pass's clear colour; otherwise it is reported as lost and its rows say nothing. The readback buffer is filled with 0xee before each copy, so a copy that did not land also shows up.

| Driver | Positive height | Negative height | FragCoord.y | Facing (CCW-in-NDC triangle) | State check |
|---|---|---|---|---|---|
| stock | rows 0..31 | rows 32..63 | the memory row | back (positive height), front (negative height) | PASS |
| 842.6 | rows 0..31 | rows 32..63 | the memory row | back, front | PASS |
| KosmicKrisp | rows 0..31 | rows 32..63 | the memory row | back, front | PASS |
| Turnip, `TU_DEBUG=sysmem` | rows 0..31 | rows 32..63 | the memory row | back, front | PASS |
| Turnip, default | rows 0..31 | rows 32..63 | the memory row | not measurable | **FAIL**: from the third pass on, neither the clear nor the draw lands; the image keeps the second pass's content |
| Turnip, `TU_DEBUG=gmem` | lost | lost | - | - | **FAIL**: no pass writes anything; the image reads as all zeros, and in one of the 7 passes the copy itself did not land (the 0xee fill stayed) |

The Turnip default fails once its autotune moves to GMEM rendering; this is a bug in that build. The probe's own synchronization is not the cause: the same T3 under the validation layer's synchronization validation (`VK_KHRONOS_VALIDATION_VALIDATE_SYNC=true`, `kk/mac/t3-sync`) reports no messages on KosmicKrisp. A deliberately broken copy, with the barrier before the readback copy missing its source stage, reports 7 `SYNC-HAZARD-WRITE-AFTER-WRITE`, which shows the check was active.

So on every working driver:
- clip y = -1 lands on memory row 0 with a positive viewport height;
- `FragCoord.y` counts memory rows;
- a triangle that is counter-clockwise in a y-up NDC is back-facing unless the viewport height is negative. This is the spec's facing rule, applied in framebuffer coordinates.

**What the Stardew shaders do with Y.** All 42 vertex shaders compute `gl_Position.y` as a plain `Fma` chain: the dot product of the position input with one row of the `vp_c3` constant buffer (SpriteBatch: `y = dot(in0, c3[1])`). No shader negates Y or reads the support buffer, which no module reads at all. The translator leaves Y and Z untouched (no Y flip, upper-left `OriginUpperLeft` in all 23 pixel shaders, depth not rewritten).

The game never calls `nvnDeviceSetWindowOriginMode` or `nvnDeviceSetDepthMode`. Every Mac reference log under `~/dm-work/android/ref/stardew-mac-*/` has only the initial "window origin: lower left" line and no "depth mode" line. So it runs with NVN's defaults: lower-left origin and depth near = -w.

**What the backend must do:**

In lower-left mode (Stardew), NVN puts NDC y = -1 on the first memory row, and window y grows with the row (`nvn_mtl.m:3430`). That is exactly what Vulkan does with a positive viewport height (T3), so:
1. **Viewport:** pass the viewport through with a positive height. This is the opposite of the Metal backend, which uses a negative height here.
2. **Front face:** reverse NVN's front-face winding (CCW→CW). T3 shows the y-up CCW triangle is back-facing under a positive height.
3. **Scissor, FragCoord, copies, texture coordinates:** leave them unchanged. They address the same rows.
4. **Present:** flip the window image, because it is stored bottom row first.

In upper-left mode (other games), use a negative viewport height (y = top + height, height < 0) and keep the winding. This follows from T3's negative-height rows and facing, but no game has tested it here.

## T4: NaN min/max

`nanmm.comp` takes its operands from a storage buffer: Turnip's push constants are broken, and constant folding has to be impossible. It evaluates 8 operations over 15 operand pairs:
- the operations: `min(a,b)`, `max(a,b)`, `max(0,a)`, `max(a,0)`, `clamp(a,0,1)`, `clamp(0.5,a,1)`, `clamp(0.5,0,a)`, `min(1,a)`;
- the operands: quiet NaN, negative quiet NaN and signalling NaN against finite values and ±Inf, in both positions; two NaNs; +0/-0 in both orders; and one pair without NaN, (2, 3).

It runs in 4 variants:
- **F:** GLSL.std.450 `FMin`/`FMax`/`FClamp` (37/40/43);
- **N:** `NMin`/`NMax`/`NClamp` (79/80/81, through `GL_EXT_spirv_intrinsics`);
- **F+preserve** and **N+preserve:** the same, with `SignedZeroInfNanPreserve` 32 (supported by all four drivers).

The reference is Maxwell FMNMX: a NaN operand gives the other operand, two NaNs give NaN, and a clamp (two FMNMX) of NaN gives 0, as `.SAT` does.

| Driver | F | N | F+preserve | N+preserve | Signed zeros |
|---|---|---|---|---|---|
| stock | 0 mismatches | 0 | 0 | 0 | -0 < +0 in every variant |
| 842.6 | 0 | 0 | 0 | 0 | -0 < +0 in every variant |
| Turnip | 0 | 0 | 0 | 0 | -0 < +0 in every variant |
| KosmicKrisp | 0 | 0 | 0 | 0 | without preserve: returns operand b (`min(+0,-0)=-0`, `min(-0,+0)=+0`); with preserve: -0 < +0 |

Each variant grades 110 of its 120 cases:
- the 60 NaN-dependent cases: 12 for each two-operand op and 6 for each of the other six;
- 50 cases without a NaN operand.

Ungraded:
- 3 clamps with minVal > maxVal, which GLSL.std.450 leaves undefined;
- 7 cases that only compare +0 with -0.

Every driver had 0 mismatches in both groups. The NaN operands come from buffer loads, so nothing is constant-folded. But T4 does not test a clamp that a driver fuses into the instruction producing the value, which is the form the game's `FClamp(x, 0, 1)` takes.

Signed zeros (`min`/`max` of +0 and -0, and the ±0 ties of `max(0,a)`, `max(a,0)` and `clamp(a,0,1)`) are reported but not graded, because the Maxwell ordering of ±0 was not established here.

**What the Stardew shaders use.** In the stripped disassembly, extended instructions 37/40/79/80 appear as:
- `FMax` 47 times: 39 as `FMax(0, x)` and 8 as `FMax(x, y)`;
- `FClamp` 33 times, all `FClamp(x, 0, 1)`;
- no `FMin`, `NMin`, `NMax` or `NClamp`.

So the translator emits the `F*` forms. That contradicts PLAN.md, which assumes it emits `NMin`/`NMax`/`NClamp`. The SPIR-V spec leaves `F*` undefined when an operand is NaN. In a compute shader on these four drivers it happens to match FMNMX, and `N*` gives the same results. All 80 of Stardew's `FMax`/`FClamp` ops are in vertex and pixel shaders, which the drivers compile through their graphics paths, possibly with other precision or fast-math choices; T4 does not cover those.

## Validation (KosmicKrisp with the Khronos layer)

Every message is in `validation.txt`, per pipeline (`t2/<set>/<phase>/pair<N> <vs>+<ps>`).

| Run | Messages | Content |
|---|---|---|
| stripped, T1-T4 | 365 | All `VUID-RuntimeSpirv-maxCombinedClipAndCullDistances-00710`: one per pipeline per phase (73 × 5). Every vertex shader declares `ClipDistance[8]` + `CullDistance[1]` = 9 > 8, but writes only `Position` and `PointSize`. Nothing else: descriptor layouts, vertex formats, interfaces and T3/T4 are clean |
| original, T1-T2 | 1,442 | The 365 above, plus `VkShaderModuleCreateInfo-pCode-08740` and `-08742` (below), plus 522 Mesa warnings |
| controls | 8 | Each control produces the error it should (below). A control passes only on its intended VUID, because each pipeline also raises VUID-00710. In every one, `vkCreateGraphicsPipelines` still returns `VK_SUCCESS` |

The original run's extra messages:
- **`VkShaderModuleCreateInfo-pCode-08740`** (capability not supported):
  - `Float64`: 65 modules;
  - `FragmentShaderPixelInterlockEXT`: 23 modules.
  - 88 per module set, in T1 and on each of the 4 devices T2 creates.
- **`-08742`** (extension `SPV_EXT_fragment_shader_interlock` not supported).
- **522 Mesa warnings** (`vk_nir.c:60`): "Unsupported SPIR-V capability" for Float64 and the interlock.
- The `GroupNonUniform*` capabilities are not flagged (KosmicKrisp supports them).

The controls:
- **Merged set 0** (SpriteBatch pair): `VkGraphicsPipelineCreateInfo-layout-07990`, a descriptor type mismatch at set 0, binding 1 (a UBO in the layout, a combined sampler in the pixel shader).
- **Pixel shader with 4 inputs paired with a vertex shader that writes 1:** `RuntimeSpirv-OpEntryPoint-08743`, at locations 2, 3 and 4.
- **`R32G32B32A32_UINT` on a float input:** `VkGraphicsPipelineCreateInfo-Input-08733`.

The drivers accept all of these, so they will not catch such errors; only the validation layer does. The phone runs have no validation layer.

### Capabilities that matter

`caps` line, exe runs:

| | stock | 842.6 | Turnip | KosmicKrisp |
|---|---|---|---|---|
| maxClip / maxCull / maxCombined distances | 8 / 8 / 8 | 8 / 8 / 8 | 8 / 8 / 8 | 8 / 8 / 8 |
| shaderFloat64 | 0 | 0 | 0 | 0 |
| VK_EXT_fragment_shader_interlock | no | no | no | no |
| VK_EXT_depth_clip_control (depthClipControl) | yes (1) | yes (1) | yes (1) | yes (1) |
| subgroup size, stages | 64, frag+comp (0x30) | 64, frag+comp+task+mesh (0xf0) | 128, all graphics + comp (0x3f) | 32, frag+comp (0x30) |
| subgroup ballot / shuffle / vote | yes | yes | yes | yes |

## Requirements for the Vulkan shader chain

1. **Stripping.**
   - Remove capabilities a module declares but never uses: `Float64` (no driver has `shaderFloat64`), `FragmentShaderPixelInterlockEXT` and `SPV_EXT_fragment_shader_interlock` (no driver has the extension), and the unused `GroupNonUniform*`.
   - Do it at translation time: `spv_strip_caps.py`'s output is what passed validation.
   - The drivers accept the unstripped modules today, but each one is a validation error and only works by leniency.
   - Keep the softfloat64 retry, because no driver has FP64.
   - Keep `spv_single_lane` for vertex shaders: stock and 842.6 report no subgroup support in the vertex stage (only Turnip does). Today no Stardew shader uses a subgroup instruction.
2. **ClipDistance/CullDistance.** Remove the unused `ClipDistance[8]`/`CullDistance[1]` members from `gl_PerVertex`, or size them to what the shader writes. 9 exceeds `maxCombinedClipAndCullDistances` (8 on all four drivers), so every pipeline is invalid as it stands (VUID-00710).
3. **Y handling** (see [T3](#t3-y-direction-and-winding)).
   - **Lower-left origin (Stardew):**
     - positive viewport height;
     - reversed front face;
     - scissor and `FragCoord` unchanged;
     - flip at present.
   - **Upper-left origin:** negative viewport height, winding kept.
   - **No shader change for Y**, because the translator's "no Y flip" output is right for Vulkan.
   - **Depth:** for near = -w (Stardew, NVN's default), use `VK_EXT_depth_clip_control` `negativeOneToOne` per pipeline. All four drivers support it. This replaces the Metal backend's `main0_z` vertex shader variant. The probe did not render depth.
4. **NaN min/max.**
   - Make the translator emit `NMin`/`NMax`/`NClamp` for FMNMX, as PLAN.md assumes. Today it emits `FMax`/`FClamp`, which the SPIR-V spec leaves undefined for NaN.
   - In a compute shader on all four drivers both forms give the FMNMX result. That does not cover the graphics stages, where Stardew's ops are, so do not rely on `F*` matching there. `N*` is the form whose result is defined, and it measured identical everywhere.
   - Do not rely on `SignedZeroInfNanPreserve` for NaN; it changes only the ±0 ordering, and only on KosmicKrisp.
5. **Descriptor layout derivation.**
   - Do not share set 0 between stages. The translator numbers both stages from binding 0 in set 0, so in 59 of 73 pairs binding 1 is a UBO in the vertex shader and a combined image sampler in the pixel shader; in 8 more, the same type means different resources.
   - The probe's scheme works and is validation-clean: vertex shader in set 0, pixel shader patched to set 1 (rewrite each `DescriptorSet` decoration, `patch_set`). A translator option giving each stage its own set, or its own binding range, does the same without patching.
   - Derive each set layout from SPIR-V reflection or `.res.json`, with exact types: `COMBINED_IMAGE_SAMPLER`, `UNIFORM_BUFFER`, `STORAGE_BUFFER`, texel buffers.
   - Binding 0 (`support_buffer`) is declared everywhere and read nowhere.
6. **Vertex input formats.**
   - Take the component type from the vertex shader input: 9 skinned vertex shaders have a `uvec4` at location 3, which needs a `_UINT` format. A float format there is VUID-08733, and the drivers do not report it.
   - Every pixel shader input is already written by its vertex shader in all 73 pairs, so `vs_pad` is not needed for Stardew. It stays needed in general (VUID-08743).
7. **Pipeline cache use.**
   - Persist a `VkPipelineCache` per driver identity: vendor, device, driver version and `pipelineCacheUUID`. No driver writes a cache of its own into the app's directories.
   - A warm cache cuts creation of the 73 pipelines from 290-400 ms to 0.3-5 ms on a prime core. Unpinned, it goes from about 1 s to 13-19 ms on Qualcomm and 1-2 ms on Turnip.
   - Data from another driver is ignored without an error, so a stale file only costs a recompile.
   - Expect about 7 KB per pipeline on Qualcomm and 12 KB on Turnip.
   - Do not use the cache-hit feedback bit to judge the cache: Turnip never sets it.
8. **Compile placement.**
   - Cold creation is CPU-bound and runs 3.3-3.6× faster on a prime core than on the 6-core cluster.
   - Prewarm and on-demand compilation should run on worker threads with prime-core affinity, or at least several threads, so a cold first frame does not stall for a second.
9. **Turnip (this a8xx build).**
   - Run it with `TU_DEBUG=sysmem`, or use a newer build. GMEM render passes lose their clears and draws.
   - It also reads garbage from compute push constants (P0.7).

## Surprises

- **Turnip a8xx loses GMEM render passes.**
  - Under the default autotune, the first two passes in the process render; from the third on, nothing lands, neither the clear nor the draw, and the image keeps the second pass's content.
  - `TU_DEBUG=gmem` loses every pass: the image reads as zeros. `TU_DEBUG=sysmem` renders everything correctly.
  - T2 and T4 are unaffected: they render nothing.
  - Synchronization validation on KosmicKrisp finds nothing wrong with the probe's barriers, so this is the driver.
- **Every driver accepts invalid modules.** `Float64` without `shaderFloat64`, an unsupported extension, 9 clip/cull distances over a limit of 8, a mistyped descriptor in a merged set, and a float format on a `uvec4` input all return `VK_SUCCESS`. Only the validation layer reports them.
- **Turnip compiles fastest on a prime core** (295 ms against about 390 ms for Qualcomm), but reports no cache hits.
- **The translator emits `F*` min/max, not `N*`,** contrary to PLAN.md's table. In a compute shader, the four drivers behave like FMNMX anyway.
- **Timings are bimodal on the phone.** An unpinned app run lands on a prime core (about 380 ms) or on the 6-core cluster at a low clock (about 1.1 s).
- **Play Protect blocked a reinstall.** After several reinstalls of the probe APK, `adb install` hung behind a Play Protect dialog (`com.android.vending/...PlayProtectDialogsActivity`). The hung install was stopped. `run_shader.sh` now reinstalls only when the APK changed, and gives up after 120 s. A later reinstall went through, and the dialog is gone.

## Method notes

- **Timing:** T2 times each `vkCreateGraphicsPipelines` call with `CLOCK_MONOTONIC`. Shader modules, set layouts and pipeline layouts are created before the timer starts. `VkPipelineCreationFeedback` is recorded per pipeline and per stage (`t2.tsv`).
- **Set patching:** for the stripped and original sets, `patch_set` rewrites the `DescriptorSet` decoration of fragment modules from 0 to 1. `--layout merged` keeps set 0, and `--controls` uses it.
- **The caps line:** it was added after most app runs, so the table takes it from the exe runs (`<driver>/exe/caps`). The last APK run, Turnip with `TU_DEBUG=sysmem` (`turnip/app/sysmem-all`), is the current build. It reports the same caps line, and every check passes: T1-T4 PASS, cold 292 ms, warm 0.3 ms.
- **Not covered:**
  - drawing with the game's shaders (T2 only builds pipelines);
  - depth behaviour;
  - NaN behaviour in the vertex and fragment stages (T4 is a compute shader);
  - performance of `N*` against `F*`;
  - Turnip's GMEM bug beyond T3.

## Reproduction

Build (outside the repo: `~/dm-work/android/probe-build/shader/`). This needs Homebrew's `glslang` and `spirv-tools`, the NDK, and libadrenotools built as for P0.7:

```
dm/android/probe/shader/build.sh        # APK + adb executable
dm/android/probe/shader/build.sh mac    # macOS executable
```

Manifest and data. The output embeds game-derived facts, so it stays outside the repo. `<game>` is the extracted title (`ExeFS/main`, `RomFS/`):

```
W=~/dm-work/android/stardew-spv
python3 -I dm/android/probe/shader/gen_manifest.py --spv $W/stripped --res $W/work/dmsh_2m8fihj9 \
    --pso dm/games/0100E65002BB8000/pso_manifest.tsv --main <game>/ExeFS/main --romfs <game>/RomFS \
    --out ~/dm-work/android/probe-results/shader/manifest
D=~/dm-work/android/probe-build/shader/data; mkdir -p $D/stripped $D/orig
cp $W/stripped/*.spv $D/stripped/; cp $W/work/dmsh_2m8fihj9/*.spv $D/orig/
cp ~/dm-work/android/probe-results/shader/manifest/manifest.txt $D/
```

Runs. Results go to `~/dm-work/android/probe-results/shader/<driver>/<app|exe|mac>/<tag>/`; set `RUN_TAG` to name the directory:

```
S=dm/android/probe/shader/run_shader.sh
$S app stock; $S app 842; $S app turnip                     # T1-T4, stripped
$S app <drv> --set orig --tests t1,t2                         # original modules
$S exe 842 --tests t1,t2                                      # adb executable
EXE_PREFIX="taskset 80" RUN_TAG=pin80 $S exe <drv> --tests t2 # pinned to cpu7 (01 = cpu0)
CACHE_IN=<results>/<drv>/app/stripped/cache.bin $S app <drv> --tests t2   # disk phase
RUN_TAG=t3-sysmem $S app turnip --tests t3 --env TU_DEBUG=sysmem
RUN_TAG=caps $S exe <drv> --tests none                        # capability line only
$S mac kk                                                     # KosmicKrisp + validation, T1-T4
RUN_TAG=controls $S mac kk --tests none --controls            # expected validation errors
$S mac kk --set orig --tests t1,t2
```

Which result directories back which section (under `~/dm-work/android/probe-results/shader/`):
- **pinned timings:** `<drv>/exe/pin80` (stripped), `<drv>/exe/orig-pin80` (original); cpu0: `<drv>/exe/pin01`;
- **unpinned timings:** `<drv>/app/stripped` and `<drv>/app/t2-rep*`;
- **original modules, app:** `<drv>/app/orig`;
- **pipeline cache from disk:** `<drv>/app/stripped-disk`, `<drv>/app/stripped-disk-from-stock`;
- **T3 and T4, final code:** `<drv>/app/t3t4` (phone), `kk/mac/t3t4`, `turnip/app/t3-gmem`, `turnip/app/t3-default`, `turnip/app/t3-sysmem`, `turnip/app/sysmem-all`, `kk/mac/t3-sync` (synchronization validation). The `stripped` directories hold T3 and T4 output from before the review fixes: no per-draw pass gating in T3, and one combined mismatch count in T4 (the same results, in the old format);
- **validation:** `kk/mac/stripped`, `kk/mac/orig`, `kk/mac/controls`;
- **capabilities:** `<drv>/exe/caps`, `kk/mac/caps`;
- **manifest:** `manifest/`.

Each result directory holds:
- `results.tsv`: one line per check;
- `summary.txt`: every step;
- `t1.tsv` and `t2.tsv`: per module and per pipeline;
- `t3.txt`: rows per draw;
- `t4.tsv`: every case;
- `validation.txt` (Mac only);
- `cache.bin`;
- `cpufreq.txt`;
- `appdirs_after.txt` or `home_after.txt`;
- `logcat.txt`.

`run_shader.sh` touches only `org.nxnative.probe.shader` and `/data/local/tmp/nxprobe-shader*`, and removes the pushed modules and the pipeline cache data (`files/res/cache.bin`, `out/res/cache.bin`) after each run.

## Files

- `dm/android/probe/shader/gen_manifest.py`: the manifest and pairing generator. It embeds no game data.
- `dm/android/probe/shader/shader_probe.c`: T1-T4, the controls and the caps line.
- `dm/android/probe/shader/ydir.vert`, `ydir.frag`: T3.
- `dm/android/probe/shader/nanmm.comp`: T4.
- `dm/android/probe/shader/build.sh`, `run_shader.sh`: build and run.
- Raw results: `~/dm-work/android/probe-results/shader/`, outside the repo:
  - `stock/`, `842/` and `turnip/`, each with `app/` and `exe/`;
  - `kk/mac/`;
  - `manifest/`.
