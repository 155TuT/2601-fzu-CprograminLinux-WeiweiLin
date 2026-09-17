CC = gcc
CPPFLAGS += -D_GNU_SOURCE
CFLAGS ?= -std=c11 -Wall -Wextra -Wpedantic -O0 -g
TARGETS = ipc_server ipc_client
OBJECTS = server.o client.o ipc.o

.PHONY: all verify clean

all: $(TARGETS)

ipc_server: server.o ipc.o
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

ipc_client: client.o ipc.o
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

%.o: %.c ipc.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

verify: all
	python3 verify.py

clean:
	rm -f $(TARGETS) $(OBJECTS)
