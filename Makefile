CC      ?= gcc
CFLAGS  ?= -Wall -Wextra -std=c11 -O2 -Iinclude
LDFLAGS ?=

# Header dependency tracking: -MMD writes src/x.d beside each object, -MP adds
# phony targets so a deleted header does not break the next build. Kept out of
# CFLAGS so it survives `make check`'s sanitizer override.
DEPFLAGS := -MMD -MP

SRCS := src/zeus.c src/isa.c src/bytecode.c src/vm.c src/net.c src/snapshot.c src/policy.c src/supervisor.c src/journal.c src/jail.c src/asm.c src/disasm.c src/main.c
OBJS := $(SRCS:.c=.o)
DEPS := $(OBJS:.o=.d)
LIB_OBJS := $(filter-out src/main.o,$(OBJS))

TARGET := zeus

SAN_CFLAGS  := -Wall -Wextra -std=c11 -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -Iinclude
SAN_LDFLAGS := -fsanitize=address,undefined

.PHONY: all clean test smoke check

all: $(TARGET) zasm zdis

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

zasm: $(TARGET)
	@echo '#!/bin/sh' > $@
	@echo 'exec ./$(TARGET) asm "$$@"' >> $@
	chmod +x $@

zdis: $(TARGET)
	@echo '#!/bin/sh' > $@
	@echo 'exec ./$(TARGET) dis "$$@"' >> $@
	chmod +x $@

test: $(TARGET)
	$(CC) $(CFLAGS) -o tests/test_vm tests/test_vm.c $(LIB_OBJS) $(LDFLAGS)
	$(CC) $(CFLAGS) -o tests/test_asm tests/test_asm.c $(LIB_OBJS) $(LDFLAGS)
	$(CC) $(CFLAGS) -o tests/test_safety tests/test_safety.c $(LIB_OBJS) $(LDFLAGS)
	$(CC) $(CFLAGS) -o tests/test_snapshot tests/test_snapshot.c $(LIB_OBJS) $(LDFLAGS)
	$(CC) $(CFLAGS) -o tests/test_guard tests/test_guard.c $(LIB_OBJS) $(LDFLAGS)
	./tests/test_vm
	./tests/test_asm
	./tests/test_safety
	./tests/test_snapshot
	./tests/test_guard
	bash tests/test_ask.sh < /dev/null
	bash tests/deny_transcript.sh < /dev/null > /dev/null
	bash tests/journal_transcript.sh < /dev/null > /dev/null

# Full test suite under ASan + UBSan, then a normal rebuild.
check:
	$(MAKE) clean
	$(MAKE) test CFLAGS="$(SAN_CFLAGS)" LDFLAGS="$(SAN_LDFLAGS)"
	$(MAKE) clean
	$(MAKE) test

smoke: all
	./smoke_test.sh

%.o: %.c
	$(CC) $(CFLAGS) $(DEPFLAGS) -c -o $@ $<

-include $(DEPS)

clean:
	rm -f $(OBJS) $(DEPS) $(TARGET) zasm zdis
	rm -f tests/test_vm tests/test_asm tests/test_safety tests/test_snapshot tests/test_guard
	rm -f examples/*.zeus
