TARGET = meowsh
GO_SRCS = $(wildcard src/*.go)

all: $(TARGET)

$(TARGET): $(GO_SRCS)
	go build -o $(TARGET) ./src

clean:
	rm -f $(TARGET)

.PHONY: all clean
