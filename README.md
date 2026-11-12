# bspwwm — bspwm on Wayland

An **experimental native Wayland compositor**, using
[bspwm](https://github.com/baskerville/bspwm)'s binary-space-partitioning model
and [wlroots](https://gitlab.freedesktop.org/wlroots/wlroots) **0.20.x** for
outputs, rendering, input and Wayland protocols. This is a working foundation,
**not yet a feature-complete or production-ready bspwm replacement**.

The full bspwm source, examples, documentation and tests are retained. The X11
implementation lives in `src/`; its original documentation is in
[README.x11.md](README.x11.md). The native compositor is in `wayland/`.
No X server, XCB, sxhkd or Xwayland is required by the Wayland build.

## Build and test

Requirements: C11 compiler, Meson, Ninja, pkg-config, wlroots **0.20.x**,
Wayland server/client development files, wayland-protocols, wayland-scanner,
xkbcommon, and Python 3 for tests. wlroots has an unstable C API; other minor
versions are deliberately not silently accepted.

For Fedora (with wlroots 0.20 available):

```sh
sudo dnf install gcc meson ninja-build pkgconf-pkg-config wlroots-devel \
    wayland-devel wayland-protocols-devel libxkbcommon-devel
meson setup build
meson compile -C build
meson test -C build --print-errorlogs
# optional: meson install -C build
```

`make`, `make test` and `make install` wrap these Wayland targets. On filesystems
with clock skew, choose a local build directory, e.g.
`meson setup /tmp/bspwwm-build .`. The original X11 version can still be built
explicitly with `make x11` (requires its original XCB dependencies).

## Try it safely, nested

Install a native Wayland terminal such as `foot`, then run this **from an
existing Wayland session**:

```sh
PATH="$PWD/build:$PATH" WLR_BACKENDS=wayland build/bspwwm -s foot
```

The nested window contains a separate desktop. **Super+Return** opens a
terminal, **Super+h/j/k/l** changes focus, **Super+q** closes a window,
**Super+f** toggles fullscreen, **Super+Space** toggles monocle, **Super+1…9**
switches desktops, **Super+Shift+1…9** moves a window, and
**Super+Shift+Escape** exits. Your parent compositor may intercept some keys.
Use `BSPWWM_TERMINAL` to change the terminal; `XKB_DEFAULT_LAYOUT` etc. configure
the keymap. `sxhkd` is X11-only and cannot provide Wayland global shortcuts.

To configure the compositor:

```sh
mkdir -p ~/.config/bspwwm
cp examples/bspwwmrc ~/.config/bspwwm/bspwwmrc
chmod +x ~/.config/bspwwm/bspwwmrc
```

This executable shell script calls the **Wayland-built** `bspc`. The compositor
exports `WAYLAND_DISPLAY` and `BSPWM_SOCKET` to its children. In a terminal
inside the compositor, for example:

```sh
bspc config window_gap 12
bspc monitor -d web code chat
bspc node -p east
bspc desktop -l monocle
bspc query -T
```

For a software-only smoke test without a desktop/GPU:

```sh
# XDG_RUNTIME_DIR must already be a private writable directory (mode 0700).
WLR_BACKENDS=headless WLR_RENDERER=pixman build/bspwwm -n
```

## Status

Implemented: native xdg-shell toplevels/popups, scene rendering and frame
callbacks, BSP splitting/preselection/ratios, rotation/flip/balance, tiled,
floating and fullscreen states, monocle, per-output desktops, keyboard/pointer
delivery, click focus, optional focus-follows-pointer, clipboard selection,
shell startup configuration and a documented subset of `bspc` commands.

Automated tests run the real compositor with two headless outputs and real
SHM clients, not just a mock layout. They exercise rendering, geometry,
fullscreen requests, desktop/output transfers, swaps, unmap/remap, orderly and
abrupt disconnects, IPC fragmentation/validation and socket collision safety.
The pure BSP suite also checks 32,000 randomized operations and invariants.
Input/output lifecycle tests and a nested-compositor test run too; all three
suites are also checked under AddressSanitizer/UndefinedBehaviorSanitizer.

Still missing: full bspc selectors/rules/subscriptions/state restore,
layer-shell panels, Xwayland, configurable keybindings, interactive floating
move/resize, full output configuration, session locking, portals/screencasting
and several desktop integration protocols. **Do not use this as a secure
daily-driver/session-lock environment.** DRM/KMS and physical input use
wlroots' backend path but have not been verified on hardware here.

See [doc/WAYLAND.md](doc/WAYLAND.md) for architecture, exact command coverage,
IPC framing, limitations, provenance and testing details. Upstream BSD-2-Clause
notices are retained in [LICENSE](LICENSE); the wlroots/tinywl reference's MIT
notice is in [LICENSE.wlroots](LICENSE.wlroots).
