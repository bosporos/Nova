all:
	clang-10 -ffreestanding -fPIC -pipe -Wall -Wextra -g -fcolor-diagnostics -I. nova.c -c -o nova.c.o -Wno-unused-parameter
	# clang-10 -ffreestanding -fPIC -pipe -Wall -Wextra -g -fcolor-diagnostics -I. nova.c.o -o nova

oclean:
	rm nova.c.o

clean: oclean
	rm -r nova.dSYM
	rm nova
