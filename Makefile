TARGET = meowsh
GO_SRCS = $(wildcard *.go)

all: $(TARGET)

$(TARGET): $(GO_SRCS)
	go build -o $(TARGET) .

clean:
	rm -f $(TARGET)

.PHONY: all clean
