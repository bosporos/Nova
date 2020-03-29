#include "nova.h"

#ifdef __APPLE__
/* mach_port_name_t
   kern_return_t? */
#    include <mach/mach_types.h>
/* THREAD_IDENTIFIER_INFO_COUNT
   thread_identifier_info_data_t
   THREAD_IDENTIFIER_INFO */
#    include <mach/thread_info.h>
/* thread_info(thread_act_t = mach_port_name_t,
               thread_flavor_t,
               thread_info_t,
               mach_msg_type_number_t) */
#    include <mach/mach.h>
#endif
#ifdef __linux__
/* gettid(void) */
#    include <sys/types.h>
#endif

/*******************************************************************************
 * TID HANDLING
 ******************************************************************************/

NOVA_DOCSTUB ();

nova_tid_t __nv_tid_impl__ ()
{
    /* okay, so this is gonna be kinda complicated.
     */
#ifdef __APPLE__
    /* Big thanks to https://stackoverflow.com/questions/1540603/mac-iphone-is-there-a-way-to-get-a-thread-identifier-without-using-objective-c */

    /*
     * On macos, there are a few options:
     *  pthread_self()
     *  pthread_threadid_np(pthread_t, uint64_t **)
     *  pthread_mach_thread_np(pthread_t) -> mach_port_name_t
     *  thread_info(mach_port_name_t, ...)
     *
     * pthread_self() returns a pthread_t, which, on darwin-xnu, is actually
     * a pointer to a _opaque_pthread_t structure which is 8176 + 16 bytes in
     * size on LP64.
     *      The pthread_t will be unique for existing pthreads in a process, but
     * it's just a blindly allocated pointer (_pthread_allocate calls mach_vm_map
     * or mach_vm_allocate), so it can't be relied on to be a process-unique ID,
     * as recycling is a function of the OS re-using the mapped memory.
     *
     * pthread_threadid_np calls the threadid_self syscall, which I can't really
     * find information on, so I can't really make assumptions about its behaviour.
     *
     * pthread_mach_thread_np(pthread_t) returns a mach_port_name_t, which would
     * seem to be unique to a process, _but_ it will return different values for
     * the same thread when called from different processes.
     *
     * However, the mach_port_name_t returned by pthread_mach_thread_np can be
     * passed to thread_info () with flavor=THREAD_IDENTIFIER_INFO, an obscure
     * XNU kernel call.
     */

    /* little trick from pthread/mach_dep.h */
#    if defined(__i386__) || defined(__x86_64__)
    void * _nv_pself;
    asm volatile("mov %%gs:%P1, %0"
                 : "=r"(_nv_pself)
#        ifdef __LP64__
                 : "i"(0 * sizeof (void *) + 0x60));
#        else
                 : "i"(0 * sizeof(void*) + 0x48)
#        endif

    mach_port_name_t _nv_mach_port = pthread_mach_thread_np (
        /* pthread_self () */
        /* slightly faster than pthread_self(); shaves off a few extra calls
            EDIT: for various reasons, not using this
            EDIT2: now we are */
        _nv_pself
        /* EDIT2: _pthread_getspecific_direct is an inline function defined in
         *        a header that is shipped standard-issue, so we're reproducing from
         *        there.
         */
        /* _pthread_getspecific_direct (_PTHREAD_TSD_SLOT_PTHREAD_SELF) */);
#    else
    mach_port_name_t _nv_mach_port = pthread_mach_thread_np (
        pthread_self ());
#    endif

    thread_identifier_info_data_t _nv_xnu_thrinfo;
    mach_msg_type_number_t _nv_mach_info_cnt = THREAD_IDENTIFIER_INFO_COUNT;
    /*
     * XNU defines thread_info(thread_act_t,
     *                         thread_flavor_t,
     *                         thread_info_t,
     *                         mach_msg_type_number_t)
     * thread_act_t is a typedef of thread_t
     * It seems that in kernelspace, thread_t is an alias of `struct thread`
     * or, in the outer kernel, `mach_port_t`.
     * However, we have reports that `thread_t` actually aliases `mach_port_name_t`
     * in proper userland, so that's what we pass in here.
     */
    kern_return_t _nv_kern_ret = thread_info (_nv_mach_port,
                                              THREAD_IDENTIFIER_INFO,
                                              (thread_info_t)&_nv_xnu_thrinfo,
                                              &_nv_mach_info_cnt);
    if (_nv_kern_ret != KERN_SUCCESS) {
        /* Charlie foxtrot. Die.
         */
        __nv_error (NVE_KERN_THREADID_XNU);
    }
    return _nv_xnu_thrinfo.thread_id;
#endif
#if defined(__linux__) || defined(NOVA_FORCE_GETTID)
    /* should be unique enough for our purposes. i think hope probably yes? */
    return gettid ();
#endif

    /* well, not much we can do at this point.
     */
    return 0;
}

#if !defined(__APPLE__)
_Thread_local nova_tid_t nv_tid;

nova_res_t nv_tid_init__ ()
{
    nv_tid = __nv_tid_impl__ ();

    return nova_ok;
}

nova_res_t nv_tid_drop__ ()
{
    /* Well, not much that we really need to do.
     * It's POD, so there's no real destruction that needs to occur.
     */
    return nova_ok;
}
#else
nova_res_t nv_tid_init__ ()
{
    return nova_ok;
}

nova_res_t nv_tid_drop__ ()
{
    return nova_ok;
}
#endif
