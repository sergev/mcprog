CC		= gcc
CFLAGS		= -Wall -g -O
OBJS		= multicore.o conf.o

all:		mcprog

mcprog:		$(OBJS) mcprog.o
		$(CC) -o mcprog mcprog.o $(OBJS)

probe:		$(OBJS) probe.o
		$(CC) -o probe probe.o $(OBJS)

jtag/jtag.a:
		$(MAKE) -C jtag all

clean:
		rm -f *~ *.o core mcprog mcprog.exe

install:	mcprog
		install -c -s -oroot -m4755 mcprog /usr/local/bin/mcprog

###
