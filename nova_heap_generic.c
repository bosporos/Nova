#include "nova.h"

/* malloc */
#include <stdlib.h>

/*******************************************************************************
 * HEAP HANDLING
 ******************************************************************************/

NOVA_DOCSTUB ();

nova_res_t nv_heap_create (nova_heap_t ** nv_heap)
{
    nvi_t _num_lkgs = nova_read_cfg (NV_SMOBJ_POOLCOUNT);
    (*nv_heap)      = malloc (sizeof (nova_heap_t *)
                         + sizeof (nvi_t)
                         + (sizeof (nova_lkg_t)
                            * _num_lkgs));
    if ((*nv_heap) == NULL) {
        return nova_fail;
    }
    return nv_heap_init (*nv_heap, _num_lkgs);
}

nova_res_t nv_heap_init (nova_heap_t * nv_heap, nvi_t nv_ln)
{
    nv_heap->nv_ln = nv_ln;
    for (nvi_t i = 0; i < nv_heap->nv_ln; i++) {
        nv_lkg_init (&nv_heap->nv_lkgs[i]);
        nv_heap->nv_lkgs[i].nv_heap = nv_heap;
    }
    nv_heap_bind_parent (nv_heap, NULL);
    return nova_ok;
}

nova_res_t nv_heap_bind_parent (nova_heap_t * nv_child, nova_heap_t * nv_parent)
{
    nv_child->nv_parent_heap = nv_parent;

    return nova_ok;
}
