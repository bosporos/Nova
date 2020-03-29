#include "nova.h"

/* UTILITY ZONE ***************************************************************/

/* __atomic */ uintptr_t _nv_dealloc_csize_cache     = 0;
/* __atomic */ uintptr_t _nv_dealloc_smobjplsz_cache = 0;

nova_res_t __nv_cache_reload_from_cfg (uintptr_t nv_override, nvcfg_t nv_cfg, uintptr_t * nv_cache)
{
    if (nv_override > 0)
        __atomic_store_n (nv_cache, nv_override, __ATOMIC_RELEASE);
    else
        __atomic_store_n (nv_cache, nova_read_cfg (nv_cfg), __ATOMIC_RELEASE);

    return nova_ok;
}

/* END UTILITY ZONE ***********************************************************/
