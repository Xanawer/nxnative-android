# nxnative on Android: porting plan

Goal: run the same rewritten Switch modules natively on an Android phone with a Snapdragon GPU, the way upstream runs them on iPhone. The game's own aarch64 code runs on the phone's CPU, the host stands in for Horizon, and NVN is reimplemented on Vulkan instead of Metal. The user brings their own NSPs and `prod.keys`, as upstream.

The CPU side is a **port**. The rewriter, the loader and the host's services carry over, with a platform layer under them and one rewriter change. The GPU side is a **rewrite**: a second replay backend next to `nvn_mtl.m`, and a Vulkan target for the shader chain.

Paths are relative to the repository root. Line numbers are as of `66abbc3` (upstream `main` when this fork was made).

## Starting point

**Target phone (the development device):** Nubia NX789J.
- SoC: SM8750 (Snapdragon 8 Elite).
- GPU: Adreno 830 v2.
- OS: Android 16 (SDK 36), kernel 6.6.92.
- 4 KB pages, 16 GB RAM.
- CPU features include `atomics lrcpc ilrcpc bti paca`.

**Vulkan drivers on hand.** The two Qualcomm columns come from the extension-name strings in the driver binary. A name being there is strong evidence but not proof; Phase 0 asks each driver at run time.

| | Stock driver V@0800.72 (Feb 2026, `/vendor/lib64/hw/vulkan.adreno.so`) | Qualcomm 842.6 package ([AdrenoToolsDrivers](https://github.com/K11MCH1/AdrenoToolsDrivers/releases/tag/v842.6), "for Elite devices") | Turnip R8 (Mesa `e9a83d530cce`) |
|---|---|---|---|
| `VK_EXT_map_memory_placed` | absent | present | always on (`tu_device.cc:307`); KGSL backend maps with `MAP_FIXED` (`tu_knl_kgsl.cc:392-397`) |
| `VK_EXT_external_memory_host` | absent | absent | absent |
| `VK_KHR_pipeline_binary` | absent | present | absent |
| Buffer device address, descriptor indexing, depth clip control, conditional rendering, custom border colour, draw indirect count, graphics pipeline library, transform feedback | present | present | present |
| Notes | — | shader compiler E031.50 (stock: E031.47) | `maxMemoryAllocationSize` 4 GB − 1 (`tu_device.cc:870`); the release notes call a8xx support "very beta and hacky" |

So no Adreno driver can wrap memory the game already has, the way `newBufferWithBytesNoCopy` does on Metal (`dm/gfx/nvn/nvn_mtl.m:419`). Placed mapping is the zero-copy path: the host allocates the guest's GPU-visible memory as Vulkan memory from the start, at the addresses the game expects. Placed mapping is missing from this phone's stock driver.

**Mac (the build machine):**
- Installed: Android SDK, NDK 27.1, adb, Java 17, spirv-cross, glslang.
- Missing: .NET, which `dm/tools/nx_extract` and `dm/gfx/tools/dmsh_xlate` need, and Gradle (or Android Studio's copy), which the app shell (Phase 1.6) and packaging (Phase 3) need. The Phase 0 probes need neither.
- [KosmicKrisp](https://gitlab.freedesktop.org/mesa/mesa/-/raw/main/docs/drivers/kosmickrisp.rst) (Mesa's Vulkan driver on Metal 4, macOS 26+) has `VK_EXT_map_memory_placed`, so the Vulkan backend can be developed in the Mac host with the same memory model as the phone.

**Upstream:**
- No Linux or Android code. The only "Android" hits are comments about the Switch SDK's MediaCodec wrapper.
- The host is about 10.5k lines; 22 of its 42 C files, plus `dm/gfx/hle_vi.c` and `hle_nv.c`, are portable as they are (list below).
- The graphics boundary is the record format in `dm/gfx/nvn/nvn_obj.h`, replayed by `mtl_exec`. `hle_nvn.c` calls about 25 functional `mtl_*` entry points plus about 30 statistics ones, and contains no Metal types.

**First games to target:** ones that use NVN with very few shaders, and only `main` + `sdk` modules:
- ANIMAL WELL: 48 shaders, about 54 MB.
- Celeste: 28 shaders.
- Stardew Valley: 65 shaders (23 pixel, 42 vertex), MonoGame compiled by BRUTE, like Celeste.

Mario Wonder (16,270 shaders) comes after those. **Stardew Valley is the game on hand** (update 1.6.15.13, `v1310720`), so it is the first game for P0.8 and the done-tests of Phases 1 and 2; ANIMAL WELL and Celeste join when supplied.

## Principles

1. **Probe first.** Phase 0 settles the questions that decide the design, on this phone, before any port code is written. Each later phase ends with a test that shows it is done.
2. **Stay mergeable with upstream.**
   - New code goes in new directories.
   - Edits to shared files stay small, sit behind platform macros, and are written so they could go upstream.
   - The Vulkan backend implements the same `mtl_*` symbol set as `nvn_mtl.m` and is chosen at build time. Nothing is renamed.
3. **Get onto the phone early.** The Mac/KosmicKrisp loop is for API correctness and validation layers. Adreno behaviour, performance and drivers are judged only on the phone.
4. **Run tests where things run.** Probes run both from `adb shell` and inside a real APK (the `untrusted_app` SELinux domain), because the two differ on exactly the points that matter: TLS, executable memory, signal chaining and pointer tagging.
5. **Bring your own files.** This fork is public. Never commit game files, keys, rewritten modules or driver packages. An APK built from a game contains that game's code and must not be shared.

## Directory layout

| Location | Contents |
|---|---|
| `dm/android/app/` | The APK. Phase 1: a NativeActivity with no Java code, built with `aapt2` / `zipalign` / `apksigner` like the probes (input through its `AInputQueue`, the window through `ANativeWindow`). Later: a Gradle project with the Kotlin launcher (from `launcher.json`), GameActivity and the IME, swapped in behind the same `front_android.c` |
| `dm/android/front/` | `front_android.c` (the `front.h` interface), input, IME, haptics, thermal (all JNI on host threads) |
| `dm/android/probe/` | Phase 0 probes: an NDK executable and a minimal NativeActivity APK |
| `dm/android/cmake/` | NDK build of the host: `dmhost` executable (adb loop) and `libdmhost.so` (app) |
| `dm/host/plat/` | `plat.h`, `plat_darwin.c`, `plat_linux.c`: the platform layer |
| `dm/gfx/null/` | `nvn_null.c`: the `mtl_*` symbol set with no GPU (CPU bring-up, Linux CI) |
| `dm/gfx/vkn/` | NVN on Vulkan: `nvn_vk.c` and its parts (memory, textures, descriptors, pipelines, passes, present), `kernels/*.glsl` |
| `dm/gfx/tools/` | `dmsh_build.py --target vk` (Vulkan stage list), `vk_fmt` table generator |
| `scripts/build_apk_from_nsp.py` | NSP folder → APK + game-data folder |
| `docs/android/` | This plan, `probes.md` (Phase 0 results), `upstream-sync.md` |

## Phase 0: probes (go / no-go)

The probes live in `dm/android/probe/`. Each one runs as an NDK executable over `adb shell`, and as a NativeActivity APK with `android:hasCode="false"`. The APK is built with `aapt2`, `zipalign` and `apksigner` from the installed build-tools, so it needs no Gradle. Results go to `docs/android/probes.md`.

| # | Question | Pass test | If it fails |
|---|---|---|---|
| P0.1 | **Executable memory.** Can the app map anonymous memory, copy code in, `mprotect` it RX and run it (SELinux `execmem` for `untrusted_app`)? | A copied function, with `__builtin___clear_cache` called on it, returns the expected value in the APK | Ship the rewritten modules as files in `nativeLibraryDir` and map them from there |
| P0.2 | **Thread slots.** The stubs reach K18/KTLS/KTLR/KGATE through `mrs tpidrro_el0` + a constant offset (`dm/tools/patch_nso.py:98-102`), checked by `tsd_check` (`dm/host/loader.c:38-44`). Linux keeps `tpidrro_el0` at zero for 64-bit tasks, so on Android it must be `tpidr_el0` (bionic's thread pointer). Candidates: (a) initial-exec TLS at a fixed offset from `tpidr_el0`, which works in an executable; bionic is believed to refuse it in a dlopen'd `.so` (verify). (b) Two loads: `tpidr_el0` → a slot in bionic's TLS area → a host per-thread block, with the layout checked at startup like `tsd_check`. (c) Launch the host as an executable child process of the app | A stub-shaped sequence reads the right per-thread value on 64 threads, after `pthread_create`, and inside signal handlers, in the APK | (c). If the stubs grow by an instruction, re-run the rewriter on Monster Hunter Rise: its patch area sits at the edge of the ±128 MB branch reach (`patch_nso.py:36-45`) |
| P0.3 | **No red zone.** Stubs save scratch registers below `sp`: −16/−24, or −112 when the instruction moves `sp` (`patch_nso.py:151-152, :212, :266, :276`). Darwin is believed to reserve 128 bytes there; Linux reserves none. Upstream installs handlers with `SA_ONSTACK` and gives each thread an alternate signal stack (`dm/host/crash.c:191-215`), but in an app ART's libsigchain sits in front | On a thread running on an SDK-sized stack: SIGUSR2, plus a write fault from the dirty tracker, leave `[sp−112, sp)` intact, in the APK | Change the stubs to write-back forms (`stp …, [sp, #-16]!`) |
| P0.4 | **Pointer tagging.** Apps targeting API 30+ get tagged heap pointers | `android:allowNativeHeapPointerTagging="false"` in the APK, `mallopt` in the executable; an assert that every host pointer handed to the guest has a zero top byte | — |
| P0.5 | **x18 and TLS.** bionic uses x18 for the shadow call stack and `tpidr_el0` for its TLS | After rewriting, guest code never changes the real x18 or `tpidr_el0`; host code called from the guest keeps working | — |
| P0.6 | **System registers.** Can EL0 read `cntvct_el0`, and is `cntfrq_el0` 19.2 MHz (the Switch rate)? What does `ctr_el0` hold, compared with the rewriter's constant (`movz #4, lsl #16`, `patch_nso.py:845-847`)? | Values logged | Keep the gates. A matching counter allows a fast path later |
| P0.7 | **GPU memory**, on all three drivers: stock, 842.6 through libadrenotools, and Turnip R8's separate `turnip_a8xx.zip` (the main R8 package may not drive an Adreno 830). Compare the three inside the APK, since libadrenotools loading is only certain to work there; `adb shell` is enough for the stock driver. Log the full extension and feature list: BC formats, geometry shader, `indexTypeUint8`, `depthBounds`, push descriptors, sparse binding and residency, subgroup stages. Check:<br>- the memory types: is there one that is HOST_VISIBLE + COHERENT + CACHED?<br>- a placed map over an existing `PROT_NONE` reservation, as `mem_init` makes (`dm/host/hle_svc.c:30-47`)<br>- one 3.5 GB allocation, and adjacent allocations at contiguous addresses<br>- unmapping with `VK_MEMORY_UNMAP_RESERVE_BIT_EXT`<br>- CPU bandwidth on placed memory compared with anonymous memory<br>- the GPU sees CPU writes without a flush, and the CPU sees GPU writes after a fence<br>- `mprotect` and the write fault work on driver-mapped pages (`dm/gfx/nvn/nvn_dirty.c`) | Placed memory is cached, coherent and as fast as anonymous memory for the CPU; GPU and CPU see each other's writes; `mprotect` works | Strategy B (shadow copies, below), or rely on 842.6 / Turnip only |
| P0.8 | **Shaders.** SPIR-V from `dmsh_xlate` for Stardew Valley | `spirv-val` is clean, pipelines build on the device, compile times recorded, and a test pattern shows the Y direction and NaN `min`/`max` behaviour | Fix the translator options before Phase 2 |
| P0.9 | **KosmicKrisp.** Build it on the Mac and run the P0.7 subset | Placed mapping works in the Mac host process | Develop the backend on the phone only |

**Decisions Phase 0 makes:**
- the thread-slot mechanism (P0.2)
- how the rewritten modules are mapped (P0.1)
- the memory strategy (P0.7), one of:
  - **A:** the whole guest heap and module RW memory are Vulkan memory.
  - **A′:** each pool's page-rounded range is placed at pool init: copy out, map placed, copy back.
  - **B:** shadow copies on the stock driver.
- the default driver for this phone.

**Made (2026-10-25, [probes.md](probes.md)):** bionic's pthread key slots, two loads (P0.2's candidate (b)); the macOS mapping path plus cache maintenance (P0.1); strategy A on Qualcomm 842.6, the default driver when the user supplies it, B on the stock driver, Turnip second (P0.7, with the conditions listed there: prefaulted placed chunks, a coarse dirty tracker with a VMA budget, no freeing on 842.6, parameters in buffers). Phase 2 changes the memory order: B first everywhere (2.2).

## Phase 1: CPU and host port

**Done when:** on the phone, with the null GPU backend, Stardew Valley boots to its frame loop, with the log matching a Mac run up to the first present (ANIMAL WELL and Celeste too when supplied). The Mac reference is a run with a fresh save, `DM_DETCLOCK`, `DM_FIXED_TIME` and a fixed `DM_KEYS` script; `dm/android/tools/lognorm.py --until-present` normalizes both logs (addresses, paths, numbers, backend-tagged and statistics lines), and two Mac runs normalize to the same text. Pixels are not compared: two Mac runs of the same script differ in 2–37% of the pixels of the same frame, `DM_DETCLOCK_PACE` included. The frame loop is checked on the wall clock (the same script in seconds): with `DM_DETCLOCK` the guest clock only advances per present, and a backend whose first frame is fast (the null backend now; Vulkan later, with a warm pipeline cache) leaves the game asleep in its fixed-timestep wait after the first present. On the Mac the first Metal frame is slow enough to hide this. This must work two ways: from `adb shell` (the `dmhost` executable), and from `libdmhost.so` inside the APK using the P0.2 thread-slot mechanism and its startup self-check. The APK run is the case where the slot mechanism is uncertain, so Phase 2 does not start until it passes. The portable unit tests (`t_cover`, `t_hmap`, `t_pace`, `t_fstep`, `tex/tests/t_tex`, then `t_dirty_*`) pass on the device. Input and audio work in the APK shell.

**Done (2026-10-25).** Both ways (`dmhost` over adb, `libdmhost.so` in the APK), Stardew Valley boots to its frame loop with the null backend, and `dm/android/tools/parity.py` finds the log up to the first present equal to the Mac reference's apart from the lines `dm/android/tools/parity_expected.txt` lists (the app's boot lines, the signal guard, the window, the slot check, the present thread's nice value in place of the time-constraint policy). It fails on any `[RED]`, `FATAL` or `CRASH` line anywhere in either log. The slot check passes in the APK. On the wall clock, with two button presses injected over adb, the game reaches its new-farm check (`hasRoom='True'`) through an AAudio stream. The unit tests, plus `t_dirty_fail` and `t_sig_guard`, pass on the phone. What the work found:
- **Input:** Android names gamepad buttons by position, so `KEYCODE_BUTTON_A` (the bottom face button) is the Switch's B, and the Switch's A is `KEYCODE_BUTTON_B`. `adb shell input keyevent` needs `--duration`: a plain one goes down and up within a millisecond, which one game read can miss.
- **Signals** (P0.3): every thread that runs guest code scans the installed handlers as it starts (`crash.c`, Linux only). The first scan reinstalls bionic's own handlers that lack SA_ONSTACK (36; 39 too in the executable) with it. Any other handler without SA_ONSTACK, a later bionic one included, is blocked on the guest threads that start after it appears (in the APK: libopenjdk's 34, perfetto_hprof's 38). Some signals are never blocked, whoever's handler they hold: the faults, SIGSYS, the host's own SIGUSR2, SIGALRM, SIGTERM and SIGABRT, and bionic's cross-thread handshakes 33, 40 and 41. Blocking 40 hangs `android_run_on_all_threads`, which holds the thread-creation lock while it waits. Open cases: a handler without SA_ONSTACK that appears after a guest thread started; a foreign handler without it on one of the never-blocked signals; and 40, whose handler never has it. 40 is sent by `android_run_on_all_threads`, a libc-private function whose one caller in bionic changes the heap-tagging level under MTE; nothing on this phone imports it and the phone has no MTE, but nothing stops a library from calling it. The complete fix is the write-back stub forms (Risks).
- **Dirty tracking:** a failed `mprotect` (a mapping split past `vm.max_map_count`, P0.7) no longer leaves a page faulting forever or a write unseen. The tracker makes its page states true again and counts the pool as always written (re-uploaded at every use). A write fault it cannot resolve goes to the crash report, and a set-read-only it cannot undo stops the host with a `[RED]` line. One case is left: a page the tracker could not make writable again stays read-only, so a host write into it through the kernel (a `read` into guest memory) fails with `EFAULT`.
- **The app's log:** the library's constructors run during `dlopen`, before the activity. The front end's first constructor opens `log.txt` and reads `env.txt`, so their lines and the settings they read are covered. When it cannot (the files directory cannot be derived or opened), the constructors' lines are kept in memory and copied over later, and the log says so in a `[RED]` line (logcat does when there is no log at all).

### 1.1 Build
- CMake with the NDK toolchain builds `dmhost` (an executable for the adb loop) and `libdmhost.so` (for the app).
- The source lists follow `dm/host/Makefile:19-22`, minus the Objective-C files: `nvn_mtl.m`, `nvn_refl.m`, `hle_vk.m`, `hle_gl.m`, `tex/nvn_deswizzle_gpu.m`, `memstat.m`, `haptics_apple.m`, `front_mac.m`.
- `game_profile.h` comes from `dm/games/game.py header`, as now.
- Branch protection stays off (no `-mbranch-protection`), so the hand-written assembly needs no `bti`.

### 1.2 Mach-O → ELF
- **Import tables.** The `swhle` and `swforce` tables use `section("__DATA,…")` (`dm/host/hle.h:9,37,46`, `hle_table.c:7`) and are read back with `getsectiondata` (`hle.c:30,49`, `loader.c:152`). On ELF they become `section("swhle")` read through the linker's `__start_swhle` / `__stop_swhle` symbols. Later, the same for `hle_vk.m:42-51`.
- **Assembly** (`entry.S`, `hle.h:44-45`, `hle_bigstack.c:41-45`, `dm/gfx/nvn/nvn_entry.S`, and the generated `nvn_thunks.S`, which needs an ELF mode in `gen_protos.py`): drop the `_` symbol prefix, `.private_extern` becomes `.hidden`, and `@PAGE`/`@PAGEOFF` become plain symbol / `:lo12:`.
- **Other `getsectiondata` users:** `hle_nvn.c:83`.

### 1.3 Rewriter target
- `patch_nso.py --target android` emits the thread-slot sequence chosen in P0.2 instead of `MRS_TPIDRRO` (`dm/tools/nso_asm.py:14`).
- The fixups header records the target, and the loader refuses a mismatch.
- The `& ~7` mask on the slot base is Darwin-only (`loader.c:42`).
- The other rewrites stay; each still has a reason on Linux:
  - x18 is bionic's shadow call stack.
  - `svc` would become a Linux system call, and seccomp kills unknown ones.
  - Linux also faults on a misaligned `sp`.
  - `cntpct` must go through the gate (P0.6).
- The 16 KB alignment (`patch_nso.py:39, :55-60`, `loader.c:203-205`) is a multiple of 4 KB, so it stays. It also keeps 16 KB-page Android devices working.
- **Cache maintenance:** after copying the text and the patch area in, and before they run, the loader calls `__builtin___clear_cache` over both (`loader.c:518-556`). Darwin takes care of this when pages become executable; Linux cannot be relied on to. A probe can pass without it by luck on fresh pages, so the call stays even if P0.1 passes.

### 1.4 Platform layer (`dm/host/plat/`)

| Apple API (where) | Android / Linux replacement |
|---|---|
| `mach_absolute_time` (`hle_os.c:39-43`, `nvn_dirty.c:44-55,122,205`, `hle_nvn.c:1364,1477,1503-1505,1670`, `nvn_entry.S:41,62`, `nvn_mtl.m:1107`); `clock_gettime_nsec_np` (`hle_time.c:22`, `hle_fs.c:494,527,639`); `mach_wait_until` (`hle_nvn.c:1505`) | `clock_gettime(CLOCK_MONOTONIC)` and `clock_nanosleep` |
| `sysctl KERN_PROC_PID` (`main.c:48-55`), `sysctlbyname` (`hle_nvn.c:1574`), `task_info` (`hle_nvn.c:1318`), `memstat.m` | `/proc/self/stat`, `/proc/self/status`, `/proc/meminfo` |
| `pthread_setname_np(name)` (`hle_thread.c`, `thread_main`) | `pthread_setname_np(pthread_self(), name)`, 15 characters |
| `pthread_get_stack*_np` (`hle_svc.c:220`) | `pthread_getattr_np` + `pthread_attr_getstack` |
| `pthread_cond_timedwait_relative_np` (`hle_movie.c:170`) | a condition variable on `CLOCK_MONOTONIC` |
| QoS (`hle_thread.c:221-245`), `THREAD_TIME_CONSTRAINT_POLICY` for the present thread (`hle_nvn.c:1474-1482`) | `setpriority` plus an ADPF `APerformanceHint` session for the game and present threads |
| Darwin `ucontext` `__ss`/`__ns`/`__es` (`crash.c:136,145-172`, `hle_thread.c:284-295`, `probe.c:108-146,188`) | Linux `mcontext`: `regs[]`, `sp`, `pc`, and the `fpsimd_context` / `esr_context` records in `__reserved` |
| Mach thread suspend and state (diagnostics only: `hle_thread.c:358-372, 532-563, 595-619`, `probe.c:300-314`); `thread_info` run state (`hle_thread.c:123-128`, off by default) | Off on Linux; `/proc/self/task/<tid>/stat` if needed |
| `vm_read_overwrite` (`crash.c`, `hle_fpslock.c`, `hle_movie.c:117-128`, `hle_nvn.c:2344`) | `process_vm_readv` on the own process (a fault-safe read) |
| `vm_region_64` (`hle_fs.c`), `mach_vm_region` (`probe.c`) | `/proc/self/maps` |
| `clonefile`, `renamex_np(RENAME_SWAP)` (`hle_fs.c`, the transactional saves) | Copy (or `FICLONE` where the file system has it); `renameat2(RENAME_EXCHANGE)` through `syscall` (check on f2fs) |
| `fcntl(F_GETPATH)` (`hle_fdstat.c`) | `readlink("/proc/self/fd/N")` |
| `malloc_zone_*`, `malloc_size` (`hle_uemalloc.c`) | Own arena, or `malloc_usable_size` |
| `sys_icache_invalidate` (`hle_count.c:44`) | `__builtin___clear_cache` |
| CommonCrypto SHA-256 (`loader.c:420,448`, `hle_ro.c:54`, `hle_nvn.c:596-599`) | A vendored portable SHA-256 |
| CFDictionary (`hle_nvn.c:571-583`) | A C hash map (`nvn_hmap.h` or similar) |
| `vm_allocate` / `vm_remap` embedding (`loader.c:464-516`, iOS only) | Not used: the Mac path (`loader.c:518-556`: read the file, `mmap`, `mprotect` RX) or P0.1's fallback |
| `vm_remap` aliasing of virtual pools (`hle_nvn.c:1906`) | Phase 4 (a dual mapping through dma-buf, or Vulkan sparse binding) |
| GCD: semaphores (`hle_audio.c:30-32`), `dispatch_source` (`hle_lifecycle.c:21-27`) | POSIX semaphores, a timer thread, `signalfd` |
| `os_unfair_lock` (`nvn_mtl.m` only) | Not needed: the Vulkan backend uses its own locks |

Portable as they are: `thread.c`, `hle_account.c`, `hle_audren.c`, `hle_bigstack.c` (apart from its assembly), `hle_codec.c`, `hle_friends.c`, `hle_glslc.c`, `hle_guestthread.c`, `hle_haptics.c`, `hle_hid.c`, `hle_mii.c`, `hle_misc.c`, `hle_net.c`, `hle_nvdrv.c`, `hle_oe.c`, `hle_offline.c`, `hle_oodle.c`, `hle_sdkinit.c`, `hle_sf.c`, `hle_swkbd.c`, `hle_table.c` (apart from its section), `hle_touch.c`, `dm/gfx/hle_vi.c`, `dm/gfx/hle_nv.c`. `arc4random` and `strlcpy` exist in bionic.

On the guest–host ABI: Apple's arm64 ABI passes variadic arguments on the stack and differs for narrow integer parameters (`hle.h:2-3`). Android uses the same AAPCS64 as Horizon, so these workarounds are only needed on Apple. They stay, since they are harmless there.

### 1.5 Services
- **Audio out:** the AudioUnit code in `hle_audio.c:146-260,434-465` becomes AAudio (48 kHz, 16-bit, low latency). The queue, downmix and volume code (`:62-142, :201-213`) stays.
- **Movie sound:** AAC (`hle_movie.c:331-371`, AudioConverter) moves to `AMediaCodec`. libvpx and stb_vorbis are already portable.
- **H.264:** `hle_movie_avc.c` (VideoToolbox) becomes `AMediaCodec` behind the same interface (`hle_movie_avc.h:7-17`), producing NV12. The SDK's decoder wraps MediaCodec too (`hle_movie.c:34`).
- **Haptics:** `haptics_apple.m` becomes Vibrator / VibratorManager, plus controller rumble through `InputDevice`, called through JNI.
- **Lifecycle:** `hle_lifecycle.c` (GCD) becomes a timer thread, driven by the activity's pause and resume.

### 1.6 Front end (`front.h`)
- **Provided by the front end:**
  - `front_run`
  - `front_native_window`, which the Vulkan backend uses. `front_metal_layer` is used only by `nvn_mtl.m` and the graphics tests.
  - `front_screen_max_fps` / `front_screen_match`: the display modes and `ANativeWindow_setFrameRate`.
  - `front_text_input`: the system keyboard.
  - `front_save_failed`
  - the weak hooks `dm_thermal_now` (`AThermal`), `dm_low_power_now` and `dm_brightness_now` (`hle_nvn.c:1336-1340`).
- **Called into the host:** `dm_touch_set`, `dm_pad_set`, `dm_focus_set`, `dm_audio_suspend`, `dm_gfx_suspend`, `dm_host_shutdown` (`front.h`, `dm_input.h:14-24`, `hle.h:51-58`).
- **The touch gamepad:** `dm/ios/Sources/DmGamepad.m`, 450 lines, is rewritten as an overlay view.
- **JNI threads.** Every JNI call is marshalled to a host-owned thread. HLE code runs on guest stacks, some as small as 8 KB (`hle_bigstack.c`), and ART cannot run there.

### 1.7 Paths and settings
- **Environment set before `dm_host_preload_start`**, as `dm/ios/Sources/main.m:26-71` does:
  - `DM_ROMFS`: app-specific external storage, with the marker check from `main.m:17-22`.
  - `DM_SAVE` and `DM_TEMP`: internal storage.
  - `DM_AOC_DIR` and `DM_FONT_DIR`
  - `log.txt` / `log.prev.txt`, `env.txt` and `pso_user.tsv` in a folder reachable over USB and adb.
- **Settings:** the Kotlin launcher reads `launcher.json` and applies the choices with `setenv` through JNI before start, as `Launcher.swift:44` does.

### 1.8 Null GPU backend (`dm/gfx/null/nvn_null.c`)
- It implements the `mtl_*` set that `hle_nvn.c` calls:
  - the functional ones in `nvn_mtl.h`
  - `nvn_obj.h:166-220`
  - the statistics externs at `hle_nvn.c:1249-1251, 1346, 1379-1383`
  - the exports used by `hle_vk` / `hle_gl` (`nvn_mtl.m:8805-8817`)
- **Behaviour:** syncs complete at once, reads return zeros, and present counts frames.
- **Later uses:** it is the template for the Vulkan backend's symbol list, and it lets a Linux CI job build the host.

## Phase 2: GPU rewrite (NVN on Vulkan)

**Done when:** Stardew Valley is playable on the phone (title screen, a new farm, then gameplay) at 60 fps, with no validation-layer errors. ANIMAL WELL and Celeste when supplied, then a Unity game (Patrick's Parabox), then Mario Wonder follow.

**What Stardew Valley uses** (the null backend's counters, title screen to the farm; 124 NVN functions, the same set as on the Mac): indexed draws only, all u16 triangle lists, nearly all with a base vertex, up to 126 a frame. One RGBA8 colour target, with D24S8 depth in about half the passes. Clears only at the start of a pass, never with a partial mask. Viewport swizzles only as a Y flip. No compute, queries, transform feedback, conditional rendering, copies, fills, MSAA, instancing or indirect draws. The first backend covers exactly this, and the rest follows game by game.

Build it in vertical slices:
1. **Mac + KosmicKrisp, validation layers on:** a clear colour, then present, then the first real frame of Stardew Valley in the Mac host.
2. **Phone:** the same slices on top of Phase 1.
3. **Widen game by game.**

### 2.1 Device, driver loading, present
- **Device:**
  - Vulkan 1.3: dynamic rendering, synchronization2, timeline semaphores.
  - Buffer device address, descriptor indexing, extended dynamic state.
- **Driver loading:** libadrenotools loads a user-supplied driver package, falling back to the system driver. The launcher shows which driver is active.
- **Present:**
  - The swapchain is on the `ANativeWindow`.
  - The pacer logic in `nvn_pace.h` is portable. It is driven by `AChoreographer` vsync callbacks plus present-wait / present-id, or Swappy.
  - On the Mac, `VK_EXT_metal_surface`.

**As built, slice 1 (2026-10-26):** `dm/gfx/vkn/`, built with `make GFX=vkn` on the Mac and `build_app.sh <GAME> --gpu vkn` for the phone; `run_app.sh --driver system|842|turnip` picks the driver. Render targets, clears, barriers, submission and present are real. Draws, binds and texture uploads are only counted, which is the next slice.
- **Submission:** four slots, one timeline value per submit (the NVN fence), and a completion thread that writes a submission's effects before its fence counts as done.
- **Present:** its own thread and timeline, FIFO. The swapchain is made in the display's natural orientation and the picture is rotated in the shader. Crop and clamp are specialization constants, because push constants hit a Turnip bug.
- **Driver check:** `vk_dev.c` reports which driver actually answered, from the mapped library's path and the driver ID.
- **A lost GPU** is reported once; submissions then complete without it. Once any submission has been retired without the GPU's answer, nothing is freed again (a leak, not a use-after-free).
- **Bounds on Android:** window loss and the suspend drain wait at most 4 s.
- **Test:** `dm/gfx/tests/t_vkn.c` (`make check_t_vkn`, strict) checks clears and pass ends by readback on KosmicKrisp, with validation on.

Results: Stardew Valley's title-screen clears on KosmicKrisp, the system driver, 842.6 and Turnip (with `TU_DEBUG=sysmem,noubwc`), with 0 `[RED]` and 0 validation messages; background and resume rebuild the swapchain.

Open after slice 1:
- **Turnip:** in its default GMEM mode the device is lost once presents run. With `sysmem,noubwc`, the present test pattern (quadrants cleared into an image, then sampled) comes out as 8 px vertical stripes; review found nothing invalid in that image's setup. Turnip is not a target until this is understood.
- **Suspend can still hang:** `hle_nvn.c` waits without a bound, before `dm_gfx_suspend(1)`, for a submit blocked on a free slot. A GPU that hangs without reporting loss can block `onStop`. No backend-only fix keeps pending work safe.
- **Crop changes:** a new crop compiles a present pipeline (4-entry cache). Games with dynamic resolution need a uniform-buffer path.
- **Format-changing views:** render-target views of another format are only reported in red, for example Mario Wonder's RGBA8 window texture written through an sRGB view.

### 2.2 Memory (by the P0.7 decision)
- **Boot order (strategies A and A′):** today the Metal device is created at graphics init (`nvn_mtl.m:282-365`). Under A, the Vulkan device has to exist before `mem_init`, the first `SetHeapSize` and the loader's RW mapping. So the host's early load at app launch creates it first: choose the driver (loading it through libadrenotools if needed), create the device and pick the memory type. Phase 1 still uses plain `mmap`, so the null backend is not affected.
- **Strategy A:**
  - `mem_init` and `SetHeapSize` (`hle_svc.c:30-47, 137-155`) map placed Vulkan memory where they now `mprotect` the reservation RW.
  - The loader's module RW memory (`loader.c:518-556`) does the same.
  - Allocations are split into chunks no larger than `maxMemoryAllocationSize` (4 GB − 1 on Turnip), placed side by side.
  - A pool that crosses a chunk join needs a sparse buffer, or heap growth aligned to chunks.
- **Strategy A′:** at `mtl_pool_add`, copy the page-rounded range out, place it, and copy it back.
- **Bring-up order (measured 2026-10-25):** B first, on all three targets; A on 842.6 later, as an optimization. On the null backend, from the title screen to walking on the farm, Stardew Valley's GPU work writes nothing the CPU reads back: no queries, transform feedback or copies into buffers, and its ReadTexels calls (12, at loads) are all on textures the CPU wrote. It makes 32 pools (194 MB) and frees 5, each an 8.44 MiB render-target pool, one at frame 2 and four at the new-game load. Placed memory is never freed on 842.6, so that rules out A′ there. The buffer data the GPU reads on the farm is 259 pages (1.06 MB), of which about 40 are written each frame. Write-protecting it page by page cost 2.7 ms of CPU a frame in a simulation, so B copies that set at each submission without tracking it, and keeps write faults for textures and the rarely written pages. Render-target pools, which the CPU cannot access, get plain device memory with no shadow. B touches only ordinary memory, so the KGSL split-mapping bugs and the mapping budget (P0.7) do not apply to it. A would save the per-submission copy, estimated at under 0.1 ms a frame for this game
- **Strategy B:** shadow `VkBuffer`s, kept current from the dirty-page tracking in `nvn_dirty.c`.
- **Address lookup:** `mtl_addr` maps a game address to (`VkBuffer`, offset, device address), the same interval table as now (`nvn_mtl.m:382-493`).
- **Virtual pools** (`hle_nvn.c:1826-1915`, used by Monster Hunter Rise) wait for Phase 4.

**As built, slice 2a (2026-10-26):** strategy B in `dm/gfx/vkn/nvn_vk.c`, tracking with `nvn_dirty.c` (one function added, `nvn_dirty_protect_wait`; the rest unchanged). It keeps the four correctness conditions of the B simulation (`stardew-surface.md`, "Correctness conditions for B"):
1. **Fences:** an NVN fence counts as done only once the GPU has signalled that submission's timeline value (slice 1's completion thread). The host never writes a shadow buffer directly.
2. **Uploads:** each submission copies the pages it needs from its own slot's host-visible staging buffer into the shadows with `vkCmdCopyBuffer`. The copies are in a second command buffer of the slot, submitted ahead of the submission's own in the same `vkQueueSubmit2`. A barrier before the copies waits for all earlier GPU work, and one after them makes the data visible to vertex, index, indirect, uniform, storage and copy reads. A slot's staging is reused only after its previous submission has completed.
   - The upload is built at commit, not in `mtl_sub_begin`, because the pages a submission reads are known only after its replay.
   - Order, under the pool lock: take the dirty-generation stamp, pick the pages written since the previous stamp (plus pages read for the first time), write-protect them, then copy them to staging. A write after the protect faults and is picked at the next submission.
   - The protect is `nvn_dirty_protect_wait`: a page another thread is unprotecting (a fault, a host pre-write) is waited for and protected after. `nvn_dirty_protect` skips such a page, and when the unprotect had stamped it before this stamp, the page ended writable with nothing newer, so its next write was never seen. A run that could not be protected (a wait timed out, a set-read-only failed) is copied again at the next submission. The Metal backend's texture path (protect, then stamp, then read) has the same latent race; it is left as it is.
   - Pages that hold command records, and edge pages (host pages the pool's own bytes do not fill: with 16 KB host pages and 4 KB-aligned pools, a neighbouring pool's tracked range can hold the same page), are copied at every submission and never protected. With 4 KB host pages there are no edge pages. Slice 2b: texture protection must leave edge pages alone too.
   - The whole dirty set is copied each time. The hot-set scheme of `stardew-surface.md` is for the gameplay sub-slice.
3. **Pool delete:** under the pool lock, the pool's pages are made writable (`dm_guest_write_prepare`, then `mprotect` read-write over the whole range) and its shadow leaves the address table. The shadow is destroyed through slice 1's dead list (`vkn_sweep`) once every submission that used it has completed, and never after `g_leak`.
4. **No shadow, no tracking:** `mtl_pool_add` gets no pool flags. A pool gets a shadow, and its pages tracking, only when a replayed record reads it as a buffer (vertex, index, uniform, storage, indirect, copy source). So render-target (CPU_NO_ACCESS) pools and shader-code pools get none. A buffer read from a pool that `mtl_tex_init` saw as CPU_NO_ACCESS is reported in red and gets no shadow.
- **Address lookup:** an address resolves to the pool whose own bytes hold it, never through the page rounding (a neighbour's shadow never has this pool's bytes of a shared page). `mtl_addr` returns the shadow `VkBuffer` (NULL before the pool has one) and the offset from the page-rounded pool start. `mtl_gpu_addr` also returns the buffer device address (`bufferDeviceAddress` is enabled), and fails for a pool with no shadow.
- **Not covered yet:** a CPU-run GPU op (buffer copy, fill, uniform update) lands in guest memory at replay and is uploaded at commit. A draw earlier in the same submission therefore sees its result. Stardew Valley makes none of these ops on this route. Texture storage is not tracked; textures are slice 2b.
- **Checking frames:** `DM_NVN_SHOT` works on vkn: the window texture is read back at the requested presents and written in the Metal backend's format and orientation. `dm/android/tools/imgdiff.py` compares two frames: mean error per channel, the share of pixels over a threshold, and the bounding box. Metal reruns of the title screen already differ from frame 600 on (cloud and title animation), so frames are compared with a tolerance.
- **Detclock workaround:** under `DM_DETCLOCK`, vkn sleeps one frame of wall time at the first present. The guest clock freezes at the first present, and MonoGame's fixed time step then waits on it forever when that present came quickly. The fix belongs in `hle_os.c`, where it would change the Metal reference frames; the workaround goes when that lands.
- **Results:** title-screen uploads of 3.26 pages a frame at 16 KB pages (the Mac) and 4.18 at 4 KB (the phone), as measured beforehand, at 0.04–0.07 ms of commit time on the Mac and about 0.27 ms on the phone.

### 2.3 Textures
- **Format table:** `nvn_fmt_vk.c`, a twin of `tex/nvn_fmt_mtl.c:7-17`, mapping NVN formats to `VkFormat` and component swizzles.
- **Deswizzle:** first on the CPU (`tex/nvn_deswizzle.c`, portable), then as a Vulkan compute shader (the kernel at `tex/nvn_deswizzle_gpu.m:5`).
- **Re-upload** on dirty pages, as now.
- **BC formats** native (P0.7 confirms BC1-7).
- **`ReadTexels`**
- **Samplers** with custom border colours, replacing Metal's three presets.

### 2.4 Shader chain, Vulkan target (`dmsh_build.py --target vk`)
The translator already emits SPIR-V with `TargetApi.Vulkan` (`dm/gfx/tools/dmsh_xlate/Program.cs:160`). What changes are the post-processing passes that work on Metal source text:

| Step now (`dmsh_build.py`) | Vulkan target |
|---|---|
| `vertex_ab`, `dmsh_xlate`, `VertexCounterLowering`, the `softfloat64` retry (`Program.cs:125,170-175`) | Keep (Adreno has no FP64 in most drivers) |
| `spv_single_lane` (`:700-737`; Metal has no SIMD-group functions in vertex functions) | Keep the idea: KosmicKrisp has no subgroups in vertex shaders either. Plus `dm/android/tools/spv_strip_caps.py`, which drops the `Float64`, `GroupNonUniform*` and interlock capabilities a module declares but does not use (all 65 Stardew modules declare them; no driver here supports `Float64` or interlock) |
| `precise()` NaN fix on the Metal source (`:40, :826`) | Today the translator emits `FMax` and `FClamp`, whose NaN result SPIR-V leaves undefined (P0.8: Stardew has 47 and 33, no `N*`). Every driver here happens to return the non-NaN operand, Maxwell's behaviour, in a compute test; the chain still rewrites them to `NMax`/`NMin`/`NClamp` so the result does not depend on the driver |
| `vs_depth` (`main0` / `main0_z`, `:657-667`) | `VK_EXT_depth_clip_control` per pipeline (`QueryHostSupportsDepthClipControl`, `Program.cs:120`) |
| `vs_pad` (`:564`, from `shader_pairs.tsv`) | A SPIR-V pass (Vulkan also needs every fragment input written). The same pass shrinks `gl_PerVertex`: every vertex shader declares 8 clip plus 1 cull distance, over the limit of 8 (the validation layer's only error on the stripped modules; drivers accept it) |
| `vs_init`, `vs_layer`, `vs_tf`, `fs_uint` (`:352-669`) | Check each one; transform feedback goes native through the extension |
| `gs_slice`, `geometry_finish` (compute emulation) | Native geometry shaders |
| Reflection by regex over the Metal source (`:454-500`, `arg_id` from `[[id(N)]]`) | Reflection from SPIR-V; `.res.json` already has the binding numbers (`Program.cs:183-195`). Each stage gets its own descriptor set: both stages now number from binding 1 in set 0, which collides in 59 of Stardew's 73 pipelines (Metal's per-stage argument tables hid it). Set layouts take the exact descriptor types from reflection, vertex formats the input's type (`uvec4` inputs need `_UINT` formats) |
| `c1_guard.py`, `cb_extent.py` (over `*_b.metal`) | Skipped at first. The conservative paths already exist: a whole 64 KiB c1 buffer per shader, and whole-binding copies (`dm/gfx/README.md:53-54`). Ported to SPIR-V later |
| `dmsh_fetch` (`_bf`, readable tile), `dmsh_vin` (`_bv`) | Drop. The tile-based optimizations are off by default upstream; vertex input is checked against Vulkan's format rules |
| `xcrun metal` → metallib | Dropped. Ship zstd-compressed SPIR-V; the output directory is `b-<V>-vk/`, with the target folded into the chain version |

**As built (2026-10-25):** `dm/gfx/tools/dmsh_build_vk.py`, a sibling of `dmsh_build.py` (editing that file would move the Metal chain's version, which hashes it), takes the shared steps from it by name and stops with a `[RED]` line if upstream changes one of them in a way it cannot follow. It writes `b-<V>-vk/<key>_b.spv.zst` and `<key>_b.json`; V folds the target, the Metal chain's version and the translator lock into one. The passes run in order:
- single-lane rewriting for vertex shaders, then a fix for the `OpSelect` it emits (invalid in SPIR-V 1.3);
- `N` min/max/clamp;
- the `gl_PerVertex` cut, only in the last stage before rasterization;
- removal of unused descriptors;
- one set per stage (vertex 0, pixel 1, geometry 2, hull 3, domain 4, compute 0);
- padding and initialising of the vertex outputs the paired pixel shaders read, matched by equivalent type (base type, components, array shape; no widening, since that needs maintenance4); interpolation decorations stay on the pixel side;
- the capability strip, then `spirv-val` for Vulkan 1.3.

The reflection gives each binding its set, binding, exact descriptor type and count (null with `runtime` or `spec_sized`), each pixel output's value type, and a `status`. A backend must refuse a module marked `unsupported`. That covers geometry and tessellation shaders (they need per-topology variants), capabilities or extensions outside an allowlist of what all four drivers accept, push constants, and modules the passes cannot follow: several entry points, decoration groups, specialization-sized arrays, and aggregate, matrix or Component-decorated interface variables.

Results on Stardew Valley: 65/65 modules ok, and 73/73 pipelines with 0 validation messages on KosmicKrisp, stock, 842.6 and Turnip. The Khronos layer loads under libadrenotools too. `gen_manifest.py --vk` builds the probe's manifest from the reflection and fails if it differs from the SPIR-V. KosmicKrisp has no geometry shaders or transform feedback, so a Mac run refuses those; the phones would run them natively.

**Y direction (settled by P0.8):** on every driver here a positive viewport height puts clip-space y < 0 on memory row 0 and `FragCoord.y` equals the memory row; the Stardew vertex shaders pass Y through unchanged, and the game keeps NVN's lower-left window origin. So for a lower-left origin the backend keeps positive viewport heights, reverses the front-face winding, and flips the picture at present; depth uses `VK_EXT_depth_clip_control` (NVN's near = −w). No shader change.

**Pipelines (P0.8):** Stardew's 73 pipelines take about 0.4 s cold on the Qualcomm drivers on a prime core (3.3–3.6× that on a little core), about 5 ms with a pipeline cache. No driver persists its own cache, so the backend keeps a `VkPipelineCache` file per driver identity (`driverID`, `driverVersion`, `pipelineCacheUUID`), and compiles on worker threads pinned to prime cores. Turnip never reports cache hits, so whether its cache works is judged by timing.

**Helper kernels.** These are written as Metal source inside `nvn_mtl.m` and need GLSL → SPIR-V counterparts:
- `ms` (`:6458`)
- `ixc` (`:6629`)
- `wide` (`:6722`)
- `mdx` (`:6853`)
- the texture-to-texture copy draw (`:7558`)
- `clear` (`:7836`)
- `texdump` (`:8265`)
- the present blit (`:8390`)
- geometry pack (`nvn_geometry.h:73`)
- the 4 compute shaders embedded in games (`nvn_builtin.h:14`)
- deswizzle

Several go away on Vulkan: triangle fans are native, indirect-count draws are native, 8-bit indices are native where `indexTypeUint8` exists, and geometry shaders are native.

### 2.5 Descriptors
- **Layouts:** per-shader set layouts from the Vulkan reflection.
- **Per-draw descriptors:** written from the same host-side resolution `encode_args_` does now (`nvn_mtl.m:4865-5105`):
  - texture handle decode: `& 0xfffff`; sampler: `>> 20`
  - bindless handles read from the guest's uniform buffer (`ubo_read`, `:4841-4926`)
  - DRV0/DRV2 synthesized driver constant buffers
  - dummies for unbound slots
- **Mechanism, chosen from P0.7:** descriptor buffers (present on 842.6), push descriptors, or per-frame pools.

### 2.6 Pipelines
- **Key:** today's `pso_skey` stores Metal pixel formats (`nvn_pso_skey.h:7`; filled from `pixelFormat` in `nvn_mtl.m`). Switch it to NVN formats, so that `pso_manifest.tsv` and `pso_user.tsv` work for both backends. This change could go upstream.
- **Cache:** a `VkPipelineCache`, plus `VK_KHR_pipeline_binary` where present, saved next to `pso_user.tsv`.
- **Fewer variants:** graphics pipeline libraries and extended dynamic state. Separately linked libraries need `graphicsPipelineLibraryIndependentInterpolationDecoration` (842.6 reports 0; stock has no `VK_EXT_graphics_pipeline_library`): without it, a padded vertex output, which carries no interpolation decoration, does not match a flat pixel input.
- **Prewarm:** in the background, as `warm_start` does (`nvn_mtl.m:4702`).
- **Stutter is a bigger risk than on iOS:** the driver compiles SPIR-V on the phone, where upstream ships precompiled Metal libraries. The prewarm lists matter more.

### 2.7 Passes, draws, compute, copies, sync
- **Render passes:**
  - Dynamic rendering, keeping the clear-folding logic.
  - Mid-pass clears through `vkCmdClearAttachments`. This closes a gap listed in `dm/gfx/README.md:182-186`; clears with a partial channel mask still need a draw.
- **Draws** (`nvn_mtl.m:7072-7354`):
  - Quads through `nvn_primitive.h`; fans native.
  - Indirect and indirect-count draws, which closes the `MultiDraw*IndirectCount` no-op.
  - Transform feedback.
  - Conditional rendering, which closes "always drawn".
  - Occlusion queries.
- **Compute** (`:7384-7450`) and **copies / clears / resolves** (`:7453-7770`).
- **Sync:** one timeline semaphore value per submit. A completion thread performs the effects NVN shows after GPU completion (events, counters, sync objects), which Metal completion handlers do now (`nvn_mtl.m:1086-1293`).

### 2.8 Diagnostics and profile keys
- **Ported switches:** `DM_NVN_SHOT` and `DM_NVN_TEXDUMP`.
- **Validation layers:** on KosmicKrisp and on the phone.
- **Profilers:** Android GPU Inspector, RenderDoc, Snapdragon Profiler.
- **Metal-only profile keys:** `feedback_snap`, `vconv_range`, `tex_dyn_slots` and `gpu_timestamps` are Metal-motivated (`dm/games/README.md:62-69`). The Vulkan backend reinterprets or ignores them.

## Phase 3: packaging (NSP → APK)

**Done when:** one command turns a folder of NSPs into an APK plus a game-data folder, which installs and plays on the phone.

- **`scripts/build_apk_from_nsp.py`.**
  - The stages shared with `build_ipa_from_nsp.py` move into a common module: scan, NSZ, profile and survey, `nx_extract`, rewrite, shader split.
  - `check_prereqs` (`:773-782`) checks per target.
  - The `clonefile` path (`:484-490`, `nx_extract/Program.cs:26,178`) gets a copy fallback.
  - Stages that boot the Mac host stay Mac-only at first: `capture_glslc :986`, `decode_ue_shaders :1030`, `capture_shaders :1098` and `heap_check :1221`.
- **APK contents** (`dm/android/app`, Gradle):
  - `libdmhost.so`
  - the rewritten modules (in `nativeLibraryDir` if P0.1 requires it)
  - the SPIR-V libraries, profile tables, fonts and `launcher.json`
  - libadrenotools
  - The app ID is `org.nxnative.<title ID>`, as on iOS (`:1717`).
- **Game data:** the RomFS and DLC (up to about 17 GB) stay out of the APK. The app copies them from a folder the user picks into app-specific storage, or they are pushed with adb.
- **Signing and install:** a local keystore made once, `apksigner`, then `adb install`.
- **Driver packages:** the user puts a package in the app's folder; it is never in the repo or the APK.

## Phase 4: breadth and quality

- **Vulkan games** (Hades, Super Mario Galaxy 1 and 2):
  - `hle_vk.m` becomes a forwarding layer over the real driver.
  - It keeps the import bridge (`:38-52`), the swapchain integration and the SPIR-V fixes (`vk_shaders.py:59-76`).
  - The Metal translation (about 60% of the file) goes.
- **OpenGL games** (Digimon Story Cyber Sleuth, Balatro): these need desktop GL on Vulkan, since Android has only GLES. Low priority.
- **Virtual pools** (Monster Hunter Rise): a dual mapping through a dma-buf export, or Vulkan sparse binding.
- **Performance:**
  - ADPF and thermal state.
  - Adreno's on-chip tile memory (GMEM): load and store ops; upstream's tile-based tools are a reference.
  - Pipeline stutter.
- **CI:** a Linux runner builds the host with the NDK and the null backend, and runs the Python tool tests.
- **Mali and other GPUs:** not planned; they lack BC and would need BC → ASTC transcoding.

## Upstream sync

- Merge `upstream/main` often. Upstream is very active, and `nvn_mtl.m` changes most.
- A script lists the upstream commits since the last sync that touch `nvn_mtl.m`, `dmsh_build.py`, `dm/gfx/tex/` or the profiles. Each one is triaged ("mirror into `dm/gfx/vkn`?") in `docs/android/upstream-sync.md`.
- Changes worth offering upstream, with the maintainers' agreement:
  - the ELF section and assembly macros
  - the vendored SHA-256 (removes CommonCrypto)
  - the NVN-format pipeline key
  - the null backend
  - a Linux CI build of the tools

## Risks

| Risk | Mitigation |
|---|---|
| Placed memory missing (stock driver) or slow (uncached memory type) | Strategy B on stock. Use 842.6 (user-supplied, cannot be redistributed) or Turnip as their a8xx support matures |
| No thread-slot mechanism that works in a `.so` | Two-level load with a startup self-check (P0.2b), or the host as an executable (P0.2c) |
| Signals clobber stub scratch below `sp` | The signal guard (Phase 1). Open: a handler without SA_ONSTACK installed after a guest thread started, a foreign one on a signal the guard never blocks, and bionic's signal 40. Write-back stub forms (P0.3) close all three |
| `execmem` denied to apps | Map the modules from `nativeLibraryDir` (P0.1) |
| Shader compile stutter | Pipeline binaries, prewarm lists, graphics pipeline libraries |
| Upstream churn in `nvn_mtl.m` | The sync process above |
| Memory pressure (no iOS-style entitlement; Android's low-memory killer) | 16 GB on the target phone; measure resident memory per game, as upstream does |
| Legal | Same posture as upstream: bring your own files. Public fork: no game files, keys, rewritten modules or driver packages in the repo; built APKs are never shared |

## Open questions

1. None at the moment: the Phase 0 questions are answered in [probes.md](probes.md).

Sources for the driver facts: [Mesa `features.txt`](https://gitlab.freedesktop.org/mesa/mesa/-/raw/main/docs/features.txt), Turnip sources at `e9a83d530cce` (`src/freedreno/vulkan/`), [AdrenoToolsDrivers releases](https://github.com/K11MCH1/AdrenoToolsDrivers/releases), and the stock driver pulled from the phone.
