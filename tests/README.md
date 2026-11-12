# Wayland tests

From the project root, run `meson test -C build --print-errorlogs`.
`bsp_test.c` tests the pure policy, `platform_test.c` tests production
wlroots input/output event handlers, and `headless.py` launches the actual
compositor, real SHM clients and a nested compositor. See `doc/WAYLAND.md`.

## Original X11 tests (unchanged scripts)

- Install *jshon*.
- Run `make` once.
- Run `./run`.
