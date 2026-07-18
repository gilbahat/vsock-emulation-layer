# vsock-emulation-layer -- emulate AF_VSOCK over Unix sockets on macOS.
#
# Targets:
#   vsock-emu             host peer CLI
#   tests/vsock_echo      plain AF_VSOCK echo server+client (test vehicle)
#   libvsock_unix.dylib   interposer (runtime-injected or link-time)
#
#   make            build everything
#   make test       build + run the self-tests
#   make clean

CC      ?= cc
CFLAGS  ?= -O2 -g -Wall -Wextra -Wno-unused-parameter
CPPFLAGS += -I$(CURDIR)

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

.PHONY: clean
clean:
	rm -f $(BINS) $(DYLIB)
	rm -rf *.dSYM tests/*.dSYM
