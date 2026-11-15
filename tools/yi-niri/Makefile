CXX ?= c++
PKG_CONFIG ?= pkg-config

# Where lib/cpp/yi.hpp lives. Override when Yi is checked out elsewhere:
#   make YI_CPP=/path/to/Yi/lib/cpp
YI_CPP ?= ../../../Yi/lib/cpp

PREFIX ?= $(HOME)/.local
BINDIR ?= $(PREFIX)/bin
UNITDIR ?= $(HOME)/.config/systemd/user

CXXFLAGS ?= -O2 -g -Wall -Wextra -Wpedantic -std=c++17
CPPFLAGS += -I$(YI_CPP) $(shell $(PKG_CONFIG) --cflags libsystemd libcurl)
LDLIBS += $(shell $(PKG_CONFIG) --libs libsystemd libcurl)

BIN := yi-niri

.PHONY: all install uninstall test clean

all: $(BIN)

$(BIN): yi-niri.cpp $(YI_CPP)/yi.hpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -o $@ yi-niri.cpp $(LDLIBS)

install: $(BIN)
	install -Dm755 $(BIN) $(BINDIR)/$(BIN)
	install -Dm644 yi-niri.service $(UNITDIR)/$(BIN).service

uninstall:
	rm -f $(BINDIR)/$(BIN) $(UNITDIR)/$(BIN).service

test: $(BIN)
	./$(BIN) --version
	./$(BIN) --help >/dev/null
	./$(BIN) --check >/dev/null

clean:
	rm -f $(BIN)
