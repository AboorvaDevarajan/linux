# SPDX-License-Identifier: GPL-2.0-only
TEST_GEN_PROGS := atomic-rmw atomic-rmw-nolibc atomic-p9-p10 atomic-p9-p10-nolibc
TEST_GEN_PROGS_EXTENDED := atomic-rmw-bench
TEST_PROGS := atomic-barrier-macros.sh run-litmus.sh
TEST_FILES := settings litmus

include ../../lib.mk
include ../flags.mk

NOLIBC_CFLAGS := -nostdlib -static -ffreestanding -fno-stack-protector

$(OUTPUT)/atomic-rmw: ../harness.c ../utils.c
$(OUTPUT)/atomic-rmw: LDLIBS += -lpthread
$(OUTPUT)/atomic-rmw-bench: ../harness.c ../utils.c
$(OUTPUT)/atomic-p9-p10: ../harness.c ../utils.c

$(OUTPUT)/atomic-rmw-nolibc: atomic-rmw-nolibc.c atomic-rmw-asm.h
	$(call msg,CC,,$@)
	$(Q)$(CC) $(CFLAGS) $(NOLIBC_CFLAGS) $(LDFLAGS) -o $@ atomic-rmw-nolibc.c

$(OUTPUT)/atomic-p9-p10-nolibc: atomic-p9-p10-nolibc.c atomic-rmw-asm.h
	$(call msg,CC,,$@)
	$(Q)$(CC) $(CFLAGS) $(NOLIBC_CFLAGS) $(LDFLAGS) -o $@ atomic-p9-p10-nolibc.c

.PHONY: atomic-rmw-nolibc atomic-p9-p10-nolibc
atomic-rmw-nolibc: $(OUTPUT)/atomic-rmw-nolibc
atomic-p9-p10-nolibc: $(OUTPUT)/atomic-p9-p10-nolibc
