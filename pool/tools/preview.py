#!/usr/bin/env python3
"""Draws the berkeDark pool so that it can be judged by eye.

    preview.py catalogue           build/catalogue-*.png: every class, drawn in its own look
    preview.py sample FILE...      build/sample-NAME.png: a text marked with classes, drawn as the pool says

Sample files: one screen row per line.
    # caption                      a grey caption row
    [[class]] rest of the row      the row takes the background of that class
    {class|text}                   text drawn as that class; {a+b|text} takes the foreground of a and the background of b
    {#rrggbb;bold;italic|text}     a look written out, for showing candidates that are not in the pool
    \\{  \\}  \\|                     the characters themselves
Needs Pillow and the JetBrains Mono Nerd Font the terminal uses."""
import re
import sys
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

sys.path.insert(0, str(Path(__file__).resolve().parent))
import pool as pooltool

ROOT = pooltool.ROOT
FONT_DIR = Path.home() / ".local/share/fonts/JetBrainsMonoNerd"
SIZE = 20
FONTS = {(b, i): ImageFont.truetype(str(FONT_DIR / f"JetBrainsMonoNerdFont-{name}.ttf"), SIZE)
         for (b, i), name in {(False, False): "Regular", (True, False): "Bold", (False, True): "Italic", (True, True): "BoldItalic"}.items()}
SPARE = ImageFont.truetype("/usr/share/fonts/TTF/Iosevka.ttc", SIZE)      # for signs the main font lacks
MISSING = bytes(FONTS[(False, False)].getmask(chr(0x10FFFF)))
CW = round(FONTS[(False, False)].getlength("M"))
CH = round(SIZE * 1.5)
LABEL_BG, LABEL_FG = "#161a21", "#8f95a0"
STATE_TR = {"decided": ("karar", "#a9ec76"), "in-use": ("kullanımda", "#8f95a0"), "proposal": ("öneri", "#ff9e60"), "open": ("açık", "#ff5c7c")}


def blend(fg, bg, part):
    a = [int(fg[i:i + 2], 16) for i in (1, 3, 5)]
    b = [int(bg[i:i + 2], 16) for i in (1, 3, 5)]
    return "#%02x%02x%02x" % tuple(round(x * part + y * (1 - part)) for x, y in zip(a, b))


class Canvas:
    def __init__(self, columns, rows, background):
        self.background = background
        self.image = Image.new("RGB", (columns * CW + 24, rows * CH + 16), background)
        self.draw = ImageDraw.Draw(self.image)

    def text(self, column, row, text, fg, bg=None, style=(), row_bg=None):
        x, y = 12 + column * CW, 8 + row * CH
        behind = bg or row_bg or self.background
        if bg:
            self.draw.rectangle([x, y, x + len(text) * CW - 1, y + CH - 1], fill=bg)
        font = FONTS[("bold" in style, "italic" in style)]
        for k, ch in enumerate(text):
            if ch != " ":
                use = SPARE if bytes(FONTS[(False, False)].getmask(ch)) == MISSING else font
                self.draw.text((x + k * CW, y + 3), ch, fill=fg, font=use)
        if "underline" in style:
            self.draw.line([x, y + CH - 5, x + len(text) * CW - 1, y + CH - 5], fill=fg, width=1)
        if "strike" in style:
            self.draw.line([x, y + CH // 2, x + len(text) * CW - 1, y + CH // 2], fill=fg, width=1)
        return column + len(text)

    def band(self, row, colour, columns):
        self.draw.rectangle([0, 8 + row * CH, 24 + columns * CW, 8 + (row + 1) * CH - 1], fill=colour)


def look_of(spec, resolved, canvas_colour):
    """A class name, several joined with +, or a written-out look -> (fg, bg, style)."""
    if spec.startswith("#"):
        parts = spec.split(";")
        return parts[0], None, [p for p in parts[1:] if p]
    fg = bg = None
    style = []
    for name in spec.split("+"):
        v = resolved[name]
        if fg is None and (pooltool_roles[name].get("fg") or v["fg-from"] != "base.text" or len(spec.split("+")) == 1):
            fg, style = v["fg"], v["style"]
        if v["bg"]:
            bg = v["bg"]
    return fg or resolved["base.text"]["fg"], bg, style


def parse(line):
    """A row of the sample -> [(spec or None, text)]."""
    out, plain, i = [], "", 0
    while i < len(line):
        if line[i] == "\\" and i + 1 < len(line):
            plain += line[i + 1]; i += 2
        elif line[i] == "{":
            close = i + 1
            while close < len(line) and not (line[close] == "}" and line[close - 1] != "\\"):
                close += 1
            spec, _, text = line[i + 1:close].partition("|")
            if plain:
                out.append((None, plain)); plain = ""
            out.append((spec, re.sub(r"\\(.)", r"\1", text)))
            i = close + 1
        else:
            plain += line[i]; i += 1
    if plain:
        out.append((None, plain))
    return out


def draw_sample(path, resolved, canvas_colour):
    rows = Path(path).read_text(encoding="utf-8").split("\n")
    while rows and not rows[-1].strip():
        rows.pop()
    parsed = []
    for line in rows:
        if line.startswith("# "):
            parsed.append(("caption", line[2:], None)); continue
        m = re.match(r"\[\[([a-z0-9.\-]+)\]\]", line)
        row_class = m.group(1) if m else None
        parsed.append(("row", parse(line[m.end():] if m else line), row_class))
    width = max([sum(len(t) for _, t in segs) for kind, segs, _ in parsed if kind == "row"] + [len(s) for kind, s, _ in parsed if kind == "caption"] + [40]) + 2
    c = Canvas(width, len(parsed), canvas_colour)
    text_colour = resolved["base.text"]["fg"]
    for r, (kind, content, row_class) in enumerate(parsed):
        if kind == "caption":
            c.band(r, LABEL_BG, width)
            c.text(0, r, content, LABEL_FG)
            continue
        row_bg = resolved[row_class]["bg"] if row_class else None
        if row_bg:
            c.band(r, row_bg, width)
        column = 0
        for spec, text in content:
            if spec is None:
                column = c.text(column, r, text, text_colour, row_bg=row_bg)
            else:
                fg, bg, style = look_of(spec, resolved, canvas_colour)
                column = c.text(column, r, text, fg, bg, style, row_bg=row_bg)
    target = ROOT / "build" / f"sample-{Path(path).stem}.png"
    c.image.save(target)
    print(target.relative_to(ROOT), c.image.size)


GROUPS = [("1-base-ansi", ("base", "ansi")), ("2-prose", ("prose",)), ("3-command-shell", ("command", "shell")), ("4-code", ("code",)),
          ("5-diff-vcs-status", ("diff", "vcs", "status")), ("6-chat", ("chat",)), ("7-ui", ("ui",))]


def draw_catalogue(pool, resolved, canvas_colour):
    palette, roles = pool["palette"], pool["roles"]
    text_colour = resolved["base.text"]["fg"]
    for name, domains in GROUPS:
        names = [n for n in roles if n.split(".")[0] in domains]
        c = Canvas(118, len(names) + 1, canvas_colour)
        c.band(0, LABEL_BG, 118)
        c.text(0, 0, "durum       sınıf" + " " * 29 + "görünüş" + " " * 27 + "renk", LABEL_FG)
        for r, n in enumerate(names, start=1):
            role, v = roles[n], resolved[n]
            label, colour = STATE_TR[role["state"]]
            c.text(0, r, label, colour)
            c.text(12, r, n, LABEL_FG)
            sample = role["tr"][:32]
            own = role.get("fg") or role.get("bg")
            if role.get("bg") and not role.get("fg"):
                c.text(46, r, " " + sample + " ", text_colour, bg=v["bg"])
            else:
                c.text(46, r, sample, v["fg"], v["bg"], v["style"])
            where = (role.get("fg") or "") + (" / " + role["bg"] if role.get("bg") and role.get("fg") else role.get("bg", "") if not role.get("fg") else "")
            hexa = palette[own]["hex"] if own else ""
            c.text(80, r, (where + " " + hexa) if own else "→ " + role.get("fallback", ""), LABEL_FG if own else "#585c66")
        target = ROOT / "build" / f"catalogue-{name}.png"
        c.image.save(target)
        print(target.relative_to(ROOT), c.image.size)


if __name__ == "__main__":
    pool = pooltool.load()
    pooltool_roles = pool["roles"]
    resolved, canvas_colour = pooltool.resolve(pool)
    (ROOT / "build").mkdir(exist_ok=True)
    if len(sys.argv) > 1 and sys.argv[1] == "catalogue":
        draw_catalogue(pool, resolved, canvas_colour)
    elif len(sys.argv) > 2 and sys.argv[1] == "sample":
        for path in sys.argv[2:]:
            draw_sample(path, resolved, canvas_colour)
    else:
        print(__doc__)
        sys.exit(2)
