CC		= gcc

CFLAGS		= -Wall -g -I/opt/local/include -O
LDFLAGS		= -s
LIBS		= -L/opt/local/lib -lusb

COMMON_OBJS     = target.o
COMMON_OBJS     += adapter-usb.o
COMMON_OBJS	+= adapter-lpt.o
COMMON_OBJS	+= adapter-bitbang.o
COMMON_OBJS	+= adapter-mpsse.o

PROG_OBJS	= mcprog.o conf.o swinfo.o $(COMMON_OBJS)

REMOTE_OBJS	= gdbproxy.o rpmisc.o remote-elvees.o $(COMMON_OBJS)

all:		mcprog mcremote mcprog-ru.mo ru/LC_MESSAGES/mcprog.mo #adapter-bitbang adapter-mpsse

mcprog:		$(PROG_OBJS)
		$(CC) $(LDFLAGS) -o $@ $(PROG_OBJS) $(LIBS)

mcremote:	$(REMOTE_OBJS)
		$(CC) $(LDFLAGS) -o $@ $(REMOTE_OBJS) $(LIBS)

adapter-bitbang: adapter-bitbang.c
		$(CC) $(LDFLAGS) $(CFLAGS) -DSTANDALONE -o $@ adapter-bitbang.c $(LIBS)

adapter-mpsse: adapter-mpsse.c
		$(CC) $(LDFLAGS) $(CFLAGS) -DSTANDALONE -o $@ adapter-mpsse.c $(LIBS)

mcprog.po:      *.c
		xgettext --from-code=utf-8 --keyword=_ mcprog.c target.c adapter-lpt.c -o $@

mcprog-ru.mo:   mcprog-ru.po
		msgfmt -c -o $@ $<

mcprog-ru-cp866.mo ru/LC_MESSAGES/mcprog.mo: mcprog-ru.po
		iconv -f utf-8 -t cp866 $< | sed 's/UTF-8/CP866/' | msgfmt -c -o $@ -
		cp mcprog-ru-cp866.mo ru/LC_MESSAGES/mcprog.mo

clean:
		rm -f *~ *.o core mcprog mcremote adapter-bitbang adapter-mpsse mcprog.po

install:	mcprog mcremote mcprog.conf mcprog-ru.mo
		install -c -s mcprog /usr/local/bin/mcprog
		install -c -s mcremote /usr/local/bin/mcremote
		[ -f //usr/local/etc/mcprog.conf ] || install -c -m644 mcprog.conf /usr/local/etc/mcprog.conf
		if [ `uname` = Linux ]; then \
		    chown root /usr/local/bin/mcprog /usr/local/bin/mcremote; \
		    chmod 4755 /usr/local/bin/mcprog /usr/local/bin/mcremote; \
		fi
		install -d /usr/local/share/locale/ru/LC_MESSAGES
		install -c -m 444 mcprog-ru.mo /usr/local/share/locale/ru/LC_MESSAGES/mcprog.mo
###
adapter-bitbang.o: adapter-bitbang.c adapter.h oncd.h
adapter-lpt.o: adapter-lpt.c adapter.h oncd.h
adapter-mpsse.o: adapter-mpsse.c adapter.h oncd.h
adapter-usb.o: adapter-usb.c adapter.h oncd.h
conf.o: conf.c conf.h
gdbproxy.o: gdbproxy.c gdbproxy.h
mcprog.o: mcprog.c target.h conf.h
remote-elvees.o: remote-elvees.c gdbproxy.h target.h
remote-skeleton.o: remote-skeleton.c gdbproxy.h
rpmisc.o: rpmisc.c gdbproxy.h
target.o: target.c target.h adapter.h oncd.h mips.h
