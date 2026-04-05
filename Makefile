CC = gcc
CFLAGS = -I. -Wall -std=c99
LDFLAGS = -Llib -lraylib -lopengl32 -lgdi32 -lwinmm

SRC = main.c
TARGET = main

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $(TARGET) $(SRC) $(LDFLAGS)

clean:
	del $(TARGET).exe 