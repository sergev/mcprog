CC		= gcc
CFLAGS		= -Wall -g -I/opt/local/include #-O
#OBJS		= multicore-lpt.o conf.o

OBJS		= multicore-usb.o conf.o
LIBS		= -L/opt/local/lib -lusb-1.0

all:		mcprog multicore-bitbang

mcprog:		$(OBJS) mcprog.o
		$(CC) -o mcprog mcprog.o $(OBJS) $(LIBS)

multicore-bitbang: multicore-bitbang.c
		$(CC) $(CFLAGS) -DSTANDALONE -o $@ multicore-bitbang.c -L/opt/local/lib -lftdi

clean:
		rm -f *~ *.o core mcprog mcprog.exe multicore-bitbang

install:	mcprog mcprog.conf
		install -c -s -oroot -m4755 mcprog /usr/local/bin/mcprog
		[ -f //usr/local/etc/mcprog.conf ] || install -c -m644 mcprog.conf /usr/local/etc/mcprog.conf

###
