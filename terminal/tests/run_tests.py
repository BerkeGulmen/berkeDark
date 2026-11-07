#!/usr/bin/env python3
"""Tests for the berkeDark colour interpreter: the byte stream first, then a real pty."""
import fcntl
import hashlib
import os
import pty
import select
import signal
import struct
import subprocess
import sys
import termios
import time
from pathlib import Path

BINARY = str(Path(__file__).resolve().parent.parent / "build" / "berkeDark")
ON = b"\x1b[38;2;255;210;92m"
STRIKE_ON = b"\x1b[38;2;245;235;208m"
BOLD_ITALIC_ON = b"\x1b[38;2;255;249;196m"
HEADING_ON = b"\x1b[38;2;255;255;0m"
OFF = b"\x1b[39m"
E = b"\x1b"

STREAM_CASES = [
    ("italic", E + b"[3mx" + E + b"[23m", E + b"[3m" + ON + b"x" + E + b"[23m" + OFF),
    ("strike", E + b"[9mx" + E + b"[29m", E + b"[9m" + STRIKE_ON + b"x" + E + b"[29m" + OFF),
    ("italic ends while struck through", E + b"[3;9mx" + E + b"[23my" + E + b"[29m",
     E + b"[3;9m" + ON + b"x" + E + b"[23m" + STRIKE_ON + b"y" + E + b"[29m" + OFF),
    ("bold and italic, reset", E + b"[1;3mx" + E + b"[0m", E + b"[1;3m" + BOLD_ITALIC_ON + b"x" + E + b"[0m"),
    ("empty reset", E + b"[3m" + E + b"[m", E + b"[3m" + ON + E + b"[m"),
    ("program colour in one mark", E + b"[3;31mx" + E + b"[0m", E + b"[3;31mx" + E + b"[0m"),
    ("program colour first", E + b"[31m" + E + b"[3mx", E + b"[31m" + E + b"[3mx"),
    ("program colour inside italic",
     E + b"[3m" + E + b"[31mx" + E + b"[39my" + E + b"[23m",
     E + b"[3m" + ON + E + b"[31mx" + E + b"[39m" + ON + b"y" + E + b"[23m" + OFF),
    ("palette index 3 is no italic", E + b"[48;5;3mx", E + b"[48;5;3mx"),
    ("rgb 3;3;3 is no italic", E + b"[48;2;3;3;3mx", E + b"[48;2;3;3;3mx"),
    ("italic after a background colour", E + b"[48;5;3;3mx", E + b"[48;5;3;3m" + ON + b"x"),
    ("colon colour", E + b"[38:2::1:2:3m" + E + b"[3mx", E + b"[38:2::1:2:3m" + E + b"[3mx"),
    ("curly underline stays underline", E + b"[4:3m" + E + b"[3mx", E + b"[4:3m" + E + b"[3m" + ON + b"x"),
    ("top heading, mark by mark",
     E + b"[1m" + E + b"[3m" + E + b"[4mx",
     E + b"[1m" + E + b"[3m" + BOLD_ITALIC_ON + E + b"[4m" + HEADING_ON + b"x"),
    ("top heading in one mark, closing", E + b"[1;3;4mx" + E + b"[24my" + E + b"[23m",
     E + b"[1;3;4m" + HEADING_ON + b"x" + E + b"[24m" + BOLD_ITALIC_ON + b"y" + E + b"[23m" + OFF),
    ("dim italic stays as it is", E + b"[2m" + E + b"[3mx", E + b"[2m" + E + b"[3mx"),
    ("private mark ending in m", E + b"[>4;2m" + E + b"[?3mx", E + b"[>4;2m" + E + b"[?3mx"),
    ("other sequences", E + b"[3J" + E + b"[3;5H" + E + b"[3 q", E + b"[3J" + E + b"[3;5H" + E + b"[3 q"),
    ("title text", E + b"]0;a[3mb\x07x", E + b"]0;a[3mb\x07x"),
    ("title ended with ST, then italic",
     E + b"]0;t" + E + b"\\" + E + b"[3mx",
     E + b"]0;t" + E + b"\\" + E + b"[3m" + ON + b"x"),
    ("sixel-like string", E + b"Pq[3m" + E + b"\\x", E + b"Pq[3m" + E + b"\\x"),
    ("cursor save and restore keep the style",
     E + b"[3m" + E + b"7" + E + b"[23m" + E + b"8x" + E + b"[23m",
     E + b"[3m" + ON + E + b"7" + E + b"[23m" + OFF + E + b"8x" + E + b"[23m" + OFF),
    ("terminal reset", E + b"[3m" + E + b"c" + E + b"[3mx", E + b"[3m" + ON + E + b"c" + E + b"[3m" + ON + b"x"),
    ("cancelled sequence", E + b"[3\x18m", E + b"[3\x18m"),
    ("turkish text", "çğıöşü İĞŞ".encode(), "çğıöşü İĞŞ".encode()),
]


BAND = E + b"[48;2;38;3;7m"
SIDE = E + b"[0;38;2;0;0;0;48;2;38;3;7m"
LINE = E + b"[0;38;2;0;0;0m"
OPEN, CLOSE = E + b"[?2026h" + E + b"7", E + b"8" + E + b"[?2026l"
FRAME_END = E + b"[?2026l"
ON_SCREEN = E + b"[?1049h"


def row_at(row, column=1):
    return E + b"[%d;%dH" % (row, column)


BOX_CASES = [
    # A band row in the middle of a 5 x 6 screen: lines above and below, sides on it.
    ("one band row",
     ON_SCREEN + row_at(3) + BAND + b"  ab  " + E + b"[49m" + FRAME_END,
     ON_SCREEN + row_at(3) + BAND + b"  ab  " + E + b"[49m" + FRAME_END + OPEN
     + row_at(2) + LINE + "▁".encode() * 6 + row_at(4) + LINE + "▔".encode() * 6
     + row_at(3) + SIDE + "▏".encode() + row_at(3, 6) + "▕".encode() + CLOSE),
    ("the prompt mark moves aside",
     ON_SCREEN + row_at(3) + BAND + "❯ ab  ".encode() + E + b"[49m" + FRAME_END,
     ON_SCREEN + row_at(3) + BAND + "❯ ab  ".encode() + E + b"[49m" + FRAME_END + OPEN
     + row_at(2) + LINE + "▁".encode() * 6 + row_at(4) + LINE + "▔".encode() * 6
     + row_at(3) + SIDE + "▏".encode() + E + b"[90m" + "❯".encode() + SIDE
     + row_at(3, 6) + "▕".encode() + CLOSE),
    ("no line on a used row",
     ON_SCREEN + row_at(2) + b"x" + row_at(3) + BAND + b"      " + E + b"[49m" + row_at(4) + b"y" + FRAME_END,
     ON_SCREEN + row_at(2) + b"x" + row_at(3) + BAND + b"      " + E + b"[49m" + row_at(4) + b"y" + FRAME_END + OPEN
     + row_at(3) + SIDE + "▏".encode() + row_at(3, 6) + "▕".encode() + CLOSE),
    ("nothing without the band", ON_SCREEN + row_at(3) + b"ab" + FRAME_END, ON_SCREEN + row_at(3) + b"ab" + FRAME_END),
    ("nothing on the normal screen", row_at(3) + BAND + b"      " + FRAME_END, row_at(3) + BAND + b"      " + FRAME_END),
]


def run_box_cases():
    failures = 0
    for name, given, wanted in BOX_CASES:
        for chunk in ("65536", "1"):
            got = subprocess.run([BINARY, "--filter", chunk, "5", "6"], input=given, capture_output=True).stdout
            if got != wanted:
                failures += 1
                print(f"FAIL {name} (chunk {chunk}):\n got  {got!r}\n want {wanted!r}")
    print(f"box: {len(BOX_CASES)} cases, each whole and byte by byte, {failures} failed")
    return failures


def run_stream_cases():
    failures = 0
    for name, given, wanted in STREAM_CASES:
        for chunk in ("65536", "1"):
            got = subprocess.run([BINARY, "--filter", chunk], input=given, capture_output=True).stdout
            if got != wanted:
                failures += 1
                print(f"FAIL {name} (chunk {chunk}): {got!r} != {wanted!r}")
    print(f"stream: {len(STREAM_CASES)} cases, each whole and byte by byte, {failures} failed")
    return failures


def spawn(*command, rows=24, columns=80):
    pid, master = pty.fork()
    if pid == 0:
        os.execv(BINARY, [BINARY, *command])
    fcntl.ioctl(master, termios.TIOCSWINSZ, struct.pack("HHHH", rows, columns, 0, 0))
    return pid, master


def read_until(master, needle, seconds=10.0):
    data = b""
    end = time.time() + seconds
    while needle not in data and time.time() < end:
        if select.select([master], [], [], 0.1)[0]:
            try:
                chunk = os.read(master, 65536)
            except OSError:
                break
            if not chunk:
                break
            data += chunk
    return data


def read_all(master, seconds=30.0):
    return read_until(master, b"\0never\0", seconds)


def finish(pid):
    _, status = os.waitpid(pid, 0)
    return os.waitstatus_to_exitcode(status)


def check(name, condition, detail=""):
    print(("ok   " if condition else "FAIL ") + name + (f": {detail}" if detail and not condition else ""))
    return 0 if condition else 1


def run_pty_cases():
    failures = 0

    pid, master = spawn("sh", "-c", "exit 7")
    read_all(master, 3)
    failures += check("exit status passes", finish(pid) == 7)
    os.close(master)

    pid, master = spawn("sh", "-c", "sleep 0.3; stty size")
    fcntl.ioctl(master, termios.TIOCSWINSZ, struct.pack("HHHH", 40, 120, 0, 0))
    text = read_all(master, 3)
    finish(pid)
    failures += check("window size passes", b"40 120" in text, repr(text))
    os.close(master)

    script = 'trap "stty size; exit 0" WINCH; echo ready; while :; do sleep 0.05; done'
    pid, master = spawn("sh", "-c", script)
    read_until(master, b"ready")
    fcntl.ioctl(master, termios.TIOCSWINSZ, struct.pack("HHHH", 33, 101, 0, 0))
    text = read_all(master, 3)
    finish(pid)
    failures += check("a resize reaches the program", b"33 101" in text, repr(text))
    os.close(master)

    pid, master = spawn("seq", "1", "300000")
    text = read_all(master, 30)
    finish(pid)
    os.close(master)
    direct = subprocess.run(["seq", "1", "300000"], capture_output=True).stdout
    failures += check("large output arrives whole",
                      hashlib.md5(text.replace(b"\r\n", b"\n")).digest() == hashlib.md5(direct).digest(),
                      f"{len(text)} bytes")

    pid, master = spawn("sh", "-c", "stty -echo; echo ready; wc -c")
    read_until(master, b"ready")
    line = b"x" * 79 + b"\n"
    total = 0
    while total < 1 << 20:
        os.write(master, line)
        total += len(line)
        if select.select([master], [], [], 0)[0]:
            os.read(master, 65536)
    os.write(master, b"\x04")
    text = read_all(master, 5)
    finish(pid)
    failures += check("a megabyte of input arrives whole", str(total).encode() in text, repr(text[-60:]))
    os.close(master)

    pid, master = spawn("sh", "-c", 'trap "echo caught; exit 3" INT; echo ready; while :; do sleep 0.05; done')
    read_until(master, b"ready")
    os.write(master, b"\x03")
    text = read_all(master, 3)
    code = finish(pid)
    failures += check("ctrl+c reaches the program", b"caught" in text and code == 3, repr(text))
    os.close(master)

    pid, master = spawn("sh", "-c", "printf '\\033[3mx\\033[23m'")
    text = read_all(master, 3)
    finish(pid)
    failures += check("a real program gets the tint", ON in text, repr(text))
    os.close(master)

    pid, master = spawn("sh", "-c", "sleep 30 & echo $!; wait")
    text = read_until(master, b"\n", 5)
    grandchild = int(text.split()[0])
    os.close(master)
    time.sleep(0.5)
    finish(pid)
    try:
        os.kill(grandchild, 0)
        alive = open(f"/proc/{grandchild}/stat").read().split()[2] != "Z"
    except OSError:
        alive = False
    failures += check("closing the window ends the programs", not alive)

    master, slave = pty.openpty()
    before = termios.tcgetattr(slave)
    subprocess.run([BINARY, "true"], stdin=slave, stdout=slave, stderr=slave)
    failures += check("terminal modes come back", termios.tcgetattr(slave) == before)
    os.close(master)
    os.close(slave)

    return failures


if __name__ == "__main__":
    signal.signal(signal.SIGCHLD, signal.SIG_DFL)
    failed = run_stream_cases() + run_box_cases() + run_pty_cases()
    print("all passed" if failed == 0 else f"{failed} failed")
    sys.exit(1 if failed else 0)
