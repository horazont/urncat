CFLAGS+=-std=c17 -D_GNU_SOURCE $(shell pkg-config --cflags liburing)
LDFLAGS+=$(shell pkg-config --libs liburing)

all: urncat

clean:
	rm -f urncat

urncat: urncat.c Makefile
	$(CC) $(CFLAGS) -Wall -Werror -Wno-error=unused-parameter -Wno-error=unused-variable -Wextra -o "$@" "$<" $(LDFLAGS)

.PHONY: all clean
