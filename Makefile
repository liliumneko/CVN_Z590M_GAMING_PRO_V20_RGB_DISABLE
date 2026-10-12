CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra -std=c11
PREFIX  ?= /usr/local
BINDIR  ?= $(PREFIX)/bin
UDEVDIR ?= /usr/lib/udev/rules.d
UNITDIR ?= /usr/lib/systemd/system

all: igame-led

igame-led: src/igame_led.c
	$(CC) $(CFLAGS) -o $@ $<

install: igame-led
	install -Dm755 igame-led $(DESTDIR)$(BINDIR)/igame-led
	install -Dm644 udev/60-igame-led.rules $(DESTDIR)$(UDEVDIR)/60-igame-led.rules
	install -Dm644 systemd/igame-led.service $(DESTDIR)$(UNITDIR)/igame-led.service

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/igame-led
	rm -f $(DESTDIR)$(UDEVDIR)/60-igame-led.rules
	rm -f $(DESTDIR)$(UNITDIR)/igame-led.service

clean:
	rm -f igame-led

.PHONY: all install uninstall clean
