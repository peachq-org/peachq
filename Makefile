CC=gcc
AR=ar
RELEASE_CFLAGS = -fPIC -Wall -Wextra -Werror -Wpedantic -std=c17 -O3 -march=native
DEBUG_CFLAGS = -fPIC -Wall -Wextra -std=c17 -g -O0 -march=native
# CFLAGS = $(DEBUG_CFLAGS)
CFLAGS = $(RELEASE_CFLAGS)
HEADERS = core/format.h core/storm.h
OBJECTS = format.o storm.o
TARGET = stormdb

%.o: core/%.c $(HEADERS)
	$(CC) -c $^ $(CFLAGS)

lib: $(OBJECTS)
	$(AR) rc lib$(TARGET).a $(OBJECTS)

main.o: app/main.c $(HEADERS)
	$(CC) -c $^ $(CFLAGS) 

app: $(OBJECTS) main.o lib
	$(CC) $(CFLAGS) -o $(TARGET) $(OBJECTS) main.o -L. -l$(TARGET) 

default: app

clean:
	-rm -f *.o
	-rm -f $(TARGET)
	-rm -f lib$(TARGET).a
	-rm core/*.gch