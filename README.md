Nova aims to be an intrusive allocator library for those who want their allocator
to be blazing fast _and_ take advantage of multithreaded environments.

Currently, Nova supports various 64-bit POSIX platforms (although it's optimized for x86-64/amd64 CPUs). The only real platform requirements are as follows:
 - has `pthread`s
 - has a way to get a process-unique thread id (more on that in our soon-to-be-released Porting document!-- at the moment, that's XNU (macOS) and anything where `gettid()` gives a unique thread id that won't ever reoccur within that process)
 - has `posix_memalign` (equivalents can easily be added; again, see our soon-to-be-released Porting document!)

User guide + wrappers in various languages: coming soon!
