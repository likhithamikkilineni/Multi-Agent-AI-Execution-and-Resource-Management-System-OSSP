CC = gcc
CFLAGS = -D_DEFAULT_SOURCE -Wall -Wextra -std=c11 -O2
TARGET = build/aura_runtime
SRC = src/aura_runtime.c

all: $(TARGET)

$(TARGET): $(SRC)
	mkdir -p build
	$(CC) $(CFLAGS) -o $(TARGET) $(SRC)

clean:
	rm -f $(TARGET)

.PHONY: all clean
