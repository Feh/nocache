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
NOCACHE_BINS=nocache.o fcntl_helpers.o
MANPAGES=$(wildcard man/*.1)

CFLAGS+= -Wall
GCC = gcc $(CFLAGS) $(CPPFLAGS) $(LDFLAGS)

.PHONY: all
all: $(CACHE_BINS) nocache.so nocache

$(CACHE_BINS):
	$(GCC) -o $@ $@.c

$(NOCACHE_BINS):
	$(GCC) -fPIC -c -o $@ $(@:.o=.c)

nocache.global:
	sed 's!##libdir##!$(subst $(DESTDIR),,$(libdir))!' <nocache.in >$@

nocache:
	sed 's!##libdir##!$$(dirname "$$0")!' <nocache.in >$@
	chmod a+x $@

nocache.so: $(NOCACHE_BINS)
	$(GCC) -pthread -shared -Wl,-soname,nocache.so -o nocache.so $(NOCACHE_BINS) -ldl

$(mandir) $(libdir) $(bindir):
	mkdir -v -p $@

install: all $(mandir) $(libdir) $(bindir) nocache.global
	install -m 0644 nocache.so $(libdir)
	install -m 0755 nocache.global $(bindir)/nocache
	install -m 0755 $(CACHE_BINS) $(bindir)
	install -m 0644 $(MANPAGES) $(mandir)

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
