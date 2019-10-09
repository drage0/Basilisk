# Basilisk Makefile
# ----------------
# The build sequence is simple enough that this simple Makefile is enough.
#
CC=gcc
FLAGS=-Wall -g -std=c99 -O0
FLAGS_R=-Wall -std=c99 -O3
LIBS=-lSDL2 -lSDL2_image -lSDL2_ttf -lm

all:
	-@mkdir obj
	$(CC) $(FLAGS) -c ./src/main.c -o ./obj/main.o
	$(CC) ./obj/main.o -o basilisk $(LIBS)

release:
	-@mkdir obj
	$(CC) $(FLAGS_R) -c ./src/main.c -o ./obj/main.o
	$(CC) ./obj/main.o -o basilisk $(LIBS)

asm:
	-@mkdir obj
	$(CC) $(FLAGS) -S ./src/main.c -o ./obj/main.asm
