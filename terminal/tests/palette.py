#!/usr/bin/env python3
"""Prints berkeDark's colours as swatches, each with its hex value."""

BLOCK = "████"
WIDTH = 31
BACKGROUND = "070f1f"


def rgb(value, text):
    red, green, blue = bytes.fromhex(value)
    return f"\033[38;2;{red};{green};{blue}m{text}\033[39m"


def swatch(value, name):
    # A colour equal to the background would hide its own label.
    if value == BACKGROUND:
        return f"{BLOCK} {value} {name}".ljust(WIDTH)
    return rgb(value, f"{BLOCK} {value} {name}".ljust(WIDTH))


def palette(index, value, name):
    code = 30 + index if index < 8 else 90 + index - 8
    if value == BACKGROUND:
        return f"{BLOCK} {value} {name}".ljust(WIDTH)
    return f"\033[{code}m" + f"{BLOCK} {value} {name}".ljust(WIDTH) + "\033[39m"


def styled(marks, text, value):
    return f"\033[{marks}m{text}\033[0m" + " " * (WIDTH - len(text) - 8) + rgb(value, f"{BLOCK} ") + " "


def heading(text):
    print(f"\n \033[1m{text}\033[22m")


def row(*cells):
    print(" " + "".join(cells))


print(" \033[1;4mberkeDark renk paleti\033[0m")

heading("Terminal: zemin ve yazı")
row(swatch("070f1f", "zemin"), swatch("ffe8c2", "yazı"), swatch("ff9e60", "imleç"))

heading("Terminal: 16 renk paleti")
names = [
    (0, "070f1f", "0 zeminle aynı"), (8, "6e7380", "8 parlak siyah"),
    (1, "ff4455", "1 kırmızı"), (9, "ff9e60", "9 parlak kırmızı"),
    (2, "a9ec76", "2 yeşil"), (10, "3fff7f", "10 parlak yeşil"),
    (3, "ffd25c", "3 sarı"), (11, "ffff00", "11 parlak sarı"),
    (4, "3f7fff", "4 mavi"), (12, "57bfff", "12 parlak mavi"),
    (5, "a3a5f5", "5 eflatun"), (13, "ff00ff", "13 parlak eflatun"),
    (6, "12ab7f", "6 camgöbeği"), (14, "6fdcf0", "14 parlak camgöbeği"),
    (7, "ffe8c2", "7 krem"), (15, "fafcff", "15 beyaz"),
]
for first in range(0, len(names), 2):
    row(palette(*names[first]), palette(*names[first + 1]))

heading("Terminal: yazı biçimleri")
row(styled("0", "Düz yazı", "ffe8c2"), styled("1", "Kalın yazı", "fafcff"), styled("3", "Eğik yazı", "ffd25c"))
row(styled("1;3", "Kalın ve eğik", "fff9c4"), styled("9", "Üstü çizili", "f5ebd0"), styled("1;3;4", "En büyük başlık", "ffff00"))
row(styled("35", "Satır içi kod", "a3a5f5"))

heading("VS Code: sözdizimi")
row(swatch("57bfff", "anahtar kelime"), swatch("ffff00", "dizge"), swatch("3f7fff", "fonksiyon"))
row(swatch("57ab64", "sınıf ve tür"), swatch("ff00ff", "sayı"), swatch("ff9e60", "parametre"))
row(swatch("707070", "açıklama"), swatch("ffdf9f", "değişken"), swatch("ffe8c2", "yazı"))

heading("Tarayıcı")
row(swatch("070f1f", "çerçeve"), swatch("6fdcf0", "simgeler"), swatch("e8f4fc", "araç çubuğu yazısı"))
