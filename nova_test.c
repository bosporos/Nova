#include "nova.h"

/* Test harness
 */
int main (
    __attribute__ ((unused)) int argc,
    __attribute__ ((unused)) char ** argv)
{
    printf ("sizeof(nova_block_t): %zu\n", sizeof (nova_block_t));
    printf ("offsetof(nova_chunk_t, nv_blocks): %zu\n", offsetof (struct nova_chunk, nv_blocks[0]));

    return EXIT_SUCCESS;
}
