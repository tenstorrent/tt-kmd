# SPDX-FileCopyrightText: © 2025 Tenstorrent Inc.
# SPDX-License-Identifier: GPL-2.0-only

CC = gcc
CXX = g++
CFLAGS = -Wall -O2
CXXFLAGS = -Wall -O2 -std=c++17

TARGETS = signal_test_daemon signal_trigger rt_signal_daemon rt_signal_trigger simple_loop

all: $(TARGETS)

signal_test_daemon: signal_test_daemon.cpp
	$(CXX) $(CXXFLAGS) -o $@ $<

signal_trigger: signal_trigger.c
	$(CC) $(CFLAGS) -o $@ $<

rt_signal_daemon: rt_signal_daemon.cpp
	$(CXX) $(CXXFLAGS) -o $@ $<

rt_signal_trigger: rt_signal_trigger.c
	$(CC) $(CFLAGS) -o $@ $<

simple_loop: simple_loop.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f $(TARGETS)

.PHONY: all clean

