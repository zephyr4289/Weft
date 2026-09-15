CC      = gcc
CFLAGS  = -std=c11 -O2 -Wall -Wextra -pthread -D_GNU_SOURCE
LDFLAGS = -pthread

TARGET  = spike
SOURCES = weft.c spike.c
HEADERS = weft.h

.PHONY: all clean run

all: $(TARGET)

$(TARGET): $(SOURCES) $(HEADERS)
	$(CC) $(CFLAGS) -o $@ $(SOURCES) $(LDFLAGS)

run: $(TARGET)
	./$(TARGET) 10 120 60 | tee spike-result.txt

clean:
	rm -f $(TARGET) spike-result.txt
