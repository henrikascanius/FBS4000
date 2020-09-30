# src = $(wildcard *.c)
CC = gcc

fbs: fbs_main.c
	gcc -o fbs fbs_main.c

.PHONY: clean
clean:
	rm -f $(obj) fbs
