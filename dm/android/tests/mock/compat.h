// forced include for the Mac build of dm/android/media: what Bionic has and macOS lacks, and dlopen / dlsym answered by the fake (binder pool "already started")
#pragma once
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <time.h>
static inline int pthread_cond_clockwait(pthread_cond_t *cv, pthread_mutex_t *mu, clockid_t clk, const struct timespec *abs) {
    struct timespec now; clock_gettime(clk, &now);
    int64_t ns = (int64_t)(abs->tv_sec - now.tv_sec) * 1000000000 + (abs->tv_nsec - now.tv_nsec);
    if (ns <= 0) return ETIMEDOUT;
    struct timespec rel = {ns / 1000000000, ns % 1000000000};
    int r = pthread_cond_timedwait_relative_np(cv, mu, &rel);
    return r == ETIMEDOUT ? 0 : r;   // the caller's loop checks the deadline again
}
#define dlopen mock_dlopen
#define dlsym mock_dlsym
void *mock_dlopen(const char *path, int mode);
void *mock_dlsym(void *h, const char *name);
