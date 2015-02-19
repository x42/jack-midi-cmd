PREFIX ?= /usr/local
bindir = $(PREFIX)/bin
mandir = $(PREFIX)/share/man/man1
CFLAGS ?= -Wall -g -O2

VERSION=0.1.0

ifeq ($(shell pkg-config --exists jack || echo no), no)
  $(error "http://jackaudio.org is required - install libjack-dev or libjack-jackd2-dev")
endif

CFLAGS+=`pkg-config --cflags jack` -DVERSION=\"$(VERSION)\"
LOADLIBES=`pkg-config --libs jack` -lm

all: jack_midi_cmd

man: jack_midi_cmd.1

jack_midi_cmd: jack_midi_cmd.c

clean:
	rm -f jack_midi_cmd

jack_midi_cmd.1: jack_midi_cmd
	help2man -N -n 'JACK MIDI Commander' -o jack_midi_cmd.1 ./jack_midi_cmd

install: install-bin install-man

uninstall: uninstall-bin uninstall-man

install-bin: jack_midi_cmd
	install -d $(DESTDIR)$(bindir)
	install -m755 jack_midi_cmd $(DESTDIR)$(bindir)

uninstall-bin:
	rm -f $(DESTDIR)$(bindir)/jack_midi_cmd

install-man:
	install -d $(DESTDIR)$(mandir)
	install -m644 jack_midi_cmd.1 $(DESTDIR)$(mandir)

uninstall-man:
	rm -f $(DESTDIR)$(mandir)/jack_midi_cmd.1
	-rmdir $(DESTDIR)$(mandir)

.PHONY: all clean install uninstall man install-man install-bin uninstall-man uninstall-bin
