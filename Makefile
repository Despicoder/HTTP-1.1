CC      := gcc
CFLAGS  := -Wall -Wextra -O2 -std=c11 -Iinclude -pthread -Wno-format-truncation
LDFLAGS := -pthread

SRC := src/main.c src/server.c src/threadpool.c src/http_parser.c src/response.c
OBJ := $(SRC:.c=.o)
TARGET := httpserver

.PHONY: all clean run

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CC) $(OBJ) -o $(TARGET) $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

run: $(TARGET)
	./$(TARGET)

clean:
	rm -f $(OBJ) $(TARGET)
