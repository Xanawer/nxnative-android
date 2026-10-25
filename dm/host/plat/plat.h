// Platform layer (docs/android/PLAN.md 1.4): what the host needs beyond POSIX, so that one source builds for Darwin (Mac, iOS) and Linux (Android).
// On Apple this header defines only the Mach-O spellings of the section and assembly helpers below and changes nothing else: every Darwin include stays in its own file behind __APPLE__.
// On Linux the Darwin services the shared code calls keep their Darwin names here (mach_absolute_time, vm_read_overwrite, the pthread _np calls, GCD semaphores...), implemented on
// Linux interfaces (static inline below, the rest in plat_linux.c), so a call site needs no #ifdef; the services that do not map (Mach thread suspend / state, task power, vm_remap)
// are compiled out at their call sites. Also included from assembly (entry.S, nvn_entry.S): only the symbol macros are seen there.
#pragma once

// ---------- Object format: symbol names in assembly, linker-built tables ----------
// Mach-O prefixes C symbols with _, ELF does not; .private_extern is ELF's .hidden; adrp / add take sym@PAGE / sym@PAGEOFF on Mach-O and sym / :lo12:sym on ELF.
// FUNC / FUNC_END give an ELF function its type and size: bionic's dladdr names a symbol only for addresses inside st_size, so a crash dump names an assembly function only with them
#ifdef __APPLE__
#ifdef __ASSEMBLER__
#define SYM(s) _##s
#define PAGE(s) _##s@PAGE
#define PAGEOFF(s) _##s@PAGEOFF
#define HIDDEN .private_extern
#define FUNC(s)
#define FUNC_END(s)
#else
#define DM_ASM_S(s) "_" s                 // s: a string literal
#define DM_ASM_HIDDEN ".private_extern"
#define DM_ASM_PAGE(s) "_" s "@PAGE"
#define DM_ASM_PAGEOFF(s) "_" s "@PAGEOFF"
#define DM_ASM_TEXT ".text\n"
#define DM_ASM_TEXT_END ""
#define DM_ASM_FUNC(s) ""
#define DM_ASM_FUNC_END(s) ""
// A table the linker gathers from every object (HLE entries, forced take-overs, NVN implementations): a __DATA section, read back with getsectiondata (mach-o/getsect.h at the caller)
#define DM_SECT(name) section("__DATA," #name)
#define DM_SECT_DATA(seg, name, szp) getsectiondata(&_mh_execute_header, seg, #name, szp)
#endif
#else
#ifdef __ASSEMBLER__
#define SYM(s) s
#define PAGE(s) s
#define PAGEOFF(s) :lo12:s
#define HIDDEN .hidden
#define FUNC(s) .type s, %function
#define FUNC_END(s) .size s, . - s
#else
#define DM_ASM_S(s) s
#define DM_ASM_HIDDEN ".hidden"
#define DM_ASM_PAGE(s) s
#define DM_ASM_PAGEOFF(s) ":lo12:" s
#define DM_ASM_TEXT ".pushsection .text\n"
#define DM_ASM_TEXT_END ".popsection\n"
#define DM_ASM_FUNC(s) ".type " s ", %function\n"
#define DM_ASM_FUNC_END(s) ".size " s ", . - " s "\n"
// ELF: a section named like an identifier gets __start_ / __stop_ symbols from the linker. retain (SHF_GNU_RETAIN) keeps it under --gc-sections, where lld's default -z start-stop-gc
// no longer counts a __start_ reference as a use; the bounds are weak (an empty table reads as NULL / 0, as getsectiondata gives) and hidden (resolved inside this image)
#define DM_SECT(name) section(#name), retain
#define DM_SECT_DATA(seg, name, szp) ({ extern char __start_##name[] __attribute__((weak, visibility("hidden"))), __stop_##name[] __attribute__((weak, visibility("hidden"))); \
    *(szp) = (unsigned long)(__stop_##name - __start_##name); (void *)(*(szp) ? (uint8_t *)__start_##name : NULL); })
#endif
#endif

#if !defined(__APPLE__) && !defined(__ASSEMBLER__)
// Every system header a shim below stands beside comes first: a later #include of it is then a no-op and cannot meet the macros
#include <errno.h>
#include <fcntl.h>
#include <malloc.h>
#include <pthread.h>
#include <sched.h>
#include <semaphore.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/ucontext.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>
#include "sha256.h"

// TargetConditionals.h is Apple's: on Linux neither OS is the target. A site where neither the macOS nor the iOS branch is right for Android says so with __ANDROID__ itself
#ifndef TARGET_OS_OSX
#define TARGET_OS_OSX 0
#endif
#ifndef TARGET_OS_IPHONE
#define TARGET_OS_IPHONE 0
#endif

// ---------- Time: the host's time base is nanoseconds on CLOCK_MONOTONIC (Apple's is mach ticks; every caller already scales by mach_timebase_info, which is 1 / 1 here) ----------
typedef struct { uint32_t numer, denom; } mach_timebase_info_data_t;
static inline int mach_timebase_info(mach_timebase_info_data_t *t) { t->numer = t->denom = 1; return 0; }
uint64_t mach_absolute_time(void);   // plat_linux.c: out of line, nvn_entry.S calls it
static inline uint64_t clock_gettime_nsec_np(clockid_t c) { struct timespec ts; clock_gettime(c, &ts); return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec; }
static inline int mach_wait_until(uint64_t deadline) {   // absolute, on mach_absolute_time's clock
    struct timespec ts = {(time_t)(deadline / 1000000000ull), (long)(deadline % 1000000000ull)};
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL) == EINTR) {}
    return 0;
}
// Code written at run time (DM_HLECOUNT stubs, probes): Darwin makes new code visible through this call; Linux needs the explicit cache maintenance
static inline void sys_icache_invalidate(void *p, size_t n) { __builtin___clear_cache((char *)p, (char *)p + n); }

// ---------- CommonCrypto's SHA-256 (loader.c, hle_ro.c, hle_nvn.c) on the vendored one ----------
typedef dm_sha256_ctx CC_SHA256_CTX;
typedef uint32_t CC_LONG;
#define CC_SHA256_DIGEST_LENGTH 32
#define CC_SHA256_Init(c) dm_sha256_init(c)
#define CC_SHA256_Update(c, p, n) dm_sha256_update(c, p, n)
#define CC_SHA256_Final(md, c) dm_sha256_final(md, c)
#define CC_SHA256(p, n, md) dm_sha256(p, n, md)

// ---------- Mach ports and fault-safe reads ----------
typedef int kern_return_t;
typedef unsigned mach_port_t, mach_msg_type_number_t;
typedef uintptr_t vm_address_t;
typedef size_t vm_size_t;
enum { KERN_SUCCESS = 0, KERN_INVALID_ADDRESS = 1, KERN_FAILURE = 5 };
static inline mach_port_t mach_task_self(void) { return 0; }
static inline mach_port_t mach_thread_self(void) { return (mach_port_t)gettid(); }   // a thread "port" is its tid
static inline mach_port_t pthread_mach_thread_np(pthread_t t) { return (mach_port_t)pthread_gettid_np(t); }
static inline kern_return_t mach_port_deallocate(mach_port_t task, mach_port_t p) { (void)task; (void)p; return KERN_SUCCESS; }
// Read memory that may be unmapped without faulting (process_vm_readv on this process): all of it or nothing, as on Darwin; usable in a signal handler
kern_return_t vm_read_overwrite(mach_port_t task, vm_address_t addr, vm_size_t size, vm_address_t data, vm_size_t *out);
// Per-thread run state and CPU time (/proc/self/task/<tid>/stat): THREAD_BASIC_INFO only
typedef struct { int seconds, microseconds; } time_value_t;
typedef struct { time_value_t user_time, system_time; int cpu_usage, policy, run_state, flags, suspend_count, sleep_time; } thread_basic_info_data_t;
typedef int *thread_info_t;
enum { THREAD_BASIC_INFO = 3, THREAD_BASIC_INFO_COUNT = (int)(sizeof(thread_basic_info_data_t) / sizeof(int)), TH_STATE_RUNNING = 1, TH_STATE_WAITING = 3 };
kern_return_t thread_info(mach_port_t thread, int flavor, thread_info_t info, mach_msg_type_number_t *count);
// Resident memory for the memory readout (phys_footprint ≈ RSS from /proc/self/statm): TASK_VM_INFO only
typedef struct { uint64_t phys_footprint; } task_vm_info_data_t;
typedef int *task_info_t;
enum { TASK_VM_INFO = 22, TASK_VM_INFO_COUNT = (int)(sizeof(task_vm_info_data_t) / sizeof(int)) };
kern_return_t task_info(mach_port_t task, int flavor, task_info_t info, mach_msg_type_number_t *count);
int thread_selfcounts(int type, void *buf, size_t nbytes);   // Darwin private {instructions, cycles}: none on Linux (zeros, returns -1)

// ---------- Threads ----------
// Darwin's stack address is the top (highest address); pthread_attr_getstack gives the bottom
static inline void *pthread_get_stackaddr_np(pthread_t t) { pthread_attr_t a; void *lo = NULL; size_t n = 0; if (!pthread_getattr_np(t, &a)) { pthread_attr_getstack(&a, &lo, &n); pthread_attr_destroy(&a); } return (uint8_t *)lo + n; }
static inline size_t pthread_get_stacksize_np(pthread_t t) { pthread_attr_t a; void *lo = NULL; size_t n = 0; if (!pthread_getattr_np(t, &a)) { pthread_attr_getstack(&a, &lo, &n); pthread_attr_destroy(&a); } return n; }
// Darwin names only the calling thread; Linux takes the thread and refuses names over 15 bytes (ERANGE): truncated in plat_linux.c
int dm_plat_setname(const char *name);
#define pthread_setname_np(name) dm_plat_setname(name)
static inline int pthread_cond_timedwait_relative_np(pthread_cond_t *c, pthread_mutex_t *m, const struct timespec *rel) {   // on CLOCK_MONOTONIC, whatever clock c was made with
    uint64_t t = clock_gettime_nsec_np(CLOCK_MONOTONIC) + (uint64_t)rel->tv_sec * 1000000000ull + (uint64_t)rel->tv_nsec;
    struct timespec ts = {(time_t)(t / 1000000000ull), (long)(t % 1000000000ull)};
    return pthread_cond_clockwait(c, m, CLOCK_MONOTONIC, &ts);
}
// QoS classes have no Linux counterpart yet (setpriority / ADPF later, PLAN.md 1.4): the calls succeed and change nothing, a thread reports "unspecified"
typedef unsigned qos_class_t;
enum { QOS_CLASS_UNSPECIFIED = 0x00, QOS_CLASS_BACKGROUND = 0x09, QOS_CLASS_UTILITY = 0x11, QOS_CLASS_DEFAULT = 0x15, QOS_CLASS_USER_INITIATED = 0x19, QOS_CLASS_USER_INTERACTIVE = 0x21 };
static inline int pthread_set_qos_class_self_np(qos_class_t q, int rel) { (void)q; (void)rel; return 0; }
static inline int pthread_get_qos_class_np(pthread_t t, qos_class_t *q, int *rel) { (void)t; if (q) *q = QOS_CLASS_UNSPECIFIED; if (rel) *rel = 0; return 0; }
typedef void *pthread_override_t;
static inline pthread_override_t pthread_override_qos_class_start_np(pthread_t t, qos_class_t q, int rel) { (void)t; (void)q; (void)rel; return NULL; }
static inline int pthread_override_qos_class_end_np(pthread_override_t o) { (void)o; return 0; }

// ---------- GCD semaphores (hle_audio.c) on POSIX semaphores ----------
typedef sem_t *dispatch_semaphore_t;
typedef uint64_t dispatch_time_t;   // CLOCK_MONOTONIC nanoseconds
#define DISPATCH_TIME_NOW 0ull
#define DISPATCH_TIME_FOREVER (~0ull)
static inline dispatch_time_t dispatch_time(dispatch_time_t when, int64_t delta) { return (when == DISPATCH_TIME_NOW ? clock_gettime_nsec_np(CLOCK_MONOTONIC) : when) + (uint64_t)delta; }
static inline dispatch_semaphore_t dispatch_semaphore_create(long v) { sem_t *s = malloc(sizeof *s); if (!s || sem_init(s, 0, (unsigned)v)) abort(); return s; }
static inline long dispatch_semaphore_signal(dispatch_semaphore_t s) { sem_post(s); return 0; }
static inline long dispatch_semaphore_wait(dispatch_semaphore_t s, dispatch_time_t t) {   // 0 = taken, non-zero = timed out
    if (t == DISPATCH_TIME_FOREVER) { while (sem_wait(s) && errno == EINTR) {} return 0; }
    struct timespec ts = {(time_t)(t / 1000000000ull), (long)(t % 1000000000ull)}; int r;
    while ((r = sem_clockwait(s, CLOCK_MONOTONIC, &ts)) && errno == EINTR) {}
    return r ? -1 : 0;
}
static inline void dispatch_release(dispatch_semaphore_t s) { sem_destroy(s); free(s); }

// ---------- Files ----------
static inline int getdtablesize(void) { return (int)sysconf(_SC_OPEN_MAX); }
int dm_plat_fd_path(int fd, char *out, size_t n);   // fcntl(F_GETPATH): readlink of /proc/self/fd/N; 0 = found
// clonefile (APFS copy-on-write tree copy) → a recursive copy, FICLONE per file where the file system has it, the user extended attributes copied (the case-collision guest names,
// hle_fs.c); the destination must not exist, and a failed copy leaves none behind. renamex_np(RENAME_SWAP) → renameat2(RENAME_EXCHANGE)
int dm_plat_clonefile(const char *src, const char *dst, int flags);
#define clonefile(src, dst, flags) dm_plat_clonefile(src, dst, flags)
#define RENAME_SWAP RENAME_EXCHANGE
static inline int renamex_np(const char *from, const char *to, unsigned flags) { return renameat2(AT_FDCWD, from, AT_FDCWD, to, flags); }
uint64_t dm_plat_proc_age_ns(void);   // how long ago this process started (/proc/self/stat start time against CLOCK_BOOTTIME); 0 = unknown
int dm_plat_page_prot(uintptr_t a);   // the protection of the mapping holding a (/proc/self/maps, PROT_* bits); -1 = not mapped

// ---------- Malloc zones (hle_uemalloc.c): the process heap, sizes from malloc_usable_size ----------
typedef struct dm_malloc_zone malloc_zone_t;
static inline malloc_zone_t *malloc_create_zone(size_t start, unsigned flags) { (void)start; (void)flags; return (malloc_zone_t *)(uintptr_t)1; }
static inline void malloc_set_zone_name(malloc_zone_t *z, const char *n) { (void)z; (void)n; }
static inline void *malloc_zone_malloc(malloc_zone_t *z, size_t n) { (void)z; return malloc(n); }
static inline void *malloc_zone_memalign(malloc_zone_t *z, size_t a, size_t n) { (void)z; return memalign(a, n); }
static inline void *malloc_zone_realloc(malloc_zone_t *z, void *p, size_t n) { (void)z; return realloc(p, n); }
static inline void malloc_zone_free(malloc_zone_t *z, void *p) { (void)z; free(p); }
static inline size_t malloc_size(const void *p) { return malloc_usable_size(p); }
static inline size_t malloc_good_size(size_t n) { return (n + 15) & ~(size_t)15; }

// ---------- Signal context (Linux mcontext): x0-x30 in regs[], the FP / SIMD and fault syndrome records in __reserved ----------
#define DM_UC_PC(uc) ((uc)->uc_mcontext.pc)
#define DM_UC_SP(uc) ((uc)->uc_mcontext.sp)
#define DM_UC_FP(uc) ((uc)->uc_mcontext.regs[29])
#define DM_UC_LR(uc) ((uc)->uc_mcontext.regs[30])
#define DM_UC_X(uc, i) ((uc)->uc_mcontext.regs[i])
#define DM_UC_CPSR(uc) ((uc)->uc_mcontext.pstate)
static inline void *dm_uc_record(const ucontext_t *uc, uint32_t magic) {   // the kernel's record list ends at a zero magic; FP / SIMD and ESR come before any extra_context
    const uint8_t *p = uc->uc_mcontext.__reserved, *e = p + sizeof uc->uc_mcontext.__reserved;
    while (p + sizeof(struct _aarch64_ctx) <= e) {
        const struct _aarch64_ctx *h = (const struct _aarch64_ctx *)p;
        if (!h->magic || h->size < sizeof *h) return NULL;
        if (h->magic == magic) return (void *)(uintptr_t)p;
        p += h->size;
    }
    return NULL;
}
// ESR of the fault: with no record (it should always be there for a data abort) bit 6 (WnR) reads 1, since a tracked write-protected page only faults on a write
static inline uint64_t dm_uc_esr(const ucontext_t *uc) { const struct esr_context *r = dm_uc_record(uc, ESR_MAGIC); return r ? r->esr : 1u << 6; }
#define DM_UC_ESR(uc) dm_uc_esr(uc)
static inline struct fpsimd_context *dm_uc_fpsimd(const ucontext_t *uc) { return dm_uc_record(uc, FPSIMD_MAGIC); }
static const __uint128_t dm_uc_vzero[32] __attribute__((unused));   // what an absent FP / SIMD record reads as (read only: DM_UC_V is for reading)
#define DM_UC_V(uc) (dm_uc_fpsimd(uc) ? dm_uc_fpsimd(uc)->vregs : dm_uc_vzero)
#elif defined(__APPLE__) && !defined(__ASSEMBLER__)
// Darwin signal context, the same expressions the shared code always used (probe.c, crash.c)
#define DM_UC_PC(uc) ((uc)->uc_mcontext->__ss.__pc)
#define DM_UC_SP(uc) ((uc)->uc_mcontext->__ss.__sp)
#define DM_UC_FP(uc) ((uc)->uc_mcontext->__ss.__fp)
#define DM_UC_LR(uc) ((uc)->uc_mcontext->__ss.__lr)
#define DM_UC_X(uc, i) ((uc)->uc_mcontext->__ss.__x[i])
#define DM_UC_ESR(uc) ((uc)->uc_mcontext->__es.__esr)
#define DM_UC_V(uc) ((uc)->uc_mcontext->__ns.__v)
#endif
