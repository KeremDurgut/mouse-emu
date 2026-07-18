CFLAGS:=-Wall -Wextra -Werror -fvisibility=hidden -fno-plt
PREFIX:=/usr
build:
	$(CC) $(CFLAGS) main.c -o mouse-emu

install:
	install -Dm644 $(DESTDIR)/$(PREFIX)/libexec/
	install mouse-emu $(DESTDIR)/$(PREFIX)/libexec/mouse-emu

clean:
	rm -f mouse-emu
