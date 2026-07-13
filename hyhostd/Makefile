# hyhostd - HyZNS CNS-area autoscaling daemon
CC      ?= cc
CFLAGS  ?= -O2 -g -Wall -Wextra -std=gnu11
OBJ      = hyhostd.o monitor.o policy.o
BIN      = hyhostd

PREFIX  ?= /usr/local
SBINDIR  = $(PREFIX)/sbin
UNITDIR ?= /etc/systemd/system
CONFDIR ?= /etc
DOCDIR   = $(PREFIX)/share/hyhostd

.PHONY: all clean test install uninstall
all: $(BIN)

$(BIN): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ)

%.o: %.c hyhostd.h
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJ) $(BIN)

# offline policy validation against mock device/report inputs
test: $(BIN)
	./scripts/selftest.sh

# deploy as a systemd-managed daemon. Does NOT overwrite an existing
# /etc/hyhostd.conf. After: systemctl daemon-reload && systemctl enable --now hyhostd
install: $(BIN)
	install -D -m755 $(BIN) $(DESTDIR)$(SBINDIR)/$(BIN)
	install -D -m644 systemd/hyhostd.service $(DESTDIR)$(UNITDIR)/hyhostd.service
	install -D -m644 README.md $(DESTDIR)$(DOCDIR)/README.md
	@if [ ! -f $(DESTDIR)$(CONFDIR)/hyhostd.conf ]; then \
	  install -D -m644 systemd/hyhostd.conf $(DESTDIR)$(CONFDIR)/hyhostd.conf; \
	  echo "installed $(CONFDIR)/hyhostd.conf"; \
	else echo "kept existing $(CONFDIR)/hyhostd.conf"; fi
	@echo "next: systemctl daemon-reload && systemctl enable --now hyhostd"

uninstall:
	rm -f $(DESTDIR)$(SBINDIR)/$(BIN) $(DESTDIR)$(UNITDIR)/hyhostd.service $(DESTDIR)$(DOCDIR)/README.md
