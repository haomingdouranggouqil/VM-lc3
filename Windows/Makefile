CC=gcc
C-FLAGS=-O3
CPP=g++
CPP-FLAGS=-std=c++14 -O3

all: lc3-win

lc3: lc3-win.c
	${CC} ${C-FLAGS} $^ -o $@

.PHONY:
clean:
	rm -f lc3-win
	rm -f lc3-alt-win
