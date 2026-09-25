PREFIX ?= $(HOME)/.local
BINDIR ?= $(PREFIX)/bin
CC     ?= gcc
CFLAGS ?= -O2 -Wall -Wextra -std=c11 -D_GNU_SOURCE
LIBS   := $(shell pkg-config --libs libusb-1.0)
INCS   := $(shell pkg-config --cflags libusb-1.0)

SRC := src/des.c src/protocol.c src/cli.c
OBJ := $(SRC:.c=.o)

.PHONY: all clean install install-udev install-service test

all: turzx

turzx: $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(LIBS)

src/%.o: src/%.c src/turzx.h src/des.h
	$(CC) $(CFLAGS) $(INCS) -c -o $@ $<

test-des: src/des.c src/test_des.c src/des.h
	$(CC) $(CFLAGS) -o $@ src/des.c src/test_des.c

test: test-des
	./test-des

install: turzx
	mkdir -p $(BINDIR)
	cp -f turzx $(BINDIR)/turzx
	@if [ -d "$(HOME)/.grok/bin" ]; then cp -f turzx $(HOME)/.grok/bin/turzx; fi
	@echo "installed $(BINDIR)/turzx"

install-udev:
	@echo "Install the udev rule (needs root) with:"
	@echo "  sudo cp udev/99-turzx.rules /etc/udev/rules.d/"
	@echo "  sudo udevadm control --reload-rules"
	@echo "  sudo udevadm trigger -s usb -a idVendor=1cbe"

install-service:
	mkdir -p $(HOME)/.config/turzx $(HOME)/.config/systemd/user
	test -f $(HOME)/.config/turzx/turzx.env || cp systemd/turzx.env $(HOME)/.config/turzx/turzx.env
	cp systemd/turzx.service $(HOME)/.config/systemd/user/turzx.service
	systemctl --user daemon-reload
	@echo "edit $(HOME)/.config/turzx/turzx.env then:"
	@echo "  systemctl --user enable --now turzx.service"

clean:
	rm -f turzx test-des src/*.o
