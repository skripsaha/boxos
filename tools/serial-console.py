#!/usr/bin/env python3
"""
serial-console.py — drive the BoxOS shell over a serial-line socket.

BoxOS's COM1 serial console (kernel serial_console_init) makes the shell fully
usable headless: TX carries the kernel log + shell output, RX injects keystrokes.
This connects to whatever socket the emulator exposes COM1 on and either runs a
batch of commands or gives an interactive session.

Endpoints:
  HOST:PORT        TCP        (Bochs  com1: mode=socket-server, dev=HOST:PORT)
  unix:/path/sock  Unix       (QEMU   -serial unix:/path/sock,server,nowait)

Usage:
  tools/serial-console.py 127.0.0.1:14400                 # interactive
  tools/serial-console.py 127.0.0.1:14400 files help      # run commands, print output
  tools/serial-console.py unix:build/serial.sock memtest

Notes:
  * Bochs socket-server blocks until this client connects, then boots — so the
    boot log streams as soon as you connect.
  * The prompt BoxOS emits is "~ "; batch mode waits for it before each command.
"""
import socket, sys, time, threading, select, os

PROMPT = b"~ "
BOOT_MARK = b"BoxOS Shell"


def connect(endpoint, retry_secs=20):
    # Bochs (socket-server) opens its listen port a second or two after launch,
    # so retry until it's up rather than failing on the first refused connect.
    end = time.time() + retry_secs
    last = None
    while time.time() < end:
        try:
            if endpoint.startswith("unix:"):
                s = socket.socket(socket.AF_UNIX)
                s.connect(endpoint[5:])
            else:
                host, _, port = endpoint.rpartition(":")
                s = socket.create_connection((host or "127.0.0.1", int(port)), timeout=3)
            s.settimeout(0.3)
            return s
        except (ConnectionRefusedError, FileNotFoundError, OSError) as e:
            last = e
            time.sleep(0.4)
    raise SystemExit(f"could not connect to {endpoint}: {last}")


def main():
    if len(sys.argv) < 2:
        print(__doc__.strip()); sys.exit(2)
    endpoint = sys.argv[1]
    commands = sys.argv[2:]

    s = connect(endpoint)
    buf = bytearray()
    alive = threading.Event(); alive.set()

    def reader():
        while alive.is_set():
            try:
                d = s.recv(4096)
                if not d:
                    break
                buf.extend(d)
                if not commands:                      # interactive: echo live
                    sys.stdout.write(d.decode(errors="replace")); sys.stdout.flush()
            except socket.timeout:
                continue
            except OSError:
                break
    threading.Thread(target=reader, daemon=True).start()

    def wait_for(pat, timeout):
        end = time.time() + timeout
        while time.time() < end:
            if pat in buf:
                return True
            time.sleep(0.1)
        return False

    if commands:
        if not wait_for(BOOT_MARK, 120):
            print("!! never saw the shell banner; last bytes:\n" +
                  bytes(buf[-800:]).decode(errors="replace"))
            sys.exit(1)
        time.sleep(1.0)
        rc = 0
        for cmd in commands:
            mark = len(buf)
            s.sendall(cmd.encode() + b"\r")
            # settle: wait for the prompt to return (bounded)
            end = time.time() + 20
            while time.time() < end:
                if buf.count(PROMPT) and buf.rfind(PROMPT) > mark:
                    break
                time.sleep(0.1)
            time.sleep(0.4)
            out = bytes(buf[mark:]).decode(errors="replace")
            print(f"\n===== {cmd} =====")
            print(out.rstrip())
        alive.clear(); s.close(); sys.exit(rc)
    else:
        # Interactive: relay local stdin lines to the guest.
        print(f"[serial-console] connected to {endpoint} — type commands, Ctrl-D to quit\n")
        try:
            while True:
                r, _, _ = select.select([sys.stdin], [], [], 0.2)
                if r:
                    line = sys.stdin.readline()
                    if not line:
                        break
                    s.sendall(line.rstrip("\n").encode() + b"\r")
        except KeyboardInterrupt:
            pass
        finally:
            alive.clear(); s.close()


if __name__ == "__main__":
    main()
