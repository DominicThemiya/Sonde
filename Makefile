CC = gcc
CFLAGS = -std=gnu23 -Wall -Wextra -O2

all: sonded sonde

WEB = web/index.html web/manifest.webmanifest web/sw.js \
      web/icon-180.png web/icon-192.png web/icon-512.png

# the web client is baked into sonded with #embed, so rebuild when it changes
sonded: sonded.c metadata.c metadata.h text.c text.h platform.h $(WEB)
	$(CC) $(CFLAGS) -o $@ sonded.c metadata.c text.c

# the app icons are drawn from web/icon.svg, needs rsvg-convert (librsvg)
icons: web/icon.svg
	for size in 180 192 512; do rsvg-convert -w $$size -h $$size web/icon.svg -o web/icon-$$size.png; done

sonde: sonde.c text.c text.h platform.h
	$(CC) $(CFLAGS) -o $@ sonde.c text.c

# Windows builds, cross-compiled with mingw-w64 (pacman -S mingw-w64-gcc).
# on Windows itself, MSYS2's UCRT64 shell works too:
#   make windows WIN_CC=gcc WIN_WINDRES=windres
WIN_CC = x86_64-w64-mingw32-gcc
WIN_WINDRES = x86_64-w64-mingw32-windres
# mingw's own printf, so %zu and %lld work on every Windows C runtime.
# static so the .exe files run without any mingw DLLs next to them
WIN_CFLAGS = $(CFLAGS) -D__USE_MINGW_ANSI_STDIO=1 -static
WIN_LIBS = -lws2_32

windows: sonded.exe sonde.exe

windows/sonde.res: windows/sonde.rc windows/sonde.ico windows/sonde.manifest
	$(WIN_WINDRES) windows/sonde.rc -O coff -o $@

sonded.exe: sonded.c metadata.c metadata.h text.c text.h platform.h $(WEB) windows/sonde.res
	$(WIN_CC) $(WIN_CFLAGS) -o $@ sonded.c metadata.c text.c windows/sonde.res $(WIN_LIBS)

sonde.exe: sonde.c text.c text.h platform.h windows/sonde.res
	$(WIN_CC) $(WIN_CFLAGS) -o $@ sonde.c text.c windows/sonde.res $(WIN_LIBS)

clean:
	rm -f sonded sonde sonded.exe sonde.exe windows/sonde.res

.PHONY: all clean icons windows
