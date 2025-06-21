# SPDX-FileCopyrightText: © 2025 Tenstorrent Inc.
# SPDX-License-Identifier: GPL-2.0-only
#
# Makefile for the Tenstorrent KMD User-space Library

# Compiler and Flags
CC ?= gcc
# Add parent directory to include path for ioctl.h
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
# Project-specific include directory for namespacing.
INCLUDE_DIR := $(PREFIX)/include/tenstorrent

# Project Files
TARGET_LIB := libttkmd.so
TARGET_EX := example

SOURCES_LIB := ttkmd.c
OBJECTS_LIB := $(SOURCES_LIB:.c=.o)
SOURCES_EX := example.c

# The main public header for our library.
HEADER_LIB := ttkmd.h
# The KMD uAPI header from the parent directory (private, not installed).
HEADER_KMD := ../ioctl.h
# The shared version header between the library and the kernel module.
HEADER_VERSION := ../version.h


# Phony targets are not files.
.PHONY: all clean install uninstall run

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
# The objects depend on both our library's public header and the private KMD uAPI header.
$(OBJECTS_LIB): %.o: %.c $(HEADER_LIB) $(HEADER_KMD) $(HEADER_VERSION)
	@echo "  CC (Library) $<"
	$(CC) $(CFLAGS_LIB) -c $< -o $@

# Target to clean up the build directory.
clean:
	@echo "  CLEAN"
	rm -f $(TARGET_LIB) $(TARGET_EX) $(OBJECTS_LIB)

# Target to install the library and the public header.
# The header is installed into a project-specific subdirectory to avoid name conflicts.
install: $(TARGET_LIB) $(HEADER_LIB)
	@echo "  INSTALL"
	install -d $(DESTDIR)$(INCLUDE_DIR)
	install -d $(DESTDIR)$(PREFIX)/lib
	install -m 644 $(HEADER_LIB) $(DESTDIR)$(INCLUDE_DIR)
	install -m 755 $(TARGET_LIB) $(DESTDIR)$(PREFIX)/lib

# Target to uninstall the library and header.
uninstall:
	@echo "  UNINSTALL"
	rm -f $(DESTDIR)$(INCLUDE_DIR)/$(HEADER_LIB)
	rm -f $(DESTDIR)$(PREFIX)/lib/$(TARGET_LIB)
	-rmdir --ignore-fail-on-non-empty $(DESTDIR)$(INCLUDE_DIR)

# Convenience target to build and run the example.
run: $(TARGET_EX)
	@echo "  RUN"
	./$(TARGET_EX)

