TARGET = meowsh
PREFIX ?= /usr/local

all: $(TARGET)

$(TARGET):
	cargo build --release
	cp target/release/meowsh $(TARGET)

install: $(TARGET)
	install -D -m 755 $(TARGET) $(DESTDIR)$(PREFIX)/bin/$(TARGET)

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/$(TARGET)

clean:
	rm -f $(TARGET)

.PHONY: all clean install uninstall
