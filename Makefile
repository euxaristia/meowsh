TARGET = meowsh
PREFIX ?= /usr/local

all: $(TARGET)

$(TARGET):
	cd meowsh_rs && cargo build --release
	cp meowsh_rs/target/release/meowsh_rs $(TARGET)

install: $(TARGET)
	install -D -m 755 $(TARGET) $(DESTDIR)$(PREFIX)/bin/$(TARGET)

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/$(TARGET)

clean:
	rm -f $(TARGET)

.PHONY: all clean install uninstall
