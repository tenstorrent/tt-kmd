# SPDX-FileCopyrightText: © 2025 Tenstorrent Inc.
# SPDX-License-Identifier: GPL-2.0-only
#
# Makefile for the Tenstorrent KMD User-space Library

# Compiler and Flags
CC ?= gcc
INCLUDES := -I..
# CFLAGS for the library (requires Position-Independent Code)
CFLAGS_LIB := -O2 -Wall -Wextra -fPIC $(INCLUDES)
# CFLAGS for the example
CFLAGS_EX := -O2 -Wall -Wextra

# Linker Flags
# -L. tells the linker to look for libraries in the current directory.
LDFLAGS := -L.
# -lttkmd links against our libttkmd.so
LDLIBS := -lttkmd

# Installation Prefix
PREFIX ?= /usr/local

# Project Files
TARGET_LIB := libttkmd.so
TARGET_EX := example

SOURCES_LIB := tt_kmd_lib.c
OBJECTS_LIB := $(SOURCES_LIB:.c=.o)
SOURCES_EX := example.c

# Headers that library objects depend on.
# The main public header for our library.
HEADER_LIB := tt_kmd_lib.h
# The KMD uAPI header from the parent directory.
HEADER_KMD := ../ioctl.h


# Phony targets are not files.
.PHONY: all clean install uninstall

# Default target: build everything.
all: $(TARGET_LIB) $(TARGET_EX)

# Rule to build the shared library from its object files.
$(TARGET_LIB): $(OBJECTS_LIB)
	@echo "  LD (Shared) $@"
	$(CC) -shared -o $@ $(OBJECTS_LIB)

# Rule to build the example program. It depends on the library being built first.
# The rpath argument ensures the example can find the .so file in the same directory.
$(TARGET_EX): $(SOURCES_EX) $(TARGET_LIB)
	@echo "  CC (Example) $@"
	$(CC) $(CFLAGS_EX) $(SOURCES_EX) -o $@ $(LDFLAGS) $(LDLIBS) -Wl,-rpath,'$$ORIGIN'

# Generic rule for creating object files from C source files for the library.
# The objects depend on both our library's public header and the KMD uAPI header.
$(OBJECTS_LIB): %.o: %.c $(HEADER_LIB) $(HEADER_KMD)
	@echo "  CC (Library) $<"
	$(CC) $(CFLAGS_LIB) -c $< -o $@

# Target to clean up the build directory.
clean:
	@echo "  CLEAN"
	rm -f $(TARGET_LIB) $(TARGET_EX) $(OBJECTS_LIB)

# Target to install the library and both required headers.
install: $(TARGET_LIB) $(HEADER_LIB) $(HEADER_KMD)
	@echo "  INSTALL"
	install -d $(DESTDIR)$(PREFIX)/include
	install -d $(DESTDIR)$(PREFIX)/lib
	install -m 644 $(HEADER_LIB) $(HEADER_KMD) $(DESTDIR)$(PREFIX)/include
	install -m 755 $(TARGET_LIB) $(DESTDIR)$(PREFIX)/lib

# Target to uninstall the library and header.
uninstall:
	@echo "  UNINSTALL"
	rm -f $(DESTDIR)$(PREFIX)/include/$(HEADER_LIB) $(DESTDIR)$(PREFIX)/include/ioctl.h
	rm -f $(DESTDIR)$(PREFIX)/lib/$(TARGET_LIB)
