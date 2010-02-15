CC		= gcc
CFLAGS		= -Wall -g -O
OBJS		= multicore-lpt.o conf.o

#OBJS		= multicore-usb.o conf.o
LIBS		= -lusb-1.0

all:		mcprog

mcprog:		$(OBJS) mcprog.o
		$(CC) -o mcprog mcprog.o $(OBJS) $(LIBS)
clean:
		rm -f *~ *.o core mcprog mcprog.exe

install:	mcprog mcprog.conf
		install -c -s -oroot -m4755 mcprog /usr/local/bin/mcprog
		[ -f //usr/local/etc/mcprog.conf ] || install -c -m644 mcprog.conf /usr/local/etc/mcprog.conf

###
