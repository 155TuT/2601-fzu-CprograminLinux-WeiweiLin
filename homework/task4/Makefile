CC = gcc
CPPFLAGS += -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE -Iinclude -Ideps
CFLAGS ?= -std=c11 -Wall -Wextra -Wpedantic -O2 -g
DEPFLAGS = -MMD -MP
COMMON = build/net/protocol.o build/net/discovery.o build/storage/files.o
BACKENDS = build/backend/model.o build/backend/process.o
CLIENT = build/client/main.o build/client/terminal.o build/client/view.o build/client/screen.o build/client/status.o build/client/state.o build/storage/checkpoint.o
OBJECTS = $(COMMON) $(BACKENDS) $(CLIENT) build/storage/session.o build/server/main.o
TARGETS = bin/stream_server bin/stream_client

.PHONY: all verify clean
all: $(TARGETS)

bin/stream_server: build/server/main.o build/storage/session.o $(BACKENDS) $(COMMON)
	@mkdir -p $(@D)
	$(CC) $(LDFLAGS) -o $@ $^$(if $(strip $(LDLIBS)), $(LDLIBS))

bin/stream_client: $(CLIENT) $(COMMON)
	@mkdir -p $(@D)
	$(CC) $(LDFLAGS) -o $@ $^$(if $(strip $(LDLIBS)), $(LDLIBS))

build/%.o: src/%.c
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(DEPFLAGS) -c $< -o $@

build/tests/status_unit: tests/status_unit.c build/client/status.o include/task4/status.h include/task4/terminal.h
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ tests/status_unit.c build/client/status.o $(LDLIBS)

build/tests/checkpoint_faults: tests/checkpoint_faults.c build/storage/checkpoint.o build/storage/files.o include/task4/checkpoint.h
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -Wl,--wrap=write,--wrap=fsync,--wrap=close,--wrap=unlink -o $@ tests/checkpoint_faults.c build/storage/checkpoint.o build/storage/files.o $(LDLIBS)

verify: all build/tests/status_unit build/tests/checkpoint_faults
	./build/tests/status_unit
	./build/tests/checkpoint_faults
	python3 tests/verify.py
	python3 tests/verify_discovery.py
	python3 tests/verify_controls.py
	python3 tests/verify_terminal.py
	python3 tests/verify_tui.py
	python3 tests/verify_mouse_render.py
	python3 tests/client_sessions_unit.py
	python3 tests/verify_client_sessions.py
	python3 tests/verify_dashboard.py
	python3 tests/model_adapter_unit.py
	python3 tests/model_launch_unit.py
	python3 tests/verify_model.py

clean:
	rm -rf build bin

-include $(OBJECTS:.o=.d)
