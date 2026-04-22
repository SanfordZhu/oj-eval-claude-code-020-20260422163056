.PHONY: all code clean

all: code

code:
	gcc -O2 -std=c11 -o code main.c buddy.c

clean:
	rm -f code test
