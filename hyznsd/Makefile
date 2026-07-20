# hyznsd - HyZNS CNS-area autoscaling daemon
CC      ?= cc
CFLAGS  ?= -O2 -g -Wall -Wextra -std=gnu11
OBJ      = hyznsd.o monitor.o policy.o
BIN      = hyznsd

PREFIX  ?= /usr/local
SBINDIR  = $(PREFIX)/sbin
UNITDIR ?= /etc/systemd/system
CONFDIR ?= /etc
DOCDIR   = $(PREFIX)/share/hyznsd

.PHONY: all clean test install uninstall
all: $(BIN)

$(BIN): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ)

%.o: %.c hyznsd.h
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJ) $(BIN)

# offline policy validation against mock device/report inputs
test: $(BIN)
	./scripts/selftest.sh

# deploy as a systemd-managed daemon. Does NOT overwrite an existing
# /etc/hyznsd.conf. After: systemctl daemon-reload && systemctl enable --now hyznsd
install: $(BIN)
	install -D -m755 $(BIN) $(DESTDIR)$(SBINDIR)/$(BIN)
	install -D -m644 systemd/hyznsd.service $(DESTDIR)$(UNITDIR)/hyznsd.service
	install -D -m644 README.md $(DESTDIR)$(DOCDIR)/README.md
	@if [ ! -f $(DESTDIR)$(CONFDIR)/hyznsd.conf ]; then \
	  install -D -m644 systemd/hyznsd.conf $(DESTDIR)$(CONFDIR)/hyznsd.conf; \
	  echo "installed $(CONFDIR)/hyznsd.conf"; \
	else echo "kept existing $(CONFDIR)/hyznsd.conf"; fi
	@echo "next: systemctl daemon-reload && systemctl enable --now hyznsd"

uninstall:
	rm -f $(DESTDIR)$(SBINDIR)/$(BIN) $(DESTDIR)$(UNITDIR)/hyznsd.service $(DESTDIR)$(DOCDIR)/README.md
