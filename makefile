CC		= gcc
CFLAGS		= -Wall -g -O
OBJS		= multicore.o

all:		flashid mcprog

mcprog:		$(OBJS) mcprog.o
		$(CC) -o mcprog mcprog.o $(OBJS)

jtag/jtag.a:
		$(MAKE) -C jtag all

flashid:	flashid.o $(OBJS)
		$(CC) -o flashid flashid.o $(OBJS)

clean:
		rm -f *~ *.o core flashid mcprog

install:	mcprog
		install -c -s -oroot -m4755 mcprog /usr/local/bin/mcprog

###
