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

# gnu11 + _DEFAULT_SOURCE, and the pair is the point: glibc gates
# strlcpy/strlcat behind __USE_MISC, where gcc's implicit-declaration
# error suggests strncpy — the one function the house standard bans.
# Measured: -std=gnu11 alone does NOT reach them while an explicit
# _POSIX_C_SOURCE is also defined, because naming any feature macro
# suppresses the implicit _DEFAULT_SOURCE. _DEFAULT_SOURCE implies
# POSIX 2008, so it replaces the macro it displaced rather than joining
# it. C11 stays out of it entirely: the ALSA alloca macro this once
# dodged is already sidestepped via snd_pcm_hw_params_malloc.
CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra -std=gnu11 -D_DEFAULT_SOURCE
LDLIBS  := -lasound -lpthread -lm

earbridge: earbridge.c earbridge.h
	$(CC) $(CFLAGS) -o $@ earbridge.c $(LDLIBS)

clean:
	rm -f earbridge

.PHONY: clean
