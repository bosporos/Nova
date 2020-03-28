#define NOVA_FORCE_NV_TID_DEFPERM 1
#define NOVA_MODE_DEBUG 1
#include "nova.h"

#ifdef __APPLE__
/* mach_port_name_t
   kern_return_t? */
#    include <mach/mach_types.h>
/* THREAD_IDENTIFIER_INFO_COUNT
   thread_identifier_info_data_t
   THREAD_IDENTIFIER_INFO */
#    include <mach/thread_info.h>
/* thread_info(thread_act_t = mach_port_name_t,
               thread_flavor_t,
               thread_info_t,
               mach_msg_type_number_t) */
#    include <mach/mach.h>
#endif
#ifdef __linux__
/* gettid(void) */
#    include <sys/types.h>
#endif

/* Test harness
 */
int main (
    __attribute__ ((unused)) int argc,
    __attribute__ ((unused)) char ** argv)
{
    printf ("sizeof(nova_block_t): %zu\n", sizeof (nova_block_t));
    printf ("offsetof(nova_chunk_t, nv_blocks): %zu\n", offsetof (struct nova_chunk, nv_blocks[0]));

    return EXIT_SUCCESS;
}

/*******************************************************************************
 * TID HANDLING
 ******************************************************************************/

NOVA_DOCSTUB ();

nova_tid_t __nv_tid_impl__ ()
{
    /* okay, so this is gonna be kinda complicated.
     */
#ifdef __APPLE__
    /* Big thanks to https://stackoverflow.com/questions/1540603/mac-iphone-is-there-a-way-to-get-a-thread-identifier-without-using-objective-c */

    /*
     * On macos, there are a few options:
     *  pthread_self()
     *  pthread_threadid_np(pthread_t, uint64_t **)
     *  pthread_mach_thread_np(pthread_t) -> mach_port_name_t
     *  thread_info(mach_port_name_t, ...)
     *
     * pthread_self() returns a pthread_t, which, on darwin-xnu, is actually
     * a pointer to a _opaque_pthread_t structure which is 8176 + 16 bytes in
     * size on LP64.
     *      The pthread_t will be unique for existing pthreads in a process, but
     * it's just a blindly allocated pointer (_pthread_allocate calls mach_vm_map
     * or mach_vm_allocate), so it can't be relied on to be a process-unique ID,
     * as recycling is a function of the OS re-using the mapped memory.
     *
     * pthread_threadid_np calls the threadid_self syscall, which I can't really
     * find information on, so I can't really make assumptions about its behaviour.
     *
     * pthread_mach_thread_np(pthread_t) returns a mach_port_name_t, which would
     * seem to be unique to a process, _but_ it will return different values for
     * the same thread when called from different processes.
     *
     * However, the mach_port_name_t returned by pthread_mach_thread_np can be
     * passed to thread_info () with flavor=THREAD_IDENTIFIER_INFO, an obscure
     * XNU kernel call.
     */

    /* little trick from pthread/mach_dep.h */
#    if defined(__i386__) || defined(__x86_64__)
    void * _nv_pself;
    asm volatile("mov %%gs:%P1, %0"
                 : "=r"(_nv_pself)
#        ifdef __LP64__
                 : "i"(0 * sizeof (void *) + 0x60));
#        else
                 : "i"(0 * sizeof(void*) + 0x48)
#        endif

    mach_port_name_t _nv_mach_port = pthread_mach_thread_np (
        /* pthread_self () */
        /* slightly faster than pthread_self(); shaves off a few extra calls
            EDIT: for various reasons, not using this
            EDIT2: now we are */
        _nv_pself
        /* EDIT2: _pthread_getspecific_direct is an inline function defined in
         *        a header that is shipped standard-issue, so we're reproducing from
         *        there.
         */
        /* _pthread_getspecific_direct (_PTHREAD_TSD_SLOT_PTHREAD_SELF) */);
#    else
    mach_port_name_t _nv_mach_port = pthread_mach_thread_np (
        pthread_self ());
#    endif

    thread_identifier_info_data_t _nv_xnu_thrinfo;
    mach_msg_type_number_t _nv_mach_info_cnt = THREAD_IDENTIFIER_INFO_COUNT;
    /*
     * XNU defines thread_info(thread_act_t,
     *                         thread_flavor_t,
     *                         thread_info_t,
     *                         mach_msg_type_number_t)
     * thread_act_t is a typedef of thread_t
     * It seems that in kernelspace, thread_t is an alias of `struct thread`
     * or, in the outer kernel, `mach_port_t`.
     * However, we have reports that `thread_t` actually aliases `mach_port_name_t`
     * in proper userland, so that's what we pass in here.
     */
    kern_return_t _nv_kern_ret = thread_info (_nv_mach_port,
                                              THREAD_IDENTIFIER_INFO,
                                              (thread_info_t)&_nv_xnu_thrinfo,
                                              &_nv_mach_info_cnt);
    if (_nv_kern_ret != KERN_SUCCESS) {
        /* Charlie foxtrot. Die.
         */
        __nv_error (NVE_KERN_THREADID_XNU);
    }
    return _nv_xnu_thrinfo.thread_id;
#endif
#if defined(__linux__) || defined(NOVA_FORCE_GETTID)
    /* should be unique enough for our purposes. i think hope probably yes? */
    return gettid ();
#endif

    /* well, not much we can do at this point.
     */
    return 0;
}

#if !defined(__APPLE__)
_Thread_local nova_tid_t nv_tid;

nova_res_t nv_tid_init__ ()
{
    nv_tid = __nv_tid_impl__ ();

    return nova_ok;
}

nova_res_t nv_tid_drop__ ()
{
    /* Well, not much that we really need to do.
     * It's POD, so there's no real destruction that needs to occur.
     */
    return nova_ok;
}
#else
nova_res_t nv_tid_init__ ()
{
    return nova_ok;
}

nova_res_t nv_tid_drop__ ()
{
    return nova_ok;
}
#endif

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

/*******************************************************************************
 * CHUNK HANDLING
 ******************************************************************************/

NOVA_DOCSTUB ();

nova_res_t nv_chunk_create (nova_chunk_t ** nv_chunk)
{
    nvi_t _nv_chunksize_cache = nova_read_cfg (NV_CHUNKSIZE);

    /* In order for the block lookup to actually work properly, we need to ensure
     * that the chunk is properly aligned (i.e. aligned to its own size) */
    nvr_t r = posix_memalign ((void **)nv_chunk,
                              _nv_chunksize_cache,
                              _nv_chunksize_cache);
    if (__builtin_expect (r != 0, 0)) {
#if NOVA_MODE_DEBUG
        if (r == ENOMEM) /* OOM */
            __nv_error (NVE_CHUNKALLOC_DRY);
        if (r == EINVAL)
            __nv_error (NVE_BADCFG, NV_CHUNKSIZE, "nv_chunk_create(...): chunksize not a multiple of the system page size.");
#endif
        /* Normal failure: leave it to the caller, but don't print debug info.
         */
        return nova_fail;
    }
    (*nv_chunk)->nv_next = NULL;

    /* size of a single block */
    nvi_t _nv_smobj_poolsize_cache = nova_read_cfg (NV_SMOBJ_POOLSIZE);
    /* Yes, 64, not 63. */
    if (_nv_smobj_poolsize_cache < __builtin_offsetof(struct nova_chunk, nv_blocks[64])) {
#if NOVA_MODE_DEBUG
        __nv_error (NVE_BADCFG, NV_SMOBJ_POOLSIZE, "nv_chunk_create(...): small object poolsize too small.");
#endif
        return nova_fail;
    }
    uint8_t * locator = (uint8_t *)(*nv_chunk);
    locator += _nv_smobj_poolsize_cache;
    for (nvi_t i = 0; i < 63; i++) {
        nv_block_init (&(*nv_chunk)->nv_blocks[i], locator);
        locator += _nv_smobj_poolsize_cache;
    }
    return nova_ok;
}

nova_res_t nv_chunk_release_blocks_to (
    nova_chunk_t * nv_chunk,
    nova_heap_t * nv_receiver,
    nvi_t nv_begin,
    nvi_t nv_end)
{
    nova_lkg_t * _nv_ulkg = &nv_receiver->nv_lkgs[0];
    nvmutex_lock (&_nv_ulkg->nv_ll);
    for (nvi_t i = nv_begin; i < nv_end; i++) {
        nvmutex_lock (&nv_chunk->nv_blocks[i].nv_fpgm);
        __nv_ulkg_receive_block_nl_sl (&nv_chunk->nv_blocks[i]);
    }
    nvmutex_unlock (&_nv_ulkg->nv_ll);
    return nova_ok;
}

nova_res_t nova_chunk_destroy (nova_chunk_t * nv_chunk)
{
    free (nv_chunk);
    return nova_ok;
}

nova_res_t nv_chunk_destroy_chained (nova_chunk_t * nv_begin, nvi_t nv_number)
{
    /* nvi_t = size_t = uint64_t, so number - 1 is actually going to be UINT64_MAX. */
    nova_chunk_t *_nv_chunk = nv_begin, *_nv_swap = NULL;
    for (nvi_t i = 0; i <= (nv_number - 1); i++) {
#if NOVA_MODE_DEBUG
        if (_nv_chunk && nv_number != 0) {
            __nv_error (NVE_BADVAL, "nv_chunk_destroy_chained(chunk, number=%zu): [chunk+%zu] is NULL.", nv_number, i);
            return nova_fail;
        }
#endif
        if (_nv_chunk == NULL) {
            return nova_ok;
        }
        _nv_swap = _nv_chunk->nv_next;
        nv_chunk_destroy (_nv_chunk);
        _nv_chunk = _nv_swap;
    }
    return nova_ok;
}

/*******************************************************************************
 * HEAP HANDLING
 ******************************************************************************/

NOVA_DOCSTUB ();

nova_res_t nv_heap_create (nova_heap_t ** nv_heap)
{
    nvi_t _num_lkgs   = nova_read_cfg (NV_SMOBJ_POOLCOUNT);
    (*nv_heap)        = malloc (sizeof (nova_heap_t *)
                         + sizeof (nvi_t)
                         + (sizeof (nova_lkg_t)
                            * _num_lkgs));
    (*nv_heap)->nv_ln = _num_lkgs;
    if ((*nv_heap) == NULL) {
        return nova_fail;
    }
    return nv_heap_init (*nv_heap);
}

nova_res_t nv_heap_destroy (nova_heap_t * nv_heap)
{
    free (nv_heap);
    return nova_ok;
}

nova_res_t nv_heap_init (nova_heap_t * nv_heap)
{
    for (nvi_t i = 0; i < nv_heap->nv_ln; i++) {
        nv_lkg_init (&nv_heap->nv_lkgs[i]);
        nv_heap->nv_lkgs[i].nv_heap = nv_heap;
    }
    return nova_ok;
}

/*******************************************************************************
 * HEAP HANDLING : LOCAL HEAPS
 ******************************************************************************/

NOVA_DOCSTUB ();

nova_res_t __nv_local_heap_alloc (nova_heap_t * nv_heap, void ** nv_obj, nova_smobjsz_t nv_osz)
{
    /* The only piece of information that (currently) needs to be gotten from the heap
     * is the linkage for the given object size.
     */

    /* @NOTE: consider unifying this function and __nv_local_lkg_alloc, because
     * the logic doesn't really need to be separated here. Might actually
     * make a few things easier, even.
     *
     * Certainly make it faster, 'cuz this is a hot path.
     * But for now, wait and make sure that we _don't_ actually need the logic
     * separated.
     *
     * Besides, debug harnessing is easier when the functionality is separated.
     */
    return __nv_local_lkg_alloc (
        &nv_heap->nv_lkgs[__nv_lindex ((nvi_t)nv_osz)],
        nv_obj,
        nv_osz,
        nv_heap);
}

nova_res_t __nv_local_heap_drop (nova_heap_t * nv_heap)
{
    /* First, drop the linkages.
     */
    nvmutex_lock (&nv_heap->nv_parent_heap->nv_lkgs[0].nv_ll);
    for (nvi_t _nv_li = 0; _nv_li < nv_heap->nv_ln; _nv_li++) {
        nvmutex_lock (&nv_heap->nv_parent_heap->nv_lkgs[_nv_li].nv_ll);
        __nv_local_lkg_drop (&nv_heap->nv_lkgs[_nv_li]);
        nvmutex_unlock (&nv_heap->nv_parent_heap->nv_lkgs[_nv_li].nv_ll);
    }
    nvmutex_unlock (&nv_heap->nv_parent_heap->nv_lkgs[0].nv_ll);
    /* Finally, go back to the general case.
     */
    nv_heap_destroy (nv_heap);
    return nova_ok;
}

nova_res_t __nv_local_heap_pass_evac_nl_sl (nova_heap_t * nv_heap,
                                            nova_block_t * nv_ev_block)
{
    __nv_regional_heap_take_block_nl_sl (nv_heap->nv_parent_heap, nv_ev_block);
    return nova_ok;
}

/*******************************************************************************
 * HEAP HANDLING : REGIONAL HEAPS
 ******************************************************************************/

NOVA_DOCSTUB ();

nova_res_t __nv_regional_heap_create (nova_heap_t ** nv_heap)
{
    nvi_t _num_lkgs = nova_read_cfg (NV_SMOBJ_POOLCOUNT);
    /* We do a little bit of magic here; we want the heap's normal members to
     * be accessible at their normal places, and we want fast access to the
     * reference count member. The only logical place we can put the refcount,
     * because the heap is a DST, is at a negative offset, then. */
    (*nv_heap)              = malloc (sizeof (uint64_t)
                         + sizeof (nova_heap_t *)
                         + sizeof (nvi_t)
                         + (sizeof (nova_lkg_t)
                            * _num_lkgs));
    *((uint64_t *)*nv_heap) = 0;
    (*nv_heap)              = &((uint64_t *)*nv_heap)[1];
    (*nv_heap)->nv_ln       = _num_lkgs;
    if ((*nv_heap) == NULL) {
        return nova_fail;
    }
    return nv_heap_init (*nv_heap);
}

nova_res_t __nv_root_heap_create (nova_heap_t ** nv_heap)
{
    nvi_t _num_lkgs = nova_read_cfg (NV_SMOBJ_POOLCOUNT);
    /* We do a little bit of magic here; we want the heap's normal members to
     * be accessible at their normal places, and we want fast access to the
     * reference count member. The only logical place we can put the refcount,
     * because the heap is a DST, is at a negative offset, then. */
    (*nv_heap) = malloc (
        /* EXTRA MEMBER: */
        sizeof (nova_chunk_t *)
        + sizeof (uint64_t)
        + sizeof (nova_heap_t *)
        + sizeof (nvi_t)
        + (sizeof (nova_lkg_t)
           * _num_lkgs));

    *((nova_chunk_t *)*nv_heap) = NULL;
    ((uint64_t *)*nv_heap)[1]   = 0;
    (*nv_heap)                  = &((uint64_t *)*nv_heap)[2];
    (*nv_heap)->nv_ln           = _num_lkgs;
    if ((*nv_heap) == NULL) {
        return nova_fail;
    }
    return nv_heap_init (*nv_heap);
}

nova_res_t __nv_regional_heap_incref (nova_heap_t * nv_heap)
{
    __atomic_add_fetch(&(uint64_t *)nv_heap)[-1], 1, __ATOMIC_ACQ_REL)

    return nova_ok;
}

nova_res_t __nv_regional_heap_decref (nova_heap_t * nv_heap)
{
    if (0 == __atomic_sub_fetch (&(uint64_t *)nv_heap)[-1], 1, __ATOMIC_ACQ_REL))
        {
            if (nv_heap->nv_parent_heap != NULL) {
                __nv_regional_heap_drop (nv_heap);
            } else {
                /* Root heap persists until explicit destruction, so we just
                 * ignore this here.
                 */
            }
        }
    return nova_ok;
}

nova_res_t __nv_regional_heap_drop (nova_heap_t * nv_heap)
{
    if (nv_heap->nv_parent_heap != NULL) {
        nvmutex_lock (&nv_heap->nv_parent_heap->nv_lkgs[0].nv_ll);
        for (nvi_t _nv_li = 0; _nv_li < nv_heap->nv_ln; _nv_li++) {
            nvmutex_lock (&nv_heap->nv_parent_heap->nv_lkgs[_nv_li].nv_ll);
            __nv_local_lkg_drop (&nv_heap->nv_lkgs[_nv_li]);
            nvmutex_unlock (&nv_heap->nv_parent_heap->nv_lkgs[_nv_li].nv_ll);
        }
        nvmutex_unlock (&nv_heap->nv_parent_heap->nv_lkgs[0].nv_ll);
    } else {
        /* We are the root heap--the root heap persists until explicit destruction,
         * therefore this isn't because of some half-assed reference drop.
         *
         * So, at this point, all the root heap can really do is just release the
         * chunks and all.
         *
         * The problem is that we need a reference to the first chunk, and it's
         * not like that's super easy to find--that, however, is why we have a
         * separate creation function for root heaps: we can include an extra
         * pointer at offset -2.
         */
        nv_chunk_destroy_chained (&((nova_chunk_t *)nv_heap)[-2],
                                  /* nv_number = */ 0);
    }

    /* Short circuit nv_heap_destroy; atm, all it does is `free()` on the heap.
     */
    if (nv_heap->nv_parent_heap != NULL) {
        free ((nova_heap_t *)&((uint64_t *)nv_heap)[-1]);
    } else {
        /* Root heap needs to be deallocated from oofset -2 because of the chunk ptr. */
        free ((nova_heap_t *)&((uint64_t *)nv_heap)[-2]);
    }
    return nova_ok;
}

nova_res_t __nv_regional_heap_take_block_nl_sl (nova_heap_t * nv_heap,
                                                nova_block_t * nv_ev_block)
{
    /* If it's empty, then we can go ahead and pass it to the unsized linkage.
     */
    if (0 == __atomic_load_n (&nv_ev_block->nv_acnt, __ATOMIC_ACQUIRE)) {
        return __nv_ulkg_receive_block_nl_sl (&nv_heap->nv_lkgs[0], nv_ev_block);
    } else {
        return __nv_slkg_receive_block_nl_sl (
            &nv_heap->nv_lkgs[__nv_lindex (nv_ev_block->nv_osz)]);
    }
}

/*******************************************************************************
 * LINKAGE HANDLING
 ******************************************************************************/

NOVA_DOCSTUB ();

nova_res_t nv_lkg_init (nova_lkg_t * nv_lkg)
{
    /* Nothing too complicated about initializing the linkage: should be as
     * low-cost as possible, in fact, because we want as much work as we can
     * to occur just-in-time on the allocation paths, because we have no idea
     * whether this linkage will ever actually see any action.
     */
    nv_lkg->nv_head = NULL;
    /* the only expensive operation: initializing the mutex. */
    nvmutex_init (&nv_lkg->nv_ll);
    nv_lkg->nv_heap = NULL;

    /* this is basically a never-fail (ignoring the invalid-linkage-pointer case
     * and the mutex-init-gone-horribly-awry cases), so we're pretty much safe to
     * return OK in all circumstances. */
    return nova_ok;
}

/*******************************************************************************
 * LINKAGE HANDLING : LOCAL LINKAGES
 ******************************************************************************/

NOVA_DOCSTUB ();

nova_res_t __nv_local_lkg_drop (nova_lkg_t * nv_lkg)
{
    /* Allocation is not going to be happening here; this method is occurring in the owning thread.
     */
    nvmutex_lock (&nv_lkg->nv_ll);
    /* Optional: at this point, there are no more allocations, so foreign deallocations
     *           are actually fine to go through to the head; instead of doing an atomic
     *           load, we do a swap, and NULL the head. */
    nova_block_t * _nv_head = __atomic_exchange_n (&nv_lkg->nv_head, NULL, __ATOMIC_ACQ_REL);

    /* Couple notes on the implementation:
     *
     * We handle the head as a separate case, because we need to clear nv_blfl,
     * and it'd be a waste of cycles to do that for any of the others.
     *
     * We lock the FPGMs on all of the blocks that we release. Since we are the
     * owning thread here, the owning thread reference should be invalid from this
     * point onwards.
     * @TODO test this assumption
     *
     * We use the order left-of-head, right-of-head, head: the only significant
     * part of this is that the head is only handled after all the other cases.
     * This is necessary because left-of-head and right-of-head rely on the head's
     * side-pointers being in a valid state: if we evac head too early, then
     * the receiving linkage will (probably) modify nv_lkgpr and nv_lkgnx,
     * which would invalidate left-of-head and right-of-head if they were to
     * be processed after the head.
     */

    /* we use __nv_local_heap_pass_evac_nl_sl here.
     * nl -> non-locking evac (tells heap to assume that preparation has occurred)
     * sl -> state:locked (FPGM locked)
     */

    /* Handle left-of-head: dominant list-search side-link: nv_lkgpr
     */
    nova_block_t *_nv_curr = _nv_head->nv_lkgpr, *_nv_ncurr = NULL;
    while (_nv_curr != NULL) {
        nvmutex_lock (&_nv_curr->nv_fpgm);

        _nv_ncurr = _nv_curr->nv_lkgpr;
        /* Probably not entirely necessary, but we do it necessary, for neatness'
         * sake. This isn't a hotpath, but we might want to stick this in a debug
         * bracket at some point anyway.
         */
        _nv_curr->nv_lkgpr = _nv_curr->nv_lkgnx = NULL;
        /* nv_lkg->nv_heap is not modified in the linkage's lifetime, so no
         * ALdAcq here.
         */
        __nv_local_heap_pass_evac_nl_sl (((nova_lkg_t *)nv_lkg)->nv_heap, _nv_curr);

        _nv_curr = _nv_ncurr;
    }

    /* Handle right-of-head: dominant list-search side-link: nv_lkgnx
     */
    _nv_curr = _nv_head->nv_lkgnx->nv_lkgnx, _nv_ncurr = NULL;
    while (_nv_curr != NULL) {
        nvmutex_lock (&_nv_curr->nv_fpgm);

        _nv_ncurr          = _nv_curr->nv_lkgnx;
        _nv_curr->nv_lkgpr = _nv_curr->nv_lkgnx = NULL;
        __nv_local_heap_pass_evac_nl_sl (((nova_lkg_t *)nv_lkg)->nv_heap, _nv_curr);

        _nv_curr = _nv_ncurr;
    }

    /* Handle the head; not that much different from the normal case, *but* we
     * do need to get rid of nv_blfl.
     */
    nvmutex_lock (&_nv_head->nv_fpgm);
    _nv_head->nv_lkgnx = _nv_head->nv_lkgpr = NULL;
    /* @MARK head-fix */
    __c11_atomic_fetch_and (&_nv_head->nv_blfl, ~NOVA_BLFL_ISHEAD, __ATOMIC_ACQ_REL);

    __nv_local_heap_pass_evac_nl_sl (((nova_lkg_t *)nv_lkg)->nv_heap, _nv_head);

    /* Not sure if we need to do an unlock here, but it feels like some
     * standards-conforming thing that's probably better to do than not.
     * We need to drop the linkage modification mutex here because this is the
     * linkage's end-of-life.
     */
    nvmutex_unlock (&nv_lkg->nv_ll);
    nvmutex_drop (&nv_lkg->nv_ll);

    return nova_ok;
}

nova_res_t __nv_local_lkg_alloc (nova_lkg_t * nv_lkg,
                                 void ** nv_obj,
                                 nova_smobjsz_t nv_osz,
                                 nova_heap_t * nv_heap)
{
    /* ATTENTION: THIS IS A HOT PATH. THIS IS NOT A DRILL.
     *
     * Guidelines:
     *  keep memory operations to a minimum (maintain cache coherence)
     *  NO extraneous operations
     *  use hotpath prediction wherever possible
     */

    /* we know that there are no concurrent writers, but we put in an __atomic_load
     * for consistency's sake, though we do cache it for the rest of this function
     * because we don't know how expensive __atomic_load is on this platform
     */
    nova_block_t * _nvc_head = __atomic_load_n (&nv_lkg->nv_head, __ATOMIC_ACQUIRE);

    /*
     * CHECK FOR NULL HEAD; THIS IS pull.upstream-direct.
     */

    if (__builtin_expect (_nvc_head == NULL, 0)) {
        /* this linkage doesn't actually have any blocks in it yet, so we have to
         * add one. benefit: block is guaranteed to have at least one free object.
         */
        if (__builtin_expect (
                nova_fail == __nv_local_heap_req_block (nv_heap, nv_osz, &_nvc_head),
                0)) {
            (*nv_obj) = NULL;
            return nova_fail;
        }
        /* @MARK block cleanup */
        _nvc_head->nv_owner = nv_tid;
        _nvc_head->nv_lkgnx = NULL;
        _nvc_head->nv_lkgpr = NULL;
        _nvc_head->nv_lkg   = nv_lkg;
        /* We assume that the block was formatted by __nv_local_heap_req_block:
         * this is so that if we do accidentally hit a block that was resting
         * in a sized chain, we don't accidentally screw up it's deallocation
         * structures.
         */
        _nvc_head->nv_owner = nv_tid;

        /* Set up nv_blfl.
         */
        __c11_atomic_fetch_or (&_nvc_head->nv_blfl, NOVA_BLFL_ISHEAD, __ATOMIC_ACQ_REL);

        nvmutex_lock (&nv_lkg->nv_ll);
        __atomic_store_n (&nv_lkg->nv_head, _nvc_head, __ATOMIC_RELEASE);

        /*
         * note: the block is guaranteed to have it's nv_fpgm in a locked state.
         *       we don't unlock it until after it's set up as head to prevent
         *       foreign deallocations.
         */
        nvmutex_unlock (&_nvc_head->nv_fpgm);

        /* Now, all we have to do is unlock the linkage mutex. */
        nvmutex_unlock (&nv_lkg->nv_ll);

        return __nv_block_alloc (_nvc_head, nv_obj);
    }

    /*
     * TRY A NORMAL ALLOCATION.
     */

    if (__builtin_expect (nova_ok == __nv_block_alloc (_nvc_head, nv_obj), 1)) {
        return nova_ok;
    }

    /*
     * TRY A SLIDE; THIS IS slide.right.
     */

    /* okay, so, block allocation failed, time to try to slide a block.
     */

    /* <edit:ignore> A slight problem here: if the slidee is deallocated from concurrently
     * to this function, it locks the FPGM before the LL, while this function
     * locks the LL after locking the FPGM.
     * Basically, what we have to do here is try to lock the FPGM, and if
     * that fails, then we unlock the LL, sleep for a few nanoseconds, and relock
     * the LL, and retry </edit:ignore>
     *
     * EDIT: we don't actually need to do this; we're switching __nv_block_dealloc
     * to locking the LL instead of the FPGM on the Block:Empty case.
     */

    nvmutex_lock (&nv_lkg->nv_ll);
    _nvc_head = __atomic_load_n (&nv_lkg->nv_head, __ATOMIC_ACQUIRE);
    if (_nvc_head->nv_lkgnx != NULL) {
        /* This is a relatively simple procedure; the FPGM locks prevent any
         * concurrent modification because we're currently located in the
         * owning thread.
         * EDIT: There is, however, as indicated previously, a possible deadlock
         * that could emerge here, so we take care of that here.
         */

        /* Lock the current head.
         */
        nvmutex_lock (&_nvc_head->nv_fpgm);

        /* Lock the slidee, and headify it <edit:ignore>(protecting for deadlock)</edit:ignore>.
         * Don't need to worry about deadlock anymore.
         */
        nvmutex_lock (&_nvc_head->nv_lkgnx->nv_fpgm);
#if 0
        while (nova_fail == nvmutex_trylock (&_nvc_head->nv_lkgnx->nv_fpgm)) {
            nvmutex_unlock (&nv_lkg->nv_ll);
            /* sleep for ~1 microsecond */
            nanosleep ((const struct timespec[]) { { 0, 1000 } }, NULL);

            nvmutex_lock (&nv_lkg->nv_ll);
        }
#endif

        /* Only unhead the current head once we finished the anti-deadlock loop.
         */
        __c11_atomic_fetch_and (&_nvc_head->nv_blfl, ~NOVA_BLFL_ISHEAD, __ATOMIC_ACQ_REL);

        __c11_atomic_fetch_or (&_nvc_head->nv_blfl, NOVA_BLFL_ISHEAD, __ATOMIC_ACQ_REL);

#if NOVA_MODE_DEBUG
        /* Scope _nvx_head locally so it doesn't leak.
         */
        {
            nova_block_t * _nvx_head = _nvc_head;
            __atomic_compare_exchange_n (&nv_lkg->nv_head, &_nvx_head, _nvc_head->nv_lkgnx, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
            if (_nvx_head != _nvc_head) {
                __nv_error (NVE_DESYNC, "__nv_local_lkg_alloc: unsanctioned concurrent modification to nv_head");
                return nova_fail;
            }
        }
#else
        __atomic_store_n (&nv_lkg->nv_head, _nvc_head->nv_lkgnx, __ATOMIC_RELEASE);
#endif
        nvmutex_unlock (&_nvc_head->nv_fpgm);
        _nvc_head = _nvc_head->nv_lkgnx;
        /* Unlock the new head, now that blfl has been dealt with.
         */
        nvmutex_unlock (&_nvc_head->nv_fpgm);

        if (__builtin_expect (nova_ok == __nv_block_alloc (_nvc_head, nv_obj), 1)) {
            return nova_ok;
        }

        /* this should be impossible. */
        __nv_error (NVE_IMPOSSIBLE, "__nv_local_lkg_alloc: nova has entered an invalid state: right-of-head block on linkage could not be allocated from.");
    }

    /*
     * LAST RESORT: SLIDE LEFT slide.left AND PULL FROM UPSTREAM pull.upstream-req.
     */

    /* okay, so, block allocation failed, and we need to grab a new block.
     * the linkage has already been locked, so we just have to do the following: */

    /* step 1: lock the foreign deallocation mutex on the head. the head is immobile,
     * so we can perform surgery. */
    nvmutex_lock (&_nvc_head->nv_fpgm);

    /* step 2: remove the head flag. we can do this 'cuz we got the foreign deallocation
     *         mutex locked-- head flag only ever changes while the FPGM is locked. */
    __c11_atomic_fetch_and (&_nvc_head->nv_blfl, ~NOVA_BLFL_ISHEAD, __ATOMIC_ACQ_REL);

    nova_block_t * _nvn;
    if (__builtin_expect (
            nova_fail == __nv_local_heap_req_block (nv_heap, nv_osz, &_nvn),
            0)) {
        (*nv_obj) = NULL;
        return nova_fail;
    }

    _nvn->nv_owner = nv_tid;
    _nvn->nv_lkg   = nv_lkg;
    _nvn->nv_owner = nv_tid;
    /* Again: formatting is handled by __nv_local_heap_req_block. */

    __c11_atomic_fetch_or (&_nvn->nv_blfl, NOVA_BLFL_ISHEAD, __ATOMIC_ACQ_REL);

    /* the side pointers of the blocks in this linkage can only be modified
     * while the linkage modification mutex is locked.
     */
    _nvn->nv_lkgpr = _nvc_head;
    _nvn->nv_lkgnx = _nvc_head->nv_lkgnx;

    _nvc_head->nv_lkgnx = _nvn;
    if (_nvn->nv_lkgnx != NULL) {
        _nvn->nv_lkgnx->nv_lkgpr = _nvn;
    }
    nvmutex_unlock (&_nvc_head->nv_fpgm);
    __atomic_store_n (&nv_lkg->nv_head, _nvn, __ATOMIC_RELEASE);

    /* Don't unlock the new head's FPGM until it's in place */
    nvmutex_unlock (&_nvn->nv_fpgm);

    /* At this point, there's nothing we can really do.
     */
    return __nv_block_alloc (_nvc_head, nv_obj);
}

/*******************************************************************************
 * BLOCK HANDLING
 ******************************************************************************/

NOVA_DOCSTUB ();

nova_res_t nv_block_init (nova_block_t * nv_block, void * nv_block_memory)
{
    nv_block->nv_base  = nv_block_memory;
    nv_block->nv_fpl   = nv_block_memory;
    nv_block->nv_fpg   = NULL;
    nv_block->nv_osz   = 0;
    nv_block->nv_blfl  = 0;
    nv_block->nv_ocnt  = 0;
    nv_block->nv_acnt  = 0;
    nv_block->nv_owner = 0;
    nv_block->nv_lkgnx = NULL;
    nv_block->nv_lkgpr = NULL;
    nv_block->nv_lkg   = NULL;
    nvmutex_init (&nv_block->nv_fpgm);

    return nova_ok;
}

nova_res_t __nv_block_fmt (nova_block_t * nv_block, nova_smobjsz_t nv_osz)
{
    /*
     * Operating assumption: block is empty, with no extant referrents.
     */
    nvi_t _nv_smobjpoolsz = nova_read_cfg (NV_SMOBJ_POOLSIZE);
#if NOVA_MODE_DEBUG
    if (!_nv_smobjpoolsz) {
        /* To be honest, it should be a lot more than nonzero, but the extra constraints
         * are kind of a bear to maintain.
         * @TODO: Put in the proper constraints here.
         * Anyway, we're trying to catch the case where the user just doesn't set up the config.
         */
        __nv_error (NVE_BADCFG, NV_SMOBJ_POOLSIZE, "small object poolsize must be nonzero");
        return nova_fail;
    }
#endif
    nv_block->nv_osz  = nv_osz;
    nv_block->nv_ocnt = _nv_smobjpoolsz / nv_osz;
    __atomic_store_n (&nv_block->nv_acnt, 0, __ATOMIC_RELEASE);

    /* a good idea? question mark?
     * but this should honestly behave similarly to the block deallocation procedures
     * so, we keep in debug-mode only.
     */

/* #if 1 */
#if NOVA_MODE_DEBUG
    memset (nv_block->nv_base, 0, _nv_smobjpoolsz);
#endif

    /* Set up the free list. Yes, there's a faster way to do this, but for now,
     * for debugging purposes, I'll just leave it like this in a (hopefully)
     * simpler and less bug-prone state.
     * (if you're wondering, we're using random-access instead of iterative
     * modification patterning).
     */
    uint8_t * _nv_bloff = NULL;
    for (nvi_t _nv_bloffi = 0; _nv_bloffi < (nv_block->nv_ocnt - 1); _nv_bloffi++) {
        _nv_bloff                = &((uint8_t *)nv_block->nv_base)[_nv_bloffi * nv_osz];
        *((uint16_t *)_nv_bloff) = (_nv_bloff + nv_osz) - ((uint8_t *)nv_block->nv_base);
    }

    /* Need to set up the last link in the free chain with the special value 0xffff;
     * this indicates to whoever's unwinding the free list that the end has been reached;
     * random detail, but on certain occasions, one will see double free lists,
     * one for local free and one for the global deallocation.
     */
    _nv_bloff                = &((uint8_t *)nv_block->nv_base)[nv_osz * (nv_block->nv_ocnt - 1)];
    *((uint16_t *)_nv_bloff) = 0xffff;

    /* Finally, set up the actual free list pointers.
     */
    nv_block->nv_fpl = nv_block->nv_base;
    __atomic_store_n (&nv_block->nv_fpg, NULL, __ATOMIC_RELEASE);

    /* Well, this should always be successful (only operates on internal memory,
     * no object-external side effects, and doesn't really do anything weird
     * _except_ in debug mode).
     *
     * If there are any problems with all the direct memory accessing occurring,
     * those'll come up as segfaults, not as anything we can actually reasonably
     * catch from a few #if'd checks in debug mode.
     */
    return nova_ok;
}

nova_res_t __nv_block_alloc (nova_block_t * nv_block, void ** nv_obj)
{
    /* Effect FPL to be non-null.
     */
    if (__builtin_expect (nv_block->nv_fpl != NULL, 1)) {
        return __nv_block_alloc_inner (nv_block, nv_obj);
    }

    /* We do _not_ expect the block to be completely empty. In any case, we'd rather
     * assume that it isn't, because that's the higher-traffic case most of the
     * time.
     */
    if (__builtin_expect (__atomic_load_n (&nv_block->nv_fpg, __ATOMIC_ACQUIRE) != NULL, 1)) {
        nvmutex_lock (&nv_block->nv_fpgm);
        nv_block->nv_fpl = __atomic_exchange_n (&nv_block->nv_fpg, NULL, __ATOMIC_ACQUIRE);
        nvmutex_unlock (&nv_block->nv_fpgm);

        /* There is no situation in which FPL is NULL right now.
         * The only place where FPG can be nulled is this allocation function,
         * which is only ever called from one thread.
         */
        /* if (__builtin_expect (nv_block->nv_fpl != NULL, 1)) { */
        return __nv_block_alloc_inner (nv_block, nv_obj);
        /* } */
    }

    return nova_fail;
}

nova_res_t __nv_block_alloc_inner (nova_block_t * nv_block, void ** nv_obj)
{
    /* Allocate an object into nv_obj from the FPL. The FPL is guaranteed to
     * be non-null.
     */

    *nv_obj = nv_block->nv_fpl;

    /* _n_ext _o_bject _off_set */
    const uint16_t _nv_nooff = *((uint16_t *)(*nv_obj));
    if (__builtin_expect (_nv_nooff != 0xffff, 1)) {
        /* 0xffff is a special value meaning end-of-free-list.
         * I chose 0xffff because, well, it's the closest I can get to an out-of-range
         * value. Technically it's possible to set it up so that this value would
         * occur in a valid context, but it wouldn't really do anything except make
         * 1 bytes inaccessible, so I'm going to call it good.
         */
        nv_block->nv_fpl = NULL;
    } else {
        nv_block->nv_fpl = (uint8_t *)nv_block->nv_base + _nv_nooff;
    }

    return nova_ok;
}

/* UTILITY ZONE ***************************************************************/

/* __atomic */ uintptr_t _nv_dealloc_csize_cache     = 0;
/* __atomic */ uintptr_t _nv_dealloc_smobjplsz_cache = 0;

nova_res_t __nv_cache_reload_from_cfg (uintptr_t nv_override, nvcfg_t nv_cfg, uintptr_t * nv_cache)
{
    if (nv_override > 0)
        __atomic_store_n (nv_cache, nv_override, __ATOMIC_RELEASE);
    else
        __atomic_store_n (nv_cache, nova_read_cfg (NV_CHUNKSIZE), __ATOMIC_RELEASE);

    return nova_ok;
}

/* END UTILITY ZONE ***********************************************************/

nova_res_t __nv_dealloc_smobj (void * nv_obj)
{
    /* ALERT: THIS IS A HOT PATH.
     */

    const uintptr_t _nv_csize_lcache = __atomic_load_n (&_nv_dealloc_csize_cache,
                                                        __ATOMIC_ACQUIRE);
    const uintptr_t _nv_sops_lcache  = __atomic_load_n (&_nv_dealloc_smobjplsz_cache,
                                                       __ATOMIC_ACQUIRE);

    /* We know the following: _nv_csize_lcache is a power-of-2, and the chunk
     * allocation is always aligned to the chunksize.
     * Say _nv_csize_lcache = 0x10_0000. We can find the chunk address by:
     *  1. subtracting 1, to fill out all the lower bits: 0x0f_ffff
     *  2. performing unary bitwise negation, to create a mask for everything except
     *     the lower bits: 0xffff_ffff_fff0_0000
     *  3. performing bitwise and with the mask and the object address, to find
     *     the base of the chunk.
     */
    nova_chunk_t * _nv_chunk = (nova_chunk_t *)((uintptr_t)nv_obj & ~(_nv_csize_lcache - 1));
    /* Grab all the chunk-internal bits of the object's address.
     */
    nvi_t _nv_ooff_ic       = (uintptr_t)nv_obj & (_nv_csize_lcache - 1);
    nvi_t _nv_bloff_ic      = _nv_ooff_ic / _nv_sops_lcache;
    nova_block_t * nv_block = &_nv_chunk->nv_blocks[_nv_bloff_ic - 1];

#if NOVA_MODE_DEBUG
    return __nv_block_dealloc (nv_block, nv_obj);
}

nova_res_t __nv_block_dealloc (nova_block_t * nv_block, void * nv_obj)
{
#endif

#if NOVA_MODE_DEBUG
    nova_res_t nvr = nova_ok;
    nvr            = __nvd_validate_block (nv_block);
    if (nvr == nova_fail)
        return nova_fail;
    nvr = __nvd_validate_range (nv_block->nv_base, nova_read_cfg (NV_SMOBJ_POOLSIZE), nv_obj);
    if (nvr == nova_fail)
        return nova_fail;
#endif

    /* Expect deallocations to be local, not global.
     *
     * \proof
     * On the use of an atomic load here:
     * [P1] It's ok if local deallocs go to global, but not vice versa.
     * P1 will be satisfied as long as nv_owner is valid and up-to-date.
     * nv_owner changes when:
     *  - block is evacuated
     *      (block is unsized) -> no items to deallocate, so no deallocation-active
     *                            possible referrants. [P1] remains satisfied.
     *      (block is sized) -> the heap is dying. there will no longer be any
     *                          local operations. [P1] remains satisfied.
     *  - block is requested
     *      (block is unsized) -> no items to deallocate, so no deallocation-active
     *                            possible referrants. [P1] remains satisfied.
     *      (block is sized) -> the block is from a dead heap. there will no longer
     *                          be any local operations on this block, there are
     *                          no possible deallocation-active local referrants.
     *                          [P1] remains satisfied.
     *
     * Therefore, for the purposes of P1, nv_owner will always be valid.
     */
    if (__builtin_expect (nv_tid == __atomic_load_n (&nv_block->nv_owner, __ATOMIC_ACQUIRE), 1)) {
        /* Shuffle the object back into the free chain.
         */

        /* First, check if there is an established local free list currently in operation.
         * We assume that it has, and optimize for this case.
         */
        if (__builtin_expect (nv_block->nv_fpl != NULL, 1)) {
            /* Get the _bl_ock _i_nternal _off_set of the current FPL, and
             * stick that into nv_obj.
             *
             * NOTE: offset is a byte offset, not an object offset.
             */
            uint16_t _nv_blioff = ((uint8_t *)nv_block->nv_fpl - (uint8_t *)nv_block->nv_base);
            *(uint16_t *)nv_obj = _nv_blioff;
            /* Push nv_obj onto the free list.
             */
            nv_block->nv_fpl = nv_obj;
        } else {
            /* Local free list is currently empty, so that means we're establishing
             * a new free list; to indicate this, we tag the object with 0xffff,
             * which indicates that this is the last item in a free list.
             */
            *(uint16_t *)nv_obj = 0xffff;
            /* Push the tagged nv_obj onto the free list.
             */
            nv_block->nv_fpl = nv_obj;
        }
    } else {
        nvmutex_lock (&nv_block->nv_fpgm);
        /* Locked, so no modifications for the duration of the lock.
         */
        void * _nv_fpg_cache = __atomic_load_n (&nv_block->nv_fpg, __ATOMIC_ACQUIRE);
        if (__builtin_expect (_nv_fpg_cache != NULL, 1)) {
            /* Store the offset of the previously available object on the global
             * free list as a byte-offset in nv_obj.
             */
            uint16_t _nv_blioff = ((uint8_t *)_nv_fpg_cache - (uint8_t *)nv_block->nv_base);
            *(uint16_t *)nv_obj = _nv_blioff;
            /* Push nv_obj onto the global free list.
             */
            __atomic_store_n (&nv_block->nv_fpg, nv_obj, __ATOMIC_RELEASE);
        } else {
            /* Global free list is currently empty, so we're establishing a new
             * free list; to indicate this, we tag the object with 0xffff,
             * which indicates that this is the last item in a free list.
             */
            *(uint16_t *)nv_obj = 0xffff;
            /* Push the tagged nv_obj to the global free list.
             */
            __atomic_store_n (&nv_block->nv_fpg, nv_obj, __ATOMIC_RELEASE);
        }

        nvmutex_unlock (&nv_block->nv_fpgm);
    }

    /*
     * Now for the tricky part.
     */

#define _NV_islalh(___nv_b___) \
    (__c11_atomic_load (&(___nv_b___)->nv_blfl, __ATOMIC_ACQUIRE) & NOVA_BLFL_ISHEAD)

    nova_smobjcnt_t _nv_racnt = __atomic_sub_fetch (&nv_block->nv_acnt, 1, __ATOMIC_ACQ_REL);
    if (0 == _nv_racnt) {
        /* If the allocation count becomes zero from this, and this is not the
         * head block of a local linkage, then ensure that no allocations occur and
         * that it does not become the head block of a local linkage, and inform
         * the parent linkage that this block is empty.
         *
         * <EDIT:IGNORE>
         * IGNORE [P2]: Once the FPGM is locked, if the block is not the head of a local linkage,
         * IGNORE       and if there are no allocations on the block, then there will be no
         * IGNORE       local operations completed on the block until the FPGM is unlocked.
         * IGNORE
         * IGNORE Therefore, we must ensure that P2 is true.
         * </EDIT:IGNORE>
         *
         * To prevent an FPGM-LL deadlock, we have to ensure synchronization of
         * LL locking: first, lock the LL, then the FPGM.
         *
         * We retain the FPGM lock to prevent concurrent mutation of the block's
         * state.
         */
        if (__builtin_expect (!_NV_islalh (nv_block), 1)) {
            nova_lkg_t * _nvc_lkg = __atomic_load_n (&nv_block->nv_lkg, __ATOMIC_ACQUIRE);
            nvmutex_lock (&_nvc_lkg->nv_ll);
            nvmutex_lock (&nv_block->nv_fpgm);
            if (__builtin_expect (!_NV_islalh (nv_block), 1)) {
                if (0 == __atomic_load_n (&nv_block->nv_acnt, __ATOMIC_ACQUIRE)) {
                    /* Well, we're good at this point.
                     * NOTE: nv_block will be passed up with its FPGM locked; that
                     * should be handled on landing.
                     */
                    __nv_lkg__lal_empty (nv_block);
                    goto nv_block_dealloc___secL___;
                }
            }

            /* it became the head (or it flipped in and out and got some allocations
             * in between) -> ignore */
            nvmutex_unlock (&nv_block->nv_fpgm);
            nvmutex_unlock (&_nvc_lkg->nv_ll);
        }
    }
    /* We use == so that this is only triggered _once_; we don't want to waste costly
     * extra cycles on this, especially when
     */
    if (_nv_racnt == (nv_block->nv_ocnt / 2)) {
        /* empty-enough condition */
        /* Although we're only modifying the side-linkage pointers, we do still
         * have to check 0!=acnt, therefore our first instinct might be to lock
         * the FPGM, so we don't get extra concurrent deallocations that:
         *  a) empty it
         *  b) empty it and push.upstream it
         *  c) empty it, push.upstream it, and then pull.downstream it for an allocation
         * However, all of the above scenarios require first locking the LL,
         * so we can actually proceed without locking the FPGM as the LL is already locked.
         */
        nova_lkg_t * _nvc_lkg = __atomic_load_n (&nv_block->nv_lkg, __ATOMIC_ACQUIRE);
        nvmutex_lock (&_nvc_lkg->nv_ll);
        /* nvmutex_lock (&nv_block->nv_fpgm); */
        if (__builtin_expect (!_NV_islalh (nv_block), 1)) {
            if (0 != __atomic_load_n (&nv_block->nv_acnt, __ATOMIC_ACQUIRE)) {
                /* We locked it, it's nonzero, and it's not head:
                 * This block is _not_ vulnerable to empty-condition occurring.
                 *
                 * Tell the linkage that this block is empty enough.
                 */
                __nv_lkg__lal_empty_e (nv_block);
                goto nv_block_dealloc___secL___;
            }
        }
        /* IGNORE Block became empty, yield to empty-condition. */
        /* nvmutex_unlock (&nv_block->nv_fpgm); */

        /* Unlock the LL; we don't actually care that this is a cached value,
         * because we want to lock the LL that was locked up above, so it's actually
         * okay if nv_block->nv_lkg has changed (spoiler alert: it hasn't), so
         * we just use the cached value.
         */
        nvmutex_unlock (&_nvc_lkg->nv_ll);
    }

nv_block_dealloc___secL___:
    return nova_ok;
}
