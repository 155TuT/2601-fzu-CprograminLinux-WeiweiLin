CC = gcc
CPPFLAGS += -D_POSIX_C_SOURCE=200809L
CFLAGS ?= -std=c11 -Wall -Wextra -Wpedantic -O0 -g
TARGETS = tcp_server tcp_client
OBJECTS = server.o client.o net.o

.PHONY: all verify clean

all: $(TARGETS)

tcp_server: server.o net.o
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

tcp_client: client.o net.o
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

%.o: %.c net.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

verify: all
	python3 verify.py

clean:
	rm -f $(TARGETS) $(OBJECTS)
