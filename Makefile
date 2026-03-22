# Compiler and flags
CC = gcc
CFLAGS = -lSDL2 -O2 -g3

all: mars_sdl_port

mars_sdl_port: mars_sdl_port.c
	$(CC) $< -o $@ $(CFLAGS)

# Clean target
clean:
	rm -f mars_sdl_port
