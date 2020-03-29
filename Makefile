LIB=nova.dylib
LIBSHORT=nova

CC=clang-10
CFLAGS=-ffreestanding -fPIC -pipe -Wall -Wextra -g -fcolor-diagnostics

OFILES=nova_block.o nova_cache.o nova_chunk.o nova_heap_generic.o nova_heap_local.o \
	nova_heap_regional.o nova_lkg_generic.o nova_lkg_local.o nova_lkg_regional.o \
	nova_mutex.o nova_tid.o nova_util.o

%.o: %.c nova.h
	ccache $(CC) -I. -c -o $@ $< $(CFLAGS)

$(LIB): $(OFILES)
	$(CC) -dynamiclib $(CFLAGS) $^ -o $@

nova_test: nova_test.c $(LIB) nova.h
	$(CC) -L. -I. -l$(LIBSHORT) $< -o $@

clean:
	rm $(OFILES)

distclean: clean
	rm $(LIB)
	rm -rf nova_test.dSYM
	rm -f nova_test
