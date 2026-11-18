#!/usr/bin/env python3
"""Makes ```diff blocks readable in Qwen Code with a custom theme such as berkeDark.

Qwen Code's createCustomTheme draws the added and removed rows of a ```diff block as full-width
grounds in the theme's AccentGreen and AccentRed:

    "hljs-addition": {backgroundColor: colors.AccentGreen, display: "inline-block", width: "100%"}

while every built-in theme (Qwen Dark, Dracula, Ayu, Atom One) draws them as text colours:

    "hljs-addition": {color: qwenDarkColors.AccentGreen}

With berkeDark the cream text then stands on bright green (1.18:1) and rose (2.48:1); readable text needs
4.5:1. The theme cannot fix it: AccentGreen and AccentRed are also the colours of success and error
messages. This script changes those two mappings in Qwen Code's installed files to the built-in form,
so the same colours become text colours (13.62:1 and 6.45:1 on berkeDark's ground).

It touches nothing but the one chunk file of Qwen Code that holds both mappings, in each installed
copy of Qwen Code it finds. Before a change it keeps the file as it was beside it
(<file>.berkeDark-backup). The change is written to a new file that replaces the old one only after it
was checked: both old strings gone, the new form present, and `node --check` passing. When the old
strings are not found exactly once each (Qwen Code rewrote that code), nothing is changed.

Qwen Code replaces its files when it updates itself, so run this again after every update.

    python3 reapply-diff-color-patch.py --dry-run     say what would change; change nothing
    python3 reapply-diff-color-patch.py               change it (restart Qwen Code afterwards)
    python3 reapply-diff-color-patch.py --restore     put back the files as they were
    python3 reapply-diff-color-patch.py --root DIR    only the Qwen Code copy in DIR (the folder with package.json)

Where it looks: first the copy the `qwen` command on PATH runs (its links followed up to the folder
with Qwen Code's package.json), then the global npm folder (`npm root -g`), ~/.local/lib/node_modules,
Bun's global folder (~/.bun/install/global, or under $BUN_INSTALL), pnpm's (`pnpm root -g`, $PNPM_HOME,
~/.local/share/pnpm), Volta's (~/.volta, or $VOLTA_HOME), and the copies Qwen Code's self-update keeps in
~/.qwen/updates/npm/*/versions/* (or under $QWEN_HOME). A copy it cannot read or write is reported and
the others are still done. Only the standard library and `node` are used.
"""

from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

PAIRS = [
    (b'"hljs-addition":{backgroundColor:colors.AccentGreen,display:"inline-block",width:"100%"}',
     b'"hljs-addition":{color:colors.AccentGreen}'),
    (b'"hljs-deletion":{backgroundColor:colors.AccentRed,display:"inline-block",width:"100%"}',
     b'"hljs-deletion":{color:colors.AccentRed}'),
]
BACKUP_SUFFIX = ".berkeDark-backup"
PACKAGE = Path("@qwen-code") / "qwen-code"


def is_qwen_package(folder: Path) -> bool:
    try:
        return json.loads((folder / "package.json").read_text(encoding="utf-8")).get("name") == "@qwen-code/qwen-code"
    except (OSError, ValueError, AttributeError):
        return False


def running_tree() -> Path | None:
    """The copy the `qwen` command on PATH runs: its links followed, then up to Qwen Code's package.json."""
    command = shutil.which("qwen")
    if command is None:
        return None
    try:
        folder = Path(command).resolve().parent
    except OSError:
        return None
    for candidate in (folder, *folder.parents):
        if is_qwen_package(candidate):
            return candidate
    return None


def global_root(tool: str) -> list[Path]:
    """The global package folder a package manager names (npm root -g, pnpm root -g)."""
    if shutil.which(tool) is None:
        return []
    try:
        done = subprocess.run([tool, "root", "-g"], capture_output=True, text=True, timeout=20)
    except (OSError, subprocess.SubprocessError):
        return []
    return [Path(done.stdout.strip()) / PACKAGE] if done.returncode == 0 and done.stdout.strip() else []


def find_trees(root: str | None) -> tuple[list[Path], Path | None]:
    """The installed copies of Qwen Code (folders with Qwen Code's package.json and a chunks folder), the one
    the `qwen` command runs first; and that one."""
    running = None
    if root:
        candidates = [Path(root).expanduser()]
    else:
        home = Path.home()
        running = running_tree()
        candidates = ([running] if running else []) + [home / ".local/lib/node_modules" / PACKAGE]
        candidates += global_root("npm")
        bun = Path(os.environ.get("BUN_INSTALL", "") or home / ".bun").expanduser()
        candidates.append(bun / "install/global/node_modules" / PACKAGE)
        candidates += global_root("pnpm")
        for pnpm in filter(None, [os.environ.get("PNPM_HOME"), str(home / ".local/share/pnpm")]):
            candidates += sorted(Path(pnpm).expanduser().glob("global/*/node_modules/" + str(PACKAGE)))
        volta = Path(os.environ.get("VOLTA_HOME", "") or home / ".volta").expanduser() / "tools/image/packages" / PACKAGE
        candidates += [volta / "lib/node_modules" / PACKAGE, volta / "node_modules" / PACKAGE]
        qwen_home = Path(os.environ.get("QWEN_HOME", "") or home / ".qwen").expanduser()
        candidates += sorted((qwen_home / "updates/npm").glob("*/versions/*/node_modules/" + str(PACKAGE)))
    trees, seen = [], set()
    for path in candidates:
        try:
            real = path.resolve()
            ok = is_qwen_package(real) and (real / "chunks").is_dir()
        except OSError:
            continue
        if ok and real not in seen:
            seen.add(real)
            trees.append(real)
    return trees, running


def version_of(tree: Path) -> str:
    try:
        return json.loads((tree / "package.json").read_text(encoding="utf-8"))["version"]
    except (OSError, ValueError, KeyError):
        return "unknown"


def is_original(data: bytes) -> bool:
    return all(data.count(old) == 1 for old, _ in PAIRS)


def is_changed(data: bytes) -> bool:
    return all(data.count(new) == 1 for _, new in PAIRS) and not any(old in data for old, _ in PAIRS)


def locate(tree: Path) -> tuple[Path | None, str]:
    """The chunk file and its state: original, changed, unclear (the strings are there, not once each) or absent."""
    unclear = None
    for path in sorted((tree / "chunks").glob("*.js")):
        try:
            data = path.read_bytes()
        except OSError:
            continue
        if b"hljs-addition" not in data or b"colors.AccentGreen" not in data:
            continue
        if is_original(data):
            return path, "original"
        if is_changed(data):
            return path, "changed"
        if any(old in data for old, _ in PAIRS):
            unclear = path
    return (unclear, "unclear") if unclear else (None, "absent")


def node_check(path: Path) -> tuple[bool, str]:
    try:
        done = subprocess.run(["node", "--check", str(path)], capture_output=True, text=True, timeout=120)
    except (OSError, subprocess.SubprocessError) as error:
        return False, str(error)
    return done.returncode == 0, done.stderr.strip()[:300]


def write_checked(path: Path, data: bytes, check) -> tuple[bool, str]:
    """Writes data to a new file beside path; it replaces path only when check(new file, data) passes."""
    handle, temporary = tempfile.mkstemp(prefix=path.name + ".", suffix=".tmp.js", dir=path.parent)
    try:
        with os.fdopen(handle, "wb") as out:
            out.write(data)
        shutil.copymode(path, temporary)
        ok, why = check(Path(temporary), data)
        if not ok:
            return False, why
        os.replace(temporary, path)
        return True, ""
    finally:
        if os.path.exists(temporary):
            os.remove(temporary)


def patch(tree: Path, dry: bool) -> str:
    path, state = locate(tree)
    if state == "absent":
        print("    no file with the diff mappings found; nothing changed")
        return "not-found"
    if state == "unclear":
        print(f"    {path.name}: the mappings are not in the expected form; nothing changed")
        return "error"
    if state == "changed":
        print(f"    {path.name}: already changed")
        return "done-before"

    data = path.read_bytes()
    if dry:
        for old, new in PAIRS:
            print(f"    - {old.decode()}\n    + {new.decode()}")
        print(f"    would change {path.name}")
        return "would-change"

    backup = path.with_name(path.name + BACKUP_SUFFIX)
    made_backup = not backup.exists()
    if made_backup:
        shutil.copy2(path, backup)
    if backup.read_bytes() != data:
        print(f"    {backup.name} differs from the file as it is now; nothing changed", file=sys.stderr)
        return "error"

    out = data
    for old, new in PAIRS:
        out = out.replace(old, new)

    def check(written: Path, expected: bytes) -> tuple[bool, str]:
        if written.read_bytes() != expected or not is_changed(expected):
            return False, "the written file is not what was meant"
        return node_check(written)

    try:
        ok, why = write_checked(path, out, check)
    except OSError:
        if made_backup:
            backup.unlink(missing_ok=True)
        raise
    if not ok:
        if made_backup:  # the file stays as it was, so the copy made for it goes
            backup.unlink(missing_ok=True)
        print(f"    check failed ({why}); the file stays as it was", file=sys.stderr)
        return "error"
    print(f"    changed {path.name} ({len(data)} -> {len(out)} bytes); the original is kept as {backup.name}")
    return "changed"


def restore(tree: Path, dry: bool) -> str:
    path, state = locate(tree)
    if path is None:
        print("    no file with the diff mappings found")
        return "not-found"
    backup = path.with_name(path.name + BACKUP_SUFFIX)
    if state == "original":
        print(f"    {path.name} is as Qwen Code installed it")
        return "original"
    if not backup.exists():
        print(f"    no backup ({backup.name}); nothing changed", file=sys.stderr)
        return "error"
    original = backup.read_bytes()
    if not is_original(original):
        print(f"    {backup.name} is not the original form; nothing changed", file=sys.stderr)
        return "error"
    if dry:
        print(f"    would put {backup.name} back")
        return "would-restore"
    ok, why = write_checked(path, original, lambda written, expected: node_check(written))
    if not ok:
        print(f"    check failed ({why}); the file stays as it was", file=sys.stderr)
        return "error"
    os.remove(backup)
    print(f"    put back the original {path.name}")
    return "restored"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--dry-run", action="store_true", help="say what would change; change nothing")
    parser.add_argument("--restore", action="store_true", help="put back the files as Qwen Code installed them")
    parser.add_argument("--root", help="only the Qwen Code copy in this folder (the one holding package.json)")
    args = parser.parse_args()

    if shutil.which("node") is None:
        print("node is not on PATH; it is needed to check the changed file. Nothing changed.", file=sys.stderr)
        return 2
    trees, running = find_trees(args.root)
    if not trees:
        print("No installed Qwen Code found. Give its folder with --root.", file=sys.stderr)
        return 2
    bad = 0
    for tree in trees:
        print(f"Qwen Code {version_of(tree)}: {tree}" + ("  (the one the qwen command runs)" if tree == running else ""))
        try:
            state = restore(tree, args.dry_run) if args.restore else patch(tree, args.dry_run)
        except OSError as error:  # PermissionError among them: this copy is left, the others are still done
            print(f"    cannot change this copy ({error}); nothing changed in it", file=sys.stderr)
            state = "error"
        bad += state in ("error", "not-found")
    if args.dry_run:
        print("Dry run: nothing was changed.")
    else:
        print("Restart Qwen Code to see the change. Run this again after every Qwen Code update.")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
