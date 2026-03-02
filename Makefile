TARGET = meowsh
GO_SRCS = $(wildcard src/*.go)
PREFIX ?= /usr/local

all: $(TARGET)

$(TARGET): $(GO_SRCS)
	go build -o $(TARGET) ./src

install: $(TARGET)
	install -D -m 755 $(TARGET) $(DESTDIR)$(PREFIX)/bin/$(TARGET)

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/$(TARGET)

clean:
	rm -f $(TARGET)

.PHONY: all clean install uninstall
