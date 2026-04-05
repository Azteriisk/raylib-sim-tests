CC = gcc
CFLAGS = -I. -Wall -std=c99
LDFLAGS = -Llib -lraylib -lopengl32 -lgdi32 -lwinmm

SRC = main.c sim_state.c bulb.c physics_render.c input_ui.c
TARGET = main.exe

all: $(TARGET)

$(TARGET): $(SRC) sim.h
	$(CC) $(CFLAGS) -o $(TARGET) $(SRC) $(LDFLAGS)

clean:
	-del /Q $(TARGET)
	-del /Q *.o
