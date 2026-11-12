#!/usr/bin/env python3
"""Exercise the real compositor, IPC and SHM clients; no X server or GPU needed."""
import json
import os
from pathlib import Path
import socket
import stat
import shlex
import subprocess
import sys
import tempfile
import time

COMPOSITOR, BSPC, CLIENT = map(os.path.abspath, sys.argv[1:])


def wait(predicate, description):
    deadline = time.monotonic() + 8
    while time.monotonic() < deadline:
        result = predicate()
        if result:
            return result
        time.sleep(0.025)
    raise AssertionError(f"Timed out: {description}")


def leaves(node):
    if not node:
        return []
    if node["client"]:
        return [node]
    return leaves(node["firstChild"]) + leaves(node["secondChild"])


with tempfile.TemporaryDirectory(prefix="bspwwm-") as directory:
    runtime = Path(directory)
    env = os.environ.copy()
    for key in ("DISPLAY", "WAYLAND_DISPLAY", "WAYLAND_SOCKET"):
        env.pop(key, None)
    env.update(XDG_RUNTIME_DIR=directory, BSPWM_SOCKET=str(runtime / "control.sock"),
               WLR_BACKENDS="headless", WLR_RENDERER="pixman", WLR_HEADLESS_OUTPUTS="2")
    processes = []
    files = []

    def run(*args, success=True):
        result = subprocess.run([BSPC, *args], env=env, capture_output=True, text=True, timeout=6)
        assert (result.returncode == 0) == success, (args, result.stdout, result.stderr)
        return result.stdout

    def tree():
        return json.loads(run("query", "-T"))

    def nodes():
        return [n for o in tree()["monitors"] for d in o["desktops"] for n in leaves(d["root"])]

    def app(name):
        return next((n for n in nodes() if n["client"]["appId"] == name), None)

    def launch(name):
        trace = runtime / (name + ".trace")
        f = trace.open("w+")
        files.append(f)
        child = subprocess.Popen([CLIENT, name], env=env, stdin=subprocess.PIPE, stdout=f, stderr=f, text=True)
        processes.append(child)
        wait(lambda: app(name), f"map {name}")
        wait(lambda: "frame " in trace.read_text(), f"render/frame callback {name}")
        return child, trace

    log = (runtime / "compositor.log").open("w+")
    files.append(log)
    compositor = subprocess.Popen([COMPOSITOR, "-n"], env=env, stdout=log, stderr=log)
    try:
        wait(lambda: Path(env["BSPWM_SOCKET"]).exists() or compositor.poll() is not None, "IPC socket")
        assert compositor.poll() is None, (runtime / "compositor.log").read_text()
        sockets = list(runtime.glob("wayland-*"))
        env["WAYLAND_DISPLAY"] = next(p.name for p in sockets if not p.name.endswith(".lock"))
        assert stat.S_IMODE(Path(env["BSPWM_SOCKET"]).stat().st_mode) == 0o700
        outputs = tree()["monitors"]
        assert len(outputs) == 2
        assert len(run("query", "-M", "--names").splitlines()) == 2
        run("monitor", "-d", "one", "two", "three")
        assert run("query", "-D", "--names", "-m", outputs[0]["name"]).splitlines() == ["one", "two", "three"]
        run("config", "window_gap", "0")
        run("config", "border_width", "0")
        run("config", "split_ratio", "0.5")
        for invalid in ("nan", "inf", "-1", "1", "0", "0.3bad"):
            run("config", "split_ratio", invalid, success=False)
        run("config", "window_gap", "-1", success=False)
        run("config", "window_gap", "1.5", success=False)
        run("config", "not_a_setting", "4", success=False)
        run("rule", "-a", "*", "state=floating", success=False)
        assert run("config", "split_ratio").strip() == "0.500000"

        # A half-written request must not stall rendering or another connection.
        idle = socket.socket(socket.AF_UNIX)
        idle.connect(env["BSPWM_SOCKET"])
        idle.sendall(b"config\0window_")
        assert run("config", "window_gap").strip() == "0"
        idle.sendall(b"gap\0")
        idle.shutdown(socket.SHUT_WR)
        assert idle.recv(100) == b"0\n"
        idle.close()
        for malformed in (b"query", b"\0", b"query\0" + b"a\0" * 128):
            with socket.socket(socket.AF_UNIX) as connection:
                connection.connect(env["BSPWM_SOCKET"])
                connection.sendall(malformed)
                connection.shutdown(socket.SHUT_WR)
                assert connection.recv(256).startswith(b"\x07")

        a, a_trace = launch("alpha")
        first_box = app("alpha")["rectangle"]
        run("node", "-p", "west")
        b, b_trace = launch("beta")
        aid, bid = str(app("alpha")["id"]), str(app("beta")["id"])
        assert app("beta")["rectangle"]["x"] < app("alpha")["rectangle"]["x"]
        assert app("alpha")["client"]["title"] == 'Test "window" \\ title\nGrüße'
        run("node", bid, "-r", "0.3")
        assert app("beta")["rectangle"]["width"] == int(first_box["width"] * 0.3)
        run("node", bid, "-f")
        run("node", "-p", "south")
        c, c_trace = launch("gamma")
        cid = str(app("gamma")["id"])
        assert len(nodes()) == 3
        assert app("gamma")["rectangle"]["y"] > app("beta")["rectangle"]["y"]
        box = app("gamma")["rectangle"]
        wait(lambda: f'configure {box["width"]} {box["height"]}' in c_trace.read_text(), "configured tile dimensions")
        c.stdin.write("popup\n"); c.stdin.flush()
        wait(lambda: "popup configured" in c_trace.read_text(), "xdg popup configure")
        c.stdin.write("unpopup\n"); c.stdin.flush()

        run("node", cid, "-t", "floating")
        assert app("gamma")["vacant"] and app("gamma")["client"]["state"] == "floating"
        run("node", cid, "-t", "~floating")
        assert not app("gamma")["vacant"]
        c.stdin.write("fullscreen\n"); c.stdin.flush()
        wait(lambda: app("gamma")["client"]["state"] == "fullscreen", "client fullscreen request")
        wait(lambda: f'configure {first_box["width"]} {first_box["height"]}' in c_trace.read_text(), "fullscreen configure")
        c.stdin.write("fullscreen\n"); c.stdin.flush()
        wait(lambda: app("gamma")["client"]["state"] == "tiled", "client exits fullscreen")
        run("desktop", "-l", "monocle")
        assert app("alpha")["rectangle"] == app("beta")["rectangle"] == app("gamma")["rectangle"]
        run("desktop", "-l", "tiled")
        run("desktop", "-B")
        before = json.loads(run("query", "-T", "-d", "one"))["root"]
        for _ in range(4):
            run("desktop", "-R", "90")
        assert before == json.loads(run("query", "-T", "-d", "one"))["root"]
        for _ in range(2):
            run("desktop", "-F", "horizontal")
        assert before == json.loads(run("query", "-T", "-d", "one"))["root"]
        run("node", aid, "-s", bid)
        assert app("alpha")["id"] == int(aid) and app("beta")["id"] == int(bid)
        run("node", cid, "-d", "two")
        run("monitor", "-d", "only", success=False)  # never discard an occupied desktop
        run("desktop", "-f", "two")
        run("node", cid, "-f")
        run("desktop", "-n", "second")
        run("node", cid, "-s", aid)  # cross-desktop swap
        assert len(nodes()) == 3
        run("node", aid, "-f")
        run("node", aid, "-d", "second")
        assert len(nodes()) == 3

        c.stdin.write("unmap\n"); c.stdin.flush()
        wait(lambda: not app("gamma"), "unmap removes leaf")
        c.stdin.write("remap\n"); c.stdin.flush()
        wait(lambda: app("gamma"), "remap creates leaf")
        assert len(nodes()) == 3
        run("node", str(app("gamma")["id"]), "-c")
        assert c.wait(timeout=6) == 0
        wait(lambda: len(nodes()) == 2, "xdg close and tree collapse")
        # Move a real window onto the second output.
        second_output = tree()["monitors"][1]
        second_desktop = second_output["desktops"][0]
        run("node", aid, "-d", f'0x{second_desktop["id"]:08X}')
        run("monitor", second_output["name"], "-f")
        run("node", aid, "-f")
        assert app("alpha")["rectangle"]["x"] >= first_box["width"]
        a.kill(); a.wait(timeout=6)
        wait(lambda: len(nodes()) == 1, "abrupt client disconnect")
        run("node", bid, "-c")
        assert b.wait(timeout=6) == 0
        wait(lambda: len(nodes()) == 0, "last client removal")

        # A colliding second compositor must not unlink our live IPC endpoint.
        inode = Path(env["BSPWM_SOCKET"]).stat().st_ino
        collision = subprocess.run([COMPOSITOR, "-n"], env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=6)
        assert collision.returncode != 0
        assert Path(env["BSPWM_SOCKET"]).stat().st_ino == inode
        assert tree()["monitors"]
        # Run the production Wayland backend nested inside the headless parent.
        # Also verify explicit config and child environment propagation.
        nested_env = env.copy()
        nested_env.update(WLR_BACKENDS="wayland", BSPWM_SOCKET=str(runtime / "nested.sock"))
        nested_log = (runtime / "nested.log").open("w+")
        files.append(nested_log)
        config_path = runtime / "nestedrc"
        config_path.write_text(f"#!/bin/sh\n{shlex.quote(BSPC)} config window_gap 13\n")
        config_path.chmod(0o700)
        environment_path = runtime / "nested.env"
        startup = 'printf "%s\\n%s\\n" "$WAYLAND_DISPLAY" "$BSPWM_SOCKET" > ' + shlex.quote(str(environment_path))
        nested = subprocess.Popen([COMPOSITOR, "-c", str(config_path), "-s", startup],
                                  env=nested_env, stdout=nested_log, stderr=nested_log)
        processes.append(nested)

        def nested_run(*args):
            result = subprocess.run([BSPC, *args], env=nested_env, capture_output=True, text=True, timeout=6)
            assert result.returncode == 0, (result.stderr, (runtime / "nested.log").read_text())
            return result.stdout

        wait(lambda: Path(nested_env["BSPWM_SOCKET"]).exists() or nested.poll() is not None, "nested compositor")
        assert nested.poll() is None, (runtime / "nested.log").read_text()
        wait(lambda: nested_run("config", "window_gap").strip() == "13", "startup config")
        wait(lambda: environment_path.exists() and len(environment_path.read_text().splitlines()) == 2, "startup child environment")
        nested_display, nested_socket = environment_path.read_text().splitlines()
        assert nested_display != env["WAYLAND_DISPLAY"] and nested_socket == nested_env["BSPWM_SOCKET"]
        assert len(json.loads(nested_run("query", "-T"))["monitors"]) == 1
        wait(lambda: len(nodes()) == 1, "nested backend maps as xdg toplevel")
        nested_run("quit")
        assert nested.wait(timeout=6) == 0, (runtime / "nested.log").read_text()
        wait(lambda: len(nodes()) == 0, "nested backend teardown")
        run("quit")
        assert compositor.wait(timeout=6) == 0
        assert not Path(env["BSPWM_SOCKET"]).exists()
        print("Headless integration passed: 2 outputs, BSP geometry, real SHM rendering, IPC, desktops, states and lifecycle")
    except BaseException:
        print((runtime / "compositor.log").read_text(), file=sys.stderr)
        for trace in runtime.glob("*.trace"):
            print(trace.name, trace.read_text(), file=sys.stderr)
        raise
    finally:
        for process in [*processes, compositor]:
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=4)
                except subprocess.TimeoutExpired:
                    process.kill(); process.wait()
        for f in files:
            f.close()
