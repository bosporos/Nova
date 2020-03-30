#include "nova.h"
/* malloc */
#include <stdlib.h>

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
    (*nv_heap) = malloc (
        /* Refcount at heap - 1 */
        sizeof (uint64_t)
        /* nova_heap_t */
        + sizeof (nova_heap_t *)
        + sizeof (nvi_t)
        + (sizeof (nova_lkg_t)
           * _num_lkgs));

    /* Check the malloc result.
     */
    if ((*nv_heap) == NULL) {
        return nova_fail;
    }

    /* Set up refcount and skip the pointer to the heap.
     */
    *((uint64_t *)*nv_heap) = 0;
    (*nv_heap)              = (void *)&((uint64_t *)*nv_heap)[1];

    /* Finish up with normal heap initialization.
     */
    return nv_heap_init (*nv_heap, _num_lkgs);
}

nova_res_t __nv_root_heap_create (nova_heap_t ** nv_heap)
{
    nvi_t _num_lkgs = nova_read_cfg (NV_SMOBJ_POOLCOUNT);
    /* We do a little bit of magic here; we want the heap's normal members to
     * be accessible at their normal places, and we want fast access to the
     * reference count member. The only logical place we can put the refcount,
     * because the heap is a DST, is at a negative offset, then.
     * We also need to put in a root chunk list pointer, and we do that here.
     */
    (*nv_heap) = malloc (
        /* Chunk pointer at heap - 2 */
        sizeof (nova_chunk_t *)
        /* Refcount at heap - 1 */
        + sizeof (uint64_t)
        /* nova_heap_t */
        + sizeof (nova_heap_t *)
        + sizeof (nvi_t)
        + (sizeof (nova_lkg_t)
           * _num_lkgs));
    /* Check the malloc result.
     */
    if ((*nv_heap) == NULL) {
        return nova_fail;
    }

    /* Set up the chunk list root pointer, 'cuz we're the root heap.
     */
    *((nova_chunk_t **)nv_heap) = NULL;
    /* Set up the reference count variable, and skip the heap pointer past all
     * this nasty business;
     * IMPORTANT: assumes 64-bit pointers.
     */
    ((uint64_t *)*nv_heap)[1] = 0;
    (*nv_heap)                = (void *)&((uint64_t *)*nv_heap)[2];
    /* Perform the normal heap initialization.
     */
    return nv_heap_init (*nv_heap, _num_lkgs);
}

nova_res_t __nv_regional_heap_incref (nova_heap_t * nv_heap)
{
    /* Nice and simple; use fetch_add so as not to create a temporary.
     */
    __atomic_add_fetch (&((uint64_t *)nv_heap)[-1], 1, __ATOMIC_ACQ_REL);

    return nova_ok;
}

nova_res_t __nv_regional_heap_decref (nova_heap_t * nv_heap)
{
    /* Basic idea: decrement refcount, and if the decrement zeroes the refcount,
     * then destroy the heap.
     *
     * Slightly more complicated than incrementing the reference, mainly because
     * there's a special case here: the _root_ heap (which happens is semantically
     * defined as a regional heap) may only be destroyed explicitly.
     *
     * Note the sub_fetch instead of fetch_sub; sub_fetch returns the post-op
     * value, fetch_sub returns the pre-op value.
     */
    if (0 == __atomic_sub_fetch (&((uint64_t *)nv_heap)[-1], 1, __ATOMIC_ACQ_REL)) {
        /* Check if it's the root heap
         */
        if (nv_heap->nv_parent_heap != NULL) {
            /* If it's just an ordinary regional heap, then drop it.
             */
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
            __nv_regional_lkg_drop (&nv_heap->nv_lkgs[_nv_li]);
            nvmutex_unlock (&nv_heap->nv_parent_heap->nv_lkgs[_nv_li].nv_ll);
        }
        nvmutex_unlock (&nv_heap->nv_parent_heap->nv_lkgs[0].nv_ll);

        /* Notify the parent heap of the drop (before nv_heap is free'd).
         */
        __nv_regional_heap_decref (nv_heap->nv_parent_heap);
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

nova_res_t __nv_regional_heap_take_evac_block_nl_sl (nova_heap_t * nv_heap,
                                                     nova_block_t * nv_ev_block)
{
    /* The ULKG LL and appropriate SLKG LL are already locked for this heap.
     * Theoretically,
     */

    /* If it's empty, then we can go ahead and pass it to the unsized linkage.
     */
    if (0 == __atomic_load_n (&nv_ev_block->nv_acnt, __ATOMIC_ACQUIRE)) {
        /* Unsized linkage is always going to be linkage 0.
         */
        if (nova_ok
            == __nv_regional_lkg_receive_block_nl_sl (
                &nv_heap->nv_lkgs[0],
                nv_ev_block)) {
            return nova_ok;
        }
    } else {
        /* If it's sized, then we just pass it on.
         * There's porbably a way to funnel the LI up from the dying heap, but
         * that'd extra complexity n' stuff.
         */
        if (nova_ok
            == __nv_regional_lkg_receive_block_nl_sl (
                &nv_heap->nv_lkgs[__nv_lindex (nv_ev_block->nv_osz)],
                nv_ev_block)) {
            return nova_ok;
        }
    }

    __nv_error (
        NVE_IMPOSSIBLE,
        "__nv_regional_heap_take_evac_block_nl_sl(%p, %p):"
        " heap's linkages refused to receive block.");

    return nova_fail;
}

nova_res_t __nv_regional_heap_req_block (nova_heap_t * nv_heap,
                                         nova_smobjsz_t nv_osz,
                                         nova_block_t ** nv_block)
{
    if (nova_ok == nv_lkg_req_block (&nv_heap->nv_lkgs[0], nv_block)) {
        __nv_block_fmt (*nv_block, nv_osz);
        return nova_ok;
    }

    if (nova_ok == nv_lkg_req_block (&nv_heap->nv_lkgs[__nv_lindex (nv_osz)], nv_block)) {
        return nova_ok;
    }

    if (nv_heap->nv_parent_heap != NULL) {
        return __nv_regional_heap_req_block (nv_heap->nv_parent_heap,
                                             nv_osz,
                                             nv_block);
    } else {
        /* Root heap.
         */

        {
            nova_chunk_t * _nv_chunk;
            if (__builtin_expect (nova_ok != nv_chunk_create (&_nv_chunk), 0)) {
                __nv_error (NVE_CASCADE,
                            "__nv_regional_heap_req_block(%p, %us, %p):"
                            " cascading error imminent: chunk allocation failed from"
                            " root heap");
                return nova_fail;
            }
            __nv_chunk_release_blocks_to (_nv_chunk, nv_heap, 0, 63);
            /* Take care of the chunk list.
             */
            nv_chunk_bind_to_root (_nv_chunk, nv_heap);
        }

        if (nova_ok == nv_lkg_req_block (&nv_heap->nv_lkgs[0], nv_block)) {
            __nv_block_fmt (*nv_block, nv_osz);
            return nova_ok;
        } else {
            return nova_fail;
        }
    }
}
