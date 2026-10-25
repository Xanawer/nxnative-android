# Phase 0 CPU probes: results

Device: Nubia NX789J, Snapdragon 8 Elite (SM8750, Oryon), Android 16 / SDK 36, kernel 6.6.92, 4 KB pages, 16 GB. No MTE in `/proc/cpuinfo` (so heap "tagging" here is TBI address tagging via scudo, not MTE).

Each probe runs in two contexts: the NDK **executable** over `adb shell` (SELinux `u:r:shell:s0`, uid 2000) and inside a **NativeActivity APK** with no Java (`u:r:untrusted_app`, uid 10542). Package `org.nxnative.probe.cpu` (tagging off) and `org.nxnative.probe.cputag` (tagging on).

- Sources: `dm/android/probe/cpu/` (`main.c`, `p0{1..6}_*.c`, `blob_fn.S`, `scratch_loop.S`, `libieonly.c`, `libgd.c`, `cpuprobe.h`, `run_cpu.sh`).
- Build outputs: `~/dm-work/android/probe-build/cpu/`. Raw results: `~/dm-work/android/probe-results/cpu/<probe>/{exe,app}/`.
- Rebuild + run everything: `dm/android/probe/cpu/run_cpu.sh all`.

Summary: **P0.1 PASS, P0.2 PASS (mechanism (c)), P0.3 PASS, P0.4 PASS, P0.5 PASS, P0.6 PASS/logged.**

---

## P0.1 Executable memory — PASS (both contexts)

What was tested: `mmap` anon RW, copy a small PIC function (`blob_fn.S`) in, `__builtin___clear_cache`, `mprotect` RX, call; plus direct RWX `mmap`; re-JIT (RX→run→RW→rewrite→RX→run); `PROT_EXEC` `mmap` of a file in the app files dir; `PROT_EXEC` `mmap` of a code blob shipped as `libblob.so` in `nativeLibraryDir` (located via `dladdr`). Every attempt guarded with sigsetjmp.

Result (identical in exe and APK):
- anon RW → clear_cache → RX → call: **PASS** (ret 0x129f = correct).
- direct `PROT_READ|WRITE|EXEC` mmap: works.
- re-JIT (RX, run, RW, rewrite, RX, run): **PASS** (0x1237 then 0x567b).
- `PROT_EXEC` of a file in the app's own files dir: **SUCCEEDED** (ret correct). W^X is **not** enforced for this app/device — contrary to the PLAN's expectation that targetSdk 36 would block it. (SELinux `untrusted_app` was able to execute a file it wrote under its data dir; the APK is `debuggable=true`.)
- `PROT_EXEC` of `libblob.so` (a raw, non-ELF code blob shipped in `nativeLibraryDir`, extracted on install): **PASS** (ret correct).

Pass test (PLAN): a copied function with `__builtin___clear_cache` returns the expected value in the APK — **met**.

Design implication: the primary scheme (anon RW→RX) works in `untrusted_app`, so `execmem` is granted. The file-mapping fallback also works from **both** `nativeLibraryDir` **and** the app files dir, giving two independent fallbacks for shipping rewritten modules. Keep the `__builtin___clear_cache` call unconditionally (loader.c:518-556): it is the step that makes re-JIT safe, and a probe could pass without it by luck on fresh pages.

---

## P0.2 Thread slots — PASS: mechanism (c), the two-load `bionic_tls.key_data` chain

This is the deciding probe. The port needs a stub-shaped sequence reaching a per-thread 8-byte slot from any guest thread using `mrs xN, tpidr_el0` + at most two dependent constant-offset loads, no calls. Linux keeps `tpidrro_el0` at 0 for 64-bit tasks, so the port must use `tpidr_el0`.

### (a) initial-exec `__thread` at a fixed `tpidr_el0` offset
- **Executable:** `&var - tpidr_el0` is a constant (0x150 in this build), identical on 64 threads and on threads created by threads. So initial-exec TLS is a valid fixed-offset slot **in an executable**.
- **APK:** NOT usable. An initial-exec `__thread` symbol compiled into the dlopen'd `libprobe.so` makes bionic **refuse to load the whole library** at dlopen time: `dlopen failed: TLS symbol "(null)" ... using IE access model`. Observed directly (it crashed the first APK build). So candidate (a) is out for the APK / `libdmhost.so`.

### (b) dlopen'd library TLS models
- initial-exec in a dlopen'd `libieonly.so`: `dlopen` **FAILS** with the same IE-access-model error, in **both** exe and APK. bionic will not load a dlopen'd `.so` that references an initial-exec TLS symbol on this device.
- default (global-dynamic) in a dlopen'd `libgd.so`: loads fine, but `&var - tpidr_el0` is **not** a fixed constant (main thread differs from other threads; bionic resolves it through `__tls_get_addr` into a per-thread, separately allocated block). Not usable as a fixed-offset slot. (Expected.)

### (c) sentinel search for a `pthread_key` slot chain — **the recommendation**
No hard-coded bionic offsets. `pthread_key_create` + `pthread_setspecific(sentinel)`, then a blind scan for a chain `tp → [tp+A] → [+B]` whose final value equals the sentinel; each raw match is confirmed by writing a fresh value through `[*(tp+A)+B]` and reading it back with `pthread_getspecific` (restoring the exact original bytes, so the scan never corrupts state). Survivors are then filtered across 8 threads each with its own sentinel (this removes stale aliases that only matched the main thread's slot). The canonical chain is selected by matching `B` to the key's own index — `(B-8)/16 == (key & ~KEY_VALID_FLAG)` — which pins `A` to `TLS_SLOT_BIONIC_TLS` without hard-coding its value.

Result — a chain is found and fully verified in **both** contexts:

| | exe | APK |
|---|---|---|
| key (raw index) | 0x80000001 (1) | 0x80000011 (17) |
| chain | `*(*(tp − 8) + 24)` | `*(*(tp − 8) + 280)` |
| `(B-8)/16` vs key index | 1 == 1 MATCH | 17 == 17 MATCH |
| JIT reader reads slot | PASS | PASS |
| JIT writer writes slot (seen by `pthread_getspecific`) | PASS | PASS |
| 64 threads: read / write-through / nested-thread | all PASS | all PASS |
| inside a signal handler (8 threads) | all PASS | all PASS |

bionic structure confirmed: `tp − 8` = `TLS_SLOT_BIONIC_TLS` → `struct bionic_tls*`; its first member is `pthread_key_data_t key_data[]` (`{uintptr_t seq; void* data;}`, 16 bytes each); the slot is `key_data[index].data` at `bionic_tls + 16*index + 8`. The `(B-8)/16 == index` identity matching empirically confirms this layout. (`B = 24` for index 1, `B = 280` for index 17.)

**Recommended mechanism and exact stub sequence.** Use a host `pthread_key` as each of K18/KTLS/KTLR/KGATE; the stub reaches the slot with:

```
mrs  xN, tpidr_el0          ; d53bd040 | N
ldur xN, [xN, #-8]          ; f85f8000 (TLS_SLOT_BIONIC_TLS -> struct bionic_tls*)
ldr  xN, [xN, #B]           ; B = 16*key_index + 8   (read the slot VALUE)
```
and to write a slot:
```
mrs  xT, tpidr_el0
ldur xT, [xT, #-8]
str  xS, [xT, #B]           ; store xS into the slot
```
`B` is a per-key constant the loader fills in after `pthread_key_create` (replacing today's key-page indirection for `tpidrro_el0`). This is `mrs` + two dependent loads, no calls — exactly what the rewriter can emit. Verified words: exe reader `d53bd040 f85f8000 f9400c00 d65f03c0`; APK reader `d53bd040 f85f8000 f9408c00 d65f03c0`.

Startup self-check (upstream `tsd_check` analogue): `pthread_setspecific(key, 0x1234)`, read the stub chain `*(*(tpidr_el0-8) + 16*key_index + 8)`, and `die()` unless it equals 0x1234 — and, since the index comes from the live key, also assert `(B-8)/16 == (key & ~KEY_VALID_FLAG)`. The `& ~7` mask upstream uses on `tpidrro_el0` is dropped (Darwin-only); `tpidr_el0` is already aligned.

Note vs PLAN's candidate (b): the plan guessed a two-load chain `tpidr_el0 → bionic TLS slot → host block`. That is exactly what works, with the second hop being `bionic_tls.key_data[index].data`. Candidate (a) (initial-exec) works only in an executable; candidate (c) is the one that works in the APK, so it is the mechanism for `libdmhost.so`.

### (d) other viable slot
`TLS_SLOT_APP` at `tp + 16` is writable and reads back on this thread — a **single-load** slot (`mrs; ldr/str [xN,#16]`), reserved for apps since API 29. But it is a single 8-byte word shared with anything else using `TLS_SLOT_APP`, and the port needs four slots. Usable as one fast slot (e.g. a pointer to a host per-thread block holding all four) if the stubs can afford the extra indirection; otherwise prefer (c).

Pass test (PLAN): a stub-shaped sequence reads the right per-thread value on 64 threads, after `pthread_create`, and inside signal handlers, in the APK — **met**.

---

## P0.3 Scratch below sp (no red zone) — PASS (both contexts)

A loop thread runs on a 64 KB `mmap`'d stack (`scratch_loop.S`, sp fixed) writing 14 sentinels into `[sp-112, sp)` and checking them every iteration. Concurrently: a thread `tgkill`'s SIGUSR2 at a high rate (handler `SA_SIGINFO|SA_ONSTACK|SA_RESTART`), and a thread keeps `mprotect`'ing a page RO that the loop writes, the SIGSEGV handler unprotecting it (the dirty-tracker pattern). Variants: `alt` (loop thread has a sigaltstack), `noalt` (none), both run in exe and APK.

| run | iterations | corruptions | SIGUSR2 delivered | SIGSEGV write-faults | result |
|---|---|---|---|---|---|
| alt, exe | 4,781 | **0** | 565,966 | 36,523 | **PASS** |
| alt, APK | 1,438,624 | **0** | 739,586 | 98,922 | **PASS** |
| noalt, exe (control) | 68,507 | 16,318 | 334,619 | 2,618 | corruption detected as expected |
| noalt, APK (control) | 452,627 | 58,547 | 912,880 | 5,384 | corruption detected as expected |

With a sigaltstack, `[sp-112, sp)` stays intact under hundreds of thousands of signals and tens of thousands of write-faults — 0 corruptions in both contexts. In the APK the installed SIGSEGV/SIGUSR2 handlers kept `SA_ONSTACK` set even with ART's libsigchain in front. The `noalt` controls corrupt the scratch area, proving the probe actually detects corruption (so the 0 result is meaningful, not a blind pass).

Pass test (PLAN): on an SDK-sized stack, SIGUSR2 plus a dirty-tracker write-fault leave `[sp-112, sp)` intact in the APK — **met**, provided every guest thread installs a sigaltstack (as upstream `crash_thread_init` does). The stubs need no write-back forms.

---

## P0.4 Pointer tagging — PASS

Top byte of `malloc` (small/large) and `mmap` pointers.

| context | heap default | heap after `mallopt(…, NONE)` | mmap |
|---|---|---|---|
| exe (shell, no manifest) | tagged 0xb4 | untagged 0x00 | 0x00 |
| APK `.cpu` (`allowNativeHeapPointerTagging=false`) | untagged 0x00 | untagged 0x00 | 0x00 |
| APK `.cputag` (`=true`) | tagged 0xb4 | untagged 0x00 | 0x00 |

The tag is a constant top byte `0xb4` (TBI, no MTE on this device), from scudo. `mmap` pointers are never tagged. `mallopt(M_BIONIC_SET_HEAP_TAGGING_LEVEL, M_HEAP_TAGGING_LEVEL_NONE)` returns 1 and clears tags in every case (cannot be re-enabled afterward).

Design implication: the port gets zero-top-byte host pointers by (a) building the APK with `android:allowNativeHeapPointerTagging="false"` — already untagged from the start — and/or (b) calling `mallopt(…, NONE)` at startup (needed for the `adb shell` / `dmhost` executable, whose heap is tagged by default). The assert "every host pointer handed to the guest has a zero top byte" is satisfiable; `mmap` memory needs nothing.

---

## P0.5 x18 / tpidr_el0 — PASS (both contexts)

- `x18` is non-zero and distinct on every thread in both contexts → bionic's **shadow call stack** is live. The rewriter must keep virtualising x18 (it already does, via slot K18).
- `tpidr_el0 == __builtin_thread_pointer()` on every thread, and `&errno` sits at `tp − 128` (inside the thread's TLS) → `tpidr_el0` is bionic's thread pointer, confirmed.

Design implication: guest code must never clobber the real x18 or `tpidr_el0`; the existing rewrites cover this.

---

## P0.6 System registers — logged; cntfrq and ctr match the rewriter

Each read guarded against SIGILL.

| register | value | note |
|---|---|---|
| `cntvct_el0` | readable | virtual counter |
| `cntpct_el0` | **traps SIGILL** | physical counter not readable at EL0 |
| `cntfrq_el0` | 0x124f800 = **19.2 MHz** | **matches the Switch rate exactly** |
| `ctr_el0` | 0xb444c004 → DminLine=4 | **matches** the rewriter's `movz #4,lsl#16` = 0x40000 (64-byte line); IminLine=4 |
| `dczid_el0` | 4 | DC ZVA block = 64 bytes, DZP=0 |
| `midr_el1` | 0x514f0014 | Qualcomm (implementer 0x51), Oryon; Linux emulates the ID read |

`cntvct` is monotonic across all 8 cores (pinned), and its measured rate over 100 ms is 19.2 MHz, 0% error vs `cntfrq`.

Design implications:
- `cntpct_el0` traps at EL0, so the rewriter must keep routing `cntpct` through the host gate (it does).
- `cntfrq` is already 19.2 MHz — the same as Horizon — so the virtual counter needs no rescaling; a fast path reading `cntvct_el0` directly (instead of gating) is viable later, since EL0 can read it and it is monotonic across cores.
- `ctr_el0` DminLine already equals the rewriter's baked constant (4), so the cache-line assumptions hold on this core.

---

## How to re-run the deciding probes

```
cd dm/android/probe/cpu

# P0.2 (thread slots) — exe and APK:
./run_cpu.sh p02
cat ~/dm-work/android/probe-results/cpu/p02/exe/p02_slots.txt
cat ~/dm-work/android/probe-results/cpu/p02/app/p02_slots.txt

# P0.3 (scratch below sp) — run each variant; exe and APK each time:
./run_cpu.sh p03 alt 2
./run_cpu.sh p03 noalt 2
cat ~/dm-work/android/probe-results/cpu/p03/{exe,app}/p03_scratch_alt.txt
cat ~/dm-work/android/probe-results/cpu/p03/{exe,app}/p03_scratch_noalt.txt
```
(`run_cpu.sh build` first if not built. `run_cpu.sh all` rebuilds and runs every probe.)
