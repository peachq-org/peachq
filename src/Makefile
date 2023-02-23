CC=clang
RELEASE_CFLAGS = -Wall -Wextra -Werror -Wpedantic -std=c17 -g -O3 -march=native
DEBUG_CFLAGS = -Wall -Wextra -std=c17 -g -O0 -march=native
HEADERS = format.h storm.h
OBJECTS = format.o storm.o main.o
TARGET = stormdb

%.o: %.c $(HEADERS)
	$(CC) $(CFLAGS) $< -c

debug: $(OBJECTS)
	$(CC) $(DEBUG_CFLAGS) $(OBJECTS) -o $(TARGET)

release: $(OBJECTS)
	$(CC) $(RELEASE_CFLAGS) $(OBJECTS) -o $(TARGET)

default: debug

clean:
	-rm -f *.o
	-rm -f $(TARGET)