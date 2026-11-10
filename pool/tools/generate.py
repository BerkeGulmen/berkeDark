#!/usr/bin/env python3
"""Writes every file that carries berkeDark's colours from the pool, the single source.

    generate.py            writes the targets
    generate.py check      writes nothing; says which targets differ from what the pool gives
    generate.py where X    lists the places in the templates that name the class or colour X

A template (pool/targets/...) is the target file with a reference in place of each colour; the target
has the template's path under the repository's root. References:

    @{prose.label}             the class's text colour
    @{bg:diff.added}           the class's ground
    @{=sky}                    a colour of the palette, by name
    @{kept:browser:frame}      a value of the pool's kept section
    @{mark:prose.task.open}    the class's mark

A form may follow a bar: # (#rrggbb, with alpha when the value has one), hex (rrggbb), rgb (r;g;b),
csv (r, g, b), rgb- (r;g;b one step lower in blue: no palette colour, and it looks the same) and
c (a mark as C escapes). Without a form the target's usual one is used (FORMS below).

A kept value is a palette name, name/alpha (two hex digits), role:CLASS, bg:CLASS or a number (#rrggbb).

Only the standard library is used."""
import json
import re
import sys
from pathlib import Path

POOL = Path(__file__).resolve().parent.parent
ROOT = POOL.parent
TARGETS = POOL / "targets"
FORMS = {"colours.h": "rgb", "foot.ini": "hex", "palette.py": "hex"}
REFERENCE = re.compile(r"@\{([^}|]+)(?:\|([^}]+))?\}")


def load():
    return json.loads((POOL / "pool.json").read_text(encoding="utf-8"))


def kept_value(pool, value):
    if value.startswith("#"):
        return value.lower()
    if value.startswith("role:"):
        return colour(pool, value[5:])
    if value.startswith("bg:"):
        return colour(pool, value)
    name, _, alpha = value.partition("/")
    return pool["palette"][name]["hex"].lower() + alpha


def colour(pool, reference):
    """The colour a reference names, as #rrggbb or #rrggbbaa."""
    if reference.startswith("="):
        return pool["palette"][reference[1:]]["hex"].lower()
    if reference.startswith("kept:"):
        _, section, key = reference.split(":", 2)
        return kept_value(pool, pool["kept"][section][key])
    side = "fg"
    if reference.startswith("bg:"):
        side, reference = "bg", reference[3:]
    name = pool["roles"][reference].get(side)
    if name is None:
        raise KeyError(f"class {reference} has no {side}")
    return pool["palette"][name]["hex"].lower()


def formed(value, form):
    red, green, blue = (int(value[i:i + 2], 16) for i in (1, 3, 5))
    if form == "#":
        return value
    if form == "hex":
        return value[1:7]
    if form == "rgb":
        return f"{red};{green};{blue}"
    if form == "csv":
        return f"{red}, {green}, {blue}"
    if form == "rgb-":
        return f"{red};{green};{blue - 1 if blue > 0 else blue + 1}"
    raise KeyError(f"unknown form {form}")


def resolve(pool, reference, form):
    if reference.startswith("mark:"):
        mark = pool["roles"][reference[5:]]["mark"]
        return "".join(ch if ord(ch) < 0x80 else "\\u%04x" % ord(ch) for ch in mark) if form == "c" else mark
    return formed(colour(pool, reference), form)


def templates():
    return sorted(p for p in TARGETS.rglob("*") if p.is_file())


def render(pool, template):
    usual = FORMS.get(template.name, "#")
    text = template.read_text(encoding="utf-8")
    return REFERENCE.sub(lambda m: resolve(pool, m.group(1), m.group(2) or ("c" if m.group(1).startswith("mark:") else usual)), text)


def main():
    command = sys.argv[1] if len(sys.argv) > 1 else "write"
    pool = load()
    if command == "where":
        wanted = sys.argv[2]
        names = {wanted} | {k for k, v in pool["roles"].items() if wanted in (v.get("fg"), v.get("bg"))}
        for template in templates():
            for number, line in enumerate(template.read_text(encoding="utf-8").splitlines(), 1):
                for match in REFERENCE.finditer(line):
                    reference = match.group(1).removeprefix("bg:").removeprefix("mark:").removeprefix("=")
                    kept = pool["kept"]
                    if reference in names or (reference.startswith("kept:") and wanted in str(
                            kept[reference.split(":")[1]][reference.split(":", 2)[2]])):
                        print(f"{template.relative_to(TARGETS)}:{number}: {line.strip()[:110]}")
        return 0
    different = 0
    for template in templates():
        target = ROOT / template.relative_to(TARGETS)
        text = render(pool, template)
        old = target.read_text(encoding="utf-8") if target.exists() else None
        if text == old:
            print(f"same     {target.relative_to(ROOT)}")
            continue
        different += 1
        if command == "check":
            print(f"DIFFERS  {target.relative_to(ROOT)}")
            for new_line, old_line in zip(text.splitlines(), (old or "").splitlines()):
                if new_line != old_line:
                    print(f"           pool: {new_line.strip()[:100]}\n           file: {old_line.strip()[:100]}")
                    break
        else:
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text(text, encoding="utf-8")
            print(f"written  {target.relative_to(ROOT)}")
    return 1 if command == "check" and different else 0


if __name__ == "__main__":
    sys.exit(main())
