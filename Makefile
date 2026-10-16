# Makefile for AcreetionOS Surf — C + Rust native browser
# Build: make       → cargo build --release, install to ~/bin/acreetion-surf
# Clean: make clean

PREFIX  ?= $(HOME)
BINDIR  ?= $(PREFIX)/bin

TARGET  := target/release/acreetion-surf
INSTALL := $(BINDIR)/acreetion-surf

.PHONY: all build install clean test

all: build install

build:
	cargo build --release

install: build
	@mkdir -p $(BINDIR)
	@cp -v $(TARGET) $(INSTALL)
	@chmod +x $(INSTALL)
	@echo "✓ Installed to $(INSTALL)"

clean:
	cargo clean

test: build
	@echo "=== smoke test (--help) ==="
	@$(TARGET) --help | head -5
	@echo "=== launch test (5s timeout) ==="
	@timeout 5 $(TARGET) https://acreetionos.org 2>&1; \
		[ $$? -eq 124 ] && echo "✓ Window stayed open (good)" || echo "✗ Exited early"