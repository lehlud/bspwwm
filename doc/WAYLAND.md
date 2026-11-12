# Wayland port notes

## Why wlroots, rather than replacing XCB calls?

An X11 window manager asks the X server to position windows. Under Wayland the
compositor **is** the display server: it must negotiate xdg-shell state,
configure client surfaces, render buffers, manage outputs and route input.
XCB/EWMH/ICCCM code cannot simply be recompiled against a different library.

The complete bspwm upstream is retained as the policy/reference base:

* `wayland/bsp.c` adapts `src/tree.c`'s full-binary-tree model, split geometry,
  vacant-branch handling, transformations and leaf-count balancing into a
  display-server-independent C module. Automatic insertion currently uses the
  longest side; bspwm's spiral/alternate/receptacle/private/history policies
  are not ported.
* `wayland/server.c` is the wlroots platform layer. A monitor owns desktops;
  each desktop owns a BSP root/focus. A leaf's data is an xdg toplevel, not an
  X window ID. wlroots' scene graph handles surfaces, subsurfaces and popups.
* `wayland/ipc.c` attaches a private Unix socket to the **same** Wayland event
  loop. No IPC request blocks input or frames while waiting for more bytes.
* `wayland/client.c` adapts the original `src/bspc.c` command transport without
  XCB/display parsing and adds bounded arguments, partial-write handling and
  unambiguous stream framing.

The original X11 code under `src/` is not linked into the Wayland executable.
Its X-specific rule/query/restore modules remain references for subsequent
ports; retaining them does not mean those features already work on Wayland.

## Build/version boundary

This port targets wlroots **>=0.20.0, <0.21.0**, pkg-config `wlroots-0.20`.
It uses its unstable API explicitly. Wayland headers must include the
xdg-shell enum headers required by that wlroots package. Meson generates the
test client's xdg-shell protocol from installed wayland-protocols XML.
The Wayland build has no direct XCB dependency. The distribution's wlroots
library itself may have optional X11 backend dependencies.

Default build/install: Meson or the root Makefile. Legacy build:
`make -f Makefile.x11`; legacy install, if really desired:
`make -f Makefile.x11 install`. **Both variants install a binary named bspc**;
do not install both into the same prefix accidentally. Legacy man pages and
shell completions describe the X11 command set and are deliberately not
installed by the Wayland build.

## Starting and session environment

`bspwwm [-d] [-n | -c executable-config] [-s shell-command] [-v]`

* `-d`: wlroots debug logging; `-n`: no startup config; `-c`: explicit config.
* Default config: `$XDG_CONFIG_HOME/bspwwm/bspwwmrc`, or
  `$HOME/.config/bspwwm/bspwwmrc`. It must be executable. Unlike X11 bspwm there
  is no restart counter, because in-place restart is not implemented.
* `-s` runs a shell command after the config has been launched. They are
  asynchronous; do not assume the config has finished before `-s` starts.
* Children inherit the compositor's `WAYLAND_DISPLAY` and `BSPWM_SOCKET`.
  The parent shell does not. For control from outside, copy the socket path
  printed in the compositor log into `BSPWM_SOCKET`.
* Without `WLR_BACKENDS`, wlroots chooses a nested Wayland/X11 backend or
  DRM/libinput according to the environment. For native TTY sessions you need
  working DRM permissions and seat management (libseat/logind/seatd), supplied
  by your system. Do not run the compositor as root to bypass this.
* The parent Wayland display is kept intact until backend startup, so nested
  sessions connect to their parent, not their own new socket.
* Every output initially gets desktop `1`. Outputs are automatically arranged
  left-to-right at their preferred/default mode. There is no user-facing mode,
  scale, transform or position configuration yet.
* On output destruction, desktops move to another output; with no outputs
  they are retained off-screen until an output returns. Names can then be
  ambiguous: desktop IDs identify an exact desktop. Geometry/layout-change
  hooks are implemented; physical hotplug has not been hardware-tested.

## Supported bspc subset

One command/action per invocation. Unsupported options, extra arguments and
invalid/non-finite numbers return nonzero and an error; no silent fake success.
Short and long forms below are accepted, unless only a short form is shown.

| Command | Supported actions |
| --- | --- |
| `config KEY [VALUE]` | `window_gap`, `border_width` (integers 0..512), `split_ratio` (0 < r < 1), `focus_follows_pointer` (`true/false/on/off`) |
| `monitor [OUTPUT] -d/--reset-desktops NAMES...` | Rename existing desktops in order, add missing ones, remove surplus **empty** ones; refuses to discard occupied desktops |
| `monitor [OUTPUT] -a/--add-desktops NAMES...` | Add uniquely named desktops on this output |
| `monitor [OUTPUT] -f [OUTPUT]` | Focus an output/current desktop |
| `desktop [DESKTOP] -f/--focus [DESKTOP]` | Switch desktop |
| `desktop [DESKTOP] -l/--layout tiled\|monocle` | Change layout |
| `desktop [DESKTOP] -R/--rotate 90\|180\|270` | Rotate tree clockwise |
| `desktop [DESKTOP] -F/--flip horizontal\|vertical` | Mirror tree across the chosen axis |
| `desktop [DESKTOP] -B/--balance` | Balance by non-vacant leaf counts |
| `desktop [DESKTOP] -n/--rename NAME` | Rename desktop |
| `node [NODE] -f/--focus [NODE]` | Focus a window |
| `node [NODE] -c/--close` | Send xdg close, not SIGKILL |
| `node [NODE] -t/--state tiled\|floating\|fullscreen` | State change; `~floating`/`~fullscreen` toggle back |
| `node [NODE] -d/--to-desktop DESKTOP` | Transfer window without following it |
| `node [NODE] -s/--swap NODE` | Swap slots, including across desktops; IDs stay with clients |
| `node [NODE] -p/--presel-dir north\|east\|south\|west\|cancel` | Set next insertion direction |
| `node [NODE] -o/--presel-ratio RATIO` | Set next insertion ratio |
| `node [NODE] -r/--ratio RATIO` | Change this leaf's **parent** split ratio |
| `query -T/--tree [-m OUTPUT\|-d DESKTOP\|-n NODE]` | JSON snapshot of compositor/output/desktop/leaf |
| `query -N/--nodes [-m OUTPUT\|-d DESKTOP\|-n NODE]` | Window IDs, one per line |
| `query -D/--desktops [--names] [-m OUTPUT\|-d DESKTOP]` | Desktop IDs or names |
| `query -M/--monitors [--names] [-m OUTPUT]` | Output IDs or names |
| `quit` | Orderly compositor shutdown |
| `--print-socket-path` | Client-only socket-path lookup |

Selectors are deliberately limited: `focused`, numeric window IDs (decimal or
`0x...`), window `next/prev`, and window directions `north/east/south/west`;
desktop names, formatted desktop IDs (`0x00000002`), desktop `next/prev` on the
active output; output names as returned by `query -M --names`. Directional
focus only considers tiled/non-vacant leaves; use next/prev/IDs for floating
or fullscreen windows. General bspwm selector expressions are not implemented.
Names on the focused output take precedence over names on other outputs.
All config settings are global, not per-monitor/per-desktop overrides.

Ratios always describe the **first** child's share, including manual
preselection. A successful insertion consumes its anchor's preselection.
Floating/fullscreen leaves remain in the tree but are vacant for tiling.
Fullscreen restores the prior tiled/floating state; focusing another window
on that desktop exits the old fullscreen state. Monocle keeps the tree intact
but configures every tiled leaf to the same rectangle. The query rectangle is
the BSP allocation **before** leaf gap/border subtraction, not an X geometry.
Floating rectangles include borders. Window IDs are compositor-local,
not XIDs, and change when the client unmaps and remaps.

JSON includes useful bspwm-style keys but is **not** yet the complete bspwm
dump schema. Branch IDs are 0; only leaf IDs are selectable. There are no
subscriptions, rules, EWMH state, X-specific size hints, restore/restart,
pseudo-tiled/private/sticky/urgent flags, history, or full command chaining.

## IPC details/security

Socket: `$BSPWM_SOCKET` if explicitly set, otherwise
`$XDG_RUNTIME_DIR/bspwwm-$WAYLAND_DISPLAY.sock`. Runtime directory must be
private (0700). The socket is created with umask 0077. An existing path is
**never** unlinked to start a second compositor. Normal exit removes the
socket owned by this instance; crash leftovers require manual cleanup after
checking no compositor is still using them.

Transport keeps bspwm's NUL-separated argv and leading `0x07` error marker.
**This Wayland client additionally half-closes its write side (`SHUT_WR`)**
after the request. The server waits for EOF so arbitrary stream fragmentation
is safe. Use the `bspc` built here: an unmodified upstream client doesn't send
EOF and will time out. The command vocabulary is compatible only to the extent
listed above; this is not a claim of full drop-in/wire compatibility.

Limits: 64 KiB per request, 128 arguments, 64 simultaneous IPC connections,
5-second lifetime per connection. Both reads and replies are nonblocking.
The socket grants full control to processes running as your user; it is not a
sandbox or protection from untrusted same-user clients.

## Validation and known gaps

`meson test -C build --print-errorlogs` runs:

1. Pure BSP unit/property tests: parent/child invariants, deterministic
   insert/remove/traversal, ratios, tiny/extreme rectangles, vacancy, geometric
   neighbors, rotate/flip/balance and 16 seeds × 2,000 randomized mutations.
2. Real compositor integration with the headless backend and Pixman renderer,
   two outputs and three real SHM xdg-shell clients: rendered frames and
   configure dimensions, JSON escaping, IPC errors and fragmented requests,
   layout transforms, client-requested fullscreen, moves/swaps, desktop reset
   safety, unmap/remap, close/disconnect, socket collision, exit cleanup,
   xdg popups, UTF-8 titles, nested Wayland backend, executable startup config
   and propagation of the child session environment.
3. Production input/output event handlers with real wlroots objects:
   keyboard/pointer hotplug and capabilities, shortcut press/release handling,
   shifted digit bindings, removal of all outputs and later reappearance.

For memory checking:

```sh
meson setup build-asan -Db_sanitize=address,undefined -Db_lundef=false
meson test -C build-asan --print-errorlogs
```

The test host needs a functional `/dev/shm` (standard on normal Linux systems),
even with Pixman. The tests create a private runtime directory and need no
physical display, GPU or X server. Physical keyboard/pointer behavior,
DRM/KMS, multi-GPU and suspend/resume still need manual hardware testing.

Not implemented yet: layer-shell/panel reservations, server-side decoration
negotiation, interactive move/resize, customizable bindings, drag-and-drop,
primary selection, input methods/text input, tablet/touch, virtual input,
output-management protocol, fractional scale, Xwayland, session-lock,
idle-inhibit, screencopy/portals. Client CSDs work; extra compositor borders
can be disabled with `border_width 0`. Popup creation is wired through the
scene graph; edge constraint/reposition policy still needs expansion.

## Provenance/licenses

Imported complete bspwm master archive (0.9.12), commit
`e11eff4cb3333216ad03c815609a4ed79e08929c` (2026-01-08), from
`https://github.com/baskerville/bspwm`. The archive's pax commit marker was
checked. X11 source and tests are preserved; original README and Makefile are
now `README.x11.md` and `Makefile.x11` (documentation filename adjusted).

BSP adaptations preserve Bastien Dejean's BSD-2-Clause header and LICENSE.
wlroots 0.20.2's tinywl example was used as the API/event-lifecycle reference;
its MIT notice is retained in `LICENSE.wlroots`. New port files use the
repository's BSD-2-Clause license unless noted otherwise.
