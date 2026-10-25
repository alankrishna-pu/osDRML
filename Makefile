CC = gcc
CFLAGS = -O2 -Wall -Wextra -std=c99 -D_POSIX_C_SOURCE=200809L

# Detect Windows vs POSIX
ifeq ($(OS),Windows_NT)
    LDLIBS = -lws2_32
    TARGET = deadlock_server.exe
else
    LDLIBS =
    TARGET = deadlock_server
endif

.PHONY: all build run clean

all: build

build:
	$(CC) $(CFLAGS) -o $(TARGET) server.c deadlock.c $(LDLIBS)

run: build
ifeq ($(OS),Windows_NT)
	.\$(TARGET)
else
	./$(TARGET)
endif

clean:
ifeq ($(OS),Windows_NT)
	-cmd /c "del /Q /F $(TARGET) *.o 2>nul"
else
	rm -f $(TARGET) *.o
endif
