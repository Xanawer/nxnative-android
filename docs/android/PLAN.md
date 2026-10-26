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
| `dm/android/app/` | The APK. Phase 1: a NativeActivity with no Java code, built with `aapt2` / `zipalign` / `apksigner` like the probes (input through its `AInputQueue`, the window through `ANativeWindow`). Phase 2: `HostActivity`, a NativeActivity subclass that adds the software keyboard's input box (`javac` and `d8` in `build_app.sh`). Later: a Gradle project with the Kotlin launcher (from `launcher.json`), GameActivity and the IME, swapped in behind the same `front_android.c` |
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
- **Suspend can still hang:** `dm_gfx_suspend(1)` waits without a bound for the submits under way (`hle_nvn.c:1157`), before `mtl_drain`'s 4 s, and a submit can be blocked:
  - on a free slot (the completion thread gives a submission up only once the GPU is reported lost);
  - on `vkn_qmu`, while the present thread holds it in a `vkQueueWaitIdle` that has no timeout (`vk_present.c:98`; `QIDLE_NS` bounds only the retries after it returns).

  A GPU that hangs without reporting loss can so block `onStop`. No backend-only fix keeps pending work safe. (Opus review of slices 1–2b.)
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
- **Not covered yet:** a CPU-run GPU op (buffer copy, fill, uniform update) lands in guest memory at replay and is uploaded at commit. A draw earlier in the same submission therefore sees its result. A uniform update inside a submission is seen in order by a draw whose uniform buffer is unaligned (its ring copy is made at replay, §2.5), but not by an aligned one, so alignment changes what such a draw reads until updates are ordered. Stardew Valley makes none of these ops on this route. Texture storage is not tracked; textures are slice 2b.
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

**As built, slice 2b (2026-10-26):** sampled textures and samplers in `dm/gfx/vkn/nvn_vk.c`. The format table is `dm/gfx/vkn/nvn_fmt_vk.c`, not in `dm/gfx/tex`, because every host build takes that folder's sources by wildcard. Nothing draws yet (slice 2c), so bound textures and samplers are resolved and made ready at each draw and dispatch, but not used.
- **Formats:** only the RGBA8 and BGRA8 families (with their sRGB and no-alpha variants) are uploaded. Slice 1's render-target and depth formats are sampled once the GPU has rendered them. Any other format is reported in red once and gets no image.
- **Images:** one `VkImage` per texture serial, device-local with optimal tiling, made at first use with every usage the format supports. A render target sampled later is the same image, and its layout is tracked as in slice 1 (a texture bound while it is the pass's target is counted, not reported in red). Views are cached on the image, keyed by format, levels, layers and swizzle. Images are freed through the dead list.
- **Registrations:** the texture and sampler pools are arrays indexed by ID. A bound slot is resolved again only when its bind or a registration changed, so slice 2c can push descriptors from per-stage arrays.
- **Upload:** done in strategy B's commit, under the pool lock: take the stamp, pick textures used for the first time or written since their stamp, `nvn_dirty_protect_wait` their range, deswizzle on the CPU into the slot's staging after the buffer pages. In the upload command buffer: a barrier to transfer-destination, `vkCmdCopyBufferToImage` per level and layer, a barrier to shader-read. A CPU-written texture registered mid-run works the same way. `ReadTexels` reads guest memory; a texture only the GPU wrote is reported in red once.
  - **Each use checks the texture's pages against its stamp.** Metal skips the check when the dirty generation has not moved since the last one. That shortcut is unsound: `nvn_dirty.c` moves the generation before it stores the page's, so a check in between would cache "clean" for good. The check costs 1 to 3 µs a frame on the Mac and about 20 µs on the phone.
  - **A LINEAR texture in a pool the CPU can write** counts as CPU-written from the start, because Metal samples the pool memory itself. If the stride is 0, rows are at `nvn_tex_layout`'s pitch.
  - **A texture that nothing has written** is filled once from its storage when Metal's `tex_init_fill` would fill it: initial content of a CPU_NO_ACCESS pool, or storage flushed before first use. Otherwise it is cleared to zero.
- **Pages shared with another pool** (16 KB host pages over 4 KB-aligned pools) are never protected, and a texture that touches one is uploaded again at every use. A write beside a texture, in its own partial pages, is compared with the bytes kept at upload (as Metal's `pkg_edges`) and is not uploaded again when they are unchanged.
- **Strategy B's buffer pages** (review round 2): a commit's stamp could fall between an unprotect's generation increment and its store of the page's generation. The page was then neither copied nor protected again, and its later writes never reached the shadow. Now the commit, after its stamp, protects every protectable tracked page, so `nvn_dirty_protect_wait` waits out an unprotect under way. It then re-scans the pages it did not pick against the previous stamp, and copies the newer ones after their protect. The argument is in the comment above `b_commit`; `nvn_dirty.c` is unchanged.
  - **Cost:** pages uploaded and mprotect calls per frame are unchanged, and the re-scan picked nothing in the runs. On the phone (system driver) the commit took 0.287 ms a frame at frame 600 and 0.316 ms at 1800, against 0.264 and 0.308 ms before. On the Mac the copies dominate and the change is within run-to-run noise.
- **Portability subset** (MoltenVK): every feature the driver offers is enabled. Where `samplerMipLodBias` or `imageViewFormatSwizzle` is missing, the sampler gets no bias and the view an identity swizzle, each reported in red once.
- **Samplers:** deduplicated by parameters. They cover filters, wrap modes (mirror-clamp-to-edge where the device has it), LOD clamp and bias (bias clamped to the device limit), anisotropy (up to 16), compare, reduction and border. The border is a core colour when it matches one exactly; otherwise the nearest, with a red line once. A min filter without mips becomes nearest-mip with LOD 0 to 0.25.
- **Test:** `t_vkn` adds:
  - a swizzled 4-level RGBA8 round trip by readback;
  - an upload again only after a guest write;
  - the shared-page rule, `ReadTexels`, and the zero fill;
  - a render target sampled later: one image, with its layout;
  - every filter and wrap mode;
  - a staged dirty-check window;
  - linear textures, stride 0 included;
  - flushed and CPU_NO_ACCESS initial content;
  - an RGBX view and a biased sampler;
  - a buffer-page write whose unprotect is under way at the commit's stamp, held there by an interposed `mprotect`. It fails with the fix reverted.
  It runs 103 checks with validation on and 0 messages, on KosmicKrisp and on MoltenVK.
- **Results**, from the title screen to character creation: 0 `[RED]` and 0 validation messages on KosmicKrisp, the system driver and 842.6.
  - **Uploads:** 17 first uploads against Metal's 16. The extra one is ser=4, a 2x2 texture that is bound but that the program never samples. Metal resolves only the program's bindings, while 2b resolves every bound slot.
  - **Samplers:** 4 registered, of which two are identical, so 3 are made.
  - **Re-uploads:** none on the phone (4 KB pages). On the Mac, three textures (704x2256, 400x655, 288x672) sit in pages shared with another pool and are uploaded every frame, 7.8 MB a frame.

Open after slice 2b:
- **Mac shared-page cost:** protect the texture's interior pages, and at each use compare its bytes in the shared pages with a copy kept at upload.
- ~~**Slice 2c** resolves only the slots the program samples.~~ Done in slice 2c: 16 first uploads, as on Metal.
- **Metal's row pitch for a stride-0 LINEAR texture** is `width * bpb` (`nvn_mtl.m:1941`). `nvn_tex_layout` rounds the row up to 32 bytes (128 for a linear render target, `nvn_tex.c:108-111`), and that layout sizes the storage the game is given (`nvnTextureBuilderGetStorageSize`). The two differ when a row is not a multiple of 32 bytes. vkn follows `nvn_tex_layout`; Metal is unchanged.
- **No initial fill when a texture's first use is as a render target.** Metal fills at creation, whatever the first use (`nvn_mtl.m:2095`). vkn fills only at the first sampled use, so a pass that loads such a target first reads undefined content.
- **Not yet:** BC and the other formats, 3D, cube and buffer textures, and the GPU deswizzle.
- **Latent in Metal, left as is (Metal must stay exact):** the "generation has not moved" shortcut in the packaged-texture check (`nvn_mtl.m:2028`) and in the index-range cache (`nvn_mtl.m:4074`) can miss a write, for the reason given above.
- **Shared front half, not changed here:**
  - A whole-level `WriteTexels` on a LINEAR texture writes nothing and prints no `[RED]`: the whole-level path skips the deswizzle, and the `-1` from `nvn_swizzle` is ignored (`hle_nvn.c:2377`). It still calls `mtl_tex_cpu_written`. A partial region does print `[RED]`.
  - `ft_add` (`hle_nvn.c:334`, under `tex_dirty_upload`) reads its stamp and then calls `nvn_dirty_protect`, which skips a page another thread is unprotecting. A write fault that has taken its generation but not yet stored it then leaves the page writable with nothing newer than the stamp, and the texture's later writes are never seen (staged in a test by the Opus review of slices 1–2b). `nvn_dirty_protect_wait` would close it. Four profiles turn `tex_dirty_upload` on (010038B015560000, 0100B04011742000, 0100F2C0115B6000, 010036E00FB20000); Stardew does not.
  - LINEAR_RENDER_TARGET (0x40) without LINEAR (0x10) is linear for Metal (`nvn_mtl.m:1935-1937`, from Mario Wonder's sky ramps) but block-linear for `nvn_tex_layout` (`nvn_tex.c:108`), which vkn follows. Which one the hardware does is not settled.

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

**As built, slice 2c (2026-10-26):** the draws, in `dm/gfx/vkn/nvn_vk.c` (section "Draws") and `dm/gfx/vkn/vk_shader.c`. The shaders are slice 0's modules (§2.4), read from `DM_VK_SHADERS`, a development-time directory of decompressed `.spv` files beside their `.json` (packaging is Phase 3's). Only `<key>_b.spv` is read: a module whose `.spv.zst` alone is there is refused, and the refusal says so.
- **Descriptors: one push-descriptor set per draw** (`VK_KHR_push_descriptor`, with a limit of 32 on KosmicKrisp, stock and 842.6). Vertex bindings keep their numbers and pixel bindings get 32 more. `vk_shader.c` patches only the modules' `DescriptorSet` and `Binding` decorations. There is one pipeline layout per (vertex, pixel) pair. Nothing is allocated or freed per draw, the descriptors live in the command buffer, and the extension is on every target; descriptor buffers are on 842.6 only, and per-frame pools need lifetime tracking. A pair over the device's push limit is said in red and its draws are skipped. Stardew Valley writes 2 descriptors a draw.
- **Resolution,** as in `encode_args_`: only the slots the program's shaders sample (16 first uploads now, as on Metal). An unbound or unusable slot gets a dummy, counted. A view's numeric class is its aspect's and format's, so a stencil view is unsigned integers. A binding the shader reads as integers, a dummy included, gets its sampler's nearest twin (nearest filters and mip, no anisotropy, comparison or reduction, the integer border colour), since Vulkan filters integer formats only with nearest. The shader's c1 bank is a buffer of the reflection's bytes. Modules that read driver banks (`drv_reads`) or bindless handles are refused. Bindless reads appear as the binding and sampler kinds `tex_ubo`, `tex_c2a`, `samp_ubo` and `samp_c2a`, which fall into the unknown-kind refusals.
- **Uniform buffers** come from strategy B's shadows.
  - **Alignment:** `minUniformBufferOffsetAlignment` is 64 on KosmicKrisp and 256 on stock and 842.6. An offset the device cannot bind is copied at replay into a 1 MiB ring per submission slot, and past the ring the draw is skipped in red. Stardew Valley binds every uniform buffer 256-aligned, so there are 0 copies on all three targets; a temporary 256-byte check on the Mac agreed, with 0 of 40,000 binds.
  - **Range:** the bound size, cut to the pool's end and to 64 KiB (`maxUniformBufferRange` on every target), counted as cut short.
  - **Declared size:** the modules declare 64 KiB blocks but are bound with the game's size (64 bytes in Stardew Valley).
- **Modules** are loaded at a program's first draw. A module is refused (said in red once, its draws skipped) when:
  - its status is not ok, or its reflection is of another key, stage or chain (the reflection's `chain` against the host's; the directory's stamp, `inputs.tsv`, names the game and the profile inputs, not the chain);
  - its size is not the reflection's `spv_bytes`, the first 64 bits of its SHA-256 are not `spv_sha256` (with the vendored `dm/host/plat/sha256.h`), or its SPIR-V has other than one entry point, `main`, whose execution model is the reflection's stage;
  - it has decoration groups, a binding kind not handled yet, a descriptor array (found in the SPIR-V's types, whatever the reflection's count says; a null count is refused too), a slot or another number that does not fit its field (never cut to it), or a capability the device does not offer;
  - an interface or image type is other than the builder's 32-bit `float`, `uint` and `sint` (16- and 64-bit types such as `float16` are a later slice), or an interface array the reflection does not describe (its `array` first location and 1–4 lengths).

  `shaderImageGatherExtended` and `shaderDrawParameters` are enabled where offered: all 65 Stardew Valley modules declare ImageGatherExtended, and the 42 vertex ones DrawParameters. `sampleRateShading` is enabled where offered, for InterpolationFunction, which no Stardew module declares.
- **Pairs:** a (vertex, pixel) pair is matched when its layout is made. Vulkan matches variables, not locations, so, as the builder's `pair_inputs` does, every pixel input variable must be written by the vertex shader as the same variable: at each of its locations the same first location, component type, components and array lengths (a `vec4[2]` is not two `vec4`s). More components out than in would need `maintenance4`, which is not enabled, and slice 0's chain pads without widening. Component decorations cannot reach the match: the builder marks them unsupported, and non-ok modules are refused. A pair that does not match is said in red and its draws are skipped; no Stardew Valley pair is refused (none has a signed or array interface).
- **Pipelines** are monolithic. They are keyed by vkn's own key in `nvn_vk.c` (modules, attachment formats, vertex layout, blend, masks, depth-stencil, raster state, topology, depth mode) and kept in a hash map with an in-memory `VkPipelineCache`; `nvn_pso_skey.h` is unchanged.
  - Vertex formats are checked against the inputs' component types, and colour formats against the outputs'.
  - Alpha to coverage is on only where the pixel shader writes all four components of colour 0 and has no output of Index 1 (the reflection does not give an output's Index, so the loader notes one in the SPIR-V); otherwise it is off, said in red. Such a shader is not refused, as its draws would then be skipped; a location that only an Index 1 output covers is not written either (the reflection lists it as colour written, but with no Index 0 output Vulkan leaves that target undefined, so the loader clears it from the write mask and the target keeps its content). No Stardew Valley module has an Index decoration.
  - NVN's near = −w uses `VK_EXT_depth_clip_control`; `SetDepthMode` near = 0 does not.
  - Dynamic state: viewport, scissor, blend constants, stencil masks and reference, depth bias.
- **Y and winding:** the viewport is flipped for an upper-left origin, and NEGATIVE_Y toggles the flip. The front face depends on the origin only, because NVN decides facing after the swizzle, as `nvn_mtl.m:7177` does.
- **Draws:** indexed (16- and 32-bit indices, base vertex), non-indexed and instanced. Indices that run past their pool's end, and the vertices of a non-indexed draw or a per-instance stream that do, are said in red once and the draw is skipped. Per-vertex streams of indexed draws are not checked: the bound needs the largest index, which the GPU reads from what the commit uploads. Indirect and multi-draw, base instance, 8-bit indices, points, quads, adjacency and patches, geometry and tessellation, and rasterizer discard are said in red once and skipped.
- **Barriers:** `OP_BARRIER` becomes a memory barrier. A render target sampled later in the same submission gets an image barrier. Sync validation is clean on `t_vkn` and on a 60 s Stardew Valley run, and it reports a hazard when that barrier is weakened.
- **Strategy B fix (a slice 2a/2b fix landing with 2c):** a pool that a commit reads before `hle_nvn.c` adds its write tracking (`nvn_dirty_span`) was copied unprotected, and a write made in that gap never reached the shadow. Now, when the range appears, every buffer page of the pool is treated as read for the first time: picked, protected, copied. Every acquisition of the range goes through one helper (`pool_span`): `b_commit`'s buffer pass, or a texture's upload in the same commit when the range appears between the two.
- **Test:** `t_vkn` adds 82 checks (185 in all), with validation on and 0 messages:
  - a 24-case orientation and winding matrix, on a triangle with no mirror image of itself about x = y, plus a viewport off the target's origin and short of its height, and a partial scissor, each with both origins and NEGATIVE_Y;
  - a render target sampled in a second pass, in the same submission and in the next, with both origins;
  - a `DM_NVN_SHOT` of a drawn corner;
  - descriptor checks: a CPU-written texture, aligned, unaligned and pool-end uniform buffers, dummies, and an unfed input;
  - near = −w and near = 0 depth;
  - pipeline-cache hit and miss;
  - a refused module and a push-limit skip;
  - the span gap, also when a texture's upload is the first to find the range, which fails with the fix reverted;
  - from the review: a stencil view sampled as unsigned integers; a linear sampler on integer bindings; indices and non-indexed vertices past their pool, skipped; InterpolationFunction only with `sampleRateShading`; alpha to coverage with no colour 0; a pair whose pixel shader reads a location the vertex shader does not write; a vertex module under a pixel reflection; a module whose bytes are not its reflection's; slots past their field or not integers; sampler arrays with count 1 or null; a reflection cut inside a string; line-mode polygons filled with the line offset; signed integers through an unfed vertex input, a flat varying and an `isampler2D`; pairs matched by variable (a `vec4[2]` against a `vec4[2]`, two `vec4`s and one); malformed interface arrays; alpha to coverage with an Index 1 output, and that output's location left unwritten. Each fails with its fix reverted; the cut reflection crashes the old code at -O0 only, as the test's -O1 drops the stray read.
  The test kernels are `dm/gfx/tests/vkn_kernels/` (GLSL, `t_vkn_spv.h`).
- **Results,** from the title screen to character creation:
  - **Clean runs:** 0 `[RED]` and 0 validation messages on KosmicKrisp, the system driver and 842.6. Every draw is indexed. 2 pipelines, the first made at frame 0 in 11–19 ms; one cold Mac run took 648 ms.
  - **Record time:** 0.3–0.6 ms a frame on the Mac. On the phone 0.6 ms without validation and 2.3 ms with it.
  - **Frames:** the Mac frames are within the Metal runs' own spread at all six shots, and the phone frames match them by eye.

Open after slice 2c:
- **Reads past a uniform buffer's bound range** are undefined in Vulkan, while Maxwell returns zeros. `robustBufferAccess2` would match it.
- **Not drawn yet:**
  - driver-bank and bindless-handle reads;
  - integer vertex formats converted on the CPU;
  - cube textures;
  - a copy for a texture sampled while it is the pass's target (it reads zeros now);
  - vertex divisors > 1;
  - logic op, alpha test and dual-source blending;
  - the draw kinds listed above;
  - other viewport swizzles than NEGATIVE_Y;
  - a view swizzle applied in the shader where the portability subset has no `imageViewFormatSwizzle`;
  - more than the push limit (a second set or pools).
- **Facing under NEGATIVE_Y is unverified on hardware.** The code assumes the swizzle comes before facing, as in `NV_viewport_swizzle`. The `t_vkn` matrix encodes the same model, so it cannot catch an error they share. The only game seen with NEGATIVE_Y so far is Absolum, which is 2D and was run on Metal (`hle_nvn.c:2296`).
- **Pipelines:** an on-disk `VkPipelineCache` per driver identity, and a prewarm (§2.4). KosmicKrisp has no `fillModeNonSolid`, so line-mode polygons are filled there, with the line mode's polygon offset enable.
- **Lifetimes:** pipelines, layouts, modules and program entries (one per `SetShaders` serial) are kept for the run, because `mtl_prog_release` is a no-op.
- **Detclock timeline:** under `DM_DETCLOCK_PACE`, background loading still runs on wall time, so the title pan is not at the same place at the same frame number from run to run.
  - Under load, vkn runs fall outside the Metal spread at f900–f1400 and inside it at f300, f600 and f1700. A 648 ms cold pipeline did it too.
  - Checked in one such run (f1200): the picture sits 32 px further along the pan than Metal's. With that offset removed, the sky matches Metal's to 3 levels, with the same colour steps.
  - So at those frames the pixel count measures timing, not rendering. A check there should align the pan first, or the shots should wait for the hle_os.c detclock fix.

**Gameplay (2026-10-26):** the full route of the GPU measurement (title, character creation, the new-game load, the farmhouse, walking on the farm; about 13,500 frames with the key list of `stardew-surface.md`), on the slice 2c build.
- **Clean runs:** 0 `[RED]`, 0 validation messages and no skipped draw (1.08 million on the phone, 1.18 million on the Mac) on KosmicKrisp, the system driver and 842.6, on a Galaxy Z Fold 7. 3 pipelines; the farm frames match the Metal reference's by eye.
- **Frame time on the phone, farm, validation off (system driver):** p50 16.66 ms, p95 17.3 ms, max 20.6 ms; NVN submit and present 2.5 ms a frame; 127 draws a frame. With validation on, p95 is about 22 ms.
- **Strategy B on the farm:** 39.9 pages a frame (160 KiB), as measured beforehand (40), at 0.29–0.38 ms of commit time on the phone. The hot set of `stardew-surface.md` is not needed for this game: the projected saving is under 0.3 ms a frame, and 60 fps holds without it.
- **Edge-page textures on the Mac:** with 16 KB host pages, packaged textures that share a page with a neighbouring pool are never protected and are uploaded at every use: on the farm 4 a frame, 9.3 MB (8 such textures logged; the log names only the first 8). The phone, with 4 KB pages, has none. Mac-only, a development target: left as it is.
- **Software keyboard:** the Android front end had no input box, so every keyboard (character creation's three) was cancelled at once, and a player could not name a character; the route ran only with `DM_KBD`. `HostActivity` (`dm/android/app/java`), a NativeActivity subclass, now shows an `AlertDialog` whose field the system keyboard types into, with OK disabled below the game's minimum length. `front_text_input` asks for it from a host-owned thread (JNI never runs on a guest stack) and waits for the answer. Checked on the phone: the box comes up at character creation, text typed into it reaches the game.

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
  - The stages shared with `build_ipa_from_nsp.py` move into a common module: scan, NSZ, profile and survey, `nx_extract`, rewrite, shader split. *As built: no common module; the Android script imports them from the iOS script by name (below).*
  - `check_prereqs` (`:773-782`) checks per target.
  - The `clonefile` path (`:484-490`, `nx_extract/Program.cs:26,178`) gets a copy fallback.
  - Stages that boot the Mac host stay Mac-only at first: `capture_glslc :986`, `decode_ue_shaders :1030`, `capture_shaders :1098` and `heap_check :1221`.
- **APK contents** (`dm/android/app`, Gradle; *as built: no Gradle, below*):
  - `libdmhost.so`
  - the rewritten modules (in `nativeLibraryDir` if P0.1 requires it; *as built: in the APK's assets, below*)
  - the SPIR-V libraries, profile tables, fonts and `launcher.json`
  - libadrenotools
  - The app ID is `org.nxnative.t<title ID in lowercase>` (`org.nxnative.t0100e65002bb8000` for Stardew Valley), with `--app-id` to override it. It differs from iOS's `org.nxnative.<title ID>` (`:1717`) because Android requires every part of a package name to start with a letter, and a title ID starts with a digit, so `aapt2` and the installer reject the iOS form.
- **Game data:** the RomFS and DLC (up to about 17 GB) stay out of the APK. The app copies them from a folder the user picks into app-specific storage, or they are pushed with adb.
- **Signing and install:** a local keystore made once, `apksigner`, then `adb install`.
- **Driver packages:** the user puts a package in the app's folder; it is never in the repo or the APK.

**As built (2026-10-26):** `python3 -I scripts/build_apk_from_nsp.py <NSP folder> -k <prod.keys>` writes `<name>.apk` and `data/` to `--out` (default `<NSP folder>/<title ID>_android/`); the header of the script lists the options. `.gitignore` covers `*_android/`, `*_build/` and `*.apk` besides.
- **Folders it writes in.** The script replaces and deletes folders inside `--out` and `--work`, so each must be new or empty, or carry the mark it leaves there (`.nxnative-apk`: this script, output or work, title ID). Any other folder is refused before anything is written: a folder holding files, another title's folder, or the output folder given as the work folder. The two folders may not be one inside the other, compared before either is made: the existing part of each path by file identity, the names below it without regard to case or Unicode normalization (as macOS's default volume compares them; on a case-sensitive volume this only refuses more). Otherwise `--out X --work X/data` would pass, and the data folder's swap would replace the work folder. The default work folder, the iOS script's `<title ID>_build/`, is taken as it is and marked. Refused besides, before anything runs:
  - a folder inside this repository, compared by file identity (`samefile` on the folder's nearest existing ancestor and each one above it, as written and with links resolved), so a path in another case, one through `/System/Volumes/Data` or one through a link is caught;
  - a folder inside any git work tree (`git rev-parse --show-toplevel`: another clone or worktree);
  - a folder that is a link or holds links in its top three levels;
  - the same checks on `<NSP folder>/<title ID>_build/` when the folder holds NSZ files. The NSZ step is the iOS script's own function, and it writes `nsp/` there whatever `--work` says: the converted NSPs, its temporary `nsp/partial/`, and a conversion made again when its source changed. The folder is named by the title ID, not the profile's ID, and a test checks the name against what the function writes. The two scripts share this `nsp/`, so the Android script takes the folder as it is, without the marker, as it takes the default work folder; what the step deletes there is its own.
- **Stages:** game profile → `nx_extract` → `patch_nso.py --target android` (`work/android/patched_<version>/`) → shader split → `dmsh_build_vk.py` through `own_compilers_vk.py` (`work/android/vkcache/b-<V>-vk/`) → the package → icon and label → `build_app.sh --gpu vkn` → the data folder. Each stage is reused when its inputs are unchanged (a second run takes seconds). The default work folder is the iOS script's (`<title ID>_build/`, with the Android stages under `android/`), so the two share its layout; whether one script's extraction and shader split pass the other's reuse checks is not tested.
- **Shared stages are imported, not moved.** The script loads `build_ipa_from_nsp.py` and takes the shared functions by name (`TAKE`): scan, NSZ, `nx_extract`, extraction, DLC, NRO collection, chain header, shader split, fingerprints, progress. Before anything runs, it checks that each name exists and that its leading parameters are unchanged, and stops in red if not. This replaces "move into a common module": the iOS script is not changed, so upstream fixes to it need no merge, and a rename is caught at once. `dm/tools/tests/test_nsp_apk.py` runs the same check on the repository's iOS script.
- **No Gradle.** `build_app.sh` keeps the probes' toolchain (`aapt2`, `javac` + `d8`, `zip`, `zipalign`, `apksigner`) and gains `--manifest`, `--res`, `--assets`, `--out`, `--version-code` and `--version-name` (with `--replace-version`). The packager writes the game's manifest from `dm/android/app/AndroidManifest.xml`, setting the package, the label and the icon; the template's `configChanges` lists every change minSdk 35 knows, `grammaticalGender` (API 34) among them, since a change the activity does not take recreates it and ends the process; `HostActivity` keeps its class name under every package, so a game's activity is `<app ID>/org.nxnative.host.HostActivity`. The Kotlin launcher and GameActivity remain later work.
- **Modules, shaders and font go in `assets/nx/`, not `nativeLibraryDir`.** P0.1 showed that the loader reads a module, maps it anonymously and makes it executable with `mprotect`, so it never needs a library path. `nativeLibraryDir` extracts only `lib*.so` names, and storing modules there uncompressed would make the APK as large as the modules (216 MB for Stardew Valley; deflated, the whole APK is 68 MB, 8 MB of it the validation layer). On the first start of a new payload, the host thread (not the UI thread) unpacks `assets/nx/` into `files/pkg.new/` in streaming mode. It checks each file's size, writes the `stamp` last, then swaps the folder in with renames; leftovers from an interrupted unpack are removed at the next start. The `stamp` covers the paths and contents (`index.txt` lists sizes and paths). The payload holds:
  - `mods/`: the rewritten modules and their tables;
  - `vksh/`: `<key>_b.spv` decompressed and checked against its reflection's size, hash and chain, plus `<key>_b.json`, `inputs.tsv` and `shader_map.tsv`;
  - `font/nintendo_ext.ttf`;
  - `env.txt`: `DM_LANG` and `--env-file` lines, read below the phone's own `files/env.txt`. The build reads `--env-file` by the phone's rule for `env.txt` (`front_android.c env_lines`). A line ends at `\n`. Spaces and tabs are cut at the start and end of a line, and before the first `=`, so `DM_LANG =fr` sets `DM_LANG` on both sides. A line starting with `#` is a comment, and a line without `=` or without a name is skipped. A name set twice keeps its first value, and the script names the repeats. On the phone, a line over 4095 bytes or with a NUL byte is ignored whole, with a `[RED]` line. Before, the fixed buffer cut such a line into pieces, so the tail of a long comment could set a variable. The build refuses such a file, and also one that is not UTF-8, so the APK never carries one. `test_nsp_apk.py` compiles `env_lines` from the source and runs the same lines through it and through the build's reader. The file's `DM_LANG` wins over the game's language; a different `-l` stops the build.

  The game profile is compiled in (CMake's `game_profile.h` from `game.tsv`). The only profile table the Vulkan host reads at run time is `shader_map.tsv` (`vk_shader.c`), which goes in `vksh/`; the other tables are build-time or Metal-only, so none ships.
- **Shaders by default.** When a payload is present, `front_android.c` sets `DM_VK_SHADERS=<files>/pkg/vksh`, `DM_FONT_DIR=<files>/pkg/font` and the module folder `<files>/pkg/mods`, without overriding a value already set. `vk_shader.c`'s code is unchanged; only its comment and the "no shader module directory" message now name the APK. The development APK (no `assets/nx`) behaves as before: `run_app.sh` pushes modules into `files/mods`, and `-` skips the modules or the RomFS for a game APK.
- **The packager checks the shader chain.** It refuses modules whose chain is not `VKN_CHAIN` (`vkn.h`), so it never builds an APK whose shaders the backend would refuse; the test checks that `VKN_CHAIN` equals `dmsh_build_vk.py --version`.
- **This Mac's `spirv-cross`.** `dmsh_build_vk.py` stops when a chain tool differs from `dmsh_chain.lock`, which would stop the build on any Mac but the maintainer's after a Homebrew upgrade. The packager runs it through `dm/gfx/tools/own_compilers_vk.py`, the Vulkan counterpart of `own_compilers.py`: `spirv-cross` may differ from the lock, with one note line naming both builds, while a missing `spirv-cross` or a different translator still stops it. Neither the chain version (`--version`, still `VKN_CHAIN`) nor the lock changes; the stage key takes `spirv-cross --revision`, so a new `spirv-cross` rebuilds the modules.
- **Only this run's shaders.** `dmsh_build_vk.py`'s early exits (the tool lock, no `spirv-val`, another game's output, no answer table) leave the last build's stamped output in place. The packager deletes the stamp (`inputs.tsv`) and the status list (`_b_build.json`) before the run and takes the output only when this run wrote both and the stamp names this game. The counts come from the status list, not from the summary line.
- **Label and icon.** The label is the game's name from its control data (`nacp.py`), or `--app-name`, escaped for `aapt2`. The icon source is, in order:
  1. the cover from `https://tinfoil.media/ti/<title ID>/512/512/`, else `/256/256/`, fetched with `curl` into its own empty folder (at most 4 MB, `--max-filesize`). It is accepted only as a whole square JPEG or PNG of at least 128 px and kept only as the PNG `sips` makes of it, which is checked in turn. "Whole" means it ends with its end marker (JPEG `FF D9`, PNG `IEND`): `sips` reads a file cut short without an error, filling the rest grey. The PNG is cached as `work/android/icon-cache/<title ID>/icon.png` (`--refresh-icon`, `--no-online-icon`); a refresh that fails keeps the cached one;
  2. else the extraction's `icon.jpg`, the packed version's first;
  3. else the system default icon, said in red.

  A source that fails the checks gives way to the next, and the script says why. The label keeps every character but control and format characters and line or paragraph separators (Unicode Cc, Cf, Zl, Zp), so the ideographic space (U+3000) and no-break space (U+00A0) of Japanese titles stay; `aapt2` keeps them, and runs of spaces, as written.

  The adaptive icon (`mipmap-anydpi-v26`, both `icon` and `roundIcon`) draws the whole cover inside the 72 dp safe zone of the 108 dp layer (a 16.67 % inset), over a background colour averaged from the cover's edge pixels. `sips` makes the PNG and the BMP the edge is read from, so the packager needs no Pillow. Covers and names go only into the APK and the work folder.
- **Game data** stays out of the APK. The script prints the adb commands: install, start once (which creates the external files folder), force-stop, then `adb push --sync data/romfs /sdcard/Android/data/<app ID>/files/`, where `romfs_locate` looks. The data folder is made again when a RomFS or DLC file's path, size or modification time changes, or the DLC extraction's own key (`work/aoc.key`), so a DLC NSP replaced by one of the same size is not taken for the old one. The in-app folder picker is not built. The APK's version code is the packed version's; installing an older version over a newer one needs `adb install -r -d`, which the script says.
- **Only registered profiles.** The Android host's CMake build reads `dm/games/<id>/game.tsv`, so a provisional or derived profile (the iOS script's, kept outside the repository) is refused, as are versions the profile does not register and non-NVN games. The stages that boot the Mac host (run-time `glslc` capture, Unreal shader decoding, Oodle) are wired as on iOS, on modules rewritten for the Mac, but no Phase 3 game has exercised them.
- **Environment:** `nx_extract` is an apphost executable. When `DOTNET_ROOT` is unset (a non-login shell does not read `~/.zprofile`), the script derives it from the `dotnet` on `PATH`.
- **Signing:** the SDK's debug keystore, as for the development APK (`APP_KEYSTORE` overrides it); the APK is debuggable, so `run-as` works.
- **Results (Stardew Valley, Galaxy Z Fold 7, system driver, validation on):**
  - One command from `~/Games/StardewValley` produced `StardewValley.apk` (68 MB, `org.nxnative.t0100e65002bb8000`, version 1.6.15 / 1310720) and `data/romfs` (3,554 files, 606 MB).
  - Install, one start, force-stop and `adb push --sync` (4.8 s) followed the printed commands. The first game start unpacked the package (147 files, 227 MB) in 0.48 s.
  - The keyed route ran from the title screen through character creation to walking on the farm: 0 `[RED]`, 0 validation messages, 1.10 million draws with none skipped, 3 pipelines; frame time p50 16.7 ms and p95 22.6 ms, as in the Phase 2 gameplay run with the development APK (16.7 and 21.8 ms).
  - The rewritten modules are byte-identical to the development set.
  - The launcher shows "Stardew Valley" with the cover icon.
  - After the review fixes (folder marks and refusals, this run's shaders only, `own_compilers_vk.py`, first-wins `env.txt`, icon checks, `grammaticalGender`), a rebuild from empty `--work` and `--out` folders took 1 min 20 s. The payload matched the earlier one file for file except `nintendo_ext.ttf`, which `make_ext_font.py` made again and whose `head` table carries the time it was made. Installed with the printed commands, the short route reached character creation at 60 s and the farmhouse at 85 s (frame 5100), with 0 `[RED]` and 0 validation messages. A second run into the same folders reused every stage; its APK differs from the first only in zip times and signature, with the same entries and CRCs. Given an existing non-empty `--out`, or a path into the repository in another case or through `/System/Volumes/Data`, the script stopped before writing anything. After round 2 it also stopped, with nothing written, for nested folders not yet made (`--out X --work X/data`, the reverse, the same folder twice, and a nesting spelled in another case) and for an `--env-file` with a 4,100-byte line. The rebuilt APK ran the short route to character creation with 0 `[RED]` and 0 validation messages. On the phone, an `env.txt` holding a 4,100-byte comment ending in `EVIL_TAIL=1` gave one `[RED]` line and set nothing from it, and `NX_TRIM_TEST =ok` set `NX_TRIM_TEST`.

Open after Phase 3:
- the standard shared font (not packaged; Stardew Valley does not ask for it) and `launcher.json` (no launcher yet);
- a copy fallback for APFS clones on other file systems (the data folder and payload use `cp -c` with a plain copy if cloning fails; `nx_extract`'s own `clonefile` path is unchanged);
- a release keystore and a non-debuggable build;
- the in-app data picker;
- driver packages still come in through `run_app.sh --driver`; the APK uses the system driver unless `files/env.txt` names another;
- the game name from `nacp.py` (`name()`, shared with `build_ipa_from_nsp.py`) is empty when it has a character `str.isprintable()` refuses, U+3000 and U+00A0 among them, so the APK build and the iOS script's IPA build stop on such a title and ask for `--app-name` (`build_ipa_from_nsp.py:1716`; its Mac target goes on without a name). The fix belongs in `nacp.py`, where both scripts would take it; it is not changed here.

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
