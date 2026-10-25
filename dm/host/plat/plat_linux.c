// Platform layer, Linux / Android side (plat.h): the Darwin services the host calls, on /proc, process_vm_readv and POSIX. Not built on Apple.
#include "plat.h"
#include <dirent.h>
#include <ftw.h>
#include <limits.h>
#include <linux/fs.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/xattr.h>

uint64_t mach_absolute_time(void) { return clock_gettime_nsec_np(CLOCK_MONOTONIC); }

kern_return_t vm_read_overwrite(mach_port_t task, vm_address_t addr, vm_size_t size, vm_address_t data, vm_size_t *out) {
    (void)task;
    struct iovec l = {(void *)data, size}, r = {(void *)addr, size};
    ssize_t n = process_vm_readv(getpid(), &l, 1, &r, 1, 0);
    if (n != (ssize_t)size) { if (out) *out = 0; return KERN_INVALID_ADDRESS; }   // a partial read (the range runs into an unmapped page) fails whole, as on Darwin
    if (out) *out = size;
    return KERN_SUCCESS;
}

// The fields of a /proc stat line after the command name (which may hold spaces and parentheses): fields 3 (state) onwards. 0 = read
static int proc_stat(const char *path, char *state, unsigned long long *f, int nf) {
    char b[1024]; int fd = open(path, O_RDONLY | O_CLOEXEC); if (fd < 0) return -1;
    ssize_t n = read(fd, b, sizeof b - 1); close(fd); if (n <= 0) return -1;
    b[n] = 0; char *p = strrchr(b, ')'); if (!p || p[1] != ' ') return -1;
    p += 2; *state = *p++;
    for (int i = 0; i < nf; i++) { char *e; f[i] = strtoull(p, &e, 10); if (e == p) return -1; p = e; }   // f[0] = field 4 (ppid) ...
    return 0;
}
kern_return_t thread_info(mach_port_t thread, int flavor, thread_info_t info, mach_msg_type_number_t *count) {
    if (flavor != THREAD_BASIC_INFO || !count || *count < (mach_msg_type_number_t)THREAD_BASIC_INFO_COUNT) return KERN_FAILURE;
    char path[64], st; unsigned long long f[12]; snprintf(path, sizeof path, "/proc/self/task/%u/stat", thread);
    if (proc_stat(path, &st, f, 12)) return KERN_FAILURE;
    long hz = sysconf(_SC_CLK_TCK); if (hz <= 0) hz = 100;
    thread_basic_info_data_t *bi = (thread_basic_info_data_t *)info; memset(bi, 0, sizeof *bi);
    unsigned long long ut = f[10], stt = f[11];   // fields 14 (utime) and 15 (stime), clock ticks
    bi->user_time.seconds = (int)(ut / hz); bi->user_time.microseconds = (int)(ut % hz * 1000000 / hz);
    bi->system_time.seconds = (int)(stt / hz); bi->system_time.microseconds = (int)(stt % hz * 1000000 / hz);
    bi->run_state = st == 'R' ? TH_STATE_RUNNING : TH_STATE_WAITING;
    *count = THREAD_BASIC_INFO_COUNT;
    return KERN_SUCCESS;
}
kern_return_t task_info(mach_port_t task, int flavor, task_info_t info, mach_msg_type_number_t *count) {
    (void)task;
    if (flavor != TASK_VM_INFO || !count || *count < (mach_msg_type_number_t)TASK_VM_INFO_COUNT) return KERN_FAILURE;
    char b[256]; int fd = open("/proc/self/statm", O_RDONLY | O_CLOEXEC); if (fd < 0) return KERN_FAILURE;
    ssize_t n = read(fd, b, sizeof b - 1); close(fd); if (n <= 0) return KERN_FAILURE;
    b[n] = 0; unsigned long long size = 0, res = 0; if (sscanf(b, "%llu %llu", &size, &res) != 2) return KERN_FAILURE;
    ((task_vm_info_data_t *)info)->phys_footprint = res * (unsigned long long)getpagesize();
    *count = TASK_VM_INFO_COUNT;
    return KERN_SUCCESS;
}
int thread_selfcounts(int type, void *buf, size_t nbytes) { (void)type; (void)buf; (void)nbytes; return -1; }

int dm_plat_setname(const char *name) {
    char b[16]; size_t n = strlen(name); if (n > 15) { n = 15; while (n && ((unsigned char)name[n] & 0xc0) == 0x80) n--; }   // not in the middle of a UTF-8 character
    memcpy(b, name, n); b[n] = 0;
    return (pthread_setname_np)(pthread_self(), b);
}

int dm_plat_fd_path(int fd, char *out, size_t n) {
    char l[64]; snprintf(l, sizeof l, "/proc/self/fd/%d", fd);
    ssize_t k = n ? readlink(l, out, n - 1) : -1; if (k <= 0 || out[0] != '/') { if (n) out[0] = 0; return -1; }   // not a path: sockets, pipes, anonymous inodes
    out[k] = 0; return 0;
}

// ---------- clonefile ----------
// The user namespace only (the guest names of case collisions); none where the file system has none. Any other failure fails the copy, as clonefile's would: a save swapped in
// without its guest names loses those files' names
static int copy_xattrs(const char *s, const char *d) {
    char names[4096]; ssize_t n = llistxattr(s, names, sizeof names);
    if (n < 0) return errno == ENOTSUP ? 0 : -1;
    for (ssize_t i = 0; i < n; i += (ssize_t)strlen(names + i) + 1) {
        if (strncmp(names + i, "user.", 5)) continue;
        char v[4096]; ssize_t k = lgetxattr(s, names + i, v, sizeof v);
        if (k < 0 || lsetxattr(d, names + i, v, (size_t)k, 0)) return -1;
    }
    return 0;
}
static int copy_file(const char *s, const char *d, mode_t mode, int *made) {
    int in = open(s, O_RDONLY | O_CLOEXEC); if (in < 0) return -1;
    int out = open(d, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600); if (out < 0) { int e = errno; close(in); errno = e; return -1; }   // writable until the end: its xattrs come after the bytes
    *made = 1;
    int r = 0;
    if (ioctl(out, FICLONE, in)) {   // no reflink on this file system (f2fs, ext4): copy the bytes
        static char buf[1 << 16]; static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER; pthread_mutex_lock(&mu);
        for (;;) {
            ssize_t k = read(in, buf, sizeof buf); if (k < 0 && errno == EINTR) continue;
            if (k <= 0) { r = (int)k; break; }
            for (ssize_t o = 0; o < k;) { ssize_t w = write(out, buf + o, (size_t)(k - o)); if (w < 0 && errno == EINTR) continue; if (w <= 0) { r = -1; break; } o += w; }
            if (r) break;
        }
        pthread_mutex_unlock(&mu);
    }
    int e = errno; close(in); if (close(out) && !r) { e = errno; r = -1; } errno = e;
    if (!r) r = copy_xattrs(s, d);   // before the final mode: setting a user xattr needs write permission, which a read-only source mode would take away
    if (!r) r = chmod(d, mode & 07777);   // the source's mode exactly, as clonefile keeps it (open's mode went through the umask)
    return r;
}
// *made: d was created here (and is this copy's to remove on failure), not found in place
static int clone_tree(const char *s, const char *d, int *made) {
    struct stat st; if (lstat(s, &st)) return -1;
    if (S_ISREG(st.st_mode)) return copy_file(s, d, st.st_mode, made);
    if (S_ISLNK(st.st_mode)) { char t[PATH_MAX]; ssize_t k = readlink(s, t, sizeof t - 1); if (k < 0) return -1; t[k] = 0; if (symlink(t, d)) return -1; *made = 1; return 0; }
    if (!S_ISDIR(st.st_mode)) { errno = ENOTSUP; return -1; }
    if (mkdir(d, 0700)) return -1;   // writable while it is filled (a read-only source directory would refuse its own contents); its mode is set once the contents are in
    *made = 1;
    DIR *dp = opendir(s); if (!dp) return -1;
    struct dirent *de; int r = 0;
    for (;;) {
        errno = 0; if (!(de = readdir(dp))) { if (errno) r = -1; break; }   // a failed directory read is a failed copy, not a shorter one
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        char a[PATH_MAX], b[PATH_MAX];
        if (snprintf(a, sizeof a, "%s/%s", s, de->d_name) >= (int)sizeof a || snprintf(b, sizeof b, "%s/%s", d, de->d_name) >= (int)sizeof b) { errno = ENAMETOOLONG; r = -1; break; }
        int sub = 0; if ((r = clone_tree(a, b, &sub))) break;
    }
    int e = errno; closedir(dp); errno = e;
    if (!r) r = copy_xattrs(s, d);
    if (!r) r = chmod(d, st.st_mode & 07777);
    return r;
}
// Removing a half-made copy: directories made writable first (one already given a read-only source mode would keep its entries), then every entry, going on past one that fails
static int rw_one(const char *p, const struct stat *st, int flag, struct FTW *f) { (void)st; (void)f; if (flag == FTW_D) chmod(p, 0700); return 0; }
static int rm_one(const char *p, const struct stat *st, int flag, struct FTW *f) { (void)st; (void)flag; (void)f; remove(p); return 0; }
int dm_plat_clonefile(const char *src, const char *dst, int flags) {
    (void)flags;
    struct stat st; if (!lstat(dst, &st)) { errno = EEXIST; return -1; }
    int made = 0, r = clone_tree(src, dst, &made);
    if (r && made) { int e = errno; nftw(dst, rw_one, 16, FTW_PHYS); nftw(dst, rm_one, 16, FTW_DEPTH | FTW_PHYS); errno = e; }   // all or nothing: a half-made copy would be taken for a good one; a dst another creator made in the meantime is not this copy's
    return r;
}

uint64_t dm_plat_proc_age_ns(void) {
    char st; unsigned long long f[19];   // field 22 = start time in clock ticks after boot
    if (proc_stat("/proc/self/stat", &st, f, 19)) return 0;
    long hz = sysconf(_SC_CLK_TCK); if (hz <= 0) return 0;
    uint64_t start = (uint64_t)(f[18] * 1000000000ull / (unsigned long long)hz), now = clock_gettime_nsec_np(CLOCK_BOOTTIME);
    return now > start ? now - start : 0;
}

int dm_plat_page_prot(uintptr_t a) {
    FILE *f = fopen("/proc/self/maps", "re"); if (!f) return -1;
    char line[512]; int prot = -1;
    while (fgets(line, sizeof line, f)) {
        unsigned long lo, hi; char p[5];
        if (sscanf(line, "%lx-%lx %4s", &lo, &hi, p) != 3 || a < lo || a >= hi) continue;
        prot = (p[0] == 'r' ? PROT_READ : 0) | (p[1] == 'w' ? PROT_WRITE : 0) | (p[2] == 'x' ? PROT_EXEC : 0);
        break;
    }
    fclose(f);
    return prot;
}
