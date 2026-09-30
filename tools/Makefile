# SPDX-License-Identifier: GPL-2.0-only
#
# Userspace tool for the Lindroid EVDI display control ioctls.
#
# Build on the device or with a cross toolchain:
#   make                          # host build
#   make CC=aarch64-linux-gnu-gcc
#
# The tool only needs the uapi header, so it can be built standalone:
#   gcc -I../drivers/lindroid-drm/uapi -I<kernel>/include/uapi lindroid-ctl.c -o lindroid-ctl

KVER  ?= $(shell uname -r)
KPATH ?= ../dm-kernel

CC     ?= cc
CFLAGS ?= -O2 -Wall -Wextra -D_GNU_SOURCE
# -idirafter keeps glibc's own headers ahead of the kernel UAPI ones.
CPPFLAGS += -I. -I../drivers/lindroid-drm/uapi -idirafter $(KPATH)/include/uapi

PREFIX ?= /usr/local

BIN := lindroid-ctl

all: $(BIN)

$(BIN): lindroid-ctl.c
	$(CC) $(CFLAGS) $(CPPFLAGS) $< -o $@

clean:
	rm -f $(BIN)

install: $(BIN)
	install -D -m 0755 $(BIN) $(DESTDIR)$(PREFIX)/bin/$(BIN)

check: test-control
	./test-control

test-control: test-control.c
	$(CC) $(CFLAGS) -I.. $(CPPFLAGS) $< -o $@

.PHONY: all clean install check
