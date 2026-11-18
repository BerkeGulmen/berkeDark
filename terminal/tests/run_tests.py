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
    ("no box while the program holds a saved cursor",
     ON_SCREEN + E + b"7" + row_at(3) + BAND + b"  ab  " + E + b"[49m" + FRAME_END,
     ON_SCREEN + E + b"7" + row_at(3) + BAND + b"  ab  " + E + b"[49m" + FRAME_END),
    ("nothing without the band", ON_SCREEN + row_at(3) + b"ab" + FRAME_END, ON_SCREEN + row_at(3) + b"ab" + FRAME_END),
    ("nothing on the normal screen", row_at(3) + BAND + b"      " + FRAME_END, row_at(3) + BAND + b"      " + FRAME_END),
]


# Claude Code's diff rows (2026-11-08): the bytes as its renderer writes them, on a 6 x 40 screen.
KEYWORD, NUMBER, STRING, PUNCT, FUNCTION = (E + b"[38;2;%sm" % c for c in (b"87;191;255", b"255;0;255", b"255;255;0",
                                                                         b"178;217;255", b"63;127;255"))
DIM_GUTTER = E + b"[37m" + E + b"[2m"
# The grounds of a diff's rows (2026-11-10): the program's greys 235, 237 and 238 give way to the theme's four grounds.
ADDED_GROUND, ADDED_WORD_GROUND, REMOVED_GROUND, REMOVED_WORD_GROUND = (E + b"[48;2;%sm" % c for c in (
    b"29;42;43", b"56;76;57", b"40;26;45", b"81;40;62"))
GREY, GREY_REMOVED_WORD, GREY_ADDED_WORD = E + b"[48;5;235m", E + b"[48;5;237m", E + b"[48;5;238m"
CODE_TEXT = E + b"[38;2;255;232;194m"   # plain code text, for a "string" that an apostrophe began
CODE_ROW = row_at(2) + DIM_GUTTER + b" 9 " + E + b"[22m" + b" "      # a context row: dim line number, code from column 4

SYNTAX_CASES = [
    ("keyword, number and punctuation on a context row",
     ON_SCREEN + CODE_ROW + E + b"[95mif" + E + b"[37m x " + E + b"[94m0" + E + b"[37m:",
     ON_SCREEN + CODE_ROW + E + b"[95m" + KEYWORD + b"if" + E + b"[37m x " + E + b"[94m" + NUMBER + b"0" + E + b"[37m" + PUNCT + b":"),
    ("an added row: the mark stays, the string after it changes",
     ON_SCREEN + row_at(3) + E + b"[92m 2 +'s'",
     ON_SCREEN + row_at(3) + E + b"[92m 2 +" + STRING + b"'s'"),
    ("a removed row keeps its dim text and stands on the removed ground",
     ON_SCREEN + row_at(3) + E + b"[91m" + E + b"[48;5;235m 2 -" + DIM_GUTTER + b"if (x)",
     ON_SCREEN + row_at(3) + E + b"[91m" + E + b"[48;5;235m" + REMOVED_GROUND + b" 2 -" + DIM_GUTTER + b"if (x)"),
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
    # The grounds of a diff's rows: the row's kind comes from its gutter, first by the gutter's colour, then from the map.
    ("an added row stands on the added ground, its changed word on the word's ground",
     ON_SCREEN + row_at(3) + E + b"[92m" + GREY + b" 2 +" + E + b"[37ma " + GREY_ADDED_WORD + b"b" + GREY + b" c",
     ON_SCREEN + row_at(3) + E + b"[92m" + GREY + ADDED_GROUND + b" 2 +" + E + b"[37ma " + GREY_ADDED_WORD + ADDED_WORD_GROUND
     + b"b" + GREY + ADDED_GROUND + b" c"),
    ("the changed word of a removed row",
     ON_SCREEN + row_at(3) + E + b"[91m" + GREY + b" 2 -" + DIM_GUTTER + b"a " + GREY_REMOVED_WORD + b"b",
     ON_SCREEN + row_at(3) + E + b"[91m" + GREY + REMOVED_GROUND + b" 2 -" + DIM_GUTTER + b"a " + GREY_REMOVED_WORD
     + REMOVED_WORD_GROUND + b"b"),
    ("a removed row written over a row with a shorter gutter: the ground starts at its first cell and holds",
     ON_SCREEN + CODE_ROW + b"a" + row_at(2) + E + b"[91m" + GREY + b" 12 -" + DIM_GUTTER + b"x",
     ON_SCREEN + CODE_ROW + b"a" + row_at(2) + E + b"[91m" + GREY + REMOVED_GROUND + b" 12 -" + DIM_GUTTER + b"x"),
    ("the grey ground on a row that is no diff's row stays",
     ON_SCREEN + row_at(3) + GREY + b"ab" + E + b"[49m", ON_SCREEN + row_at(3) + GREY + b"ab" + E + b"[49m"),
    ("leaving a diff's row writes the program's grey again",
     ON_SCREEN + row_at(3) + E + b"[92m" + GREY + b" 2 +" + row_at(5) + E + b"[37mx",
     ON_SCREEN + row_at(3) + E + b"[92m" + GREY + ADDED_GROUND + b" 2 +" + row_at(5) + E + b"[37m" + GREY + b"x"),
    # A diff's rows are coloured one by one: inside a comment of several rows an apostrophe looked like a string's start.
    ("an apostrophe after a word starts no string: plain code text, its signs in their colour",
     ON_SCREEN + CODE_ROW + E + b"[37m* doesn" + E + b"[92m't go",
     ON_SCREEN + CODE_ROW + E + b"[37m* doesn" + E + b"[92m" + PUNCT + b"'" + CODE_TEXT + b"t go"),
    ("a part of such a run written again stays plain",
     ON_SCREEN + CODE_ROW + E + b"[37m* doesn" + E + b"[92m't go" + row_at(2, 15) + E + b"[92mgo",
     ON_SCREEN + CODE_ROW + E + b"[37m* doesn" + E + b"[92m" + PUNCT + b"'" + CODE_TEXT + b"t go" + row_at(2, 15) + E + b"[92m"
     + CODE_TEXT + b"go"),
    ("a string after a prefix letter is a string",
     ON_SCREEN + CODE_ROW + E + b"[37mx = f" + E + b"[92m'a'", ON_SCREEN + CODE_ROW + E + b"[37mx = f" + E + b"[92m" + STRING + b"'a'"),
    ("a character in quotes after a blank is a string",
     ON_SCREEN + CODE_ROW + E + b"[37mc = " + E + b"[92m'x'", ON_SCREEN + CODE_ROW + E + b"[37mc = " + E + b"[92m" + STRING + b"'x'"),
    ("an apostrophe inside a string is the string's",
     ON_SCREEN + CODE_ROW + E + b"[37mc = " + E + b"[92m\"it's\"",
     ON_SCREEN + CODE_ROW + E + b"[37mc = " + E + b"[92m" + STRING + b"\"it's\""),
    ("dim text right after a dim number is no gutter",
     ON_SCREEN + row_at(2, 3) + E + b"[2m3 dosya" + E + b"[22m" + row_at(2, 14) + E + b"[95mif",
     ON_SCREEN + row_at(2, 3) + E + b"[2m3 dosya" + E + b"[22m" + row_at(2, 14) + E + b"[95mif"),
]


REPLY = ON_SCREEN + row_at(2) + "● ".encode()
def painted(row, column, text, bold=False, colour=b"255;0;255"):
    return E + b"7" + row_at(row, column) + E + (b"[0;1;38;2;" if bold else b"[0;38;2;") + colour + b"m" + text + E + b"8"

NUMBER_CASES = [
    ("a number in a reply", REPLY + b"pil 60 s", ON_SCREEN + row_at(2) + "● ".encode() + b"pil 60 " + painted(2, 7, b"60") + b"s"),
    ("words and names stay", REPLY + b"v10 W7 3D #3f7fff a_9.tex m/s ", REPLY + b"v10 W7 3D #3f7fff a_9.tex m/s "),
    ("date and time, then the row's end", REPLY + b"2026-11-08, 12:38 ", REPLY + b"2026-11-08, " + painted(2, 3, b"2026-11-08", colour=b"232;127;224") + b"12:38 " + painted(2, 15, b"12:38", colour=b"232;127;224")),
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
    # The blank after the 7 has taken the old 2's cell on the terminal, so the number is 7 alone. (Until 2026-11-10 the
    # word was read before the blank reached the map, and the old 2 was written back over the blank.)
    ("nothing while the program holds a saved cursor", REPLY + E + b"7" + b"42 " + E + b"8" + b"7 ",
     REPLY + E + b"7" + b"42 " + E + b"8" + b"7 " + painted(2, 3, b"7")),
    ("nothing in insert mode", REPLY + E + b"[4h42 " + E + b"[4l", REPLY + E + b"[4h42 " + E + b"[4l"),
    ("nothing in origin mode", REPLY + E + b"[?6h" + row_at(2, 3) + b"42 ", REPLY + E + b"[?6h" + row_at(2, 3) + b"42 "),
    ("a repeated character moves the column", REPLY + b"-" + E + b"[3b 42 ", REPLY + b"-" + E + b"[3b 42 " + painted(2, 8, b"42")),
    # The screen's behaviour in these cases was measured on foot (2026-11-08).
    ("lines deleted below the scrolling region change nothing",
     ON_SCREEN + E + b"[2;3r" + row_at(5, 4) + E + b"[M42 ", ON_SCREEN + E + b"[2;3r" + row_at(5, 4) + E + b"[M42 " + painted(5, 4, b"42")),
    ("lines inserted below the scrolling region change nothing",
     ON_SCREEN + E + b"[2;3r" + row_at(5, 4) + E + b"[L42 ", ON_SCREEN + E + b"[2;3r" + row_at(5, 4) + E + b"[L42 " + painted(5, 4, b"42")),
    ("lines inserted inside the region send the cursor to the row's start",
     ON_SCREEN + row_at(3, 9) + E + b"[L42 ", ON_SCREEN + row_at(3, 9) + E + b"[L42 " + painted(3, 1, b"42")),
    ("the cursor saved and restored with CSI s and u",
     ON_SCREEN + row_at(3, 4) + E + b"[s" + row_at(5, 9) + E + b"[u42 ",
     ON_SCREEN + row_at(3, 4) + E + b"[s" + row_at(5, 9) + E + b"[u42 " + painted(3, 4, b"42")),
    ("nothing while the program holds a cursor saved with CSI s",
     ON_SCREEN + row_at(3, 4) + E + b"[s42 ", ON_SCREEN + row_at(3, 4) + E + b"[s42 "),
    ("no wrapping once the program has turned it off",
     ON_SCREEN + E + b"[?7l" + row_at(2, 39) + b"abcd 42 ", ON_SCREEN + E + b"[?7l" + row_at(2, 39) + b"abcd 42 "),
    ("a region that is no region is ignored",
     ON_SCREEN + E + b"[2;5r" + row_at(4, 3) + E + b"[5;2r42 ", ON_SCREEN + E + b"[2;5r" + row_at(4, 3) + E + b"[5;2r42 " + painted(4, 3, b"42")),
    ("leaving origin mode sends the cursor home",
     ON_SCREEN + row_at(4, 3) + E + b"[?6h" + E + b"[?6l42 ", ON_SCREEN + row_at(4, 3) + E + b"[?6h" + E + b"[?6l42 " + painted(1, 1, b"42")),
    ("a position in origin mode is relative to the region",
     ON_SCREEN + E + b"[3;5r" + E + b"[?6h" + row_at(1, 1) + b"abc" + E + b"[?6l" + row_at(3, 4) + b"4 ",
     ON_SCREEN + E + b"[3;5r" + E + b"[?6h" + row_at(1, 1) + b"abc" + E + b"[?6l" + row_at(3, 4) + b"4 "),
    ("a pending wrap stays through a line deletion that does nothing",
     ON_SCREEN + E + b"[2;3r" + row_at(5, 40) + b"x" + E + b"[M42 ", ON_SCREEN + E + b"[2;3r" + row_at(5, 40) + b"x" + E + b"[M42 " + painted(6, 1, b"42")),
    ("a pending wrap stays through a region that is no region",
     ON_SCREEN + row_at(5, 40) + b"x" + E + b"[5;2r42 ", ON_SCREEN + row_at(5, 40) + b"x" + E + b"[5;2r42 " + painted(6, 1, b"42")),
    # The blank that ends a word is on the terminal before the word is read: its cell is not the old number's any more.
    ("a word written over the start of a number leaves the blank after it alone",
     REPLY + b"12345 " + row_at(2, 3) + b"ab " + E + b"[9G",
     REPLY + b"12345 " + painted(2, 3, b"12345") + row_at(2, 3) + b"ab " + E + b"[9G"),
    # How many cells foot gives these characters was measured on foot 1.28 (2026-11-10): two each.
    ("an emoji selector widens its character, so the number after it is found in its place",
     REPLY + "⚠️ 42 ".encode(), REPLY + "⚠️ 42 ".encode() + painted(2, 6, b"42")),
    ("pictures tied by joiners keep two cells",
     REPLY + "👨‍👩‍👧 42 ".encode(), REPLY + "👨‍👩‍👧 42 ".encode() + painted(2, 6, b"42")),
    ("a skin tone stays in its emoji's cells",
     REPLY + "👍🏽 42 ".encode(), REPLY + "👍🏽 42 ".encode() + painted(2, 6, b"42")),
    ("a keycap is no number", REPLY + "1️⃣ x ".encode(), REPLY + "1️⃣ x ".encode()),
    ("very large numbers in sequences do no harm",
     ON_SCREEN + E + b"[2147483647C" + E + b"[2147483647B" + E + b"[2147483647X" + E + b"[" + b"9" * 200 + b"D\r42 ",
     ON_SCREEN + E + b"[2147483647C" + E + b"[2147483647B" + E + b"[2147483647X" + E + b"[" + b"9" * 200 + b"D\r42 " + painted(6, 1, b"42")),
]


# Claude Code's input box (2026-11-10): a rule from edge to edge, the mark, the typed text; on a 6 x 40 screen.
INPUT = E + b"[38;2;63;255;127m"
RULE = E + b"[90m" + "─".encode() * 40 + E + b"[39m"
MARK = "❯".encode() + WHITE_MARK + E + b"[39m"   # the mark as berkeDark leaves it
BOX = ON_SCREEN + row_at(3) + RULE + row_at(4)    # the rule on row 3, the cursor at the start of row 4
BOX_HIGHER = ON_SCREEN + row_at(2) + RULE + row_at(3)
SHORT_RULE = E + b"[90m" + "─".encode() * 39 + E + b"[39m"
LIST_POINTER = BOX + E + b"[96m" + "❯ x".encode() + E + b"[39m" + row_at(5, 3) + b"yz"
SHELL_MODE = ON_SCREEN + row_at(3) + E + b"[91m" + "─".encode() * 40 + row_at(4) + b"! " + E + b"[39m" + b"ls"

INPUT_CASES = [
    ("typed text takes the input colour", BOX + "❯ ab 12 ".encode(), BOX + MARK + b" " + INPUT + b"ab 12 "),
    ("typing one character at a time",
     BOX + "❯ ".encode() + row_at(4, 3) + b"a" + row_at(6) + row_at(4, 4) + b"b",
     BOX + MARK + b" " + row_at(4, 3) + INPUT + b"a" + row_at(6) + row_at(4, 4) + b"b"),
    ("a row of the box under the mark",
     BOX + "❯ ab".encode() + row_at(5, 3) + b"cd", BOX + MARK + b" " + INPUT + b"ab" + row_at(5, 3) + b"cd"),
    ("after an empty row of the box, with a number",
     BOX_HIGHER + "❯ a".encode() + row_at(5, 3) + b"7 ", BOX_HIGHER + MARK + b" " + INPUT + b"a" + row_at(5, 3) + b"7 "),
    ("the hint of an empty box stays dim",
     BOX + "❯ ".encode() + E + b"[2mTry" + E + b"[22m", BOX + MARK + b" " + E + b"[2mTry" + E + b"[22m"),
    ("the grey mark of a working Claude, text still coloured",
     BOX + E + b"[90m" + "❯".encode() + E + b"[39m" + b" ab",
     BOX + E + b"[90m" + "❯".encode() + E + b"[39m" + b" " + INPUT + b"ab"),
    ("nothing under the lower rule",
     BOX_HIGHER + "❯ a".encode() + row_at(4) + RULE + row_at(5, 3) + b"xy",
     BOX_HIGHER + MARK + b" " + INPUT + b"a" + row_at(4) + RULE + row_at(5, 3) + b"xy"),
    ("leaving the box gives the colour back",
     BOX + "❯ a".encode() + row_at(1) + b"z", BOX + MARK + b" " + INPUT + b"a" + row_at(1) + OFF + b"z"),
    ("the typed colour gives way to the program's and comes back",
     BOX + "❯ a".encode() + E + b"[96mb" + E + b"[39mc",
     BOX + MARK + b" " + INPUT + b"a" + E + b"[96mb" + E + b"[39m" + INPUT + b"c"),
    ("italic right after typed text keeps its gold",
     BOX + "❯ a".encode() + row_at(1) + E + b"[3mi" + E + b"[23m",
     BOX + MARK + b" " + INPUT + b"a" + row_at(1) + E + b"[3m" + ON + b"i" + E + b"[23m" + OFF),
    ("a dim hint that starts with a digit does not turn the box into a code row",
     BOX + "❯ ".encode() + E + b"[2m1a 2b" + E + b"[22m" + row_at(4, 3) + b"7" + E + b"[K",
     BOX + MARK + b" " + E + b"[2m1a 2b" + E + b"[22m" + row_at(4, 3) + INPUT + b"7" + E + b"[K"),
    ("a mark with no rule above is no input box",
     ON_SCREEN + row_at(4) + "❯ ab".encode(), ON_SCREEN + row_at(4) + MARK + b" ab"),
    ("a short rule is no box",
     ON_SCREEN + row_at(3) + SHORT_RULE + row_at(4) + "❯ ab".encode(),
     ON_SCREEN + row_at(3) + SHORT_RULE + row_at(4) + MARK + b" ab"),
    ("a list's coloured pointer under a rule stays as it is", LIST_POINTER, LIST_POINTER),
    ("shell mode keeps the program's look", SHELL_MODE, SHELL_MODE),
    ("nothing on the normal screen",
     row_at(3) + RULE + row_at(4) + "❯ ab".encode(), row_at(3) + RULE + row_at(4) + "❯ ab".encode()),
]


# Claude Code's reply text (2026-11-10): the rows of a reply are read again when a frame ends; on an 8 x 40 screen.
REPLY_MARK = E + b"[38;2;111;220;240m" + "●".encode() + E + b"[39m"   # the mark in the theme's text colour
TOOL_MARK = E + b"[38;2;169;236;118m" + "●".encode() + E + b"[39m"    # a finished tool's mark
PROSE = ON_SCREEN + row_at(1) + REPLY_MARK + b" "                      # a reply's first row; its text goes on from column 3
BOLD, LIGHT = E + b"[1m", E + b"[22m"
FLAME, PINK, ICE = (E + b"[0;1;38;2;%sm" % c for c in (b"255;95;7", b"243;139;168", b"178;217;255"))
SKY, TURQUOISE, RED, ORANGE, GREEN, FAINT = (E + b"[0;38;2;%sm" % c for c in (
    b"87;191;255", b"111;220;240", b"255;0;0", b"255;158;96", b"169;236;118", b"110;115;128"))
QUOTE = E + b"[38;2;245;194;231m"
BLUE94, PLAIN = E + b"[94m", E + b"[39m"                               # the colour Claude Code gives inline code, and its end
LAVENDER, GOLD, CLASS_C, FUNCTION_C, TYPE_C, CALL_C, KEY_C, STRING_C, NUMBER_C, ICE_C = (E + b"[0;38;2;%sm" % c for c in (
    b"163;165;245", b"255;210;92", b"87;171;100", b"63;127;255", b"63;255;127", b"255;175;31", b"255;191;31", b"255;255;0",
    b"255;0;255", b"178;217;255"))
COMMENT_C, DECORATOR_C = E + b"[0;3;38;2;112;112;112m", E + b"[0;3;38;2;18;171;127m"
OPTION_C = E + b"[0;3;38;2;255;158;96m"
BACK, BACK_BOLD = E + b"[0m", E + b"[0;1m"                              # a cell back in the program's own look
BAR = E + b"[2m" + "▎".encode() + E + b"[22m" + b" "                    # the quote bar as Claude Code writes it
TABLE = [row_at(2, 3) + "┌───┬───┐".encode(), row_at(3, 3) + "│ A │ B │".encode(), row_at(4, 3) + "├───┼───┤".encode(),
         row_at(5, 3) + "│ x │ 7 ".encode(), "│".encode() + row_at(6, 3) + "└───┴───┘".encode()]


def settled(*parts):
    return OPEN + b"".join(parts) + CLOSE


REPLY_CASES = [
    ("a row that is bold from end to end is a heading line",
     PROSE + BOLD + b"Ad Soyad" + LIGHT + FRAME_END,
     PROSE + BOLD + b"Ad Soyad" + LIGHT + FRAME_END + settled(row_at(1, 3) + FLAME + b"Ad" + row_at(1, 6) + b"Soyad")),
    ("a label opens the row",
     PROSE + BOLD + b"Not:" + LIGHT + b" ab" + FRAME_END,
     PROSE + BOLD + b"Not:" + LIGHT + b" ab" + FRAME_END + settled(row_at(1, 3) + PINK + b"Not:")),
    ("bold words that do not end with a colon stay",
     PROSE + BOLD + b"Not" + LIGHT + b": ab" + FRAME_END, PROSE + BOLD + b"Not" + LIGHT + b": ab" + FRAME_END),
    ("bold words inside a sentence stay, with a colon too",
     PROSE + b"ab " + BOLD + b"Not:" + LIGHT + b" cd" + FRAME_END, PROSE + b"ab " + BOLD + b"Not:" + LIGHT + b" cd" + FRAME_END),
    ("a label of two words after a list's mark, with nothing after it",
     ON_SCREEN + row_at(2, 3) + b"- " + BOLD + b"Ad Soyad:" + LIGHT + FRAME_END,
     ON_SCREEN + row_at(2, 3) + b"- " + BOLD + b"Ad Soyad:" + LIGHT + FRAME_END
     + settled(row_at(2, 3) + RED + b"-" + row_at(2, 5) + PINK + b"Ad" + row_at(2, 8) + b"Soyad:")),
    ("a bold row that ends with a colon is a heading line",
     PROSE + BOLD + b"Sorular:" + LIGHT + FRAME_END,
     PROSE + BOLD + b"Sorular:" + LIGHT + FRAME_END + settled(row_at(1, 3) + FLAME + b"Sorular:")),
    ("the marks of a list: a hyphen, a nested hyphen, a number",
     ON_SCREEN + row_at(2, 3) + b"- bir" + row_at(3, 5) + b"- iki" + row_at(4, 3) + b"12. on iki" + FRAME_END,
     ON_SCREEN + row_at(2, 3) + b"- bir" + row_at(3, 5) + b"- iki" + row_at(4, 3) + b"12. on iki" + FRAME_END
     + settled(row_at(2, 3) + RED + b"-" + row_at(3, 5) + RED + b"-" + row_at(4, 3) + RED + b"12.")),
    ("a list number deep in a list turns from the number colour to the mark's",
     ON_SCREEN + row_at(2, 9) + b"3. uc" + FRAME_END,
     ON_SCREEN + row_at(2, 9) + b"3. " + painted(2, 9, b"3") + b"uc" + FRAME_END + settled(row_at(2, 9) + RED + b"3.")),
    ("a task's box gives way to its mark, the hyphen stays",
     ON_SCREEN + row_at(2, 3) + b"- [ ] ac" + row_at(3, 3) + b"- [x] ok" + FRAME_END,
     ON_SCREEN + row_at(2, 3) + b"- [ ] ac" + row_at(3, 3) + b"- [x] ok" + FRAME_END
     + settled(row_at(2, 3) + RED + b"-" + row_at(2, 5) + BACK + b" " + row_at(2, 7) + ORANGE + "○".encode()
               + row_at(3, 3) + RED + b"-" + row_at(3, 5) + BACK + b"  " + GREEN + "✓".encode())),
    ("a link's address in brackets, and an address standing alone without its full stop",
     PROSE + b"site (https://a.io) ve http://b.io/x." + FRAME_END,
     PROSE + b"site (https://a.io) ve http://b.io/x." + FRAME_END
     + settled(row_at(1, 9) + SKY + b"https://a.io" + row_at(1, 26) + TURQUOISE + b"http://b.io/x")),
    ("a bold address stays",
     PROSE + b"bak " + BOLD + b"https://a.io" + LIGHT + b" ab" + FRAME_END,
     PROSE + b"bak " + BOLD + b"https://a.io" + LIGHT + b" ab" + FRAME_END),
    ("a table: faint lines, the names of the columns, a number in a cell",
     ON_SCREEN + b"".join(TABLE) + FRAME_END,
     ON_SCREEN + TABLE[0] + TABLE[1] + TABLE[2] + TABLE[3] + painted(5, 9, b"7") + TABLE[4] + FRAME_END
     + settled(row_at(2, 3) + FAINT + "┌───┬───┐".encode()
               + row_at(3, 3) + FAINT + "│".encode() + row_at(3, 5) + ICE + b"A" + row_at(3, 7) + FAINT + "│".encode()
               + row_at(3, 9) + ICE + b"B" + row_at(3, 11) + FAINT + "│".encode()
               + row_at(4, 3) + FAINT + "├───┼───┤".encode()
               + row_at(5, 3) + FAINT + "│".encode() + row_at(5, 7) + "│".encode() + row_at(5, 11) + "│".encode()
               + row_at(6, 3) + FAINT + "└───┴───┘".encode())),
    ("a box in a drawing takes the lines' colour and has no header",
     ON_SCREEN + row_at(2, 3) + "┌──┐".encode() + row_at(3, 3) + "│ab│".encode() + row_at(4, 3) + "└──┘".encode() + FRAME_END,
     ON_SCREEN + row_at(2, 3) + "┌──┐".encode() + row_at(3, 3) + "│ab│".encode() + row_at(4, 3) + "└──┘".encode() + FRAME_END
     + settled(row_at(2, 3) + FAINT + "┌──┐".encode() + row_at(3, 3) + FAINT + "│".encode() + row_at(3, 6) + "│".encode()
               + row_at(4, 3) + FAINT + "└──┘".encode())),
    ("the lines of a tree are no table",
     ON_SCREEN + row_at(2, 3) + "├── src".encode() + FRAME_END, ON_SCREEN + row_at(2, 3) + "├── src".encode() + FRAME_END),
    ("a rule becomes a line across the text",
     ON_SCREEN + row_at(3, 3) + b"---" + FRAME_END,
     ON_SCREEN + row_at(3, 3) + b"---" + FRAME_END + settled(row_at(3, 3) + FAINT + "─".encode() * 36)),
    ("three hyphens with text right under them are no rule",
     ON_SCREEN + row_at(3, 3) + b"---" + row_at(4, 3) + b"ad: x" + FRAME_END,
     ON_SCREEN + row_at(3, 3) + b"---" + row_at(4, 3) + b"ad: x" + FRAME_END),
    ("a rule gives its cells back when text comes under it",
     ON_SCREEN + row_at(3, 3) + b"---" + FRAME_END + row_at(4, 3) + b"ad" + FRAME_END,
     ON_SCREEN + row_at(3, 3) + b"---" + FRAME_END + settled(row_at(3, 3) + FAINT + "─".encode() * 36)
     + row_at(4, 3) + b"ad" + FRAME_END + settled(row_at(3, 3) + BACK + b"---" + b" " * 33)),
    ("quoted text takes its colour as it is printed",
     ON_SCREEN + row_at(2, 3) + BAR + E + b"[3msoz" + E + b"[23m",
     ON_SCREEN + row_at(2, 3) + BAR + E + b"[3m" + ON + QUOTE + b"soz" + E + b"[23m" + OFF),
    ("bold inside a quote keeps the quote's colour",
     ON_SCREEN + row_at(2, 3) + BAR + E + b"[3ma " + BOLD + b"b" + LIGHT + b" c" + E + b"[23m",
     ON_SCREEN + row_at(2, 3) + BAR + E + b"[3m" + ON + QUOTE + b"a " + BOLD + BOLD_ITALIC_ON + QUOTE + b"b" + LIGHT + ON + QUOTE
     + b" c" + E + b"[23m" + OFF),
    ("italic text without the bar keeps its gold",
     ON_SCREEN + row_at(2, 3) + E + b"[3msoz" + E + b"[23m", ON_SCREEN + row_at(2, 3) + E + b"[3m" + ON + b"soz" + E + b"[23m" + OFF),
    ("rows under a tool's mark are not reply text",
     ON_SCREEN + row_at(1) + TOOL_MARK + b" " + BOLD + b"Bash" + LIGHT + b"(ls)" + row_at(2, 3) + BOLD + b"Ad:" + LIGHT + b" x"
     + row_at(3, 3) + b"- y" + FRAME_END,
     ON_SCREEN + row_at(1) + TOOL_MARK + b" " + BOLD + b"Bash" + LIGHT + b"(ls)" + row_at(2, 3) + BOLD + b"Ad:" + LIGHT + b" x"
     + row_at(3, 3) + b"- y" + FRAME_END + settled(row_at(1, 8) + TYPE_C + b"ls")),
    ("rows under a rule, a dialog's, are not reply text",
     ON_SCREEN + row_at(2) + E + b"[90m" + "─".encode() * 40 + E + b"[39m" + row_at(3, 3) + BOLD + b"Baslik" + LIGHT
     + row_at(4, 3) + b"- a" + FRAME_END,
     ON_SCREEN + row_at(2) + E + b"[90m" + "─".encode() * 40 + E + b"[39m" + row_at(3, 3) + BOLD + b"Baslik" + LIGHT
     + row_at(4, 3) + b"- a" + FRAME_END),
    ("the user's own row is not reply text, the rows under it may be",
     ON_SCREEN + row_at(3) + BAND + b"  - a" + b" " * 35 + E + b"[49m" + row_at(5, 3) + b"- b" + FRAME_END,
     ON_SCREEN + row_at(3) + BAND + b"  - a" + b" " * 35 + E + b"[49m" + row_at(5, 3) + b"- b" + FRAME_END + OPEN
     + row_at(2) + LINE + "▁".encode() * 40 + row_at(4) + LINE + "▔".encode() * 40
     + row_at(3) + SIDE + "▏".encode() + row_at(3, 40) + "▕".encode() + CLOSE + settled(row_at(5, 3) + RED + b"-")),
    # While a long reply scrolls, Claude Code keeps the user's prompt on the top row, right above the reply's text.
    ("the user's prompt kept on the top row does not hide the reply under it",
     ON_SCREEN + row_at(1) + BAND + "❯ soru".encode() + b" " * 34 + E + b"[49m" + row_at(2, 3) + b"- a" + FRAME_END,
     ON_SCREEN + row_at(1) + BAND + "❯".encode() + SENT_MARK + E + b"[39m" + b" soru" + b" " * 34 + E + b"[49m"
     + row_at(2, 3) + b"- a" + FRAME_END + OPEN + row_at(1) + SIDE + row_at(1, 40) + "▕".encode() + CLOSE
     + settled(row_at(2, 3) + RED + b"-")),
    ("text typed into the input box is not reply text",
     ON_SCREEN + row_at(3) + RULE + row_at(4) + "❯ - [ ] a".encode() + FRAME_END,
     ON_SCREEN + row_at(3) + RULE + row_at(4) + MARK + b" " + INPUT + b"- [ ] a" + FRAME_END),
    ("a heading line gives its colour back when plain text joins the row",
     PROSE + BOLD + b"Ad" + LIGHT + FRAME_END + row_at(1, 6) + b"x" + FRAME_END,
     PROSE + BOLD + b"Ad" + LIGHT + FRAME_END + settled(row_at(1, 3) + FLAME + b"Ad")
     + row_at(1, 6) + b"x" + FRAME_END + settled(row_at(1, 3) + BACK_BOLD + b"Ad")),
    ("a cell the program writes again is painted again, the others stay",
     PROSE + BOLD + b"Ad" + LIGHT + FRAME_END + row_at(1, 4) + BOLD + b"e" + LIGHT + FRAME_END,
     PROSE + BOLD + b"Ad" + LIGHT + FRAME_END + settled(row_at(1, 3) + FLAME + b"Ad")
     + row_at(1, 4) + BOLD + b"e" + LIGHT + FRAME_END + settled(row_at(1, 4) + FLAME + b"e")),
    ("a number in a heading line takes the heading's colour",
     PROSE + BOLD + b"Adim 2" + LIGHT + FRAME_END,
     PROSE + BOLD + b"Adim 2" + painted(1, 8, b"2", True) + LIGHT + FRAME_END
     + settled(row_at(1, 3) + FLAME + b"Adim" + row_at(1, 8) + b"2")),
    ("the number gets its own colour back when the heading line ends",
     PROSE + BOLD + b"Adim 2" + LIGHT + FRAME_END + row_at(1, 10) + b"x" + FRAME_END,
     PROSE + BOLD + b"Adim 2" + painted(1, 8, b"2", True) + LIGHT + FRAME_END
     + settled(row_at(1, 3) + FLAME + b"Adim" + row_at(1, 8) + b"2") + row_at(1, 10) + b"x" + FRAME_END
     + settled(row_at(1, 3) + BACK_BOLD + b"Adim" + row_at(1, 8) + E + b"[0;1;38;2;255;0;255m" + b"2")),
    ("the settling waits while the program holds a saved cursor",
     PROSE + E + b"7" + BOLD + b"Ad" + LIGHT + FRAME_END + E + b"8" + FRAME_END,
     PROSE + E + b"7" + BOLD + b"Ad" + LIGHT + FRAME_END + E + b"8" + FRAME_END + settled(row_at(1, 3) + FLAME + b"Ad")),
    ("a wide character in a heading line is written whole",
     PROSE + BOLD + "a漢b".encode() + LIGHT + FRAME_END,
     PROSE + BOLD + "a漢b".encode() + LIGHT + FRAME_END + settled(row_at(1, 3) + FLAME + "a漢b".encode())),
    ("a character with a joined mark is left as the program drew it",
     PROSE + BOLD + "éb".encode() + LIGHT + FRAME_END,
     PROSE + BOLD + "éb".encode() + LIGHT + FRAME_END + settled(row_at(1, 4) + FLAME + b"b")),
    ("turkish letters are written again as they were",
     PROSE + BOLD + "Çağrı:".encode() + LIGHT + " şu".encode() + FRAME_END,
     PROSE + BOLD + "Çağrı:".encode() + LIGHT + " şu".encode() + FRAME_END + settled(row_at(1, 3) + PINK + "Çağrı:".encode())),
    # Code in a reply (2026-11-10): the runs Claude Code draws in palette colours are read with their text.
    ("inline code is lavender, a path gold",
     PROSE + b"bak " + BLUE94 + b"kod" + PLAIN + b" ve " + BLUE94 + b"src/a.c" + PLAIN + FRAME_END,
     PROSE + b"bak " + BLUE94 + b"kod" + PLAIN + b" ve " + BLUE94 + b"src/a.c" + PLAIN + FRAME_END
     + settled(row_at(1, 7) + LAVENDER + b"kod" + row_at(1, 14) + GOLD + b"src/a.c")),
    ("what is no path stays lavender: a ratio, a date, an address, words",
     PROSE + BLUE94 + b"m/s" + PLAIN + b" " + BLUE94 + b"1/2/3" + PLAIN + b" " + BLUE94 + b"http://a.io/x.py" + PLAIN + b" "
     + BLUE94 + b"sh a.sh" + PLAIN + FRAME_END,
     PROSE + BLUE94 + b"m/s" + PLAIN + b" " + BLUE94 + b"1/2/3" + PLAIN + b" " + BLUE94 + b"http://a.io/x.py" + PLAIN + b" "
     + BLUE94 + b"sh a.sh" + PLAIN + FRAME_END
     + settled(row_at(1, 3) + LAVENDER + b"m/s" + row_at(1, 7) + b"1/2/3" + row_at(1, 13) + b"http://a.io/x.py"
               + row_at(1, 30) + b"sh" + row_at(1, 33) + b"a.sh")),
    ("the forms of a path: from the root, from home, with a place in the file, a name with a known kind",
     PROSE + BLUE94 + b"/a" + PLAIN + b" " + BLUE94 + b"~/b" + PLAIN + b" " + BLUE94 + b"a/b.c:12" + PLAIN + b" " + BLUE94
     + b"x.md" + PLAIN + b" " + BLUE94 + b"a/b/c" + PLAIN + FRAME_END,
     PROSE + BLUE94 + b"/a" + PLAIN + b" " + BLUE94 + b"~/b" + PLAIN + b" " + BLUE94 + b"a/b.c:12" + PLAIN + b" " + BLUE94
     + b"x.md" + PLAIN + b" " + BLUE94 + b"a/b/c" + PLAIN + FRAME_END
     + settled(row_at(1, 3) + GOLD + b"/a" + row_at(1, 6) + b"~/b" + row_at(1, 10) + b"a/b.c:12" + row_at(1, 19) + b"x.md"
               + row_at(1, 24) + b"a/b/c")),
    ("bold inline code keeps lavender, by a colour one step off the palette's",
     PROSE + BLUE94 + BOLD + b"kod" + LIGHT + PLAIN + FRAME_END,
     PROSE + BLUE94 + BOLD + b"kod" + LIGHT + PLAIN + FRAME_END + settled(row_at(1, 3) + E + b"[0;1;38;2;163;165;244m" + b"kod")),
    ("inline code goes back to the program's own colour when its row is no reply text any more",
     PROSE + BLUE94 + b"kod" + PLAIN + FRAME_END + row_at(1) + TOOL_MARK + FRAME_END,
     PROSE + BLUE94 + b"kod" + PLAIN + FRAME_END + settled(row_at(1, 3) + LAVENDER + b"kod") + row_at(1) + TOOL_MARK + FRAME_END
     + settled(row_at(1, 3) + E + b"[0;94m" + b"kod")),
    ("a block with no language is lavender, word by word",
     ON_SCREEN + row_at(2, 3) + BLUE94 + b"a b" + PLAIN + FRAME_END,
     ON_SCREEN + row_at(2, 3) + BLUE94 + b"a b" + PLAIN + FRAME_END + settled(row_at(2, 3) + LAVENDER + b"a" + row_at(2, 5) + b"b")),
    ("the words of a block of code, and the signs of its plain text",
     ON_SCREEN + row_at(2, 3) + E + b"[34mdef" + PLAIN + b" " + E + b"[33malan" + PLAIN + b"(r: " + E + b"[36mfloat" + PLAIN + b") "
     + E + b"[32m# not" + PLAIN + row_at(3, 3) + E + b"[34mreturn" + PLAIN + b" " + E + b"[34mNone" + PLAIN + b" " + E + b"[32m1.5"
     + PLAIN + b" " + E + b"[31m\"a\"" + PLAIN + b" " + E + b"[36mlen" + PLAIN + b"(x)" + FRAME_END,
     ON_SCREEN + row_at(2, 3) + E + b"[34mdef" + PLAIN + b" " + E + b"[33malan" + PLAIN + b"(r: " + E + b"[36mfloat" + PLAIN + b") "
     + E + b"[32m# not" + PLAIN + row_at(3, 3) + E + b"[34mreturn" + PLAIN + b" " + E + b"[34mNone" + PLAIN + b" " + E + b"[32m1.5"
     + PLAIN + b" " + E + b"[31m\"a\"" + PLAIN + b" " + E + b"[36mlen" + PLAIN + b"(x)" + FRAME_END
     + settled(row_at(2, 3) + SKY + b"def" + row_at(2, 7) + FUNCTION_C + b"alan" + ICE_C + b"(" + row_at(2, 13) + b":"
               + row_at(2, 15) + TYPE_C + b"float" + ICE_C + b")" + row_at(2, 22) + COMMENT_C + b"#" + row_at(2, 24) + b"not"
               + row_at(3, 3) + SKY + b"return" + row_at(3, 10) + ICE_C + b"None" + row_at(3, 15) + NUMBER_C + b"1.5"
               + row_at(3, 19) + STRING_C + b"\"a\"" + row_at(3, 23) + CALL_C + b"len" + ICE_C + b"(" + row_at(3, 28) + b")")),
    ("a row of signs inside a block of code is code; one that stands alone is not",
     ON_SCREEN + row_at(2, 3) + E + b"[34mif" + PLAIN + b" x {" + row_at(3, 3) + b"}" + row_at(5, 3) + b"}" + FRAME_END,
     ON_SCREEN + row_at(2, 3) + E + b"[34mif" + PLAIN + b" x {" + row_at(3, 3) + b"}" + row_at(5, 3) + b"}" + FRAME_END
     + settled(row_at(2, 3) + SKY + b"if" + row_at(2, 8) + ICE_C + b"{" + row_at(3, 3) + ICE_C + b"}")),
    ("the keyword class, the class's name, and the class it grows from",
     ON_SCREEN + row_at(2, 3) + E + b"[34mclass" + PLAIN + b" " + E + b"[34mDaire" + PLAIN + b"(" + E + b"[34mSekil" + PLAIN + b"):"
     + FRAME_END,
     ON_SCREEN + row_at(2, 3) + E + b"[34mclass" + PLAIN + b" " + E + b"[34mDaire" + PLAIN + b"(" + E + b"[34mSekil" + PLAIN + b"):"
     + FRAME_END + settled(row_at(2, 3) + SKY + b"class" + row_at(2, 9) + CLASS_C + b"Daire" + ICE_C + b"(" + CLASS_C + b"Sekil"
                           + ICE_C + b"):")),
    ("a type is dim cyan, a key ends with a colon, a decorator starts with its sign",
     ON_SCREEN + row_at(2, 3) + E + b"[36m" + E + b"[2mint" + E + b"[22m" + PLAIN + b" x" + row_at(3, 3) + E + b"[36mad:" + PLAIN
     + b" " + E + b"[31mdeneme" + PLAIN + row_at(4, 3) + E + b"[90m@veri" + PLAIN + FRAME_END,
     ON_SCREEN + row_at(2, 3) + E + b"[36m" + E + b"[2mint" + E + b"[22m" + PLAIN + b" x" + row_at(3, 3) + E + b"[36mad:" + PLAIN
     + b" " + E + b"[31mdeneme" + PLAIN + row_at(4, 3) + E + b"[90m@veri" + PLAIN + FRAME_END
     + settled(row_at(2, 3) + TYPE_C + b"int" + row_at(3, 3) + KEY_C + b"ad:" + row_at(3, 7) + STRING_C + b"deneme"
               + row_at(4, 3) + DECORATOR_C + b"@veri")),
    ("the removed and the added row of a diff block keep their colours",
     ON_SCREEN + row_at(2, 3) + E + b"[31m-eski" + PLAIN + row_at(3, 3) + E + b"[32m+yeni" + PLAIN + FRAME_END,
     ON_SCREEN + row_at(2, 3) + E + b"[31m-eski" + PLAIN + row_at(3, 3) + E + b"[32m+yeni" + PLAIN + FRAME_END),
    ("coloured words under a tool's mark stay",
     ON_SCREEN + row_at(1) + TOOL_MARK + b" x" + row_at(2, 3) + E + b"[34mif" + PLAIN + b" " + BLUE94 + b"kod" + PLAIN + FRAME_END,
     ON_SCREEN + row_at(1) + TOOL_MARK + b" x" + row_at(2, 3) + E + b"[34mif" + PLAIN + b" " + BLUE94 + b"kod" + PLAIN + FRAME_END),
    # Command lines and the input box (2026-11-10, the pool's fourth step).
    ("shell mode: the parts of the command typed after the mark",
     ON_SCREEN + row_at(3) + E + b"[91m" + "─".encode() * 40 + row_at(4) + "!\u00a0".encode() + PLAIN + b"ls -la a/b.c | wc" + FRAME_END,
     ON_SCREEN + row_at(3) + E + b"[91m" + "─".encode() * 40 + row_at(4) + "!\u00a0".encode() + PLAIN + b"ls -la a/b.c | wc" + FRAME_END
     + settled(row_at(4, 3) + TYPE_C + b"ls" + row_at(4, 6) + OPTION_C + b"-la" + row_at(4, 10) + GOLD + b"a/b.c"
               + row_at(4, 16) + E + b"[0;38;2;255;255;255m" + b"|" + row_at(4, 18) + TYPE_C + b"wc")),
    ("the dim words offered in the input box take the suggestion colour",
     BOX + "❯ ".encode() + E + b"[2mTry" + E + b"[22m" + FRAME_END,
     BOX + MARK + b" " + E + b"[2mTry" + E + b"[22m" + FRAME_END + settled(row_at(4, 3) + E + b"[0;38;2;35;135;79m" + b"Try")),
    ("the grey line of a running tool: a command after the dollar sign",
     ON_SCREEN + row_at(2, 3) + E + b"[90m" + "⎿  $ sh a.sh -x".encode() + PLAIN + FRAME_END,
     ON_SCREEN + row_at(2, 3) + E + b"[90m" + "⎿  $ sh a.sh -x".encode() + PLAIN + FRAME_END
     + settled(row_at(2, 8) + TYPE_C + b"sh" + row_at(2, 11) + GOLD + b"a.sh" + row_at(2, 16) + OPTION_C + b"-x")),
    ("the path in a tool's heading",
     ON_SCREEN + row_at(1) + TOOL_MARK + b" " + BOLD + b"Update" + LIGHT + b"(src/a.c)" + FRAME_END,
     ON_SCREEN + row_at(1) + TOOL_MARK + b" " + BOLD + b"Update" + LIGHT + b"(src/a.c)" + FRAME_END + settled(row_at(1, 10) + GOLD + b"src/a.c")),
    ("nothing on the normal screen",
     row_at(1, 3) + BOLD + b"Ad" + LIGHT + row_at(2, 3) + b"- a" + FRAME_END,
     row_at(1, 3) + BOLD + b"Ad" + LIGHT + row_at(2, 3) + b"- a" + FRAME_END),
]


# Programs other than Claude Code (2026-11-18), as Qwen Code draws: "--filter CHUNK 6 40 other" takes the stream as another
# program's, "--filter CHUNK 6 40" as Claude Code's.
BODY = E + b"[38;2;255;232;194m"            # berkeDark's text colour, as Qwen Code's theme now names it
BODY_COLON = E + b"[38:2::255:232:194m"
SOFT = E + b"[48;2;212;212;212m"            # Qwen Code's own cursor: the character on a light grey ground
NO_GROUND = E + b"[49m"
UNDERLINED = E + b"[0;4;58;2;255;158;96m"   # the underline in the cursor's colour
FRAME = ON_SCREEN + E + b"[?2026h" + E + b"[?25l"
OWN_STYLE = E + b"[0 q"                      # the terminal's own cursor style, from foot.ini
WHITE = E + b"[38;2;255;255;255m"
QWEN_SELECTION = E + b"[48;2;38;79;120m"
SELECTION = E + b"[48;2;37;34;97m"           # the ground of berkeDark's selection class
GROK_BAND = E + b"[48;2;33;33;39m"

OTHER_CASES = [
    # (name, taken as Claude Code's, given, wanted)
    ("a number in berkeDark's text colour", False, ON_SCREEN + row_at(2) + BODY + b"pil 60 s",
     ON_SCREEN + row_at(2) + BODY + b"pil 60 " + painted(2, 5, b"60") + b"s"),
    ("the text colour written with colons", False, ON_SCREEN + row_at(2) + BODY_COLON + b"pil 60 s",
     ON_SCREEN + row_at(2) + BODY_COLON + b"pil 60 " + painted(2, 5, b"60") + b"s"),
    ("a date in berkeDark's text colour", False, ON_SCREEN + row_at(2) + BODY + b"2026-11-18 ",
     ON_SCREEN + row_at(2) + BODY + b"2026-11-18 " + painted(2, 1, b"2026-11-18", colour=b"232;127;224")),
    ("a colour one step off stays a colour", False, ON_SCREEN + row_at(2) + E + b"[38;2;255;232;195mpil 60 s",
     ON_SCREEN + row_at(2) + E + b"[38;2;255;232;195mpil 60 s"),
    ("under Claude Code the text colour stays a colour", True, ON_SCREEN + row_at(2) + BODY + b"pil 60 s",
     ON_SCREEN + row_at(2) + BODY + b"pil 60 s"),
    ("a number in the text colour that grows into a word gets the text colour back", False,
     ON_SCREEN + row_at(2) + BODY + b"42 " + row_at(2, 3) + b"x" + E + b"[9G",
     ON_SCREEN + row_at(2) + BODY + b"42 " + painted(2, 1, b"42") + row_at(2, 3) + b"x" + E + b"7" + row_at(2, 1) + E
     + b"[0m" + BODY + b"4" + row_at(2, 2) + E + b"[0m" + BODY + b"2" + E + b"8" + E + b"[9G"),
    ("italic in the text colour takes the tint, and the text colour comes back", False,
     BODY + E + b"[3mx" + E + b"[23my", BODY + E + b"[3m" + ON + b"x" + E + b"[23m" + BODY + b"y"),
    ("italic in the text colour under Claude Code stays", True, BODY + E + b"[3mx" + E + b"[23my", BODY + E + b"[3mx" + E + b"[23my"),
    ("the drawn cursor loses its ground where the terminal's cursor stands", False,
     FRAME + row_at(3) + b"ab" + SOFT + b"c" + NO_GROUND + b"d" + row_at(3, 3) + E + b"[?25h" + FRAME_END,
     FRAME + row_at(3) + b"ab" + SOFT + NO_GROUND + b"c" + NO_GROUND + b"d" + row_at(3, 3) + E + b"[?25h" + FRAME_END),
    ("the drawn cursor is underlined where the terminal's cursor is elsewhere", False,
     FRAME + row_at(3) + b"ab" + SOFT + b"c" + NO_GROUND + b"d" + row_at(5, 1) + E + b"[?25h" + FRAME_END,
     FRAME + row_at(3) + b"ab" + SOFT + NO_GROUND + b"c" + NO_GROUND + b"d" + row_at(5, 1) + E + b"[?25h" + FRAME_END
     + E + b"7" + row_at(3, 3) + UNDERLINED + b"c" + E + b"8"),
    ("the drawn cursor is underlined while the terminal's cursor is hidden", False,
     FRAME + row_at(3) + b"ab" + SOFT + b"c" + NO_GROUND + row_at(3, 3) + FRAME_END,
     FRAME + row_at(3) + b"ab" + SOFT + NO_GROUND + b"c" + NO_GROUND + row_at(3, 3) + FRAME_END
     + E + b"7" + row_at(3, 3) + UNDERLINED + b"c" + E + b"8"),
    ("the drawn cursor in the text colour keeps it under the underline", False,
     FRAME + row_at(3) + BODY + b"ab" + SOFT + b"c" + NO_GROUND + row_at(5, 1) + E + b"[?25h" + FRAME_END,
     FRAME + row_at(3) + BODY + b"ab" + SOFT + NO_GROUND + b"c" + NO_GROUND + row_at(5, 1) + E + b"[?25h" + FRAME_END
     + E + b"7" + row_at(3, 3) + UNDERLINED + BODY + b"c" + E + b"8"),
    ("the drawn cursor's ground written with colons", False,
     FRAME + row_at(3) + E + b"[48:2::212:212:212mc" + NO_GROUND + row_at(3, 1) + E + b"[?25h" + FRAME_END,
     FRAME + row_at(3) + E + b"[48:2::212:212:212m" + NO_GROUND + b"c" + NO_GROUND + row_at(3, 1) + E + b"[?25h" + FRAME_END),
    ("a drawn cursor written over before the frame ends is forgotten", False,
     FRAME + row_at(3) + SOFT + b"c" + NO_GROUND + row_at(3, 1) + b"e" + row_at(5, 1) + E + b"[?25h" + FRAME_END,
     FRAME + row_at(3) + SOFT + NO_GROUND + b"c" + NO_GROUND + row_at(3, 1) + b"e" + row_at(5, 1) + E + b"[?25h" + FRAME_END),
    ("under Claude Code a light grey ground stays", True,
     FRAME + row_at(3) + SOFT + b"c" + NO_GROUND + row_at(5, 1) + E + b"[?25h" + FRAME_END,
     FRAME + row_at(3) + SOFT + b"c" + NO_GROUND + row_at(5, 1) + E + b"[?25h" + FRAME_END),
    ("on the normal screen a light grey ground stays", False, SOFT + b"c" + NO_GROUND, SOFT + b"c" + NO_GROUND),
    ("another light ground stays", False, FRAME + row_at(3) + E + b"[48;2;212;212;213mc" + NO_GROUND + FRAME_END,
     FRAME + row_at(3) + E + b"[48;2;212;212;213mc" + NO_GROUND + FRAME_END),
    ("a steady block cursor gives way to the terminal's own", False, E + b"[2 qx", E + b"[2 q" + OWN_STYLE + b"x"),
    ("a blinking block cursor gives way to the terminal's own", False, E + b"[1 q", E + b"[1 q" + OWN_STYLE),
    ("a bar and an underline stay", False, E + b"[6 q" + E + b"[4 q" + E + b"[0 q", E + b"[6 q" + E + b"[4 q" + E + b"[0 q"),
    ("a block cursor on the full screen gives way too", False, ON_SCREEN + E + b"[2 q", ON_SCREEN + E + b"[2 q" + OWN_STYLE),
    ("under Claude Code a block cursor stays", True, E + b"[2 q" + ON_SCREEN + E + b"[2 q", E + b"[2 q" + ON_SCREEN + E + b"[2 q"),
    ("a private sequence with a blank stays", False, E + b"[?2 q", E + b"[?2 q"),
    # Qwen Code's selection (OpenTUI, measured 2026-11-18): white, then the ground #264f78, then the text.
    ("Qwen Code's selection takes berkeDark's", False, FRAME + row_at(3) + WHITE + QWEN_SELECTION + b"deneme" + E + b"[0m",
     FRAME + row_at(3) + WHITE + QWEN_SELECTION + SELECTION + BODY + b"deneme" + E + b"[0m"),
    ("the selection's ground first, then white", False, QWEN_SELECTION + WHITE + b"x" + E + b"[0m",
     QWEN_SELECTION + SELECTION + WHITE + BODY + b"x" + E + b"[0m"),
    ("the selection's ground with another text colour keeps that colour", False, E + b"[31m" + QWEN_SELECTION + b"x",
     E + b"[31m" + QWEN_SELECTION + SELECTION + b"x"),
    ("white without the selection stays white", False, WHITE + b"x", WHITE + b"x"),
    ("under Claude Code the selection's ground stays", True, WHITE + QWEN_SELECTION + b"x", WHITE + QWEN_SELECTION + b"x"),
    # Grok's band behind the user's messages in its terminal theme (measured 2026-11-18 on Grok 1.0.89).
    ("Grok's band takes berkeDark's", False, ON_SCREEN + row_at(2) + GROK_BAND + b"x" + E + b"[49m",
     ON_SCREEN + row_at(2) + GROK_BAND + BAND + b"x" + E + b"[49m"),
    ("Grok's band on the normal screen too", False, GROK_BAND + b"x", GROK_BAND + BAND + b"x"),
    ("under Claude Code that ground stays", True, GROK_BAND + b"x", GROK_BAND + b"x"),
    ("a ground one step off stays", False, E + b"[48;2;33;33;40mx", E + b"[48;2;33;33;40mx"),
]


def run_other_cases():
    failures = 0
    for name, is_claude, given, wanted in OTHER_CASES:
        for chunk in ("65536", "1"):
            command = [BINARY, "--filter", chunk, "6", "40"] + ([] if is_claude else ["other"])
            got = subprocess.run(command, input=given, capture_output=True).stdout
            if got != wanted:
                failures += 1
                print(f"FAIL {name} (chunk {chunk}):\n got  {got!r}\n want {wanted!r}")
    print(f"other programs: {len(OTHER_CASES)} cases, each whole and byte by byte, {failures} failed")
    return failures



# Grok 1.0.89 in its terminal theme (2026-11-18): "--filter CHUNK 6 40 grok" takes the stream as Grok's.
GROK_FRAME = ON_SCREEN + E + b"[?2026h"
WINE = b"48;2;38;3;7"
GROK_CASES = [
    ("the user's message gets the band, with the empty rows around it",
     GROK_FRAME + row_at(3, 6) + E + b"[1m" + "❯ hi".encode() + E + b"[22m" + FRAME_END,
     GROK_FRAME + row_at(3, 6) + E + b"[1m" + "❯ hi".encode() + E + b"[22m" + FRAME_END + E + b"[?2026h" + E + b"7"
     + row_at(2, 3) + E + b"[0;" + WINE + b"m" + b" " * 36
     + row_at(3, 3) + E + b"[0;" + WINE + b"m   " + E + b"[0;1;" + WINE + b"m" + "❯".encode() + E + b"[0;" + WINE + b"m "
     + E + b"[0;1;" + WINE + b"mhi" + E + b"[0;" + WINE + b"m" + b" " * 29
     + row_at(4, 3) + E + b"[0;" + WINE + b"m" + b" " * 36 + E + b"8" + E + b"[?2026l"),
    ("code above a language row takes the pool's classes",
     GROK_FRAME + row_at(2, 3) + E + b"[38;5;5mdef" + E + b"[39m " + E + b"[38;5;4mf" + E + b"[39m" + E + b"[38;5;4m(" + E
     + b"[38;5;3mr" + E + b"[38;5;4m)" + E + b"[38;5;6m:" + E + b"[39m x = " + E + b"[38;5;1m42" + E + b"[39m" + row_at(3, 3)
     + E + b"[38;5;8m" + "◇ python".encode() + E + b"[39m" + FRAME_END,
     GROK_FRAME + row_at(2, 3) + E + b"[38;5;5mdef" + E + b"[39m " + E + b"[38;5;4mf" + E + b"[39m" + E + b"[38;5;4m(" + E
     + b"[38;5;3mr" + E + b"[38;5;4m)" + E + b"[38;5;6m:" + E + b"[39m x = " + E + b"[38;5;1m42" + E + b"[39m" + row_at(3, 3)
     + E + b"[38;5;8m" + "◇ python".encode() + E + b"[39m" + FRAME_END + E + b"[?2026h" + E + b"7" + row_at(2, 3)
     + E + b"[0;38;2;87;191;255mdef" + row_at(2, 7) + E + b"[0;38;2;63;127;255mf" + E + b"[0;38;2;178;217;255m(" + E
     + b"[0;3;38;2;255;158;96mr" + E + b"[0;38;2;178;217;255m):" + row_at(2, 17) + E + b"[0;38;2;255;0;255m42" + E + b"8"
     + E + b"[?2026l"),
    ("a diff block's marks and added text",
     GROK_FRAME + row_at(2, 3) + E + b"[38;5;6m-" + E + b"[38;5;1mold" + row_at(3, 3) + E + b"[38;5;6m+new" + row_at(4, 3) + E
     + b"[38;5;8m" + "◇ diff".encode() + E + b"[39m" + FRAME_END,
     GROK_FRAME + row_at(2, 3) + E + b"[38;5;6m-" + E + b"[38;5;1mold" + row_at(3, 3) + E + b"[38;5;6m+new" + row_at(4, 3) + E
     + b"[38;5;8m" + "◇ diff".encode() + E + b"[39m" + FRAME_END + E + b"[?2026h" + E + b"7" + row_at(2, 3)
     + E + b"[0;38;2;255;92;124m-" + row_at(3, 3) + E + b"[0;38;2;63;255;127m+" + E + b"[0;38;2;169;236;118mnew" + E + b"8"
     + E + b"[?2026l"),
    ("a bold row without the mark gets no band; it is a heading line",
     GROK_FRAME + row_at(3, 6) + E + b"[1mhi there" + E + b"[22m" + FRAME_END,
     GROK_FRAME + row_at(3, 6) + E + b"[1mhi there" + E + b"[22m" + FRAME_END + E + b"[?2026h" + E + b"7" + row_at(3, 6)
     + E + b"[0;1;38;2;255;95;7mhi" + row_at(3, 9) + b"there" + E + b"8" + E + b"[?2026l"),
    ("the input box's mark after its border gets no band",
     GROK_FRAME + row_at(3, 3) + "│ ".encode() + E + b"[1m" + "❯ hi".encode() + E + b"[22m" + FRAME_END,
     GROK_FRAME + row_at(3, 3) + "│ ".encode() + E + b"[1m" + "❯ hi".encode() + E + b"[22m" + FRAME_END),
    ("coloured rows without a language row under them stay",
     GROK_FRAME + row_at(2, 3) + E + b"[38;5;5mdef" + E + b"[39m" + FRAME_END,
     GROK_FRAME + row_at(2, 3) + E + b"[38;5;5mdef" + E + b"[39m" + FRAME_END),
]
# On 8 x 40 screens: (name, given, what berkeDark adds after the program's bytes).
GROK_CASES_8 = [
    ("a number on the band keeps its colour",
     GROK_FRAME + row_at(3, 6) + E + b"[1m" + "❯ pil 42 ".encode() + E + b"[22m" + FRAME_END,
     None),
    ("an empty line inside a block of code does not end it",
     GROK_FRAME + row_at(2, 3) + E + b"[38;5;5mdef" + E + b"[39m" + row_at(4, 3) + E + b"[38;5;1m7" + E + b"[39m" + row_at(5, 3) + E
     + b"[38;5;8m" + "◇ python".encode() + E + b"[39m" + FRAME_END,
     E + b"[?2026h" + E + b"7" + row_at(2, 3) + E + b"[0;38;2;87;191;255mdef" + row_at(4, 3) + E + b"[0;38;2;255;0;255m7" + E + b"8"
     + E + b"[?2026l"),
    ("text above an empty line is not taken into the block",
     GROK_FRAME + row_at(2, 3) + b"satir" + row_at(4, 3) + E + b"[38;5;1m7" + E + b"[39m" + row_at(5, 3) + E + b"[38;5;8m"
     + "◇ python".encode() + E + b"[39m" + FRAME_END,
     E + b"[?2026h" + E + b"7" + row_at(4, 3) + E + b"[0;38;2;255;0;255m7" + E + b"8" + E + b"[?2026l"),
    ("an edit's diff stands on the diff grounds, its numbers grey",
     GROK_FRAME + row_at(2, 3) + "◆ ".encode() + E + b"[1mEdit" + E + b"[22m " + E + b"[38;5;6mf.py" + E + b"[39m" + row_at(4, 6) + E
     + b"[38;5;1m3  a = 1" + row_at(5, 6) + E + b"[38;5;2m3  a = 2" + E + b"[39m" + FRAME_END,
     E + b"[?2026h" + E + b"7" + row_at(4, 6) + E + b"[0;38;2;110;115;128;48;2;40;26;45m3" + E + b"[0;31;48;2;40;26;45m  a = 1" + E
     + b"[0;48;2;40;26;45m" + b" " * 25 + row_at(5, 6) + E + b"[0;38;2;110;115;128;48;2;29;42;43m3" + E
     + b"[0;32;48;2;29;42;43m  a = 2" + E + b"[0;48;2;29;42;43m" + b" " * 25 + E + b"8" + E + b"[?2026l"),
    ("a list mark, a task box, inline code and a link's address",
     GROK_FRAME + row_at(2, 3) + "• ".encode() + E + b"[2m[ ]" + E + b"[22m is" + row_at(3, 3) + E + b"[1;38;5;6mrun" + E
     + b"[0m ve (https://a.org)" + FRAME_END,
     E + b"[?2026h" + E + b"7" + row_at(2, 3) + E + b"[0;38;2;255;0;0m" + "•".encode() + row_at(2, 5) + E + b"[0;2m  " + E
     + b"[0;38;2;255;158;96m" + "○".encode() + row_at(3, 3) + E + b"[0;1;38;2;163;165;244mrun" + row_at(3, 11) + E
     + b"[0;38;2;87;191;255mhttps://a.org" + E + b"8" + E + b"[?2026l"),
]
GROK_RECORDING = Path(__file__).resolve().parent / "recordings" / "grok-1.0.89-reply-90x150.bin.gz"
GROK_RECORDING_2 = Path(__file__).resolve().parent / "recordings" / "grok-1.0.89-reply2-90x150.bin.gz"
GROK_RECORDING_EDIT = Path(__file__).resolve().parent / "recordings" / "grok-1.0.89-edit-90x150.bin.gz"


def run_grok_cases():
    import gzip
    failures = 0
    for name, given, wanted in GROK_CASES:
        for chunk, mode in (("65536", "grok"), ("1", "grok")):
            got = subprocess.run([BINARY, "--filter", chunk, "6", "40", mode], input=given, capture_output=True).stdout
            if got != wanted:
                failures += 1
                print(f"FAIL {name} (chunk {chunk}):\n got  {got!r}\n want {wanted!r}")
        # Another program's stream is left as it is (save for what applies to every program, here nothing).
        other = subprocess.run([BINARY, "--filter", "65536", "6", "40", "other"], input=given, capture_output=True).stdout
        if other != given:
            failures += 1
            print(f"FAIL {name} (as another program's): {other!r}")
    for name, given, added in GROK_CASES_8:
        for chunk in ("65536", "1"):
            got = subprocess.run([BINARY, "--filter", chunk, "8", "40", "grok"], input=given, capture_output=True).stdout
            ok = got.startswith(given) and (added is None or got[len(given):] == added)
            if name.startswith("a number on the band"):
                ok = got.endswith(b"\x1b[0;1;38;2;255;0;255;48;2;38;3;7m42" + E + b"[0;48;2;38;3;7m" + b" " * 25 + row_at(4, 3) + E
                                  + b"[0;48;2;38;3;7m" + b" " * 36 + E + b"8" + E + b"[?2026l")
            if not ok:
                failures += 1
                print(f"FAIL {name} (chunk {chunk}):\n got  {got!r}")
    print(f"grok: {len(GROK_CASES) + len(GROK_CASES_8)} cases, each whole, byte by byte (and as another program's), {failures} failed")
    # A real recording: Grok's reply to the look sample, from a local stand-in endpoint.
    stream = gzip.decompress(GROK_RECORDING.read_bytes())
    whole = subprocess.run([BINARY, "--filter", "65536", "90", "150", "grok"], input=stream, capture_output=True).stdout
    bytewise = subprocess.run([BINARY, "--filter", "7", "90", "150", "grok"], input=stream, capture_output=True).stdout
    failures += check("grok recording: the same in pieces of 7 bytes", whole == bytewise)
    failures += check("grok recording: the user's message on the band", ("1;" + "48;2;38;3;7m❯").encode() in whole.replace(b"\x1b[0;", b""))
    for what, needle in (("keyword", b"38;2;87;191;255mdef"), ("number", b"38;2;255;0;255m3"), ("comment", b"3;38;2;112;112;112m"),
                         ("JSON key", b"38;2;255;191;31mad"), ("string", b"38;2;255;255;0mdeneme"), ("type", b"38;2;63;255;127mfloat"),
                         ("added mark", b"38;2;63;255;127m+"), ("removed mark", b"38;2;255;92;124m-")):
        failures += check(f"grok recording: {what} in the pool's colour", needle in whole)
    stream = gzip.decompress(GROK_RECORDING_2.read_bytes())
    whole = subprocess.run([BINARY, "--filter", "65536", "90", "150", "grok"], input=stream, capture_output=True).stdout
    bytewise = subprocess.run([BINARY, "--filter", "7", "90", "150", "grok"], input=stream, capture_output=True).stdout
    failures += check("grok recording 2: the same in pieces of 7 bytes", whole == bytewise)
    for what, needle in (("heading 1", b"1;3;4;38;2;255;255;0m"), ("heading line", b"1;38;2;255;95;7m"), ("inline code", b"1;38;2;163;165;244m"),
                         ("path", b"1;38;2;255;210;91m"), ("link words", b"4;38;2;87;191;255m"), ("link address", b"38;2;87;191;255mhttps"),
                         ("bare address", b"38;2;111;220;240mhttps"), ("quote", b"3;38;2;245;194;231m"), ("list mark", b"38;2;255;0;0m"),
                         ("open task", "38;2;255;158;96m○".encode()), ("done task", "38;2;169;236;118m✓".encode()),
                         ("comment after an empty line in a block", b"3;38;2;112;112;112m#"),
                         ("number on the band", b"38;2;255;0;255;48;2;38;3;7m2")):
        failures += check(f"grok recording 2: {what}", needle in whole)
    stream = gzip.decompress(GROK_RECORDING_EDIT.read_bytes())
    whole = subprocess.run([BINARY, "--filter", "65536", "90", "150", "grok"], input=stream, capture_output=True).stdout
    failures += check("grok edit recording: removed row on the removed ground", b"31;48;2;40;26;45m" in whole)
    failures += check("grok edit recording: added row on the added ground", b"32;48;2;29;42;43m" in whole)
    failures += check("grok edit recording: a number in the text is no line number", b"32;48;2;29;42;43m    pi = 3.141592653" in whole)
    return failures


def run_reply_cases():
    failures = 0
    for name, given, wanted in REPLY_CASES:
        for chunk in ("65536", "1"):
            got = subprocess.run([BINARY, "--filter", chunk, "8", "40"], input=given, capture_output=True).stdout
            if got != wanted:
                failures += 1
                print(f"FAIL {name} (chunk {chunk}):\n got  {got!r}\n want {wanted!r}")
    print(f"reply text: {len(REPLY_CASES)} cases, each whole and byte by byte, {failures} failed")
    return failures


def run_input_cases():
    failures = 0
    for name, given, wanted in INPUT_CASES:
        for chunk in ("65536", "1"):
            got = subprocess.run([BINARY, "--filter", chunk, "6", "40"], input=given, capture_output=True).stdout
            if got != wanted:
                failures += 1
                print(f"FAIL {name} (chunk {chunk}):\n got  {got!r}\n want {wanted!r}")
    print(f"input: {len(INPUT_CASES)} cases, each whole and byte by byte, {failures} failed")
    return failures


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
            script.write_text("#!/bin/sh\nsleep 0.4\nprintf '\\033[?1049h\\033[2;3Hprice 42 now \\033[4;1H\\033[48;2;38;3;7m❯ a"
                              "\\033[49m\\033[6;1H" + "─" * 80 + "\\033[7;1H❯ hi \\033[?2026h\\033[1;3H- madde\\033[?2026l'\n"
                              "sleep 0.2\n")
            script.chmod(0o755)
            named_pid, named_master = spawn(str(script))
            text = read_all(named_master, 2)
            finish(named_pid)
            failures += check(f"numbers in any full-screen program ({name})", b"255;0;255" in text, repr(text))
            failures += check(f"sent mark only under Claude Code ({name})", ("▶".encode() in text) == is_claude, repr(text))
            failures += check(f"typed text colour only under Claude Code ({name})", (b"63;255;127" in text) == is_claude,
                              repr(text))
            failures += check(f"reply text colours only under Claude Code ({name})", (b"0;38;2;255;0;0m-" in text) == is_claude,
                              repr(text))
            os.close(named_master)

        # Grok (2026-11-18): the band under the user's message and the code colours follow the program's name.
        for name, is_grok in (("grok", True), ("notgrok", False)):
            script = Path(folder) / name
            script.write_text("#!/bin/sh\nsleep 0.4\nprintf '\\033[?1049h\\033[?2026h\\033[3;6H\\033[1m❯ hi\\033[22m"
                              "\\033[5;3H\\033[38;5;1m42\\033[39m\\033[6;3H\\033[38;5;8m◇ python\\033[39m\\033[?2026l'\n"
                              "sleep 0.2\n")
            script.chmod(0o755)
            named_pid, named_master = spawn(str(script))
            text = read_all(named_master, 2)
            finish(named_pid)
            failures += check(f"the user's band only under Grok ({name})", (b"48;2;38;3;7m" in text) == is_grok, repr(text))
            failures += check(f"Grok's code colours only under Grok ({name})", (b"38;2;255;0;255m42" in text) == is_grok, repr(text))
            os.close(named_master)

        # Qwen Code's looks (2026-11-18): its text colour, its block cursor and its own drawn cursor.
        for name, is_claude in (("claude", True), ("qwen", False)):
            script = Path(folder) / name
            script.write_text("#!/bin/sh\nsleep 0.4\nprintf '\\033[?1049h\\033[2;3H\\033[38;2;255;232;194mpil 77 \\033[39m"
                              "\\033[2 q\\033[3;1H\\033[48;2;212;212;212mc\\033[49m'\nsleep 0.2\n")
            script.chmod(0o755)
            named_pid, named_master = spawn(str(script))
            text = read_all(named_master, 2)
            finish(named_pid)
            failures += check(f"numbers in the text colour outside Claude Code ({name})",
                              (b"0;38;2;255;0;255m77" in text) != is_claude, repr(text))
            failures += check(f"the terminal's own cursor style outside Claude Code ({name})", (E + b"[0 q" in text) != is_claude,
                              repr(text))
            failures += check(f"the drawn cursor's ground taken away outside Claude Code ({name})",
                              (b"212;212;212m" + E + b"[49mc" in text) != is_claude, repr(text))
            os.close(named_master)

    os.close(master)
    os.close(slave)

    return failures


def run_pool_check():
    """Every file that carries colours is written from the pool; none of them may be changed by hand."""
    tool = Path(__file__).resolve().parents[2] / "pool" / "tools" / "generate.py"
    if not tool.exists():
        return 0
    done = subprocess.run([sys.executable, str(tool), "check"], capture_output=True, text=True)
    return check("every colour file matches the pool", done.returncode == 0, done.stdout)


if __name__ == "__main__":
    signal.signal(signal.SIGCHLD, signal.SIG_DFL)
    failed = (run_stream_cases() + run_box_cases() + run_syntax_cases() + run_number_cases() + run_input_cases()
              + run_reply_cases() + run_other_cases() + run_grok_cases() + run_pty_cases() + run_pool_check())
    print("all passed" if failed == 0 else f"{failed} failed")
    sys.exit(1 if failed else 0)
