CC = gcc
CFLAGS = -Wall -Wextra -Wno-unused-parameter -Isrc -g -O2 -D_POSIX_C_SOURCE=200809L -D_GNU_SOURCE
LDFLAGS = 

SRCS = $(wildcard src/*.c)
OBJS = $(SRCS:.c=.o)
TARGET = meowsh

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(OBJS) -o $(TARGET) $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET)

.PHONY: all clean
