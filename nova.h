#pragma once

/* Note: do not use NV_SMOBJ_POOLSIZE=65335 with osz=1! the last byte will be
 * inaccessible, leaving you with only 6533_4_ bytes available for actual use.
 */

/* uint??_t */
#include <stdint.h>
/* pthread_mutexattr_init
 * pthread_mutexattr_settype
 * pthread_mutexattr_destroy
 *
 * pthread_mutex_init
 * pthread_mutex_lock
 * pthread_mutex_trylock
 * pthread_mutex_unlock
 * pthread_mutex_destroy
 *
 * pthread_mutex_t
 */
#include <pthread.h>
/* size_t
 */
#include <stddef.h>

#define NOVA_MODE_DEBUG 1
#define NOVA_FORCE_NV_TID_DEFPERM 1
/* #define NOVA_LAZY_TIDINIT 1 */
#define NOVA_TID_RECYCLING 1

/* We want this to be settable from the client; basically, to turn this on or off
 * the library's client should do the following
 * ```c
 * // 1 for debug-mode, 0 for release-mode
 * #define NOVA_MODE_DEBUG 0
 * #include <nova.h>
 * ```
 */
#if !defined(NOVA_MODE_DEBUG)
#    define NOVA_MODE_DEBUG 0
#endif /* !@NOVA_MODE_DEBUG */

#define NOVA_DOCSTUB()

/* A nv_blfl flag.
 */
#define NOVA_BLFL_ISHEAD 1

typedef enum nova_res { nova_ok   = 0,
                        nova_fail = 1 } nova_res_t;
typedef size_t nvi_t;
typedef int32_t nvr_t;
typedef pthread_mutex_t nova_mutex_t;
typedef uint64_t nova_tid_t;
typedef uint16_t nova_smobjsz_t;
typedef uint16_t nova_smobjcnt_t;

/* Developer note: maximum size/alignment for this is 0x4000/64 = 2^14 / 2^6 = 2^8
 *                 = 256 bytes, so keep that in mind when making chnages to this
 *                 structure.
 * Developer note: add checking to make sure the nova_mutex_t doesn't overflow
 *                 our size limit--it comes to exactly 128 bytes on macos, not sure
 *                 about on other platforms.
 */
/* aligned-128 in the chunk */
typedef struct __attribute__ ((packed, aligned (128))) nova_block
{
    void * nv_base; /* +0 (8) */
    void * nv_fpl; /* +8 (8) */
    void * nv_fpg; /* +16 (8) */

    nova_smobjsz_t /* (uint16_t) */ nv_osz; /* +24 (2) */
    /* __atomic */ _Atomic uint16_t nv_blfl; /* +26 (2) */
    nova_smobjcnt_t /* (uint16_t) */ nv_ocnt; /* +28 (2) */
    /* __atomic */ nova_smobjcnt_t nv_acnt; /* +30 (2) */

    nova_tid_t nv_owner; /* +32 (8) */
    struct nova_block * nv_lkgnx; /* +40 (8) */
    struct nova_block * nv_lkgpr; /* +48 (8) */
    void * nv_lkg; /* +56 (8) */

    nova_mutex_t nv_fpgm; /* +64 (64) */

    /* yeah, just ignore this. */
    int nv_rbl[0]; /* +128 (0) */
} nova_block_t;

typedef struct nova_lkg
{
    nova_block_t * nv_head;
    nova_mutex_t nv_ll;
    void * nv_heap;
} nova_lkg_t;

typedef struct nova_heap
{
    struct nova_heap * nv_parent_heap;
    nvi_t nv_ln;
    nova_lkg_t nv_lkgs[];
} nova_heap_t;

typedef struct nova_chunk
{
    struct nova_chunk * nv_next;
    nova_block_t nv_blocks[63];
} nova_chunk_t;

/** Initializes `nv_mutex` as a normal, non-reentrant mutex.
 */
nova_res_t nvmutex_init (nova_mutex_t * nv_mutex);
/** Lock the mutex; this will always succeed if used properly.
 * If nova was compiled with NOVA_MODE_DEBUG=1, then it will trap errors, though
 * it will always return nova_ok if not compiled with the debug flag.
 */
nova_res_t nvmutex_lock (nova_mutex_t * nv_mutex);
/** Try to lock the mutex. Return nova_ok if the lock was successfully acquired.
 * Will return nova_fail if not.
 * If nova was compiled with NOVA_MODE_DEBUG=1, then it will trap errors, though
 * if not in debug mode, errors will cause nova_fail to be returned--a return
 * value of nova_fail may indicate either a normal "busy signal" or an error when
 * not in debug mode.
 */
nova_res_t nvmutex_trylock (nova_mutex_t * nv_mutex);
/** Unlock the mutex; this will always succeed if used properly.
 * If nova was compiled with NOVA_MODE_DEBUG=1, then it will trap errors, though
 * it will always return nova_ok if not compiled with the debug flag.
 */
nova_res_t nvmutex_unlock (nova_mutex_t * nv_mutex);
/** Destroy the mutex; this will always succeed if used properly.
 * If nova was compiled with NOVA_MODE_DEBUG=1, then it will trap errors, though
 * it will always return nova_ok if not compiled with the debug flag.
 */
nova_res_t nvmutex_drop (nova_mutex_t * nv_mutex);

nova_res_t nv_chunk_create (nova_chunk_t ** nv_chunk);
/* \source regional heap
 * \target chunk
 *
 * \param nova_heap_t* nv_receiver regional heap to move the chunk's blocks to.
 */
nova_res_t __nv_chunk_release_blocks_to (nova_chunk_t * nv_chunk,
                                         nova_heap_t * nv_receiver,
                                         nvi_t nv_begin,
                                         nvi_t nv_end);
/* Destroy a chunk. Invalidates the nv_next pointer of the previous chunk in the chain. */
nova_res_t __nv_chunk_destroy (nova_chunk_t * nv_chunk);
nova_res_t nv_chunk_destroy_chained (nova_chunk_t * nv_chunk, nvi_t nv_number);
nova_res_t nv_chunk_bind_to_root (nova_chunk_t * nv_chunk, nova_heap_t * nv_heap);

nova_res_t nv_heap_create (nova_heap_t ** nv_heap);
nova_res_t nv_heap_init (nova_heap_t * nv_heap, nvi_t nv_ln);
nova_res_t nv_heap_bind_parent (nova_heap_t * nv_child, nova_heap_t * nv_parent);

/** Give an evacuating block to a local heap to be passed up to the regional heap.
 *
 * \source local linkage
 * \target local heap
 * \payload block to pass on to regional heap
 * \notes should only be called when the local heap has informed the regional heap
 *        to prepare for the evacuation.
 *
 * \param nova_heap_t* nv_heap target
 * \param nova_block_t* nv_block payload
 */
nova_res_t __nv_local_heap_pass_evac_nl_sl (nova_heap_t * nv_heap,
                                            nova_block_t * nv_block);
/** Try to allocate an object of size `nv_osz` into `nv_obj` from the given heap.
 * \source client-path
 * \target local heap
 *
 * \param nova_heap_t* nv_heap target
 * \param void** nv_obj reverse-payload
 * \param nova_smobjsz_t nv_osz the size of the object to allocate.
 */
nova_res_t __nv_local_heap_alloc (nova_heap_t * nv_heap,
                                  void ** nv_obj,
                                  nova_smobjsz_t nv_osz);
nova_res_t __nv_local_heap_req_block (nova_heap_t * nv_heap,
                                      nova_smobjsz_t nv_osz,
                                      nova_block_t ** nv_block);

/** Try to destroy the local heap `nv_heap`.
 * \source client
 * \target local heap
 *
 * \behaviour will first drop all of the attached linkages, and then destroy
 *            itself.
 *
 * \param nova_heap_t* nv_heap target
 */
nova_res_t __nv_local_heap_drop (nova_heap_t * nv_heap);

nova_res_t __nv_regional_heap_create (nova_heap_t ** nv_heap);
nova_res_t __nv_root_heap_create (nova_heap_t ** nv_heap);
nova_res_t __nv_regional_heap_take_evac_block_nl_sl (nova_heap_t * nv_heap,
                                                     nova_block_t * nv_ev_block);
nova_res_t __nv_regional_heap_pass_evac_block_nl_sl (nova_heap_t * nv_heap,
                                                     nova_block_t * nv_block);
nova_res_t __nv_regional_heap_incref (nova_heap_t * nv_heap);
nova_res_t __nv_regional_heap_decref (nova_heap_t * nv_heap);
nova_res_t __nv_regional_heap_drop (nova_heap_t * nv_heap);
nova_res_t __nv_regional_heap_req_block (nova_heap_t * nv_heap,
                                         nova_smobjsz_t nv_osz,
                                         nova_block_t ** nv_block);

nova_res_t nv_lkg_init (nova_lkg_t * nv_lkg);
nova_res_t nv_lkg_req_block (nova_lkg_t * nv_lkg,
                             nova_block_t ** nv_block);

/** Drop a local linkage.
 *
 * \source local heap
 * \target local linkage
 * \notes should only be called after the regional heap has been prepped.
 *
 * \param nova_lkg_t* nv_lkg target
 */
nova_res_t __nv_local_lkg_drop (nova_lkg_t * nv_lkg);
/** Try to allocate an object of size `nv_osz` into `nv_obj` from the given linkage.
 *
 * \source local heap
 * \target local linkage
 *
 * \behaviour the linkage will fall back on `nv_heap` as a source of further blocks
 *            if necessary.
 *
 * \notes by convention, `nv_heap` will be the calling heap, but this is not actually
 *        a requirement: it can be any (local) heap.
 *
 * \param nova_lkg_t* nv_lkg target
 * \param void** nv_obj reverse-payload
 * \param nova_smobjsz_t nv_osz allocated object size
 * \param nova_heap_t* nv_heap the fallback heap
 */
nova_res_t __nv_local_lkg_alloc (nova_lkg_t * nv_lkg,
                                 void ** nv_obj,
                                 nova_smobjsz_t nv_osz,
                                 nova_heap_t * nv_heap);

nova_res_t __nv_regional_lkg_drop (nova_lkg_t * nv_lkg);
nova_res_t __nv_regional_lkg_receive_block_nl_sl (nova_lkg_t * nv_lkg,
                                                  nova_block_t * nv_block);

nova_res_t nv_block_init (nova_block_t * nv_block, void * nv_block_memory);
/** Formats nv_block into objects of size `nv_osz`.
 * \behaviour this sets up the free list in memory, and modifies nv_fpl and nv_fpg
 *            to get things set up.
 *
 * \notes this assumes that the block is already empty.
 *
 * \source local linkage
 * \target soon-to-be head block (empty and unlinked)
 *
 * \param nova_block_t* nv_block target
 * \param nv_smobjsz_t nv_osz object size to format to
 */
nova_res_t __nv_block_fmt (nova_block_t * nv_block, nova_smobjsz_t nv_osz);
nova_res_t __nv_block_alloc (nova_block_t * nv_block, void ** nv_obj);
nova_res_t __nv_block_alloc_inner (nova_block_t * nv_block, void ** nv_obj);

#if NOVA_MODE_DEBUG
nova_res_t __nv_block_dealloc (nova_block_t * nv_block, void * nv_obj);
#endif

nova_res_t __nv_dealloc_smobj (void * nv_obj);

typedef enum nvcfg {
    /* Retrieves the size of a chunk, in bytes
     */
    NV_CHUNKSIZE,
    /* Retrieves the size of small object pool, in bytes
     */
    NV_SMOBJ_POOLSIZE,
    /* Retrieves number of pools per heap
     */
    NV_SMOBJ_POOLCOUNT,
} nvcfg_t;

nvi_t nova_read_cfg (nvcfg_t);

typedef enum nve {
    /* We're actually ok
     */
    NVE_OK = 0,
    /* Acceptable failure (i.e. nvmutex_trylock).
     */
    NVE_FAIL = 1,

    /* SECTION: Client screwed up. */

    /* Bad value in the configuration.
     * \param nvcfg_t configuration parameter
     * \param const char * description of error
     * \note this function accepts a printf-style formatstring as the error description;
     *       parameters should be passed in VLA style after the formatstring.
     */
    NVE_BADCFG,
    /* Bad value passed to function.
     * \param const char * description of error
     * \note this function accepts a printf-style formatstring as the error description;
     *       parameters should be passed in VLA style after the formatstring.
     */
    NVE_BADVAL,
    /* Client called a function in a way that contradicts that function's environmental
     * assumptions.
     * \param const char * description of error
     * \note this function accepts a printf-style formatstring as the error description;
     *       parameters should be passed in VLA style after the formatstring.
     */
    NVE_BADCALL,

    /* The program has entered a situation in which its behaviour is not defined.
     * That is, something has occurred that should not ever be able to happen.
     * \param const char * description of error.
     * \note this function accepts a printf-style formatstring as the error description;
     *       parameters should be passed in VLA style after the formatstring.
     */
    NVE_IMPOSSIBLE,

    /* Client screwed up the heap hierarchy.
     * \param const char * description of error
     * \note this function accepts a printf-style formatstring as the error description;
     *       parameters should be passed in VLA style after the formatstring.
     */
    NVE_HIERARCHY,

    /* SECTION: Special */

    /* This error is the effect of some error further down the call tree.
     * \param const char * description of error
     * \note this function accepts a printf-style formatstring as the error description;
     *       parameters should be passed in VLA style after the formatstring.
     */
    NVE_CASCADE,

    /* SECTION: Concurrency. */

    /* A concurrency error was detected.
     * \param const char * description of error
     * \note this function accepts a printf-style formatstring as the error description;
     *       parameters should be passed in VLA style after the formatstring.
     */
    NVE_DESYNC,

    /* SECTION: System problems. */

    /* Chunk allocation failed: out of memory.
     * \note for obvious reasons, this error accepts no additional parameters.
     */
    NVE_CHUNKALLOC_DRY,

    /* Structure allocation failed: out of memory.
     * This occurs when nova is unable to allocate memory for an internal structure.
     * \param const char * description of error
     * \note for obvious reasons, this error does not accept printf-style variadic
     *       arguments.
     */
    NVE_STRUCTALLOC_DRY,

    /* SECTION: Bad days down at the kernel boundary */

    /* This is a right whammy: couldn't get the kernel to give us a thread identifier.
     * \param kern_return_t kernel error code
     */
    NVE_KERN_THREADID_XNU,

} nve_t;

void __nv_error (nve_t nv_err, ...);
void __nv_dbg_assert (int nv_assert_expr, const char * nv_efmt, ...);

#if NOVA_MODE_DEBUG
nova_res_t __nvd_validate_block (nova_block_t * nv_block);
nova_res_t __nvd_validate_range (void * nv_range_base,
                                 nvi_t nv_range_size,
                                 void * nv_obj);
#endif

/* \behaviour shall never return 0
 *            shall not yet return 1
 *            may return values >= 2
 */
nvi_t __nv_lindex (nvi_t nv_osz);

nvi_t __nv_canonicalize_osz (nvi_t nv_osz);

nova_res_t __nv_cache_reload_from_cfg (uintptr_t nv_override,
                                       nvcfg_t nv_cfg,
                                       uintptr_t * nv_cache);
extern uintptr_t _nv_dealloc_csize_cache, _nv_dealloc_smobjplsz_cache;

nova_tid_t __nv_tid ();
nova_res_t __nv_tid_thread_init ();
nova_res_t __nv_tid_thread_drop ();
nova_res_t __nv_tid_recycle_init ();
nova_res_t __nv_tid_recycle_drop ();
