# core/c/Makefile — local C build target (also driven by the root Makefile)

CC       = gcc
CFLAGS   = -O2 -std=c11 -Wall -Wextra -pthread -D_GNU_SOURCE
CFLAGS_DBG = -O0 -g -fsanitize=thread -std=c11 -Wall -Wextra -pthread -D_GNU_SOURCE

TARGET   = spike
TARGET_TSAN = spike-tsan

all: $(TARGET)

$(TARGET): weft.c litmus_runner.c weft.h
	$(CC) $(CFLAGS) -o $(TARGET) weft.c litmus_runner.c

$(TARGET_TSAN): weft.c litmus_runner.c weft.h
	$(CC) $(CFLAGS_DBG) -o $(TARGET_TSAN) weft.c litmus_runner.c

clean:
	rm -f $(TARGET) $(TARGET_TSAN)

.PHONY: all clean
