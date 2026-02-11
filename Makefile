CC = gcc
CFLAGS = -Wall -Wextra -Wno-unused-parameter -Isrc -g -O2 -D_POSIX_C_SOURCE=200809L -D_GNU_SOURCE
LDFLAGS = 
PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin

SRCS = $(wildcard src/*.c)
OBJS = $(SRCS:.c=.o)
TARGET = meowsh

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(OBJS) -o $(TARGET) $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET) tests/*.o tests/test_completion

test: tests/test_completion
	./tests/test_completion

fuzz: tests/fuzzer meowsh
	./tests/fuzzer 2000

install: test $(TARGET)
	install -d "$(DESTDIR)$(BINDIR)"
	install -m 755 "$(TARGET)" "$(DESTDIR)$(BINDIR)/$(TARGET)"

local-install: test $(TARGET)
	install -d "$(HOME)/.local/bin"
	install -m 755 "$(TARGET)" "$(HOME)/.local/bin/$(TARGET)"

tests/test_completion: tests/test_completion.o $(filter-out src/main.o, $(OBJS))
	$(CC) $^ -o $@ $(LDFLAGS)

tests/fuzzer: tests/fuzzer.c
	$(CC) $(CFLAGS) $< -o $@

.PHONY: all clean test fuzz install local-install
