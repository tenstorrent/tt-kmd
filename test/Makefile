# SPDX-FileCopyrightText: © 2024 Tenstorrent Inc.
# SPDX-License-Identifier: GPL-2.0-only

all::

PROG := ttkmd_test

TEST_SOURCES := get_driver_info.cpp get_device_info.cpp query_mappings.cpp \
	dma_buf.cpp pin_pages.cpp config_space.cpp lock.cpp hwmon.cpp map_peer_bar.cpp \
	ioctl_overrun.cpp ioctl_zeroing.cpp tlbs.cpp release.cpp

CORE_SOURCES := enumeration.cpp util.cpp devfd.cpp main.cpp test_failure.cpp
SOURCES := $(CORE_SOURCES) $(TEST_SOURCES)

BUILDDIR := build
OBJS := $(patsubst %.cpp,$(BUILDDIR)/%.o,$(SOURCES))

OPT_FLAGS := -O2
CXXFLAGS := -std=c++17 -Wall -Wno-narrowing $(OPT_FLAGS)

.PHONY: all
all:: $(PROG)

$(PROG): $(OBJS)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) $(LIBS) $^ -o $@

$(OBJS): $(BUILDDIR)/%.o: %.cpp | $(BUILDDIR)
	$(CXX) $(CXXFLAGS) -c $^ -o $@

$(BUILDDIR):
	mkdir $(BUILDDIR)

.PHONY: clean
clean::
	-rm -rf $(BUILDDIR)
	-rm -f $(PROG)
