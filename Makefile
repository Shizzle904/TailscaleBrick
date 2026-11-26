CROSS_COMPILE ?= $(shell ls /opt/*/bin/*-gcc 2>/dev/null | head -n1 | sed 's/gcc$$//')
CC      = $(CROSS_COMPILE)gcc
STRIP   = $(CROSS_COMPILE)strip
SYSROOT ?= $(shell $(CC) -print-sysroot)
HOST_CC ?= gcc

CFLAGS  = -std=c99 -O2 -Wall -Wextra -Wno-unused-parameter -pthread -D_REENTRANT \
          -I$(SYSROOT)/usr/include/SDL2 $(EXTRA_CFLAGS)
LIBS    = -lSDL2_ttf -lSDL2 -lm -lpthread

OBJS = src/main.c src/platform.c src/ui_gfx.c src/ts_ctrl.c src/qrcode.c src/cJSON.c
HDRS = src/platform.h src/ui_gfx.h src/ts_ctrl.h src/qrcode.h src/cJSON.h src/stb_image.h

.PHONY: all clean

all: tailscale_ui

tailscale_ui: $(OBJS) $(HDRS)
	$(CC) $(CFLAGS) $(OBJS) -o $@ $(LDFLAGS) $(LIBS)
	$(STRIP) $@

test_ts: src/test_ts.c src/ts_ctrl.c src/qrcode.c src/cJSON.c src/ts_ctrl.h src/qrcode.h src/cJSON.h
	$(HOST_CC) -std=c99 -O2 -Wall -Wextra -Wno-unused-parameter -pthread \
		src/test_ts.c src/ts_ctrl.c src/qrcode.c src/cJSON.c -o $@ -lm -lpthread

clean:
	rm -f tailscale_ui test_ts
