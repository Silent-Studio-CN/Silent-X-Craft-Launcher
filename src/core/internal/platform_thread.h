/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

#ifndef SXCL_INTERNAL_PLATFORM_THREAD_H
#define SXCL_INTERNAL_PLATFORM_THREAD_H

#include "platform_lock.h"

#if defined(_WIN32)
typedef HANDLE sxcl_thread_t;
typedef DWORD(WINAPI *sxcl_thread_fn_t)(LPVOID);

static __inline int sxcl_thread_start(sxcl_thread_t *t, sxcl_thread_fn_t fn, void *arg) {
    *t = CreateThread(NULL, 0, fn, arg, 0, NULL);
    return (*t != NULL) ? 0 : -1;
}
static __inline void sxcl_thread_join(sxcl_thread_t t) {
    WaitForSingleObject(t, INFINITE);
    CloseHandle(t);
}
static __inline void sxcl_thread_sleep_ms(unsigned ms) { Sleep(ms); }
#define SXCL_THREAD_FN(name) static DWORD WINAPI name(LPVOID arg)
#define SXCL_THREAD_RETURN(n) return (DWORD)(n)
#else
#  include <time.h>
typedef pthread_t sxcl_thread_t;
typedef void *(*sxcl_thread_fn_t)(void *);

static __inline int sxcl_thread_start(sxcl_thread_t *t, sxcl_thread_fn_t fn, void *arg) {
    return pthread_create(t, NULL, fn, arg) == 0 ? 0 : -1;
}
static __inline void sxcl_thread_join(sxcl_thread_t t) { (void)pthread_join(t, NULL); }
static __inline void sxcl_thread_sleep_ms(unsigned ms) {
    struct timespec ts;
    ts.tv_sec = (time_t)(ms / 1000u);
    ts.tv_nsec = (long)((ms % 1000u) * 1000000u);
    nanosleep(&ts, NULL);
}
#define SXCL_THREAD_FN(name) static void *name(void *arg)
#define SXCL_THREAD_RETURN(n) return (void *)(n)
#endif

#endif /* SXCL_INTERNAL_PLATFORM_THREAD_H */
