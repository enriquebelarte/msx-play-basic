CC ?= gcc
CFLAGS ?= -Wall -Wextra -O2 -std=c99 -D_POSIX_C_SOURCE=200809L
LDFLAGS ?= -lm
PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin

TARGET = msx_play
SRC = msx_play.c
SCRIPT = midi2mus.py

.PHONY: all clean install uninstall test help

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) $(SRC) -o $(TARGET) $(LDFLAGS)

clean:
	rm -f $(TARGET) *.o /tmp/msx_play_*.wav

install: $(TARGET)
	install -d $(DESTDIR)$(BINDIR)
	install -m 755 $(TARGET) $(DESTDIR)$(BINDIR)
	install -m 755 $(SCRIPT) $(DESTDIR)$(BINDIR)

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/$(TARGET)
	rm -f $(DESTDIR)$(BINDIR)/$(SCRIPT)

test: $(TARGET)
	@echo "Running MSX PLAY test suite..."
	@echo "1. Testing command-line help..."
	./$(TARGET) --help > /dev/null
	@echo "2. Testing channel status display..."
	./$(TARGET) --status
	@echo "3. Testing WAV generation (scale, 3 channels)..."
	./$(TARGET) -o /tmp/msx_test.wav "T140 O4 V15 MN C4 D4 E4 F4 G4 A4 B4 > C4" "O3 V12 C4 E4 G4 > C4" "O2 V10 C2 G2"
	@test -s /tmp/msx_test.wav && echo "WAV file successfully generated: /tmp/msx_test.wav"
	@rm -f /tmp/msx_test.wav
	@echo "4. Testing MIDI-to-.mus converter helper..."
	python3 ./$(SCRIPT) --help > /dev/null
	@echo "All tests passed successfully!"

help:
	@echo "Available Makefile targets:"
	@echo "  all       : Build the msx_play binary (default)"
	@echo "  clean     : Remove compiled binaries and temporary files"
	@echo "  test      : Run automated tests and verify WAV generation"
	@echo "  install   : Install msx_play and midi2mus.py to $(BINDIR)"
	@echo "  uninstall : Remove msx_play and midi2mus.py from $(BINDIR)"
