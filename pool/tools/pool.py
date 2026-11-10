#!/usr/bin/env python3
"""Reads the berkeDark pool: checks it, resolves every class to plain values, lists what awaits a decision.

    pool.py check      faults in the file, then what a reader should know (same looks, low contrast)
    pool.py resolve    writes build/pool.resolved.json: class -> fg, bg, style as plain values
    pool.py open       the classes that await the author: open ones first, then proposals
    pool.py list [domain]
    pool.py format     writes pool.json again in its fixed layout, after an edit

Only the standard library is used."""
import json
import sys
from collections import Counter, defaultdict
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
STATES = ("decided", "in-use", "proposal", "open")
STYLES = ("bold", "italic", "underline", "strike")


def load():
    return json.loads((ROOT / "pool.json").read_text(encoding="utf-8"))


def save(pool):
    """Writes pool.json in its fixed layout: one colour and one class per line, a blank line between domains."""
    def line(key, value):
        return "    " + json.dumps(key, ensure_ascii=False) + ": " + json.dumps(value, ensure_ascii=False)

    out = ["{"]
    for key in ("name", "version", "state", "updated"):
        out.append("  " + json.dumps(key) + ": " + json.dumps(pool[key], ensure_ascii=False) + ",")
    out.append('  "principles": [')
    out.append(",\n".join("    " + json.dumps(p, ensure_ascii=False) for p in pool["principles"]))
    out.append("  ],")
    out.append('  "reading": ' + json.dumps(pool["reading"], ensure_ascii=False, indent=4).replace("\n", "\n  ") + ",")
    out.append('  "palette": {')
    out.append(",\n".join(line(k, v) for k, v in pool["palette"].items()))
    out.append("  },")
    out.append('  "roles": {')
    rows, last = [], None
    for k, v in pool["roles"].items():
        domain = k.split(".")[0]
        rows.append(("\n" if last and domain != last else "") + line(k, v))
        last = domain
    out.append(",\n".join(rows))
    out.append("  },")
    out.append('  "media": {')
    out.append(",\n".join(line(k, v) for k, v in pool["media"].items()))
    out.append("  },")
    out.append('  "kept": ' + json.dumps(pool["kept"], ensure_ascii=False, indent=4).replace("\n", "\n  "))
    out.append("}")
    (ROOT / "pool.json").write_text("\n".join(out) + "\n", encoding="utf-8")


def luminance(hexa):
    def channel(v):
        v /= 255
        return v / 12.92 if v <= 0.03928 else ((v + 0.055) / 1.055) ** 2.4
    r, g, b = (int(hexa[i:i + 2], 16) for i in (1, 3, 5))
    return 0.2126 * channel(r) + 0.7152 * channel(g) + 0.0722 * channel(b)


def contrast(a, b):
    la, lb = luminance(a), luminance(b)
    return (max(la, lb) + 0.05) / (min(la, lb) + 0.05)


def chain(roles, name):
    """The class and the classes it falls back to, nearest first."""
    seen = []
    while name and name not in seen:
        seen.append(name)
        name = roles[name].get("fallback")
    return seen


def resolve(pool):
    """Every class with plain values; a missing fg or bg comes from the fallback chain."""
    palette, roles = pool["palette"], pool["roles"]
    canvas = palette[roles["base.background"]["bg"]]["hex"]
    text = palette[roles["base.text"]["fg"]]["hex"]
    out = {}
    for name in roles:
        fg = bg = None
        fg_from = bg_from = None
        for step in chain(roles, name):
            r = roles[step]
            if fg is None and r.get("fg"):
                fg, fg_from = palette[r["fg"]]["hex"], step
            if bg is None and r.get("bg"):
                bg, bg_from = palette[r["bg"]]["hex"], step
        own = roles[name]
        out[name] = {"fg": fg or text, "bg": bg, "style": own.get("style", []), "state": own["state"],
                     "fg-from": fg_from or "base.text", "colour": own.get("fg") or own.get("bg")}
        if bg_from and bg_from != name:
            out[name]["bg-from"] = bg_from
    return out, canvas


def faults(pool):
    palette, roles = pool["palette"], pool["roles"]
    found = []
    for name, c in palette.items():
        h = c.get("hex", "")
        if len(h) != 7 or h[0] != "#" or any(x not in "0123456789abcdef" for x in h[1:]):
            found.append(f"palette {name}: {h!r} is not a colour like #rrggbb in small letters")
    for name, r in roles.items():
        for key in ("fg", "bg"):
            if r.get(key) and r[key] not in palette:
                found.append(f"{name}: {key} names {r[key]!r}, which is not in the palette")
        if r.get("state") not in STATES:
            found.append(f"{name}: state {r.get('state')!r}")
        for s in r.get("style", []):
            if s not in STYLES:
                found.append(f"{name}: style {s!r}")
        if r.get("fallback") and r["fallback"] not in roles:
            found.append(f"{name}: falls back to {r['fallback']!r}, which does not exist")
        elif r.get("fallback") and name in chain(roles, r["fallback"]):
            found.append(f"{name}: its fallback chain comes back to itself")
        for a in r.get("alt", []):
            if a.get("fg") and a["fg"] not in palette:
                found.append(f"{name}: an alt names {a['fg']!r}, which is not in the palette")
        if not r.get("tr") or not r.get("what") or "source" not in r:
            found.append(f"{name}: tr, what and source are all needed")
    used = {r.get(k) for r in roles.values() for k in ("fg", "bg")} | {a.get("fg") for r in roles.values() for a in r.get("alt", [])}
    for section in pool.get("kept", {}).values():
        if isinstance(section, dict):
            used |= {value.partition("/")[0] for value in section.values()}
    alt_hexes = {a.get("hex") for r in roles.values() for a in r.get("alt", [])}
    for name in palette:
        if name not in used and palette[name]["hex"] not in alt_hexes:
            found.append(f"palette {name}: no class uses it")
    return found


def same_looks(pool):
    """Within one domain, classes that would be drawn exactly alike though neither falls back to the other."""
    resolved, _ = resolve(pool)
    roles = pool["roles"]
    groups = defaultdict(list)
    for name, v in resolved.items():
        if roles[name].get("fg") or roles[name].get("bg"):
            groups[(name.split(".")[0], v["fg"], v["bg"], tuple(sorted(v["style"])))].append(name)
    out = []
    for (domain, fg, bg, style), names in sorted(groups.items(), key=lambda item: (item[0][0], item[0][1], item[0][2] or "", item[0][3])):
        apart = [n for n in names if not any(o != n and o in chain(roles, n) for o in names)]
        if len(apart) > 1:
            out.append((domain, fg, style, names))
    return out


def command_check(pool):
    found = faults(pool)
    for f in found:
        print("FAULT", f)
    roles = pool["roles"]
    resolved, canvas = resolve(pool)
    states = Counter(r["state"] for r in roles.values())
    print(f"{len(pool['palette'])} colours, {len(roles)} classes: " + ", ".join(f"{states[s]} {s}" for s in STATES))
    print("\nDrawn alike inside one domain (to be judged: wanted, or two things that should differ):")
    for domain, fg, style, names in same_looks(pool):
        print(f"  {fg} {'+'.join(style) or 'plain':22} {', '.join(names)}")
    print("\nHard to read on the canvas (contrast under 3 to 1):")
    for name, v in resolved.items():
        if roles[name].get("fg") and not v["bg"] and contrast(v["fg"], canvas) < 3:
            print(f"  {name:28} {v['fg']}  {contrast(v['fg'], canvas):.1f}")
    return 1 if found else 0


def command_resolve(pool):
    resolved, canvas = resolve(pool)
    target = ROOT / "build" / "pool.resolved.json"
    target.parent.mkdir(exist_ok=True)
    lines = ",\n".join("  " + json.dumps(k) + ": " + json.dumps(v, ensure_ascii=False) for k, v in resolved.items())
    target.write_text("{\n" + lines + "\n}\n", encoding="utf-8")
    print(f"{target.relative_to(ROOT)}: {len(resolved)} classes")
    return 0


def command_open(pool):
    palette, roles = pool["palette"], pool["roles"]
    for state in ("open", "proposal"):
        print(f"== {state}")
        for name, r in roles.items():
            if r["state"] != state:
                continue
            look = (r.get("fg") or "") + ("/" + r["bg"] if r.get("bg") else "") + (" " + "+".join(r["style"]) if r.get("style") else "")
            alts = "; ".join((a.get("fg") or a.get("hex", "")) + " (" + a["from"] + ")" for a in r.get("alt", []))
            print(f"  {name:30} {look:28} {r['source']}" + (f"\n{'':33}or: {alts}" if alts else ""))
    return 0


def command_list(pool, domain=None):
    resolved, _ = resolve(pool)
    for name, v in resolved.items():
        if domain and not name.startswith(domain + "."):
            continue
        r = pool["roles"][name]
        print(f"{name:32} {v['fg']} {(v['bg'] or '-'):8} {'+'.join(v['style']) or '-':22} {r['state']:9} {r['tr']}")
    return 0


if __name__ == "__main__":
    action = sys.argv[1] if len(sys.argv) > 1 else "check"
    pool = load()
    if action == "check":
        sys.exit(command_check(pool))
    if action == "resolve":
        sys.exit(command_resolve(pool))
    if action == "open":
        sys.exit(command_open(pool))
    if action == "format":
        save(pool)
        print("pool.json written in its fixed layout")
        sys.exit(0)
    if action == "list":
        sys.exit(command_list(pool, sys.argv[2] if len(sys.argv) > 2 else None))
    print(__doc__)
    sys.exit(2)
