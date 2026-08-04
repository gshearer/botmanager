# earbridge — built ON the robot's CM4, not by the botman meson tree.
#
# Deliberately a plain Makefile: this is the only artifact in the project that
# compiles on a different machine (Debian 13 on a Raspberry Pi CM4), and the
# toolchain there is stock gcc/make with libasound2-dev. Adding it to meson
# would mean cross-compiling for aarch64 to save one `make`.
#
#   make            build
#   make clean      remove the binary
#
# Deploy with ../earbridge/deploy.sh from the botman host, which copies these
# sources over and runs this Makefile remotely.

CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra -std=c11 -D_POSIX_C_SOURCE=200809L
LDLIBS  := -lasound -lpthread

earbridge: earbridge.c earbridge.h
	$(CC) $(CFLAGS) -o $@ earbridge.c $(LDLIBS)

clean:
	rm -f earbridge

.PHONY: clean
