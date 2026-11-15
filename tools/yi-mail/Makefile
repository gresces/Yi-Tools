CC ?= cc
PKG_CONFIG ?= pkg-config

PREFIX ?= $(HOME)/.local
BINDIR ?= $(PREFIX)/bin
UNITDIR ?= $(HOME)/.config/systemd/user

CFLAGS ?= -O2 -g -Wall -Wextra -Wpedantic -std=c11
CPPFLAGS += $(shell $(PKG_CONFIG) --cflags openssl libcurl)
LDLIBS += $(shell $(PKG_CONFIG) --libs openssl libcurl)

BIN := yi-mail

.PHONY: all install uninstall test clean

all: $(BIN)

$(BIN): yi-mail.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ yi-mail.c $(LDLIBS)

install: $(BIN)
	install -Dm755 $(BIN) $(BINDIR)/$(BIN)
	install -Dm644 yi-mail.service $(UNITDIR)/$(BIN).service

uninstall:
	rm -f $(BINDIR)/$(BIN) $(UNITDIR)/$(BIN).service

test: $(BIN)
	./$(BIN) --version
	./$(BIN) --help >/dev/null
	./$(BIN) --check >/dev/null

clean:
	rm -f $(BIN)
