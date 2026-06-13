CC      ?= gcc
CFLAGS  ?= -Wall -Wextra -std=c11 -O2 -Iinclude
LDFLAGS ?=

SRCS := src/zeus.c src/isa.c src/bytecode.c src/vm.c src/net.c src/asm.c src/disasm.c src/main.c
OBJS := $(SRCS:.c=.o)

TARGET := zeus

.PHONY: all clean test smoke

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
	$(CC) $(CFLAGS) -o tests/test_vm tests/test_vm.c $(filter-out src/main.o,$(OBJS)) $(LDFLAGS)
	$(CC) $(CFLAGS) -o tests/test_asm tests/test_asm.c $(filter-out src/main.o,$(OBJS)) $(LDFLAGS)
	./tests/test_vm
	./tests/test_asm

smoke: all
	./smoke_test.sh

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJS) $(TARGET) zasm zdis tests/test_vm tests/test_asm
	rm -f examples/*.zeus
