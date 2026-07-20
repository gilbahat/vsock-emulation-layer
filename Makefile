# vsock-emulation-layer -- emulate AF_VSOCK over Unix sockets on macOS.
#
# Targets:
#   vsock-emu             host peer CLI
#   tests/vsock_echo      plain AF_VSOCK echo server+client (test vehicle)
#   libvsock_unix.dylib   interposer (runtime-injected or link-time)
#
#   make            build everything
#   make test       build + run the self-tests
#   make install    install vsock-emu, libvsock_unix.dylib, vsock_unix.h under PREFIX
#   make uninstall  remove the installed files
#   make clean

CC      ?= cc
CFLAGS  ?= -O2 -g -Wall -Wextra -Wno-unused-parameter
CPPFLAGS += -I$(CURDIR)

# Install layout (overridable; Homebrew passes PREFIX=#{prefix}). DESTDIR is
# honoured for staged installs.
PREFIX  ?= /usr/local
BINDIR  ?= $(PREFIX)/bin
LIBDIR  ?= $(PREFIX)/lib
INCDIR  ?= $(PREFIX)/include
INSTALL ?= install

BINS    := vsock-emu tests/vsock_echo
DYLIB   := libvsock_unix.dylib

.PHONY: all
all: $(BINS) $(DYLIB)

vsock-emu: vsock-emu.c vsock_unix.h
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ vsock-emu.c

tests/vsock_echo: tests/vsock_echo.c vsock_unix.h
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ tests/vsock_echo.c

# The interposer is a dynamic library usable two ways: injected at runtime via
# DYLD_INSERT_LIBRARIES (tier 1), or linked as a normal dependency (tier 2,
# "library mode"). The @rpath install name makes the linked form resolvable;
# it does not affect the injected form (insertion keys off the env-var path).
$(DYLIB): interpose.c vsock_unix.h
	$(CC) $(CFLAGS) $(CPPFLAGS) -dynamiclib \
	    -install_name @rpath/libvsock_unix.dylib -o $@ interpose.c

.PHONY: test
test: all
	@echo "== convention: vsock-emu <-> vsock-emu (helpers, no interposer) =="
	@sh tests/run_convention.sh
	@echo "== interposer: unchanged AF_VSOCK vsock_echo, injected at runtime =="
	@sh tests/run_interposer.sh

# Install the consumable pieces: the host peer CLI, the interposer/library-mode
# dylib, and the convention header. tests/vsock_echo is a test vehicle and is
# intentionally not installed.
.PHONY: install
install: vsock-emu $(DYLIB)
	$(INSTALL) -d $(DESTDIR)$(BINDIR) $(DESTDIR)$(LIBDIR) $(DESTDIR)$(INCDIR)
	$(INSTALL) -m 0755 vsock-emu $(DESTDIR)$(BINDIR)/vsock-emu
	$(INSTALL) -m 0755 $(DYLIB) $(DESTDIR)$(LIBDIR)/$(DYLIB)
	$(INSTALL) -m 0644 vsock_unix.h $(DESTDIR)$(INCDIR)/vsock_unix.h

.PHONY: uninstall
uninstall:
	rm -f $(DESTDIR)$(BINDIR)/vsock-emu
	rm -f $(DESTDIR)$(LIBDIR)/$(DYLIB)
	rm -f $(DESTDIR)$(INCDIR)/vsock_unix.h

.PHONY: clean
clean:
	rm -f $(BINS) $(DYLIB)
	rm -rf *.dSYM tests/*.dSYM
