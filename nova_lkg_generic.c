#include "nova.h"

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

nova_res_t nv_lkg_req_block (nova_lkg_t * nv_lkg, nova_block_t ** nv_block)
{
    /* A block has been requested from this linkage; we should try to
     * comply.
     *
     * First, lock the LL so we can access the head (nv_head is only __atomic
     * for local sized heaps), which certainly simplifies a few things.
     */
    nvmutex_lock (&nv_lkg->nv_ll);

    /* Check if there are any blocks in the linkage; we just need _one_ block,
     * so we can just check if the head is NULL.
     * h(lkg)=nil <=> l(lkg)=0
     * h(lkg)≠nil <=> l(lkg)≠0, l(S) in |N => l(S)≠0 <=> l(S)>0
     */
    if (nv_lkg->nv_head != NULL) {
        /* Simple singly linked list front-pull & deletion: replace the head of the list
         * with its right side linkage. We know that nv_head is non-null already,
         * so we don't need to worry about null pointer dereferencing.
         */
        (*nv_block)     = nv_lkg->nv_head;
        nv_lkg->nv_head = nv_lkg->nv_head->nv_lkgnx;
        /* We do have to take care of the new head's left side linkage; if it's
         * not NULl, then we do so.
         */
        if (nv_lkg->nv_head != NULL) {
            nv_lkg->nv_head->nv_lkgpr = NULL;
        }
        /* We're clear to unlock here: no remaining changes to nv_head or the
         * side linkages of any blocks on this chain.
         *
         * This is an unsized linkage (i.e. full of empty blocks), therefore
         * there are no extant deallocating referencers, so it's still safe to
         * do the lkgnx modification on nv_block after this is unlocked,
         */
        nvmutex_unlock (&nv_lkg->nv_ll);
        /* (*nv_block)->nv_lkgpr == NULL by definition _if this is an unsized
         * linkage_.
         */
        (*nv_block)->nv_lkgnx = NULL;

        /* FPGM is expected to be locked at this ponit
         */
        nvmutex_lock (&(*nv_block)->nv_fpgm);

        return nova_ok;
    }

    /* We failed; unlock and leave.
     */
    nvmutex_unlock (&nv_lkg->nv_ll);
    return nova_fail;
}
