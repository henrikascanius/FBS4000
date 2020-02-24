# src = $(wildcard *.c)
CC = gcc

fbs: fbs_main.c fbs_log.h
	gcc -o fbs fbs_main.c

.PHONY: clean
clean:
	rm -f $(obj) fbs
