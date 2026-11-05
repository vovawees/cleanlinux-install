# cleanlinux-install: the installer of CleanLinux
#
#   make                 cleanlinux-install and cleanlinux-setup (the TUI)
#   make check           unit, CLI and TUI tests (no root needed)
#   make install         DESTDIR=... PREFIX=/usr
#   make dist            cleanlinux-install-VERSION.tar.gz from git HEAD

VERSION := 0.8.0
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

COMMON = src/plan.o src/system.o src/util.o src/copy.o
OBJ = src/main.o $(COMMON)
SETUP_OBJ = src/setup.o src/tui.o $(COMMON)

all: cleanlinux-install cleanlinux-setup

src/%.o: src/%.c src/util.h src/install.h
	$(CC) $(ALL_CFLAGS) -c -o $@ $<

src/tui.o: src/tui.h
src/setup.o: src/tui.h src/setup_text.h

cleanlinux-install: $(OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJ) $(BLKID_LIBS) $(CRYPT_LIBS)

cleanlinux-setup: $(SETUP_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(SETUP_OBJ) $(BLKID_LIBS) $(CRYPT_LIBS)

tests/unit: tests/unit.c src/plan.o src/util.o src/tui.o src/copy.o src/install.h src/util.h src/tui.h
	$(CC) $(ALL_CFLAGS) -Isrc -o $@ tests/unit.c src/plan.o src/util.o src/tui.o src/copy.o

check: all tests/unit
	./tests/unit
	./tests/cli.sh ./cleanlinux-install
	python3 tests/setup.py ./cleanlinux-setup

install: all
	install -Dm755 cleanlinux-install $(DESTDIR)$(BINDIR)/cleanlinux-install
	install -Dm755 cleanlinux-setup $(DESTDIR)$(BINDIR)/cleanlinux-setup

dist:
	git archive --format=tar --prefix=cleanlinux-install-$(VERSION)/ HEAD | gzip -n -9 >cleanlinux-install-$(VERSION).tar.gz

clean:
	rm -f src/*.o tests/unit cleanlinux-install cleanlinux-setup cleanlinux-install-*.tar.gz

.PHONY: all check install dist clean
