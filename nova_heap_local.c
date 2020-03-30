#include "nova.h"

/* free */
#include <stdlib.h>

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

    /* Notify the parent heap of destruction; we know it's a regional, so we just
     * pass it a decref message.
     */
    __nv_regional_heap_decref (nv_heap->nv_parent_heap);

    /* Finally, go back to the general case.
     */
    free (nv_heap);
    return nova_ok;
}

nova_res_t __nv_local_heap_pass_evac_nl_sl (nova_heap_t * nv_heap,
                                            nova_block_t * nv_ev_block)
{
    return __nv_regional_heap_take_evac_block_nl_sl (nv_heap->nv_parent_heap, nv_ev_block);
}

nova_res_t __nv_local_heap_req_block (nova_heap_t * nv_heap,
                                      nova_smobjsz_t nv_osz,
                                      nova_block_t ** nv_block)
{
    /* NOTE: we leave it up to the call site to have canonicalized the osz
     */

    /* Formatting is left to the heap, not to the callsite, because the heap is
     * the one that actually can see which linkage this block came from.
     *
     * We take this measure because formatting non-empty blocks (i.e. any formatting
     * any block that came from a sized heap) will destroy the free chain, clear
     * the block's memory, and generally completely invalidate any external
     * references to it. Which is, y'know, kinda not cool for an allocator to do.
     */

    if (nova_ok == nv_lkg_req_block (&nv_heap->nv_lkgs[0], nv_block)) {
        /* It's unsized, so we format it here.
         */
        __nv_block_fmt (*nv_block, nv_osz);

        return nova_ok;
    }
    /* Don't try to allocate from a sized heap--the appropriately sized heap is
     * the one requesting the block, after all, so it would be _very_ unproductive
     * (and also undefined behaviour because the LL mutexes are _not_ reentrant).
     *
     * Instead, we go directly to the regional heap (if we have one, which we
     * really should).
     */
    if (__builtin_expect (nv_heap->nv_parent_heap != NULL, 1)) {
        /* The regional is a bicameral heap just like this one, so it has
         * jurisdiction on block formatting.
         */
        return __nv_regional_heap_req_block (nv_heap->nv_parent_heap,
                                             nv_osz,
                                             nv_block);
    } else {
#if NOVA_MODE_DEBUG
        /* In release, this should fail quietly, but we _do_ want to make sure
         * that it fails loudly in debug mode.
         */
        __nv_error (NVE_HIERARCHY, "__nv_local_heap_req_block(%p): orphaned local heap.", nv_heap);
#endif
        return nova_fail;
    }
}
