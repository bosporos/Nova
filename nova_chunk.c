#include "nova.h"
#include <errno.h>

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

nova_res_t __nv_chunk_release_blocks_to (
    nova_chunk_t * nv_chunk,
    nova_heap_t * nv_receiver,
    nvi_t nv_begin,
    nvi_t nv_end)
{
    nova_lkg_t * _nv_ulkg = &nv_receiver->nv_lkgs[0];
    nvmutex_lock (&_nv_ulkg->nv_ll);
    for (nvi_t i = nv_begin; i < nv_end; i++) {
        nvmutex_lock (&nv_chunk->nv_blocks[i].nv_fpgm);
        __nv_regional_lkg_receive_block_nl_sl (&nv_receiver->nv_lkgs[0], &nv_chunk->nv_blocks[i]);
    }
    nvmutex_unlock (&_nv_ulkg->nv_ll);
    return nova_ok;
}

nova_res_t __nv_chunk_destroy (nova_chunk_t * nv_chunk)
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
        __nv_chunk_destroy (_nv_chunk);
        _nv_chunk = _nv_swap;
    }
    return nova_ok;
}

nova_res_t nv_chunk_bind_to_root (nova_chunk_t * nv_chunk, nova_heap_t * nv_heap)
{
    nv_chunk->nv_next              = ((nova_chunk_t **)nv_heap)[-2];
    ((nova_chunk_t **)nv_heap)[-2] = nv_chunk;

    return nova_ok;
}
