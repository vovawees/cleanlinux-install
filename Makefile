# cleanlinux-install: the installer of CleanLinux
#
#   make                 cleanlinux-install
#   make check           unit tests (no root needed)
#   make install         DESTDIR=... PREFIX=/usr
#   make dist            cleanlinux-install-VERSION.tar.gz from git HEAD

VERSION := 0.2.0
PREFIX ?= /usr
BINDIR ?= $(PREFIX)/bin

CC ?= cc
PKG_CONFIG ?= pkg-config
CFLAGS ?= -O2 -g
WARN = -std=c17 -Wall -Wextra -Wshadow -Wstrict-prototypes -Wmissing-prototypes -Wno-unused-parameter
DEFS = -DVERSION='"$(VERSION)"'

BLKID_CFLAGS := $(shell $(PKG_CONFIG) --cflags blkid 2>/dev/null)
BLKID_LIBS := $(or $(shell $(PKG_CONFIG) --libs blkid 2>/dev/null),-lblkid)
CRYPT_CFLAGS := $(shell $(PKG_CONFIG) --cflags libxcrypt 2>/dev/null)
CRYPT_LIBS := $(or $(shell $(PKG_CONFIG) --libs libxcrypt 2>/dev/null),-lcrypt)

ALL_CFLAGS = $(WARN) $(DEFS) $(BLKID_CFLAGS) $(CRYPT_CFLAGS) $(CPPFLAGS) $(CFLAGS)

OBJ = src/main.o src/plan.o src/system.o src/util.o

all: cleanlinux-install

src/%.o: src/%.c src/util.h src/install.h
	$(CC) $(ALL_CFLAGS) -c -o $@ $<

cleanlinux-install: $(OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJ) $(BLKID_LIBS) $(CRYPT_LIBS)

tests/unit: tests/unit.c src/plan.o src/util.o src/install.h src/util.h
	$(CC) $(ALL_CFLAGS) -Isrc -o $@ tests/unit.c src/plan.o src/util.o

check: cleanlinux-install tests/unit
	./tests/unit
	./tests/cli.sh ./cleanlinux-install

install: all
	install -Dm755 cleanlinux-install $(DESTDIR)$(BINDIR)/cleanlinux-install

dist:
	git archive --format=tar --prefix=cleanlinux-install-$(VERSION)/ HEAD | gzip -n -9 >cleanlinux-install-$(VERSION).tar.gz

clean:
	rm -f src/*.o tests/unit cleanlinux-install cleanlinux-install-*.tar.gz

.PHONY: all check install dist clean
