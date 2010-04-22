CC		= gcc
CFLAGS		= -Wall -g -I/opt/local/include -O
LIBS		= -L/opt/local/lib -lusb-1.0

OBJS		= mcprog.o target.o conf.o
OBJS		+= adapter-usb.o
OBJS		+= adapter-lpt.o
OBJS		+= adapter-bitbang.o

all:		mcprog adapter-bitbang

mcprog:		$(OBJS)
		$(CC) -o mcprog $(OBJS) $(LIBS)

adapter-bitbang: adapter-bitbang.c
		$(CC) $(CFLAGS) -DSTANDALONE -o $@ adapter-bitbang.c -L/opt/local/lib -lusb

clean:
		rm -f *~ *.o core mcprog mcprog.exe adapter-bitbang

install:	mcprog mcprog.conf
		install -c -s -oroot -m4755 mcprog /usr/local/bin/mcprog
		[ -f //usr/local/etc/mcprog.conf ] || install -c -m644 mcprog.conf /usr/local/etc/mcprog.conf

###
