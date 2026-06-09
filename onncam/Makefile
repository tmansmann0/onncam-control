PREFIX ?= /opt/homebrew
CFLAGS += -Wall -Wextra -O2 -std=c11 -I$(PREFIX)/include/libusb-1.0
LDFLAGS += -L$(PREFIX)/lib -lusb-1.0

onncam: onncam.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

clean:
	rm -f onncam
