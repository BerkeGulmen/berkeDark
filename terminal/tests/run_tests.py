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
import tempfile
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


WHITE_MARK = E + b"[D" + E + b"[38;2;250;252;255m" + "❯".encode()   # the input row's mark written again in white
SENT_MARK = E + b"[D" + E + b"[38;2;250;252;255m" + "▶".encode()    # the mark of a sent row



BOX_CASES = [
    # A band row in the middle of a 5 x 6 screen: lines above and below, sides on it.
    ("one band row",
     ON_SCREEN + row_at(3) + BAND + b"  ab  " + E + b"[49m" + FRAME_END,
     ON_SCREEN + row_at(3) + BAND + b"  ab  " + E + b"[49m" + FRAME_END + OPEN
     + row_at(2) + LINE + "▁".encode() * 6 + row_at(4) + LINE + "▔".encode() * 6
     + row_at(3) + SIDE + "▏".encode() + row_at(3, 6) + "▕".encode() + CLOSE),
    # The prompt mark keeps its cell and the blank after it (the author, 2026-11-08); the line gives way to it.
    ("the prompt mark keeps its place, solid and white on a sent row",
     ON_SCREEN + row_at(3) + BAND + "❯ ab  ".encode() + E + b"[49m" + FRAME_END,
     ON_SCREEN + row_at(3) + BAND + "❯".encode() + SENT_MARK + E + b"[39m" + " ab  ".encode() + E + b"[49m" + FRAME_END
     + OPEN + row_at(2) + LINE + "▁".encode() * 6 + row_at(4) + LINE + "▔".encode() * 6
     + row_at(3) + SIDE + row_at(3, 6) + "▕".encode() + CLOSE),
    ("the grey mark of a sent row turns solid and white, the grey comes back",
     ON_SCREEN + row_at(3) + E + b"[90m" + BAND + "❯ a".encode(),
     ON_SCREEN + row_at(3) + E + b"[90m" + BAND + "❯".encode() + SENT_MARK + E + b"[90m" + " a".encode()),
    ("the input row's mark in white",
     ON_SCREEN + row_at(5) + E + b"[39m" + "❯\u00a0".encode(),
     ON_SCREEN + row_at(5) + E + b"[39m" + "❯".encode() + WHITE_MARK + E + b"[39m" + "\u00a0".encode()),
    ("the grey input mark while Claude works stays grey",
     ON_SCREEN + row_at(5) + E + b"[90m" + "❯\u00a0".encode(), ON_SCREEN + row_at(5) + E + b"[90m" + "❯\u00a0".encode()),
    ("a coloured mark at the start of a row, a menu's, stays as it is",
     ON_SCREEN + row_at(5) + E + b"[96m" + "❯ x".encode(), ON_SCREEN + row_at(5) + E + b"[96m" + "❯ x".encode()),
    ("a dim mark stays as it is", ON_SCREEN + row_at(5) + E + b"[2m" + "❯".encode(), ON_SCREEN + row_at(5) + E + b"[2m" + "❯".encode()),
    ("a mark inside a row stays as it is", ON_SCREEN + row_at(5, 3) + "❯".encode(), ON_SCREEN + row_at(5, 3) + "❯".encode()),
    ("no white mark on the normal screen", row_at(5) + "❯".encode(), row_at(5) + "❯".encode()),
    ("no line on a used row",
     ON_SCREEN + row_at(2) + b"x" + row_at(3) + BAND + b"      " + E + b"[49m" + row_at(4) + b"y" + FRAME_END,
     ON_SCREEN + row_at(2) + b"x" + row_at(3) + BAND + b"      " + E + b"[49m" + row_at(4) + b"y" + FRAME_END + OPEN
     + row_at(3) + SIDE + "▏".encode() + row_at(3, 6) + "▕".encode() + CLOSE),
    ("nothing without the band", ON_SCREEN + row_at(3) + b"ab" + FRAME_END, ON_SCREEN + row_at(3) + b"ab" + FRAME_END),
    ("nothing on the normal screen", row_at(3) + BAND + b"      " + FRAME_END, row_at(3) + BAND + b"      " + FRAME_END),
]


# Claude Code's diff rows (2026-11-08): the bytes as its renderer writes them, on a 6 x 40 screen.
KEYWORD, NUMBER, STRING, PUNCT, FUNCTION = (E + b"[38;2;%sm" % c for c in (b"87;191;255", b"255;0;255", b"255;255;0",
                                                                         b"178;217;255", b"63;127;255"))
DIM_GUTTER = E + b"[37m" + E + b"[2m"
CODE_ROW = row_at(2) + DIM_GUTTER + b" 9 " + E + b"[22m" + b" "      # a context row: dim line number, code from column 4

SYNTAX_CASES = [
    ("keyword, number and punctuation on a context row",
     ON_SCREEN + CODE_ROW + E + b"[95mif" + E + b"[37m x " + E + b"[94m0" + E + b"[37m:",
     ON_SCREEN + CODE_ROW + E + b"[95m" + KEYWORD + b"if" + E + b"[37m x " + E + b"[94m" + NUMBER + b"0" + E + b"[37m" + PUNCT + b":"),
    ("an added row: the mark stays, the string after it changes",
     ON_SCREEN + row_at(3) + E + b"[92m 2 +'s'",
     ON_SCREEN + row_at(3) + E + b"[92m 2 +" + STRING + b"'s'"),
    ("a removed row is dim and stays as it is",
     ON_SCREEN + row_at(3) + E + b"[91m" + E + b"[48;5;235m 2 -" + DIM_GUTTER + b"if (x)",
     ON_SCREEN + row_at(3) + E + b"[91m" + E + b"[48;5;235m 2 -" + DIM_GUTTER + b"if (x)"),
    ("a wrapped added row below a code row",
     ON_SCREEN + CODE_ROW + b"a" + row_at(3) + E + b"[92m   +'y'",
     ON_SCREEN + CODE_ROW + b"a" + row_at(3) + E + b"[92m   +" + STRING + b"'y'"),
    ("a mark without a number, not below a code row",
     ON_SCREEN + row_at(3) + E + b"[92m   +'y'",
     ON_SCREEN + row_at(3) + E + b"[92m   +'y'"),
    ("dim spaces below a code row: a wrapped context row",
     ON_SCREEN + CODE_ROW + b"a" + row_at(3) + E + b"[2m   " + E + b"[22m " + E + b"[92m'b'",
     ON_SCREEN + CODE_ROW + b"a" + row_at(3) + E + b"[2m   " + E + b"[22m " + E + b"[92m" + STRING + b"'b'"),
    ("a partial rewrite: dim number, then a jump into the code",
     ON_SCREEN + row_at(4) + DIM_GUTTER + b"3" + E + b"[10G" + E + b"[22m" + E + b"[92m'a'",
     ON_SCREEN + row_at(4) + DIM_GUTTER + b"3" + E + b"[10G" + E + b"[22m" + E + b"[92m" + STRING + b"'a'"),
    ("leaving a code row writes the program's colour again",
     ON_SCREEN + CODE_ROW + E + b"[95mif" + row_at(5) + b"x",
     ON_SCREEN + CODE_ROW + E + b"[95m" + KEYWORD + b"if" + row_at(5) + E + b"[95mx"),
    ("interface colours outside code rows stay",
     ON_SCREEN + row_at(2) + E + b"[92m" + "●".encode() + b" ok " + E + b"[94m* 3" + E + b"[95mx",
     ON_SCREEN + row_at(2) + E + b"[92m" + "●".encode() + b" ok " + E + b"[94m* 3" + E + b"[95mx"),
    ("writing over the gutter ends the code row",
     ON_SCREEN + CODE_ROW + b"a" + row_at(2) + E + b"[92m" + "●".encode() + E + b"[95m if",
     ON_SCREEN + CODE_ROW + b"a" + row_at(2) + E + b"[92m" + "●".encode() + E + b"[95m if"),
    ("erasing the row ends the code row",
     ON_SCREEN + CODE_ROW + b"a" + row_at(2) + E + b"[2K" + row_at(2, 6) + E + b"[95mif",
     ON_SCREEN + CODE_ROW + b"a" + row_at(2) + E + b"[2K" + row_at(2, 6) + E + b"[95mif"),
    ("punctuation in plain code text, each mark once",
     ON_SCREEN + CODE_ROW + b"a(b)/c:d'e",
     ON_SCREEN + CODE_ROW + b"a" + PUNCT + b"(" + E + b"[37mb" + PUNCT + b")/" + E + b"[37mc" + PUNCT + b":"
     + E + b"[37md" + PUNCT + b"'" + E + b"[37me"),
    ("a function name on a code row",
     ON_SCREEN + CODE_ROW + E + b"[96mdef" + E + b"[37m " + E + b"[93mratio" + E + b"[37m(",
     ON_SCREEN + CODE_ROW + E + b"[96mdef" + E + b"[37m " + E + b"[93m" + FUNCTION + b"ratio" + E + b"[37m" + PUNCT + b"("),
    ("bright yellow outside code rows stays",
     ON_SCREEN + row_at(2) + E + b"[93mwarning" + E + b"[39m",
     ON_SCREEN + row_at(2) + E + b"[93mwarning" + E + b"[39m"),
    ("nothing on the normal screen",
     CODE_ROW + E + b"[95mif" + E + b"[37m:",
     CODE_ROW + E + b"[95mif" + E + b"[37m:"),
]


REPLY = ON_SCREEN + row_at(2) + "● ".encode()
def painted(row, column, text, bold=False, colour=b"232;127;224"):
    return E + b"7" + row_at(row, column) + E + (b"[0;1;38;2;" if bold else b"[0;38;2;") + colour + b"m" + text + E + b"8"

NUMBER_CASES = [
    ("a number in a reply", REPLY + b"pil 60 s", ON_SCREEN + row_at(2) + "● ".encode() + b"pil 60 " + painted(2, 7, b"60") + b"s"),
    ("words and names stay", REPLY + b"v10 W7 3D #3f7fff a_9.tex m/s ", REPLY + b"v10 W7 3D #3f7fff a_9.tex m/s "),
    ("date and time, then the row's end", REPLY + b"2026-11-08, 12:38 ", REPLY + b"2026-11-08, " + painted(2, 3, b"2026-11-08", colour=b"163;165;245") + b"12:38 " + painted(2, 15, b"12:38", colour=b"163;165;245")),
    ("percent, range, decimal comma, suffix", REPLY + "%16 0.67-0.69, 58,3 2'si ".encode(), REPLY + b"%16 " + painted(2, 3, b"%16") + b"0.67-0.69, " + painted(2, 7, b"0.67-0.69") + b"58,3 " + painted(2, 18, b"58,3") + b"2'si " + painted(2, 23, b"2")),
    ("a number ended by an escape", REPLY + b"33" + E + b"[9G", REPLY + b"33" + painted(2, 3, b"33") + E + b"[9G"),
    ("bold stays bold", REPLY + E + b"[1m5 ", REPLY + E + b"[1m5 " + painted(2, 3, b"5", True)),
    ("grey status stays", ON_SCREEN + row_at(2) + E + b"[90mdone 12:38 ", ON_SCREEN + row_at(2) + E + b"[90mdone 12:38 "),
    ("input row stays", ON_SCREEN + row_at(5) + E + b"[39m" + "❯".encode() + b" 42 ", ON_SCREEN + row_at(5) + E + b"[39m" + "❯".encode() + WHITE_MARK + E + b"[39m" + b" 42 "),
    ("second input row stays",
     ON_SCREEN + row_at(4) + E + b"[39m" + "❯ a".encode() + row_at(5, 3) + b"42 ",
     ON_SCREEN + row_at(4) + E + b"[39m" + "❯".encode() + WHITE_MARK + E + b"[39m" + b" a" + row_at(5, 3) + b"42 "),
    ("sent row stays", ON_SCREEN + row_at(2) + BAND + b"  42 ", ON_SCREEN + row_at(2) + BAND + b"  42 "),
    ("tool result stays", ON_SCREEN + row_at(2, 3) + "⎿  Added 1 line ".encode(), ON_SCREEN + row_at(2, 3) + "⎿  Added 1 line ".encode()),
    ("tool summary stays", ON_SCREEN + row_at(2, 3) + b"Reading 1 file ", ON_SCREEN + row_at(2, 3) + b"Reading 1 file "),
    ("list number stays, value goes", ON_SCREEN + row_at(2, 3) + b"1. a 7 ", ON_SCREEN + row_at(2, 3) + b"1. a 7 " + painted(2, 8, b"7")),
    ("erased before its end is not drawn back", REPLY + b"33" + E + b"[2K", REPLY + b"33" + painted(2, 3, b"33") + E + b"[2K"),
    ("nothing on the normal screen", row_at(2) + b"60 ", row_at(2) + b"60 "),
    ("a changed digit brings the whole number's colour",
     REPLY + b"42 " + row_at(2, 4) + b"3" + E + b"[9G",
     REPLY + b"42 " + painted(2, 3, b"42") + row_at(2, 4) + b"3" + painted(2, 3, b"43") + E + b"[9G"),
    ("a number that grows into a word gives its colour back",
     REPLY + b"42 " + row_at(2, 5) + b"x" + E + b"[9G",
     REPLY + b"42 " + painted(2, 3, b"42") + row_at(2, 5) + b"x" + E + b"7" + row_at(2, 3) + E + b"[0m4" + row_at(2, 4) + E + b"[0m2" + E + b"8" + E + b"[9G"),
    ("reverse video stays", REPLY + E + b"[7m42 ", REPLY + E + b"[7m42 "),
    ("nothing while the program holds a saved cursor", REPLY + E + b"7" + b"42 " + E + b"8" + b"7 ",
     REPLY + E + b"7" + b"42 " + E + b"8" + b"7 " + painted(2, 3, b"72")),
    ("nothing in insert mode", REPLY + E + b"[4h42 " + E + b"[4l", REPLY + E + b"[4h42 " + E + b"[4l"),
    ("nothing in origin mode", REPLY + E + b"[?6h" + row_at(2, 3) + b"42 ", REPLY + E + b"[?6h" + row_at(2, 3) + b"42 "),
    ("a repeated character moves the column", REPLY + b"-" + E + b"[3b 42 ", REPLY + b"-" + E + b"[3b 42 " + painted(2, 8, b"42")),
]


def run_number_cases():
    failures = 0
    for name, given, wanted in NUMBER_CASES:
        for chunk in ("65536", "1"):
            got = subprocess.run([BINARY, "--filter", chunk, "6", "40"], input=given, capture_output=True).stdout
            if got != wanted:
                failures += 1
                print(f"FAIL {name} (chunk {chunk}):\n got  {got!r}\n want {wanted!r}")
    print(f"numbers: {len(NUMBER_CASES)} cases, each whole and byte by byte, {failures} failed")
    return failures


def run_syntax_cases():
    failures = 0
    for name, given, wanted in SYNTAX_CASES:
        for chunk in ("65536", "1"):
            got = subprocess.run([BINARY, "--filter", chunk, "6", "40"], input=given, capture_output=True).stdout
            if got != wanted:
                failures += 1
                print(f"FAIL {name} (chunk {chunk}):\n got  {got!r}\n want {wanted!r}")
    print(f"syntax: {len(SYNTAX_CASES)} cases, each whole and byte by byte, {failures} failed")
    return failures


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

    # Numbers go by every full-screen program; Claude Code's marks follow the foreground program's name,
    # and a script's process takes the script's name.
    with tempfile.TemporaryDirectory() as folder:
        for name, is_claude in (("claude", True), ("other", False)):
            script = Path(folder) / name
            script.write_text("#!/bin/sh\nsleep 0.4\nprintf '\\033[?1049h\\033[2;3Hprice 42 now \\033[4;1H\\033[48;2;38;3;7m❯ a'\nsleep 0.2\n")
            script.chmod(0o755)
            named_pid, named_master = spawn(str(script))
            text = read_all(named_master, 2)
            finish(named_pid)
            failures += check(f"numbers in any full-screen program ({name})", b"232;127;224" in text, repr(text))
            failures += check(f"sent mark only under Claude Code ({name})", ("▶".encode() in text) == is_claude, repr(text))
            os.close(named_master)

    os.close(master)
    os.close(slave)

    return failures


if __name__ == "__main__":
    signal.signal(signal.SIGCHLD, signal.SIG_DFL)
    failed = run_stream_cases() + run_box_cases() + run_syntax_cases() + run_number_cases() + run_pty_cases()
    print("all passed" if failed == 0 else f"{failed} failed")
    sys.exit(1 if failed else 0)
