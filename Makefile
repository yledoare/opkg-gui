PROG=opkg-gui

OPKG_DEV=../../opkg/pkg/usr/

debug=1

ifeq ($(debug),1)
CFLAGS_DEBUG=-g # -D_DEBUG
endif

CC=gcc
RM=rm -fv

CFLAGS= `pkg-config --cflags gtk+-2.0` -I $(OPKG_DEV)/include $(CFLAGS_DEBUG)
LDFLAGS=`pkg-config --libs gtk+-2.0` -L $(OPKG_DEV)/lib -lopkg

$(PROG): opkg-gui.c
	echo $(CC) $(CFLAGS) $(OBJS) -o $(PROG) $(LDFLAGS)
	$(CC) $(CFLAGS) opkg-gui.c -o $(PROG) $(LDFLAGS)

install: $(PROG)
	install -d $(DESTDIR)/$(PREFIX)/bin
	install -m 0755 $(PROG) $(DESTDIR)/$(PREFIX)/bin/$(PROG)

clean:
	$(RM) $(OBJS) $(PROG)

debug:
	$(MAKE) debug=1
