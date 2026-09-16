/* 内部头:跨平台互斥锁(不进公共 API,不安装)。 */
#ifndef SXCL_INTERNAL_PLATFORM_LOCK_H
#define SXCL_INTERNAL_PLATFORM_LOCK_H

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
typedef CRITICAL_SECTION sxcl_lock_t;
static __inline void sxcl_lock_init(sxcl_lock_t *l) { InitializeCriticalSection(l); }
static __inline void sxcl_lock_destroy(sxcl_lock_t *l) { DeleteCriticalSection(l); }
static __inline void sxcl_lock_acquire(sxcl_lock_t *l) { EnterCriticalSection(l); }
static __inline void sxcl_lock_release(sxcl_lock_t *l) { LeaveCriticalSection(l); }
#else
#  include <pthread.h>
typedef pthread_mutex_t sxcl_lock_t;
static __inline void sxcl_lock_init(sxcl_lock_t *l) { (void)pthread_mutex_init(l, NULL); }
static __inline void sxcl_lock_destroy(sxcl_lock_t *l) { (void)pthread_mutex_destroy(l); }
static __inline void sxcl_lock_acquire(sxcl_lock_t *l) { (void)pthread_mutex_lock(l); }
static __inline void sxcl_lock_release(sxcl_lock_t *l) { (void)pthread_mutex_unlock(l); }
#endif

#endif /* SXCL_INTERNAL_PLATFORM_LOCK_H */
