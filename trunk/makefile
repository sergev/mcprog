CC		= gcc
CFLAGS		= -Wall -g -I/opt/local/include #-O
#OBJS		= adapter-lpt.o conf.o

OBJS		= adapter-usb.o target.o conf.o
LIBS		= -L/opt/local/lib -lusb-1.0

all:		mcprog adapter-bitbang

mcprog:		$(OBJS) mcprog.o
		$(CC) -o mcprog mcprog.o $(OBJS) $(LIBS)

adapter-bitbang: adapter-bitbang.c
		$(CC) $(CFLAGS) -DSTANDALONE -o $@ adapter-bitbang.c -L/opt/local/lib -lusb

clean:
		rm -f *~ *.o core mcprog mcprog.exe adapter-bitbang

install:	mcprog mcprog.conf
		install -c -s -oroot -m4755 mcprog /usr/local/bin/mcprog
		[ -f //usr/local/etc/mcprog.conf ] || install -c -m644 mcprog.conf /usr/local/etc/mcprog.conf

###
