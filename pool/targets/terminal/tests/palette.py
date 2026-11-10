#!/usr/bin/env python3
"""Prints berkeDark's colours as swatches, each with its hex value."""

BLOCK = "████"
WIDTH = 31
BACKGROUND = "@{=navy}"


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
row(swatch("@{=navy}", "zemin"), swatch("@{=cream}", "yazı"), swatch("@{=orange}", "imleç"))

heading("Terminal: 16 renk paleti")
names = [
    (0, "@{ansi.black}", "0 zeminle aynı"), (8, "@{ansi.bright-black}", "8 parlak siyah"),
    (1, "@{ansi.red}", "1 kırmızı"), (9, "@{ansi.bright-red}", "9 parlak kırmızı"),
    (2, "@{ansi.green}", "2 yeşil"), (10, "@{ansi.bright-green}", "10 parlak yeşil"),
    (3, "@{ansi.yellow}", "3 sarı"), (11, "@{ansi.bright-yellow}", "11 parlak sarı"),
    (4, "@{ansi.blue}", "4 mavi"), (12, "@{ansi.bright-blue}", "12 parlak mavi"),
    (5, "@{ansi.magenta}", "5 eflatun"), (13, "@{ansi.bright-magenta}", "13 parlak eflatun"),
    (6, "@{ansi.cyan}", "6 camgöbeği"), (14, "@{ansi.bright-cyan}", "14 parlak camgöbeği"),
    (7, "@{ansi.white}", "7 krem"), (15, "@{ansi.bright-white}", "15 beyaz"),
]
for first in range(0, len(names), 2):
    row(palette(*names[first]), palette(*names[first + 1]))

heading("Terminal: yazı biçimleri")
row(styled("0", "Düz yazı", "@{=cream}"), styled("1", "Kalın yazı", "@{=white}"), styled("3", "Eğik yazı", "@{=gold}"))
row(styled("1;3", "Kalın ve eğik", "@{=lemon}"), styled("9", "Üstü çizili", "@{=parchment}"), styled("1;3;4", "En büyük başlık", "@{=yellow}"))
row(styled("35", "Satır içi kod", "@{=lavender}"))

heading("VS Code: sözdizimi")
row(swatch("@{=sky}", "anahtar kelime"), swatch("@{=yellow}", "dizge"), swatch("@{=blue}", "fonksiyon"))
row(swatch("@{=moss}", "sınıf ve tür"), swatch("@{=magenta}", "sayı"), swatch("@{=orange}", "parametre"))
row(swatch("@{=grey-comment}", "açıklama"), swatch("@{=sand}", "değişken"), swatch("@{=cream}", "yazı"))

heading("Tarayıcı")
row(swatch("@{=navy}", "çerçeve"), swatch("@{=turquoise}", "simgeler"), swatch("e8f4fc", "araç çubuğu yazısı"))
