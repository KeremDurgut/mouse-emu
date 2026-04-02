CFLAGS:=-Wall -Wextra -Werror $(shell pkg-config --cflags libevdev)
PREFIX:=/usr

build:
	$(CC) $(CFLAGS) -fno-plt main.c -o main $(shell pkg-config --libs libevdev) -lpthread

install:
	install -Dm644 $(DESTDIR)/$(PREFIX)/libexec/
	install main $(DESTDIR)/$(PREFIX)/libexec/mouse-emu

clean:
	rm -f main
