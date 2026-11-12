CC := gcc
CFLAGS := -std=c11 -O2 -Wall -Wextra

fan-tray-lite: fan-tray-lite.c
	$(CC) $(CFLAGS) fan-tray-lite.c -o fan-tray-lite $$(pkg-config --cflags --libs libsystemd)

clean:
	rm -f fan-tray-lite
