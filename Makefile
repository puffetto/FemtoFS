.PHONY: all clean run native install

DESTDIR ?=
KMODDIR ?= /boot/modules
BINDIR ?= /usr/sbin
MANDIR ?= /usr/share/man
INSTALL ?= install

# Resolve a caller's relative SYSDIR before the kernel submake changes directory.
# Leave source-tree discovery to bsd.kmod.mk when no override was supplied.
KMOD_MAKE = set --; \
	if test -n "$(SYSDIR)"; then \
	    sysdir=$$(cd "$(SYSDIR)" && pwd -P) || exit 1; \
	    set -- "SYSDIR=$$sysdir"; \
	fi; \
	$(MAKE) -C ko "$$@"

all:
	$(MAKE) -C programs all

clean:
	$(MAKE) -C programs clean

run: all
	./programs/femtofsSim misc/list

# Native FreeBSD builds use the running kernel's matching source tree.
native:
	$(MAKE) -C programs makefemtofs
	$(KMOD_MAKE)
	$(MAKE) -C ko -f Makefile.mount
	$(MAKE) -C ko -f Makefile.ctl

install: native
	$(INSTALL) -d "$(DESTDIR)$(KMODDIR)" "$(DESTDIR)$(BINDIR)" \
	    "$(DESTDIR)$(MANDIR)/man5" "$(DESTDIR)$(MANDIR)/man8"
	$(KMOD_MAKE) install DESTDIR="$(DESTDIR)" KMODDIR="$(KMODDIR)"
	$(MAKE) -C ko -f Makefile.mount install DESTDIR="$(DESTDIR)" BINDIR="$(BINDIR)"
	$(MAKE) -C ko -f Makefile.ctl install DESTDIR="$(DESTDIR)" BINDIR="$(BINDIR)"
	$(INSTALL) -m 0555 programs/makefemtofs "$(DESTDIR)$(BINDIR)/makefemtofs"
	$(INSTALL) -m 0444 man/femtofs.5 "$(DESTDIR)$(MANDIR)/man5/femtofs.5"
	$(INSTALL) -m 0444 man/makefemtofs.8 man/mount_femtofs.8 man/femtoctl.8 \
	    "$(DESTDIR)$(MANDIR)/man8/"
