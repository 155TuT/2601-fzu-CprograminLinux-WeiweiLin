CC = gcc
CPPFLAGS += -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE -Iinclude
CFLAGS ?= -std=c11 -Wall -Wextra -Wpedantic -O2 -g
DEPFLAGS = -MMD -MP
COMMON = build/net/protocol.o build/storage/files.o
OBJECTS = $(COMMON) build/backend/corpus.o build/storage/session.o build/storage/checkpoint.o build/server/main.o build/client/main.o build/generator/main.o
TARGETS = bin/stream_server bin/stream_client bin/text_generator

.PHONY: all verify clean
all: $(TARGETS)

bin/stream_server: build/server/main.o build/storage/session.o build/backend/corpus.o $(COMMON)
	@mkdir -p $(@D)
	$(CC) $(LDFLAGS) -o $@ $^$(if $(strip $(LDLIBS)), $(LDLIBS))

bin/stream_client: build/client/main.o build/storage/checkpoint.o $(COMMON)
	@mkdir -p $(@D)
	$(CC) $(LDFLAGS) -o $@ $^$(if $(strip $(LDLIBS)), $(LDLIBS))

bin/text_generator: build/generator/main.o build/net/protocol.o
	@mkdir -p $(@D)
	$(CC) $(LDFLAGS) -o $@ $^$(if $(strip $(LDLIBS)), $(LDLIBS))

build/%.o: src/%.c
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(DEPFLAGS) -c $< -o $@

verify: all
	python3 tests/verify.py

clean:
	rm -rf build bin

-include $(OBJECTS:.o=.d)
