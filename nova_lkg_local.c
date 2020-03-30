#include "nova.h"

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
                nova_fail == __nv_local_heap_req_block (nv_heap, __nv_canonicalize_osz (nv_osz), &_nvc_head),
                0)) {
            (*nv_obj) = NULL;
            return nova_fail;
        }
        /* @MARK block cleanup */
        _nvc_head->nv_owner = __nv_tid ();
        _nvc_head->nv_lkgnx = NULL;
        _nvc_head->nv_lkgpr = NULL;
        _nvc_head->nv_lkg   = nv_lkg;
        /* We assume that the block was formatted by __nv_local_heap_req_block:
         * this is so that if we do accidentally hit a block that was resting
         * in a sized chain, we don't accidentally screw up it's deallocation
         * structures.
         */

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
            nova_fail == __nv_local_heap_req_block (nv_heap, __nv_canonicalize_osz (nv_osz), &_nvn),
            0)) {
        (*nv_obj) = NULL;
        return nova_fail;
    }

    _nvn->nv_owner = __nv_tid ();
    _nvn->nv_lkg   = nv_lkg;
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
