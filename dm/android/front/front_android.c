// Android app front end (front.h; docs/android/PLAN.md 1.6, 1.7): libdmhost.so in a NativeActivity (dm/android/app; its one Java class, HostActivity, adds the software keyboard's input box).
// It does what the iOS app's main.m and front_ios.m do:
// the environment, the directories and the log are set before the host starts, dm_main runs on a big-stack thread, the activity's lifecycle drives focus, audio, the guest clock and graphics,
// and the activity's input queue feeds player 1's controller path (dm_pad_set) and the keyboard path (dm_input_set). Every callback here runs on the activity's main (UI) thread; the host calls
// it makes are lock-free (hle_hid.c) or take short host locks (hle_oe.c, hle_audio.c, hle_os.c, the input box's), and no Java is called from them (JNI calls stay on host-owned threads, PLAN.md 1.6).
// The window is recorded for the Vulkan backend (front_native_window, Phase 2), whose swapchain draws it; the null backend's build paints it black, as nothing else draws to it
#include "dm.h"
#include "hle.h"
#include "front.h"
#include "dm_input.h"
#include "game_profile.h"   // DM_GAME_ROMFS_MARKER
#include <android/input.h>
#include <android/keycodes.h>
#include <android/log.h>
#include <android/looper.h>
#include <android/native_activity.h>
#include <android/native_window.h>
#include <android/window.h>
#include <jni.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <sys/mman.h>
#include <sys/stat.h>

int dm_main(int argc, char **argv);   // dm/host/main.c built as dm_main (host_main.c)

static ANativeActivity *g_act;
static char g_files[512], g_ext[512], g_log[600];
static char *g_argv[3];
static _Atomic int g_host_on;          // the host thread was started (game files found): the lifecycle and input reach the host only then
static _Atomic(ANativeWindow *) g_win;

// ---------- front.h ----------
void front_run(void (*guest)(void)) { guest(); }   // already on the host thread (host_thread below), as on iOS
void *front_metal_layer(void) { return NULL; }
void *front_native_window(void) { return atomic_load(&g_win); }   // the activity's window, NULL while it has none (the Vulkan backend's surface, Phase 2)
int front_screen_max_fps(void) { return 0; }        // not known yet (Phase 2: the display modes)
void front_screen_match(int fps) { (void)fps; }
// ---------- Text input box (front.h): HostActivity.showTextInput (dm/android/app/java), an AlertDialog the system keyboard types into ----------
// The calling (guest) thread starts a host-owned thread for the JNI call (PLAN.md 1.6: ART cannot run on guest stacks), which attaches, asks for the box and detaches; then it waits for
// nativeTextDone (tx_done), which the UI thread calls once the box is answered. One box at a time. -1 (taken as a cancel) only when HostActivity's method or its native could not be bound
// in onCreate: not expected, since the manifest names HostActivity and an APK without its class does not start
extern int g_swkbd_min_chars;   // hle_swkbd.c: this input's minimum length (the applet keeps OK disabled until the text is long enough), -1 for none
static void input_release(void);
static jmethodID g_tx_show;    // HostActivity.showTextInput, set in onCreate (text_box_init)
static pthread_mutex_t g_tx_one = PTHREAD_MUTEX_INITIALIZER, g_tx_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_tx_cv = PTHREAD_COND_INITIALIZER;
static int64_t g_tx_token; static int g_tx_state;   // the box asked for, and its answer under g_tx_mu: 0 none yet, 1 OK, 2 cancel
static char *g_tx_out; static int g_tx_cap;
typedef struct { const char *title, *sub, *guide, *initial; int max, min; int64_t token; int ok; } tx_req;
static void *tx_call(void *p) {   // its own stack, not the guest's: the message is built here
    tx_req *q = p; JNIEnv *env; JavaVM *vm = g_act->vm;
    char msg[1280]; snprintf(msg, sizeof msg, "%s%s%s", q->sub, *q->sub && *q->guide ? "\n" : "", q->guide);   // sub (129 UTF-16 units) and guide (257) are at most 1159 bytes of UTF-8: never cut
    const char *str[4] = { q->title, msg, q->guide, q->initial };
    if ((*vm)->AttachCurrentThread(vm, &env, NULL) != JNI_OK) return NULL;
    jbyteArray b[4] = { 0 }; int nb = 0;
    for (; nb < 4; nb++) {   // a failed allocation leaves an exception pending: no other JNI call until it is cleared below
        jsize n = (jsize)strlen(str[nb]);
        if (!(b[nb] = (*env)->NewByteArray(env, n))) break;
        (*env)->SetByteArrayRegion(env, b[nb], 0, n, (const jbyte *)str[nb]);
    }
    if (nb == 4) (*env)->CallVoidMethod(env, g_act->clazz, g_tx_show, (jlong)q->token, b[0], b[1], b[2], b[3], (jint)q->max, (jint)q->min);
    q->ok = nb == 4 && !(*env)->ExceptionCheck(env);
    if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionDescribe(env); (*env)->ExceptionClear(env); }
    for (int i = 0; i < 4; i++) if (b[i]) (*env)->DeleteLocalRef(env, b[i]);
    (*vm)->DetachCurrentThread(vm);
    return NULL;
}
static void JNICALL tx_done(JNIEnv *env, jclass c, jlong token, jboolean ok, jbyteArray text) {   // the UI thread
    (void)c;
    pthread_mutex_lock(&g_tx_mu);
    if (token == g_tx_token && !g_tx_state) {
        int len = ok && text ? (int)(*env)->GetArrayLength(env, text) : 0, n = len < g_tx_cap - 1 ? len : g_tx_cap - 1;
        if (n > 0) (*env)->GetByteArrayRegion(env, text, 0, n + (n < len), (jbyte *)g_tx_out);   // one byte past a cut, to see where the cut falls
        while (n > 0 && n < len && ((unsigned char)g_tx_out[n] & 0xC0) == 0x80) n--;   // cut short: at a character's start
        g_tx_out[n] = 0;
        g_tx_state = ok ? 1 : 2; pthread_cond_signal(&g_tx_cv);
    }
    pthread_mutex_unlock(&g_tx_mu);
    // As the Mac box: the box took the keys and touches, so the game never saw the release of the press that opened it; nothing may stay pressed when it closes (onWindowFocusChanged also
    // releases them as the box takes focus)
    input_release();
}
static void text_box_init(ANativeActivity *act) {   // onCreate, the UI thread
    JNIEnv *env = act->env; jclass c = (*env)->GetObjectClass(env, act->clazz);
    jmethodID show = c ? (*env)->GetMethodID(env, c, "showTextInput", "(J[B[B[B[BII)V") : NULL;
    JNINativeMethod m = { "nativeTextDone", "(JZ[B)V", (void *)tx_done };
    if (show && !(*env)->ExceptionCheck(env) && !(*env)->RegisterNatives(env, c, &m, 1)) g_tx_show = show;
    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
    if (c) (*env)->DeleteLocalRef(env, c);
    if (!g_tx_show) fprintf(stderr, "boot: [RED] HostActivity.showTextInput or its native nativeTextDone could not be bound: software keyboards are cancelled, no box is shown\n");
}
int front_text_input(const char *title, const char *sub, const char *guide, const char *initial, int max_chars, char *out, int out_cap) {
    if (!g_tx_show || !out || out_cap < 1) return -1;
    pthread_mutex_lock(&g_tx_one);
    pthread_mutex_lock(&g_tx_mu);
    tx_req q = { title ? title : "", sub ? sub : "", guide ? guide : "", initial ? initial : "", max_chars > 0 ? max_chars : 0, g_swkbd_min_chars > 0 ? g_swkbd_min_chars : 0, ++g_tx_token, 0 };
    g_tx_state = 0; g_tx_out = out; g_tx_cap = out_cap;
    pthread_mutex_unlock(&g_tx_mu);
    pthread_t t; int r = -1;
    LOG("front end: input box %lld asked for (title '%s', at most %d and at least %d characters; 0: no limit)", (long long)q.token, q.title, q.max, q.min);
    if (!pthread_create(&t, NULL, tx_call, &q)) { pthread_join(t, NULL); r = q.ok ? 0 : -1; }
    pthread_mutex_lock(&g_tx_mu);
    if (r == 0) { while (!g_tx_state) pthread_cond_wait(&g_tx_cv, &g_tx_mu); r = g_tx_state == 1; }
    else LOG("front end: [RED] the input box could not be shown (the JNI call failed): taken as a cancel");
    g_tx_out = NULL; g_tx_cap = 0; g_tx_state = 2;   // a late answer to this box is ignored (tx_done checks the state)
    pthread_mutex_unlock(&g_tx_mu);
    pthread_mutex_unlock(&g_tx_one);
    return r;
}

// ---------- Log: <files>/log.txt (the previous run's kept as log.prev.txt, as on iOS) and logcat, tag "dmhost" ----------
// The host writes only to stderr: fd 2 itself becomes the file, so the host, the crash handler's raw write(2, …) and any library land there synchronously and a crash loses nothing.
// A thread follows the file and copies its lines to logcat (the end of a crash may miss logcat; the file has it).
// Constructors run while the activity dlopens this library, before onCreate, and some log (hle_nvn.c: the null-device line); fd 2 is the zygote's /dev/null then. Priority 101 runs early_log
// before every default-priority constructor of the library: it opens log.txt as fd 2 at once in the files directory derived the way Android lays it out (files_derive; NativeActivity's
// onCreate makes it, as getFilesDir, in the arguments of the call that dlopens this library), so every line lands in order and nothing is copied, then reads env.txt (env_early, below) for
// the constructors that read the environment. Only if that fails does fd 2 become a memory file that log_open copies to the head of log.txt: best effort (a line written during the copy may
// be lost or split), and said so in red
static char g_files_early[512];   // the derived files directory ("" = none)
static int g_log_early, g_early_err;   // early_log made fd 2 log.txt; else why not (errno, 0 = no directory derived)
static int g_early_fd = -1;   // the memory file fd 2 is instead
static off_t g_early_at;   // how much of the memory file has been copied out
static void env_early(void);
static int files_derive(char *dir, size_t cap) {   // /data/user/<uid / 100000>/<package, the process name in /proc/self/cmdline>/files; 0 = not an app's process
    char pkg[256]; int fd = open("/proc/self/cmdline", O_RDONLY | O_CLOEXEC); ssize_t n = fd >= 0 ? read(fd, pkg, sizeof pkg - 1) : -1;
    if (fd >= 0) close(fd);
    if (n <= 0) return 0;
    pkg[n] = 0; char *c = strchr(pkg, ':'); if (c) *c = 0;   // "<package>:<name>" for a component's own process
    if (!*pkg || strchr(pkg, '/')) return 0;   // not an app's process name
    snprintf(dir, cap, "/data/user/%u/%s/files", (unsigned)(getuid() / 100000), pkg);
    return 1;
}
static int g_keep_err;   // log_create could not keep the last log as log.prev.txt (errno): this run's is appended to it, said in red once the log is up
static off_t g_log_from;   // where this run's log starts in log.txt (the follower starts there)
static int log_create(const char *dir) {   // a fresh dir/log.txt (the last one kept as log.prev.txt), -1 = it cannot be opened (errno)
    char prev[600]; snprintf(g_log, sizeof g_log, "%s/log.txt", dir); snprintf(prev, sizeof prev, "%s/log.prev.txt", dir);
    int keep = rename(g_log, prev) && errno != ENOENT;   // ENOENT: no last log. Else (log.prev.txt a directory, say) emptying log.txt would lose the last log unseen: append to it
    g_keep_err = keep ? errno : 0;
    int fd = open(g_log, O_WRONLY | O_CREAT | (keep ? 0 : O_TRUNC) | O_APPEND | O_CLOEXEC, 0644);   // appends: a copied early log and the host's lines never overwrite each other
    struct stat sb; g_log_from = fd >= 0 && keep && !fstat(fd, &sb) ? sb.st_size : 0;
    return fd;
}
__attribute__((constructor(101))) static void early_log(void) {
    if (files_derive(g_files_early, sizeof g_files_early)) {
        int fd = log_create(g_files_early);
        if (fd < 0) g_early_err = errno; else { g_log_early = dup2(fd, 2) >= 0; if (!g_log_early) g_early_err = errno; if (fd != 2) close(fd); }
    }
    if (g_log_early) setvbuf(stderr, NULL, _IOLBF, 0);   // LOG's text and its newline in one write
    else { int fd = memfd_create("dmhost-early-log", MFD_CLOEXEC); if (fd >= 0 && dup2(fd, 2) >= 0) g_early_fd = fd; else if (fd >= 0) close(fd); }
    env_early();
}
// The memory file past g_early_at, to log.txt's descriptor or (-1) to logcat. Whole lines only unless all: a write still being copied in shows its first page's part first (seen on the phone),
// and a line cut there would have a line written straight to the log in its middle
static void early_drain(int to, int all) {
    if (g_early_fd < 0) return;
    static char b[16384]; ssize_t n;
    while ((n = pread(g_early_fd, b, sizeof b - 1, g_early_at)) > 0) {
        if (!all) { while (n > 0 && b[n - 1] != '\n') n--; if (!n) break; }
        g_early_at += n;
        if (to >= 0) { if (write(to, b, (size_t)n) != n) break; continue; }
        b[n] = 0; for (char *s = b, *nl; *s; s = nl ? nl + 1 : s + strlen(s)) { if ((nl = strchr(s, '\n'))) *nl = 0; __android_log_write(ANDROID_LOG_INFO, "dmhost", s); if (!nl) break; }
    }
}
// The fallback's handoff: fd 2 goes to fd2 (log.txt, or /dev/null; -1 = left as it is) between drains: a line another thread writes after the first drain still lands in the memory file, and after
// the dup2 only a write that had already entered the kernel with it does (it holds the file; on the phone one allocating a page outlasted the second drain), so a last drain 10 ms on takes it. Then
// the file is closed. A LOG cut between its text and its newline by the dup2 is split
static void early_log_end(int to, int fd2) {
    early_drain(to, 0); if (fd2 >= 0) dup2(fd2, 2); early_drain(to, 0);
    if (g_early_fd >= 0) { usleep(10000); early_drain(to, 1); close(g_early_fd); g_early_fd = -1; }
}
static void log_nowhere(void) {   // no log.txt: what the constructors wrote goes to logcat, and fd 2 to /dev/null (not on into memory); one opened at load stays
    if (g_log_early) return;
    int fd = open("/dev/null", O_WRONLY | O_CLOEXEC);
    early_log_end(-1, fd); if (fd >= 0) close(fd);
}
static void *log_follow(void *p) {
    (void)p;
    int fd = open(g_log, O_RDONLY | O_CLOEXEC); if (fd < 0) return NULL;
    if (g_log_from) lseek(fd, g_log_from, SEEK_SET);   // not the last run's log again
    static char b[4096]; size_t have = 0;
    for (;;) {
        ssize_t n = read(fd, b + have, sizeof b - 1 - have);
        if (n <= 0) { usleep(100000); continue; }   // at the end for now: the host writes more
        have += (size_t)n;
        char *s = b, *e = b + have, *nl;
        while ((nl = memchr(s, '\n', (size_t)(e - s)))) { *nl = 0; __android_log_write(ANDROID_LOG_INFO, "dmhost", s); s = nl + 1; }
        have = (size_t)(e - s); memmove(b, s, have);
        if (have == sizeof b - 1) { b[have] = 0; __android_log_write(ANDROID_LOG_INFO, "dmhost", b); have = 0; }   // a line longer than the buffer goes out in pieces
    }
}
static void log_open(void) {
    if (g_log_early) {   // fd 2 has been log.txt since the first constructor: no rename, no reopen, only the follower (from where this run's log starts)
        if (strcmp(g_files, g_files_early)) fprintf(stderr, "boot: [RED] the log is %s, derived at load, not in the activity's files directory\n", g_log);
    } else {
        int fd = log_create(g_files), mem = g_early_fd >= 0;
        if (fd < 0) { __android_log_print(ANDROID_LOG_ERROR, "dmhost", "boot: [RED] %s cannot be written (errno %d): the log goes nowhere (logcat gets only the constructors' lines kept in memory)", g_log, errno); log_nowhere(); return; }
        early_log_end(fd, fd); if (fd != 2) close(fd);   // the constructors' lines first, then fd 2 is log.txt
        setvbuf(stderr, NULL, _IOLBF, 0);
        char why[600]; if (*g_files_early) snprintf(why, sizeof why, "%s/log.txt: errno %d", g_files_early, g_early_err); else snprintf(why, sizeof why, "no files directory derived");
        fprintf(stderr, "boot: [RED] log.txt was not opened at load (%s): %s\n", why,
                mem ? "the constructors' lines were kept in memory and copied above, and a line written during the copy may be lost or split" : "the constructors' lines went nowhere");
    }
    if (g_keep_err) fprintf(stderr, "boot: [RED] log.txt could not be kept as log.prev.txt (errno %d): this run's log is appended to the previous one\n", g_keep_err);
    pthread_t t; if (!pthread_create(&t, NULL, log_follow, NULL)) pthread_detach(t);
}

// ---------- Environment, before the host starts (iOS main.m boot_env) ----------
// <files>/env.txt: one KEY=VALUE per line, # starts a comment; a variable already set wins (a sideloaded app gets no environment from outside: adb pushes this file, run_app.sh).
// Read in early_log, before the library's other constructors, as three of those read the environment once; the activity's internalDataPath is not known then, so the directory is the derived one
// (files_derive). boot_env checks it against internalDataPath and reads a file missed there late
static const char *const g_ctor_env[] = {"DM_NVN_LOG", "DM_NVN_PROF", "DM_FD_LOG"};   // read by constructors: hle_nvn.c nvn_init_slots and nvn_nops_register, hle_fdstat.c fdstat_init
static char g_env_keys[2048]; static size_t g_env_kl; static int g_env_early_read;
static int env_file(const char *path, char *ctor, size_t ctor_cap) {   // 1 = the file was there; ctor gets the constructor-read variables it set
    FILE *f = fopen(path, "r"); if (!f) return 0;
    char ln[4096]; size_t cl = 0;
    while (fgets(ln, sizeof ln, f)) {
        char *s = ln, *e = ln + strlen(ln);
        while (*s == ' ' || *s == '\t') s++;
        while (e > s && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' ' || e[-1] == '\t')) *--e = 0;
        char *eq = strchr(s, '=');
        if (!*s || *s == '#' || !eq || eq == s) continue;
        *eq = 0;
        if (getenv(s)) continue;
        setenv(s, eq + 1, 0);
        int w = snprintf(g_env_keys + g_env_kl, sizeof g_env_keys - g_env_kl, "%s%s", g_env_kl ? " " : "", s); g_env_kl = w > 0 && (size_t)w < sizeof g_env_keys - g_env_kl ? g_env_kl + (size_t)w : g_env_kl;
        for (size_t k = 0; ctor && k < sizeof g_ctor_env / sizeof *g_ctor_env; k++)
            if (!strcmp(s, g_ctor_env[k])) { int c = snprintf(ctor + cl, ctor_cap - cl, "%s%s", cl ? " " : "", s); cl = c > 0 && (size_t)c < ctor_cap - cl ? cl + (size_t)c : cl; }
        fprintf(stderr, "boot: env %s=%s (env.txt)\n", s, eq + 1);
    }
    fclose(f);
    setenv("DM_ENV_FILE_KEYS", g_env_keys, 1);   // as iOS sets it for its launcher: what an env file set
    return 1;
}
static void env_early(void) {
    char ef[600]; if (!*g_files_early) return;
    snprintf(ef, sizeof ef, "%s/env.txt", g_files_early); g_env_early_read = env_file(ef, NULL, 0);
}
static int has_marker(const char *dir) { char p[1024]; snprintf(p, sizeof p, "%s/%s", dir, DM_GAME_ROMFS_MARKER); return !access(p, R_OK); }
// The game files (RomFS): <external files>/romfs, the app's own external directory that adb can push to (the app cannot read /data/local/tmp), else <files>/romfs; only a directory holding
// the game profile's romfs_marker path counts (iOS romfs_locate). An env file's DM_ROMFS is taken as given
static const char *romfs_locate(char *buf, size_t n) {
    if (getenv("DM_ROMFS")) return getenv("DM_ROMFS");
    const char *base[2] = {g_ext, g_files};
    for (int i = 0; i < 2; i++) { if (!*base[i]) continue; snprintf(buf, n, "%s/romfs", base[i]); if (has_marker(buf)) return buf; }
    return NULL;
}
static void set_dir(const char *var, const char *sub) {   // a directory under <files>, made if missing, unless an env file set the variable
    char d[600]; snprintf(d, sizeof d, "%s/%s", g_files, sub); mkdir(d, 0700); setenv(var, d, 0);
}
static int boot_env(void) {
    log_open();
    fprintf(stderr, "boot: files %s, external files %s\n", g_files, *g_ext ? g_ext : "(none)");
    char ef[600]; snprintf(ef, sizeof ef, "%s/env.txt", g_files);
    if (!g_env_early_read || strcmp(g_files, g_files_early)) {   // missed at load (another path, or no file then): set now, too late for what the constructors read
        char late[256] = "";
        if (env_file(ef, late, sizeof late) && *late)
            fprintf(stderr, "boot: [RED] env.txt was read only after the library's constructors (they looked in %s): %s took no effect\n", *g_files_early ? g_files_early : "(no directory)", late);
    }
    setenv("DM_ROOT", g_files, 0);
    set_dir("DM_SAVE", "save"); set_dir("DM_TEMP", "temp");
    // Real audio output by default, as on iOS: the host's default elsewhere is the silent sink (hle_audio.c real_audio: adb-shell runs stay opt-in with DM_AUDIO=1); DM_AUDIO=0 in env.txt silences the app
    setenv("DM_AUDIO", "1", 0);
    char d[600];
    if (*g_ext) {   // the shared-font stand-ins and add-on content, pushed beside the RomFS when a game needs them (iOS: <App>/dm/font, <App>/aoc)
        snprintf(d, sizeof d, "%s/font", g_ext); if (!access(d, R_OK)) setenv("DM_FONT_DIR", d, 0);
        snprintf(d, sizeof d, "%s/aoc", g_ext); if (!access(d, R_OK)) setenv("DM_AOC_DIR", d, 0);
    }
    fprintf(stderr, "boot: game language %s\n", getenv("DM_LANG") ?: "en-US");
    char rb[600]; const char *romfs = romfs_locate(rb, sizeof rb);
    if (!romfs) { fprintf(stderr, "boot: game files not found (%s/romfs or %s/romfs holding %s): the host does not start\n", *g_ext ? g_ext : "<external files>", g_files, DM_GAME_ROMFS_MARKER); return 0; }
    fprintf(stderr, "boot: game files %s\n", romfs);
    setenv("DM_ROMFS", romfs, 0);
    snprintf(d, sizeof d, "%s/mods", g_files); mkdir(d, 0700);   // the rewritten modules (patch_nso.py --target android output), pushed by run_app.sh
    fprintf(stderr, "boot: modules %s\n", d);
    if (chdir(g_files)) fprintf(stderr, "boot: cannot enter %s (errno %d)\n", g_files, errno);
    g_argv[0] = strdup("dmhost"); g_argv[1] = strdup(d);
    return 1;
}
static void *host_thread(void *p) {
    (void)p;
    int rc = dm_main(2, g_argv);
    fprintf(stderr, "boot: dm_main returned %d\n", rc);
    ANativeActivity_finish(g_act);   // any thread may ask; onDestroy then ends the process
    return NULL;
}

// ---------- Input: the activity's input queue on the main thread's looper ----------
// Controllers map by position, as on the Mac and iPhone (front_mac.m pad_read, DmGamepad.m read_pad): Android names the face buttons by position too (BUTTON_A south, B east, X west, Y north),
// so south is the Switch's B and east its A. Player 1 only for now (every controller merged; the Mac's per-controller players, pad_slots.h, come later)
static uint64_t g_pad_keys, g_pad_hat, g_pad_trig, g_kbd;   // main thread only
static int32_t g_lx, g_ly, g_rx, g_ry;
static int g_w, g_a, g_s, g_d, g_input_log;
static uint64_t pad_key_bit(int32_t c) {
    switch (c) {
        case AKEYCODE_BUTTON_A: return DM_BTN_B; case AKEYCODE_BUTTON_B: return DM_BTN_A; case AKEYCODE_BUTTON_X: return DM_BTN_Y; case AKEYCODE_BUTTON_Y: return DM_BTN_X;
        case AKEYCODE_BUTTON_L1: return DM_BTN_L; case AKEYCODE_BUTTON_R1: return DM_BTN_R; case AKEYCODE_BUTTON_L2: return DM_BTN_ZL; case AKEYCODE_BUTTON_R2: return DM_BTN_ZR;
        case AKEYCODE_BUTTON_THUMBL: return DM_BTN_LS; case AKEYCODE_BUTTON_THUMBR: return DM_BTN_RS; case AKEYCODE_BUTTON_START: return DM_BTN_PLUS; case AKEYCODE_BUTTON_SELECT: return DM_BTN_MINUS;
        case AKEYCODE_DPAD_LEFT: return DM_BTN_LEFT; case AKEYCODE_DPAD_UP: return DM_BTN_UP; case AKEYCODE_DPAD_RIGHT: return DM_BTN_RIGHT; case AKEYCODE_DPAD_DOWN: return DM_BTN_DOWN;
    }
    return 0;
}
// Keyboard fallbacks, the Mac's layout (front_mac.m key_bit): J K U I = A B X Y, F G = stick presses, Q E Z C = L R ZL ZR, Enter / Tab = + / −, WASD = left stick, Space = shake; the arrows arrive as DPAD keys
static uint64_t kbd_key_bit(int32_t c) {
    switch (c) {
        case AKEYCODE_J: return DM_BTN_A; case AKEYCODE_K: return DM_BTN_B; case AKEYCODE_U: return DM_BTN_X; case AKEYCODE_I: return DM_BTN_Y;
        case AKEYCODE_F: return DM_BTN_LS; case AKEYCODE_G: return DM_BTN_RS;
        case AKEYCODE_Q: return DM_BTN_L; case AKEYCODE_E: return DM_BTN_R; case AKEYCODE_Z: return DM_BTN_ZL; case AKEYCODE_C: return DM_BTN_ZR;
        case AKEYCODE_ENTER: return DM_BTN_PLUS; case AKEYCODE_TAB: return DM_BTN_MINUS; case AKEYCODE_SPACE: return DM_BTN_SHAKE;
    }
    return 0;
}
static void pad_push(void) { dm_pad_set(g_pad_keys | g_pad_hat | g_pad_trig, g_lx, g_ly, g_rx, g_ry); }
static void kbd_push(void) { dm_input_set(g_kbd, (g_d - g_a) * DM_STICK_MAX, (g_w - g_s) * DM_STICK_MAX, 0, 0); }
// Focus lost, the queue gone, the activity paused: no key-up or stick release follows, so whatever the front end holds is let go (DM_KEYS steps hold on, as on the Mac)
static void input_release(void) {
    if (!atomic_load(&g_host_on)) return;
    g_pad_keys = g_pad_hat = g_pad_trig = g_kbd = 0; g_lx = g_ly = g_rx = g_ry = 0; g_w = g_a = g_s = g_d = 0;
    pad_push(); kbd_push();
}
static int32_t axis(float v) { return (int32_t)(fmaxf(-1.f, fminf(1.f, v)) * DM_STICK_MAX); }
// A radial dead zone on each stick, a fraction of full travel (DM_STICK_DEADZONE, 0.15 by default; read once env.txt is in), rescaled so travel past its edge starts from 0 and the rim still
// reaches DM_STICK_MAX: without it a stick at rest that reads a few percent off centre walks the player (0.24 would reach the guest as 7,864)
static float g_dz = 0.15f;
static void stick(float x, float y, int32_t *ox, int32_t *oy) {
    float m = sqrtf(x * x + y * y);
    if (!(m > g_dz)) { *ox = *oy = 0; return; }   // NaN too
    float k = (fminf(m, 1.f) - g_dz) / (1.f - g_dz) / m;
    *ox = axis(x * k); *oy = axis(y * k);
}
static int key_event(const AInputEvent *e) {
    int32_t c = AKeyEvent_getKeyCode(e), act = AKeyEvent_getAction(e);
    if (act != AKEY_EVENT_ACTION_DOWN && act != AKEY_EVENT_ACTION_UP) return 0;
    int down = act == AKEY_EVENT_ACTION_DOWN; uint64_t m;
    if ((m = pad_key_bit(c))) { g_pad_keys = down ? g_pad_keys | m : g_pad_keys & ~m; pad_push(); }
    else if ((m = kbd_key_bit(c)) || c == AKEYCODE_W || c == AKEYCODE_A || c == AKEYCODE_S || c == AKEYCODE_D) {
        if (c == AKEYCODE_W) g_w = down; else if (c == AKEYCODE_A) g_a = down; else if (c == AKEYCODE_S) g_s = down; else if (c == AKEYCODE_D) g_d = down;
        g_kbd = down ? g_kbd | m : g_kbd & ~m; kbd_push();
    } else return 0;   // the rest (volume, back, home) is the system's: back finishes the activity, which ends the host (onDestroy)
    if (g_input_log && AKeyEvent_getRepeatCount(e) == 0)
        LOG("front end: key %d %s (source %#x, device %d) → controller %#llx keyboard %#llx (%.2f s after launch)", c, down ? "down" : "up", AInputEvent_getSource(e), AInputEvent_getDeviceId(e),
            (unsigned long long)(g_pad_keys | g_pad_hat | g_pad_trig), (unsigned long long)g_kbd, (now_ns() - g_t0_ns) / 1e9);
    return 1;
}
// A controller's sticks, triggers and hat: Android's left stick is X / Y and its right stick Z / RZ, y pointing down (Switch sticks point up); analog triggers past half travel press ZL / ZR
static int motion_event(const AInputEvent *e) {
    if (!(AInputEvent_getSource(e) & AINPUT_SOURCE_CLASS_JOYSTICK) || (AMotionEvent_getAction(e) & AMOTION_EVENT_ACTION_MASK) != AMOTION_EVENT_ACTION_MOVE) return 0;   // touches: the overlay, later
    stick(AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_X, 0), -AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_Y, 0), &g_lx, &g_ly);
    stick(AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_Z, 0), -AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_RZ, 0), &g_rx, &g_ry);
    float lt = fmaxf(AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_LTRIGGER, 0), AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_BRAKE, 0));
    float rt = fmaxf(AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_RTRIGGER, 0), AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_GAS, 0));
    float hx = AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_HAT_X, 0), hy = AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_HAT_Y, 0);
    g_pad_trig = (lt > 0.5f ? DM_BTN_ZL : 0) | (rt > 0.5f ? DM_BTN_ZR : 0);
    g_pad_hat = (hx < -0.5f ? DM_BTN_LEFT : 0) | (hx > 0.5f ? DM_BTN_RIGHT : 0) | (hy < -0.5f ? DM_BTN_UP : 0) | (hy > 0.5f ? DM_BTN_DOWN : 0);
    pad_push();
    return 1;
}
static AInputQueue *g_queue;
static int on_input(int fd, int events, void *data) {
    (void)fd; (void)events; (void)data;
    AInputEvent *e;
    while (g_queue && AInputQueue_getEvent(g_queue, &e) >= 0) {
        if (AInputQueue_preDispatchEvent(g_queue, e)) continue;   // taken by the IME first
        int handled = 0;
        if (atomic_load(&g_host_on)) handled = AInputEvent_getType(e) == AINPUT_EVENT_TYPE_KEY ? key_event(e) : AInputEvent_getType(e) == AINPUT_EVENT_TYPE_MOTION ? motion_event(e) : 0;
        AInputQueue_finishEvent(g_queue, e, handled);
    }
    return 1;
}
static void on_input_created(ANativeActivity *a, AInputQueue *q) { (void)a; g_queue = q; AInputQueue_attachLooper(q, ALooper_forThread(), 1, on_input, NULL); }
static void on_input_destroyed(ANativeActivity *a, AInputQueue *q) { (void)a; AInputQueue_detachLooper(q); g_queue = NULL; input_release(); }

// ---------- Window: recorded for the GPU backend (front_native_window); the null backend's build paints it black, as nothing else draws to it ----------
// The Vulkan backend (dm/gfx/vkn/vk_present.c) defines both hooks: its swapchain owns the window (a CPU lock would connect the window as a CPU producer, and the swapchain could not connect), and on
// window loss it releases the surface before this callback returns, as Android requires (the window may go once onNativeWindowDestroyed returns). Weak: the null build links neither
__attribute__((weak)) int dm_gfx_owns_window(void) { return 0; }
__attribute__((weak)) void dm_gfx_window_lost(void) {}
static void win_black(ANativeWindow *w) {
    if (dm_gfx_owns_window()) return;
    ANativeWindow_Buffer b;
    if (ANativeWindow_setBuffersGeometry(w, 0, 0, WINDOW_FORMAT_RGBX_8888) || ANativeWindow_lock(w, &b, NULL)) return;
    for (int y = 0; y < b.height; y++) memset((uint8_t *)b.bits + (size_t)y * b.stride * 4, 0, (size_t)b.width * 4);
    ANativeWindow_unlockAndPost(w);
}
static void on_win_created(ANativeActivity *a, ANativeWindow *w) {
    (void)a; atomic_store(&g_win, w);
    LOG("front end: window %dx%d", ANativeWindow_getWidth(w), ANativeWindow_getHeight(w));   // one wording for both backends (parity_expected.txt); the Vulkan backend logs its surface
    win_black(w);
}
static void on_win_redraw(ANativeActivity *a, ANativeWindow *w) { (void)a; win_black(w); }   // the swapchain's next present redraws it (FIFO, every frame) with the Vulkan backend
static void on_win_destroyed(ANativeActivity *a, ANativeWindow *w) { (void)a; (void)w; atomic_store(&g_win, NULL); dm_gfx_window_lost(); }   // NULL first: no new surface is made on it
static void on_focus(ANativeActivity *a, int has) { (void)a; if (!has) input_release(); }

// ---------- Lifecycle (iOS front_ios_focus_event): paused = focus lost and audio suspended; stopped = in the background, the guest clock frozen and graphics suspended ----------
// The first onStart / onResume of a launch only begins it: the host starts in focus (hle_oe.c), and pushing 2 then 1 would send the game focus messages while it boots. Main thread only
static int g_paused, g_stopped;
static void on_pause(ANativeActivity *a) { (void)a; if (!atomic_load(&g_host_on) || g_paused) return; g_paused = 1; input_release(); dm_focus_set(2); dm_audio_suspend(1); }
static void on_stop(ANativeActivity *a) { (void)a; if (!atomic_load(&g_host_on) || g_stopped) return; g_stopped = 1; dm_focus_set(3); guest_clock_freeze_for(1, 1); dm_gfx_suspend(1); }
static void on_start(ANativeActivity *a) { (void)a; if (!g_stopped) return; g_stopped = 0; dm_gfx_suspend(0); guest_clock_freeze_for(1, 0); dm_focus_set(2); }
static void on_resume(ANativeActivity *a) { (void)a; if (!g_paused) return; g_paused = 0; dm_audio_suspend(0); dm_focus_set(1); }
// Destroyed (back, finish, the system): the host cannot start again in this process, so it ends it, the way SIGTERM does (hle_lifecycle.c: audio stopped, graphics drained, then _exit)
static void on_destroy(ANativeActivity *a) {
    (void)a;
    if (atomic_load(&g_host_on)) dm_host_shutdown("activity destroyed");
    fprintf(stderr, "boot: activity destroyed\n"); _exit(0);
}
static void *on_save(ANativeActivity *a, size_t *n) { (void)a; *n = 0; return NULL; }
static void on_low_memory(ANativeActivity *a) { (void)a; if (atomic_load(&g_host_on)) LOG("front end: the system reports low memory"); }

void ANativeActivity_onCreate(ANativeActivity *act, void *saved, size_t saved_size) {
    (void)saved; (void)saved_size;
    g_act = act;
    ANativeActivityCallbacks *cb = act->callbacks;
    cb->onStart = on_start; cb->onResume = on_resume; cb->onPause = on_pause; cb->onStop = on_stop; cb->onDestroy = on_destroy; cb->onSaveInstanceState = on_save;
    cb->onWindowFocusChanged = on_focus; cb->onNativeWindowCreated = on_win_created; cb->onNativeWindowRedrawNeeded = on_win_redraw; cb->onNativeWindowDestroyed = on_win_destroyed;
    cb->onInputQueueCreated = on_input_created; cb->onInputQueueDestroyed = on_input_destroyed; cb->onLowMemory = on_low_memory;
    ANativeActivity_setWindowFlags(act, AWINDOW_FLAG_KEEP_SCREEN_ON | AWINDOW_FLAG_FULLSCREEN, 0);   // the game takes no touches yet, so nothing else would keep the screen on
    static int once; if (once++) return;   // a second activity in this process (singleTask makes it rare): the host is already running
    snprintf(g_files, sizeof g_files, "%s", act->internalDataPath ? act->internalDataPath : "");
    snprintf(g_ext, sizeof g_ext, "%s", act->externalDataPath ? act->externalDataPath : "");
    if (!*g_files) { __android_log_write(ANDROID_LOG_ERROR, "dmhost", "boot: [RED] no internal data path: the host does not start"); log_nowhere(); return; }
    if (!boot_env()) return;
    text_box_init(act);
    g_input_log = getenv("DM_INPUT_LOG") && atoi(getenv("DM_INPUT_LOG"));   // log each key press with what reached the controller (the done-test's evidence)
    { const char *e = getenv("DM_STICK_DEADZONE"); char *end; float v = e ? strtof(e, &end) : 0;
      if (e && end != e && v >= 0.f && v <= 0.9f) g_dz = v; else if (e) fprintf(stderr, "boot: DM_STICK_DEADZONE=%s is not a fraction from 0 to 0.9: the dead zone stays %.2f\n", e, g_dz); }
    pthread_attr_t at; pthread_attr_init(&at); pthread_attr_setstacksize(&at, 64u << 20);   // as the iOS host thread: the Switch main thread's stack is set by npdm, the host gives plenty
    pthread_t t;
    if (pthread_create(&t, &at, host_thread, NULL)) { fprintf(stderr, "boot: the host thread cannot start (errno %d)\n", errno); pthread_attr_destroy(&at); return; }
    pthread_detach(t); pthread_attr_destroy(&at);
    atomic_store(&g_host_on, 1);
}
