# SPDX-License-Identifier: GPL-2.0
noarg:
	$(MAKE) -C ../

TEST_PROGS := cpuidle_latency.sh
TEST_FILES := settings

# Driven by cpuidle_latency.sh rather than run on its own.
TEST_GEN_FILES := timer_wakeup

top_srcdir = ../../../../..
include ../../lib.mk
include ../flags.mk
