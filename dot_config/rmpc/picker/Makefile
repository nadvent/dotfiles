CC      := gcc
CFLAGS  := -std=c11 -O2 -Wall -Wextra -Werror -D_POSIX_C_SOURCE=200809L
PKG     := gtk4-layer-shell-0 gtk4 libmpdclient

PKGCFLAGS := $(shell pkg-config --cflags $(PKG))
PKGLIBS   := $(shell pkg-config --libs $(PKG))

TARGET  := ../bin/rmpc-picker
SRCS    := src/main.c src/mpd_client.c src/picker_ui.c src/library_browser.c src/waveform_daemon.c
HEADERS := src/mpd_client.h src/picker_ui.h src/library_browser.h src/waveform_daemon.h
OBJS    := $(SRCS:.c=.o)

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(PKGLIBS)

src/%.o: src/%.c $(HEADERS)
	$(CC) $(CFLAGS) $(PKGCFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJS) $(TARGET)
