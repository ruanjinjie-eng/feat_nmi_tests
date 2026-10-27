CC ?= aarch64-linux-gnu-gcc
CFLAGS ?= -O2 -Wall -Wextra -static

all: init

init: init.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f init

.PHONY: all clean
