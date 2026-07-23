# SPDX-FileCopyrightText: © 2024 Tenstorrent Inc.
# SPDX-License-Identifier: GPL-2.0-only

all::

PROG := ttkmd_test

# Host-only unit test binary (no hardware/kernel module required).
UNIT_PROG := fwlog_test

TEST_SOURCES := get_driver_info.cpp get_device_info.cpp query_mappings.cpp \
	dma_buf.cpp pin_pages.cpp config_space.cpp lock.cpp hwmon.cpp map_peer_bar.cpp \
	ioctl_overrun.cpp ioctl_zeroing.cpp tlbs.cpp dmabuf_export.cpp release.cpp \
	mappings_debugfs.cpp procfs_pids.cpp excl.cpp

CORE_SOURCES := enumeration.cpp util.cpp devfd.cpp main.cpp test_failure.cpp
SOURCES := $(CORE_SOURCES) $(TEST_SOURCES)

BUILDDIR := build
OBJS := $(patsubst %.cpp,$(BUILDDIR)/%.o,$(SOURCES))
UNIT_OBJS := $(BUILDDIR)/fwlog_test.o $(BUILDDIR)/fwlog_parser.o

OPT_FLAGS := -O2
CXXFLAGS := -std=c++17 -Wall -Wno-narrowing $(OPT_FLAGS)

# fwlog_test.cpp includes the driver's fwlog.h directly from the parent directory.
$(BUILDDIR)/fwlog_test.o: CXXFLAGS += -I..
$(BUILDDIR)/fwlog_test.o: fwlog_compat.h ../fwlog.h

# Compile the same parser implementation linked into the kernel module.  The
# compatibility header supplies kernel-style integer names to the host compiler.
$(BUILDDIR)/fwlog_parser.o: ../fwlog_parser.c fwlog_compat.h ../fwlog.h | $(BUILDDIR)
	$(CXX) $(CXXFLAGS) -I.. -include fwlog_compat.h -x c++ -c $< -o $@

.PHONY: all
all:: $(PROG) $(UNIT_PROG)

$(PROG): $(OBJS)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) $(LIBS) $^ -o $@

$(UNIT_PROG): $(UNIT_OBJS)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) $^ -o $@

$(BUILDDIR)/%.o: %.cpp | $(BUILDDIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILDDIR):
	mkdir $(BUILDDIR)

# Build and run the host-only unit tests.
.PHONY: check
check: $(UNIT_PROG)
	./$(UNIT_PROG)

.PHONY: clean
clean::
	-rm -rf $(BUILDDIR)
	-rm -f $(PROG) $(UNIT_PROG)
