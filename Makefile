CC = gcc
CFLAGS = -Wall -Wextra -Wno-unused-parameter -Isrc -g -O2 -D_POSIX_C_SOURCE=200809L -D_GNU_SOURCE
LDFLAGS = 
PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin
SHELLS_FILE ?= /etc/shells

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
	./tests/test_interactive.sh

fuzz: tests/fuzzer meowsh
	./tests/fuzzer 2000

install: test $(TARGET)
	install -d "$(DESTDIR)$(BINDIR)"
	install -m 755 "$(TARGET)" "$(DESTDIR)$(BINDIR)/$(TARGET)"
	@if [ -n "$(DESTDIR)" ]; then \
		echo "Skipping $(SHELLS_FILE) update because DESTDIR is set."; \
	elif [ ! -f "$(SHELLS_FILE)" ] || [ ! -w "$(SHELLS_FILE)" ]; then \
		echo "Note: could not update $(SHELLS_FILE)."; \
		echo "Run: echo \"$(BINDIR)/$(TARGET)\" | sudo tee -a $(SHELLS_FILE)"; \
	elif grep -Fqx "$(BINDIR)/$(TARGET)" "$(SHELLS_FILE)"; then \
		echo "$(BINDIR)/$(TARGET) already present in $(SHELLS_FILE)."; \
	else \
		echo "$(BINDIR)/$(TARGET)" >> "$(SHELLS_FILE)"; \
		echo "Added $(BINDIR)/$(TARGET) to $(SHELLS_FILE)."; \
	fi

local-install: test $(TARGET)
	install -d "$(HOME)/.local/bin"
	install -m 755 "$(TARGET)" "$(HOME)/.local/bin/$(TARGET)"

tests/test_completion: tests/test_completion.o $(filter-out src/main.o, $(OBJS))
	$(CC) $^ -o $@ $(LDFLAGS)

tests/fuzzer: tests/fuzzer.c
	$(CC) $(CFLAGS) $< -o $@

.PHONY: all clean test fuzz install local-install
