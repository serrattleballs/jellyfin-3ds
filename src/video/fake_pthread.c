/**
 * fake_pthread.c — POSIX pthreads → libctru mapping for FFmpeg
 *
 * FFmpeg requires pthreads for its multithreaded decoders. The 3DS
 * has no POSIX threads, but libctru provides equivalent primitives.
 * This shim maps pthread calls to libctru at link time.
 *
 * Reference: ThirdTube/source/system/fake_pthread.cpp
 */

#include <3ds.h>
#include <stdlib.h>
#include <string.h>

/* Match the simple types from our pthread.h shim used during FFmpeg build */
typedef unsigned long pthread_t;
typedef unsigned long pthread_mutex_t;
typedef unsigned long pthread_cond_t;
typedef unsigned long pthread_once_t;

/* ── Thread tracking ─────────────────────────────────────────────── */

#define MAX_THREADS 8

typedef struct {
    Thread  handle;
    void   *(*start_routine)(void *);
    void   *arg;
    void   *retval;
    bool    in_use;
} thread_entry_t;

static thread_entry_t s_threads[MAX_THREADS];
static LightLock      s_thread_lock;
static bool           s_thread_lock_init = false;
static int            s_next_core = 1; /* round-robin core assignment, start on core 1 */

static void ensure_lock_init(void)
{
    if (!s_thread_lock_init) {
        LightLock_Init(&s_thread_lock);
        s_thread_lock_init = true;
    }
}

static void thread_wrapper(void *arg)
{
    thread_entry_t *entry = (thread_entry_t *)arg;
    entry->retval = entry->start_routine(entry->arg);
}

/* ── Threads ─────────────────────────────────────────────────────── */

int pthread_create(pthread_t *t, const void *attr, void *(*fn)(void *), void *arg)
{
    (void)attr;
    ensure_lock_init();
    LightLock_Lock(&s_thread_lock);

    int slot = -1;
    for (int i = 0; i < MAX_THREADS; i++) {
        if (!s_threads[i].in_use) {
            slot = i;
            break;
        }
    }

    if (slot < 0) {
        LightLock_Unlock(&s_thread_lock);
        return -1; /* EAGAIN */
    }

    s_threads[slot].start_routine = fn;
    s_threads[slot].arg = arg;
    s_threads[slot].retval = NULL;
    s_threads[slot].in_use = true;

    /* Round-robin core assignment across cores 1-3 (keep core 0 for main) */
    int core = s_next_core;
    s_next_core = (s_next_core % 3) + 1;

    s32 prio = 0;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);

    s_threads[slot].handle = threadCreate(thread_wrapper, &s_threads[slot],
                                           32 * 1024, prio - 1, core, false);

    if (!s_threads[slot].handle) {
        s_threads[slot].in_use = false;
        LightLock_Unlock(&s_thread_lock);
        return -1;
    }

    *t = (pthread_t)slot;
    LightLock_Unlock(&s_thread_lock);
    return 0;
}

int pthread_join(pthread_t t, void **retval)
{
    int slot = (int)t;
    if (slot < 0 || slot >= MAX_THREADS || !s_threads[slot].in_use)
        return -1;

    threadJoin(s_threads[slot].handle, U64_MAX);
    threadFree(s_threads[slot].handle);

    if (retval)
        *retval = s_threads[slot].retval;

    s_threads[slot].in_use = false;
    return 0;
}

/* ── Mutex ───────────────────────────────────────────────────────── */

int pthread_mutex_init(pthread_mutex_t *m, const void *attr)
{
    (void)attr;
    LightLock *lock = malloc(sizeof(LightLock));
    if (!lock) return -1;
    LightLock_Init(lock);
    *m = (pthread_mutex_t)(uintptr_t)lock;
    return 0;
}

int pthread_mutex_destroy(pthread_mutex_t *m)
{
    if (*m) {
        free((void *)(uintptr_t)*m);
        *m = 0;
    }
    return 0;
}

int pthread_mutex_lock(pthread_mutex_t *m)
{
    /* Handle PTHREAD_MUTEX_INITIALIZER (static init) */
    if (*m == 0)
        pthread_mutex_init(m, NULL);
    LightLock_Lock((LightLock *)(uintptr_t)*m);
    return 0;
}

int pthread_mutex_unlock(pthread_mutex_t *m)
{
    if (*m == 0) return 0;
    LightLock_Unlock((LightLock *)(uintptr_t)*m);
    return 0;
}

/* ── Condition variable ──────────────────────────────────────────── */

int pthread_cond_init(pthread_cond_t *c, const void *attr)
{
    (void)attr;
    CondVar *cv = malloc(sizeof(CondVar));
    if (!cv) return -1;
    CondVar_Init(cv);
    *c = (pthread_cond_t)(uintptr_t)cv;
    return 0;
}

int pthread_cond_destroy(pthread_cond_t *c)
{
    if (*c) {
        free((void *)(uintptr_t)*c);
        *c = 0;
    }
    return 0;
}

int pthread_cond_signal(pthread_cond_t *c)
{
    if (*c == 0)
        pthread_cond_init(c, NULL);
    CondVar_Signal((CondVar *)(uintptr_t)*c);
    return 0;
}

int pthread_cond_broadcast(pthread_cond_t *c)
{
    if (*c == 0)
        pthread_cond_init(c, NULL);
    CondVar_WakeUp((CondVar *)(uintptr_t)*c, MAX_THREADS);
    return 0;
}

int pthread_cond_wait(pthread_cond_t *c, pthread_mutex_t *m)
{
    if (*c == 0)
        pthread_cond_init(c, NULL);
    if (*m == 0)
        pthread_mutex_init(m, NULL);
    CondVar_Wait((CondVar *)(uintptr_t)*c, (LightLock *)(uintptr_t)*m);
    return 0;
}

/* ── Once ────────────────────────────────────────────────────────── */

static LightLock s_once_lock;
static bool      s_once_lock_init = false;

int pthread_once(pthread_once_t *once, void (*fn)(void))
{
    if (!s_once_lock_init) {
        LightLock_Init(&s_once_lock);
        s_once_lock_init = true;
    }
    LightLock_Lock(&s_once_lock);
    if (*once == 0) {
        fn();
        *once = 1;
    }
    LightLock_Unlock(&s_once_lock);
    return 0;
}

/* ── sysconf ─────────────────────────────────────────────────────── */

#ifndef _SC_NPROCESSORS_ONLN
#define _SC_NPROCESSORS_ONLN 84
#endif

long sysconf(int name)
{
    if (name == _SC_NPROCESSORS_ONLN)
        return 4; /* New 3DS has 4 cores */
    return -1;
}
