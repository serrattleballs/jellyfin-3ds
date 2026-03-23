/**
 * fake_pthread.c — Supplements for FFmpeg's pthread usage on 3DS
 *
 * Recent versions of devkitARM's newlib (libsysbase) provide full
 * pthread implementations (create, join, mutex, cond, once).
 * We only need to provide sysconf() which FFmpeg calls to determine
 * the number of CPU cores for its thread pool.
 */

#include <3ds.h>

#ifndef _SC_NPROCESSORS_ONLN
#define _SC_NPROCESSORS_ONLN 84
#endif

long sysconf(int name)
{
    if (name == _SC_NPROCESSORS_ONLN)
        return 4; /* New 3DS has 4 cores */
    return -1;
}
