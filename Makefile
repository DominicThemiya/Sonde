CC = gcc
CFLAGS = -std=gnu23 -Wall -Wextra -O2

all: sonded sonde

# web/index.html is baked into sonded with #embed, so rebuild when it changes
sonded: sonded.c metadata.c metadata.h web/index.html
	$(CC) $(CFLAGS) -o $@ sonded.c metadata.c

sonde: sonde.c
	$(CC) $(CFLAGS) -o $@ sonde.c

clean:
	rm -f sonded sonde

.PHONY: all clean
