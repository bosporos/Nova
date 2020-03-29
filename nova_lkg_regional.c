#include "nova.h"

/*******************************************************************************
 * LINKAGE HANDLING : REGIONAL LINKAGES
 ******************************************************************************/

nova_res_t __nv_regional_lkg_receive_block_nl_sl (nova_lkg_t * nv_lkg,
                                                  nova_block_t * nv_block)
{
    /* Works for both ULKGs and SLKGs.
     */

    /* Called from:
     *  - __nv_regional_heap_take_evac_block_nl_sl (two entry points)
     *  - __nv_chunk_release_blocks_to
     * Both of these functions lock the LL before calling this function.
     */
    nv_block->nv_lkg   = nv_lkg;
    nv_block->nv_owner = nv_tid;

    nv_block->nv_lkgnx = nv_lkg->nv_head;
    nv_block->nv_lkgpr = NULL;
    if (nv_block->nv_lkgnx != NULL) {
        nv_block->nv_lkgnx->nv_lkgpr = nv_block;
    }
    nv_lkg->nv_head = nv_block;

    /* The FPGM will be locked at this point.
     */
    nvmutex_unlock (&nv_block->nv_fpgm);

    return nova_ok;
}

nova_res_t __nv_regional_lkg_drop (nova_lkg_t * nv_lkg)
{
    /* Rather simpler than the local linkage drop function.
     * For starters, no __atomic head, and the chain only runs in
     * the one direction, so we can make it single-pass.
     */

    nvmutex_lock (&nv_lkg->nv_ll);

    nova_block_t *_nv_curr = nv_lkg->nv_head, *_nv_next;
    nv_lkg->nv_head        = NULL;
    while (_nv_curr != NULL) {
        _nv_next = _nv_curr->nv_lkgnx;
        nvmutex_lock (&_nv_curr->nv_fpgm);
        _nv_curr->nv_lkgpr = _nv_curr->nv_lkgnx = NULL;
        __nv_regional_heap_pass_evac_block_nl_sl (nv_lkg->nv_heap, _nv_curr);
        _nv_curr = _nv_next;
    }

    nvmutex_unlock (&nv_lkg->nv_ll);
    /* Mutex end-of-life.
     */
    nvmutex_drop (&nv_lkg->nv_ll);

    return nova_ok;
}
