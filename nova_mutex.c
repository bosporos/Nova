#include "nova.h"
#include <errno.h>

/* memcpy */
#include <string.h>

/*******************************************************************************
 * MUTEX HANDLING
 ******************************************************************************/

NOVA_DOCSTUB ();

nova_res_t nvmutex_init (nova_mutex_t * nv_mutex)
{
    /* We can't do *nv_mutex = PTHREAD_MUTEX_INITIALIZER, because (at least on darwin)
     * pthread_mutex_t is a 64-byte opaque struct
     */
    static const pthread_mutex_t _nv_mutex_init = PTHREAD_MUTEX_INITIALIZER;
    memcpy (nv_mutex, &_nv_mutex_init, sizeof (pthread_mutex_t));

    /* Pretty standard init; we _could_ also do error checking (in debug mode),
     * but I honestly don't see much value in that atm, because these are pretty
     * much guaranteed-success if called properly.
     */
    pthread_mutexattr_t _nv_pmattr;
    pthread_mutexattr_settype (&_nv_pmattr, PTHREAD_MUTEX_NORMAL);
    pthread_mutex_init (nv_mutex, &_nv_pmattr);
    pthread_mutexattr_destroy (&_nv_pmattr);

    return nova_ok;
}

nova_res_t nvmutex_lock (nova_mutex_t * nv_mutex)
{
    /* Again, very standard: We do an 'error check' here because it doesn't
       us much, but we still put in a debug #if */
#if NOVA_MODE_DEBUG
    __nv_dbg_assert (0 == pthread_mutex_lock (nv_mutex),
                     "nvmutex_lock(%p): pthread_mutex_lock failed",
                     nv_mutex);
    return nova_ok;
#else /* NOVA_MODE_DEBUG || */
    /* Return value is non-dependent, so this allows slightly more cpu-level
     * parallelism. */
    pthread_mutex_lock (nv_mutex);
    return nova_ok;
#endif /* NOVA_MODE_DEBUG */
}

nova_res_t nvmutex_trylock (nova_mutex_t * nv_mutex)
{
    /* We don't do _proper_ error checking here, because that would be a pain,
     * considering that we would have to deal with, well, return representation of
     * error values when nova_fail is a valid/correct state to return.
     */
#if NOVA_MODE_DEBUG
    const int r = pthread_mutex_trylock (nv_mutex);
    __nv_dbg_assert (r == EBUSY || r == 0,
                     "nvmutex_trylock(%p): pthread_mutex_trylock failed",
                     nv_mutex);
    if (!r)
        return nova_ok;
    return nova_fail;
#else
    if (!pthread_mutex_trylock (nv_mutex))
        return nova_ok;
    return nova_fail;
#endif
}

nova_res_t nvmutex_unlock (nova_mutex_t * nv_mutex)
{
    /* Again, very standard.
     */
#if NOVA_MODE_DEBUG
    __nv_dbg_assert (0 == pthread_mutex_unlock (nv_mutex),
                     "nvmutex_unlock(%p): phread_mutex_unlock failed",
                     nv_mutex);
    return nova_ok;
#else /*  NOVA_MODE_DEBUG || */
    /* allows for better branch prediction/computational lookahead, I believe. */
    pthread_mutex_unlock (nv_mutex);
    return nova_ok;
#endif /* NOVA_MODE_DEBUG */
}

nova_res_t nvmutex_drop (nova_mutex_t * nv_mutex)
{
    /* Error checking: not really any.
     * NOTE: If someone does eventually decide to put in proper error checking,
     *       DragonFly BSD has a weird bug where it'll return EINVAL or something
     *       if the mutex is destroyed without ever having been locked--same
     *       thing happens with condvar and rwlock.
     * EDIT: Not sure if the DragonFly thing is still relevant, looking at the
     *       bug report at https://bugs.dragonflybsd.org/issues/2763 doesn't really
     *       make it clear whether this was resolved or not.
     */
#if NOVA_MODE_DEBUG
#    if defined(__DragonFly__)
    const int r = pthread_mutex_destroy (nv_mutex);
    __nv_dbg_assert (0 == r || EINVAL == r,
                     "nvmutex_drop(%p): pthread_mutex_destroy failed", )
#    else
    __nv_dbg_assert (0 == pthread_mutex_destroy (nv_mutex),
                     "nvmutex_drop(%p): pthread_mutex_destroy failed",
                     nv_mutex);
#    endif
#else
    pthread_mutex_destroy (nv_mutex);
#endif
        return nova_ok;
}
