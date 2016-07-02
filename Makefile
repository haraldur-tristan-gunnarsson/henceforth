CC = gcc
WARNINGS =  -Wall -Wextra #-pedantic #-Wstrict-prototypes -Wpointer-arith -Wcast-align \
			-Wwrite-strings \
			-Wredundant-decls -Wnested-externs -Winline \
			-Wuninitialized# -Wconversion -Wmissing-declarations -Wshadow -Wmissing-prototypes
CFLAGS = --std=gnu89 $(WARNINGS) -g#-Werror -O2# -O2 -Og #-O0
#CFLAGS = --std=gnu11 $(WARNINGS) -g#-Werror -O2# -O2 -Og #-O0
#perhaps better to use gnu89?
#CFLAGS = --std=c11 $(WARNINGS) -g#-Werror -O2# -O2 -Og #-O0
LIBS = -lm

all: inner.elf

inner.elf: inner.c
	$(CC) $(CFLAGS) -o $@ $^ $(LIBS)

#.c.o:
#	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	-rm -f *.elf
#	-rm -f *.o

rebuild: clean all

.PHONY: all clean rebuild
