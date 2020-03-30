#include "nova.h"

/* malloc, free */
#include <stdlib.h>

/*******************************************************************************
 * TID HANDLING
 ******************************************************************************/

NOVA_DOCSTUB ();

#if !defined(NOVA_TID_RECYCLING)
static /* __atomic */ nova_tid_t __nv_tid_next = 1;
#endif
static _Thread_local nova_tid_t __nv_tid_local = 0;

nova_tid_t __nv_tid ()
{
#if defined(NOVA_LAZY_TIDINIT) && !defined(NOVA_TID_RECYCLING)
    if (__nv_tid_local == 0) {
        __nv_tid_local = __atomic_add_fetch (&__nv_tid_next, 1, __ATOMIC_ACQ_REL);
    }
#endif
    return __nv_tid_local;
}

typedef struct nv_tid_recycle_info
{
    nova_tid_t nv_tid;
    struct nv_tid_recycle_info * nv_next;
} nv_tid_recycle_info_t;

typedef struct
{
    nv_tid_recycle_info_t * nv_head;
    nvi_t nv_length;
} nv_tid_recycle_chain_t;

static nv_tid_recycle_chain_t __nv_tid_recycle_chain = {
    .nv_head   = NULL,
    .nv_length = 0
};
static nova_mutex_t __nv_tid_recycle_chain_lock;

nova_res_t __nv_tid_recycle_init ()
{
    nvmutex_init (&__nv_tid_recycle_chain_lock);
    return nova_ok;
}

nova_res_t __nv_tid_recycle_drop ()
{
    nvmutex_drop (&__nv_tid_recycle_chain_lock);
    return nova_ok;
}

nova_res_t __nv_tid_thread_init ()
{
#if !defined(NOVA_LAZY_TIDINIT) && !defined(NOVA_TID_RECYCLING)
    __nv_tid_local = __atomic_add_fetch (&__nv_tid_next, 1, __ATOMIC_ACQ_REL);
#elif defined(NOVA_TID_RECYCLING)
    nv_tid_recycle_info_t * _nv_info = malloc (sizeof *_nv_info);
    if (_nv_info == NULL) {
#    if NOVA_MODE_DEBUG
        __nv_error (NVE_STRUCTALLOC_DRY,
                    "__nv_tid_init(): (recyclation variation): could not allocate"
                    " memory for a link in the thread id recycle chain.");
#    endif
        return nova_fail;
    }
    uint64_t _nv_mark = 1, _nv_gap;
    nvmutex_lock (&__nv_tid_recycle_chain_lock);
    nv_tid_recycle_info_t *_nv_curr = __nv_tid_recycle_chain.nv_head,
                          *_nv_prev = NULL;
    for (nvi_t i = 0; i < __nv_tid_recycle_chain.nv_length; i++) {
        _nv_gap = _nv_curr->nv_tid - _nv_mark;
        if (_nv_gap > 0) {
            /* @TODO: switch to internal structure allocation API.
             */
            _nv_info->nv_tid  = _nv_mark;
            _nv_info->nv_next = _nv_curr;
            if (__builtin_expect (_nv_prev != NULL, 1)) {
                _nv_prev->nv_next = _nv_info;
            } else {
                __nv_tid_recycle_chain.nv_head = _nv_info;
            }
            __nv_tid_recycle_chain.nv_length++;

            break;
        }
        /* Set up for the next iteration of the loop
         */
        _nv_mark = _nv_curr->nv_tid + 1;
        _nv_prev = _nv_curr;

        /* Iterate.
         */
        _nv_curr = _nv_curr->nv_next;
    }

    /* We pull it out of the loop so that we can handle both the 0-length case
     * and then end-of-chain case.
     */
    if (_nv_curr == NULL) {
        /* So, it's not in any of the gaps (or there are no gaps because the chain
         * doesn't have any members yet)--that means we just add it to the
         * end of the chain/start the chain.
         *
         * _nv_mark is currently equal to the nv_tid of the previous link + 1, or,
         * in the case of the start-of-chain scenario, it's 1.
         */
        _nv_info->nv_tid = _nv_mark;
        if (__builtin_expect (_nv_prev != NULL, 1)) {

        } else {
            __nv_tid_recycle_chain.nv_head = _nv_info;
        }

        __nv_tid_recycle_chain.nv_length++;
    }

    nvmutex_unlock (&__nv_tid_recycle_chain_lock);
#endif

    return nova_ok;
}

nova_res_t __nv_tid_thread_drop ()
{
#if defined(NOVA_TID_RECYCLING)
    /* Local cache, hopefully slightly faster than the TLV getters.
     */
    nova_tid_t _nv_tid = __nv_tid_local;
    nvmutex_lock (&__nv_tid_recycle_chain_lock);
    nv_tid_recycle_info_t *_nv_curr = __nv_tid_recycle_chain.nv_head,
                          *_nv_prev = NULL;
    for (nvi_t i = 0; i < __nv_tid_recycle_chain.nv_length; i++) {
        if (_nv_curr->nv_tid == _nv_tid) {
            if (__builtin_expect (_nv_prev != NULL, 1)) {
                _nv_prev->nv_next = _nv_curr->nv_next;
            } else {
                __nv_tid_recycle_chain.nv_head = _nv_curr->nv_next;
            }
            free (_nv_curr);
            __nv_tid_recycle_chain.nv_length--;
            break;
        }
        _nv_prev = _nv_curr;
        _nv_curr = _nv_curr->nv_next;
    }
    nvmutex_unlock (&__nv_tid_recycle_chain_lock);
    if (_nv_curr == NULL) {
        /* Failed to find the relevant tid/no tids in chain
         */
        __nv_error (NVE_BADCALL,
                    "__nv_tid_drop(): tid was not initialized, or was previously"
                    " dropped, for this thread {tid=%lu}.",
                    _nv_tid);
        return nova_fail;
    }
#endif
    return nova_ok;
}
