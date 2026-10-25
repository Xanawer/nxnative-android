// P0.1 executable memory.
// Core pass test (PLAN.md): mmap anon RW, copy a small PIC function in, __builtin___clear_cache, mprotect RX, call it.
// Also records, each guarded so one failure does not kill the run:
//   - direct PROT_READ|WRITE|EXEC mmap then run
//   - re-JIT: mprotect RX, run, mprotect RW, rewrite, mprotect RX, run again
//   - mmap PROT_EXEC of a file written into the app files dir (expected to fail at targetSdk 36 / W^X)
//   - mmap PROT_EXEC of the code blob shipped in nativeLibraryDir (libblob.so), dir found via dladdr
#include <dlfcn.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include "cpuprobe.h"

extern const unsigned char probe_code1_begin[], probe_code1_end[];
extern const unsigned char probe_code2_begin[], probe_code2_end[];

typedef int (*fn2_t)(int, int);

// A marker symbol inside libprobe.so; dladdr on it yields this library's path.
void p01_main_marker(void) {}

// A call guarded against a crash; returns 1 and sets *out on success, 0 on signal (*sig set).
static int guarded_call(fn2_t fn, int a, int b, int *out, int *sig) {
    *sig = 0;
    if (GUARD_TRY()) { *out = fn(a, b); GUARD_OK(); return 1; }
    *sig = g_guard_sig; return 0;
}

static long pagesz(void) { return sysconf(_SC_PAGESIZE); }

// locate this .so's directory (== nativeLibraryDir in the APK) via dladdr on our own symbol
static int self_dir(char *buf, size_t n) {
    Dl_info di;
    if (!dladdr((void *)p01_main_marker, &di) || !di.dli_fname) return 0;
    const char *slash = strrchr(di.dli_fname, '/');
    if (!slash) return 0;
    size_t len = (size_t)(slash - di.dli_fname);
    if (len >= n) len = n - 1;
    memcpy(buf, di.dli_fname, len); buf[len] = 0;
    return 1;
}

int p01_main(const char *out, int in_app, int argc, char **argv) {
    (void)argc; (void)argv;
    FILE *f = res_open(out, "p01_execmem.txt");
    if (!f) return 2;
    res_context(f, in_app);
    GUARD_INSTALL();

    long ps = pagesz();
    size_t c1 = (size_t)(probe_code1_end - probe_code1_begin);
    size_t c2 = (size_t)(probe_code2_end - probe_code2_begin);
    fprintf(f, "code1_bytes=%zu code2_bytes=%zu page=%ld\n", c1, c2, ps);

    int pass_core = 0;

    // --- 1. anon RW -> copy -> clear_cache -> mprotect RX -> call (the pass test) ---
    {
        void *m = mmap(NULL, (size_t)ps, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (m == MAP_FAILED) {
            fprintf(f, "anon_rw_rx: mmap RW failed errno=%d(%s)\n", errno, strerror(errno));
        } else {
            memcpy(m, probe_code1_begin, c1);
            __builtin___clear_cache((char *)m, (char *)m + c1);
            int rc = mprotect(m, (size_t)ps, PROT_READ | PROT_EXEC);
            if (rc != 0) {
                fprintf(f, "anon_rw_rx: mprotect RX failed errno=%d(%s)\n", errno, strerror(errno));
            } else {
                int r, sig;
                if (guarded_call((fn2_t)m, 100, 7, &r, &sig)) {
                    int ok = (r == 100 + 7 + 0x1234);
                    fprintf(f, "anon_rw_rx: called ret=0x%x expected=0x%x %s\n",
                            r, 100 + 7 + 0x1234, ok ? "PASS" : "WRONG");
                    pass_core = ok;
                } else {
                    fprintf(f, "anon_rw_rx: crashed sig=%s\n", signame(sig));
                }
            }
            munmap(m, (size_t)ps);
        }
    }

    // --- 2. direct PROT_READ|WRITE|EXEC mmap ---
    {
        errno = 0;
        void *m = mmap(NULL, (size_t)ps, PROT_READ | PROT_WRITE | PROT_EXEC,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (m == MAP_FAILED) {
            fprintf(f, "anon_rwx: mmap PROT_READ|WRITE|EXEC failed errno=%d(%s)\n", errno, strerror(errno));
        } else {
            memcpy(m, probe_code1_begin, c1);
            __builtin___clear_cache((char *)m, (char *)m + c1);
            int r, sig;
            if (guarded_call((fn2_t)m, 10, 20, &r, &sig))
                fprintf(f, "anon_rwx: ret=0x%x expected=0x%x %s\n", r, 30 + 0x1234,
                        r == 30 + 0x1234 ? "ok" : "WRONG");
            else
                fprintf(f, "anon_rwx: mapped ok but call crashed sig=%s\n", signame(sig));
            munmap(m, (size_t)ps);
        }
    }

    // --- 3. re-JIT: executed, then rewrite (RW), then RX and run again ---
    {
        void *m = mmap(NULL, (size_t)ps, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (m == MAP_FAILED) {
            fprintf(f, "rejit: mmap failed errno=%d(%s)\n", errno, strerror(errno));
        } else {
            memcpy(m, probe_code1_begin, c1);
            __builtin___clear_cache((char *)m, (char *)m + c1);
            mprotect(m, (size_t)ps, PROT_READ | PROT_EXEC);
            int r1, sig1, ok1 = guarded_call((fn2_t)m, 1, 2, &r1, &sig1);
            int rc_rw = mprotect(m, (size_t)ps, PROT_READ | PROT_WRITE);
            memcpy(m, probe_code2_begin, c2);
            __builtin___clear_cache((char *)m, (char *)m + c2);
            int rc_rx = mprotect(m, (size_t)ps, PROT_READ | PROT_EXEC);
            int r2, sig2, ok2 = guarded_call((fn2_t)m, 1, 2, &r2, &sig2);
            fprintf(f, "rejit: first %s ret=0x%x (exp 0x%x); mprotect RW rc=%d RX rc=%d; second %s ret=0x%x (exp 0x%x) %s\n",
                    ok1 ? "ran" : signame(sig1), r1, 3 + 0x1234,
                    rc_rw, rc_rx,
                    ok2 ? "ran" : signame(sig2), r2, 3 + 0x5678,
                    (ok1 && ok2 && r1 == 3 + 0x1234 && r2 == 3 + 0x5678) ? "PASS" : "see values");
            munmap(m, (size_t)ps);
        }
    }

    // --- 4. mmap PROT_EXEC of a file in the app files dir (W^X: expected to fail at targetSdk 36) ---
    {
        char path[700]; snprintf(path, sizeof path, "%s/code_in_files.bin", out);
        int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
        if (fd < 0) {
            fprintf(f, "file_exec(files): open failed errno=%d(%s)\n", errno, strerror(errno));
        } else {
            if (write(fd, probe_code1_begin, c1) != (ssize_t)c1)
                fprintf(f, "file_exec(files): short write\n");
            errno = 0;
            void *m = mmap(NULL, (size_t)ps, PROT_READ | PROT_EXEC, MAP_PRIVATE, fd, 0);
            if (m == MAP_FAILED) {
                fprintf(f, "file_exec(files): mmap PROT_EXEC FAILED errno=%d(%s) (expected at targetSdk 36)\n",
                        errno, strerror(errno));
            } else {
                int r, sig;
                if (guarded_call((fn2_t)m, 5, 6, &r, &sig))
                    fprintf(f, "file_exec(files): mmap PROT_EXEC SUCCEEDED ret=0x%x (W^X not enforced here)\n", r);
                else
                    fprintf(f, "file_exec(files): mmap PROT_EXEC mapped but call crashed sig=%s\n", signame(sig));
                munmap(m, (size_t)ps);
            }
            close(fd);
            unlink(path);
        }
    }

    // --- 5. mmap PROT_EXEC of libblob.so in nativeLibraryDir (raw code blob) ---
    {
        char dir[512];
        if (!self_dir(dir, sizeof dir)) {
            fprintf(f, "file_exec(nativelib): dladdr failed; skipping (likely the executable build)\n");
        } else {
            fprintf(f, "file_exec(nativelib): dir=%s\n", dir);
            char path[700]; snprintf(path, sizeof path, "%s/libblob.so", dir);
            int fd = open(path, O_RDONLY);
            if (fd < 0) {
                fprintf(f, "file_exec(nativelib): open %s failed errno=%d(%s)\n", path, errno, strerror(errno));
            } else {
                struct stat st; fstat(fd, &st);
                size_t len = (size_t)((st.st_size + ps - 1) & ~(ps - 1));
                errno = 0;
                void *m = mmap(NULL, len, PROT_READ | PROT_EXEC, MAP_PRIVATE, fd, 0);
                if (m == MAP_FAILED) {
                    fprintf(f, "file_exec(nativelib): mmap PROT_EXEC failed errno=%d(%s)\n", errno, strerror(errno));
                } else {
                    // libblob.so is the two functions concatenated: call the first.
                    int r, sig;
                    if (guarded_call((fn2_t)m, 40, 2, &r, &sig))
                        fprintf(f, "file_exec(nativelib): mmap PROT_EXEC SUCCEEDED ret=0x%x expected=0x%x %s\n",
                                r, 42 + 0x1234, r == 42 + 0x1234 ? "PASS" : "see value");
                    else
                        fprintf(f, "file_exec(nativelib): mapped but call crashed sig=%s\n", signame(sig));
                    munmap(m, len);
                }
                close(fd);
            }
        }
    }

    fprintf(f, "RESULT core(anon RW->RX->call)=%s\n", pass_core ? "PASS" : "FAIL");
    fclose(f);
    return pass_core ? 0 : 1;
}
