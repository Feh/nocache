#!/usr/bin/make -f
# -*- makefile -*-

PREFIX ?= /usr/local
MANDIR ?= /share/man/man1
BINDIR ?= /bin
LIBDIR ?= /lib
mandir  = $(DESTDIR)$(PREFIX)$(MANDIR)
bindir  = $(DESTDIR)$(PREFIX)$(BINDIR)
libdir  = $(DESTDIR)$(PREFIX)$(LIBDIR)

CACHE_BINS=cachedel cachestats
NOCACHE_BINS=nocache.o fcntl_helpers.o pageinfo.o
MANPAGES=$(wildcard man/*.1)

CC ?= gcc
CFLAGS+= -Wall
COMPILE = $(CC) $(CFLAGS) $(CPPFLAGS) $(LDFLAGS)

.PHONY: all
all: $(CACHE_BINS) nocache.so nocache

$(CACHE_BINS):
	$(COMPILE) -o $@ $@.c

$(NOCACHE_BINS): $(NOCACHE_BINS:.o=.c)
	$(COMPILE) -fPIC -c -o $@ $(@:.o=.c)

nocache.global: nocache.in
	sed 's!##libdir##!$(subst $(DESTDIR),,$(libdir))!' <nocache.in >$@

nocache: nocache.in
	sed 's!##libdir##!$$(dirname "$$0")!' <nocache.in >$@
	chmod a+x $@

nocache.so: $(NOCACHE_BINS)
	$(COMPILE) -pthread -shared -Wl,-soname,nocache.so -o nocache.so $(NOCACHE_BINS) -ldl

$(mandir) $(libdir) $(bindir):
	mkdir -v -p $@

install: all $(mandir) $(libdir) $(bindir) nocache.global
	install -pm 0644 nocache.so $(libdir)
	install -pm 0755 nocache.global $(bindir)/nocache
	install -pm 0755 $(CACHE_BINS) $(bindir)
	install -pm 0644 $(MANPAGES) $(mandir)

.PHONY: uninstall
uninstall:
	cd $(mandir) && $(RM) -v $(notdir $(MANPAGES))
	$(RM) -v $(bindir)/nocache $(libdir)/nocache.so

.PHONY: clean distclean
clean distclean:
	$(RM) -v $(CACHE_BINS) $(NOCACHE_BINS) nocache.so nocache nocache.global

.PHONY: test
test: all
	cd t; prove -v .
