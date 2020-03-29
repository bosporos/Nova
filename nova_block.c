#include "nova.h"
/* memcpy */
#include <string.h>

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
                    __nv_lkg_empty (nv_block);
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
                __nv_lkg_empty_e (nv_block);
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
