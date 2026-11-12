# Wayland is the default. The complete upstream X11 build is kept separately.
PREFIX ?= /usr/local
BUILD_DIR ?= build

all: wayland

$(BUILD_DIR)/build.ninja: meson.build
	meson setup $(BUILD_DIR) --prefix=$(PREFIX)

wayland: $(BUILD_DIR)/build.ninja
	meson compile -C $(BUILD_DIR)

test: wayland
	meson test -C $(BUILD_DIR) --print-errorlogs

install: wayland
	meson install -C $(BUILD_DIR)

clean:
	@if test -f $(BUILD_DIR)/build.ninja; then meson compile -C $(BUILD_DIR) --clean; fi

x11:
	$(MAKE) -f Makefile.x11 all

.PHONY: all wayland test install clean x11
